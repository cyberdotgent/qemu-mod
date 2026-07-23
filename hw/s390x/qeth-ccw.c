/*
 * Emulated OSA Express QETH adapter
 *
 * This implements the subset used by the Linux qeth OSD layer-2 driver:
 * three peer CCW subchannels, IDX/MPC/IPA control traffic, format-0 QDIO,
 * conventional PCI, and Ethernet transport through a QEMU netdev.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/s390x/css-bridge.h"
#include "hw/s390x/qeth-ccw.h"
#include "net/eth.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "migration/vmstate.h"

#define QETH_CU_TYPE             0x1731
#define QETH_CU_MODEL            0x01
#define QETH_DEV_TYPE            0x1732
#define QETH_DEV_MODEL           0x01
#define QETH_CHPID_TYPE          0x11

#define QETH_CMD_WRITE           0x01
#define QETH_CMD_READ            0x02
#define QETH_CMD_NOP             0x03
#define QETH_CMD_SENSE           0x04
#define QETH_CMD_RCD             0xfa
#define QETH_CMD_SII             0x81
#define QETH_CMD_RNI             0x82
#define QETH_CMD_EQ              0x1b
#define QETH_CMD_AQ              0x1f
#define QETH_CMD_SENSE_ID        0xe4

#define QETH_IDX_SIZE            0x22
#define QETH_MPC_CM_ENABLE_SIZE  0x63
#define QETH_MPC_CM_SETUP_SIZE   0x64
#define QETH_MPC_ULP_ENABLE_SIZE 0x6b
#define QETH_MPC_ULP_SETUP_SIZE  0x6c
#define QETH_MPC_DM_ACT_SIZE     0x55
#define QETH_MAX_CTRL            4096
#define QETH_MAX_FRAME           65536

#define QETH_IPA_STARTLAN        0x01
#define QETH_IPA_STOPLAN         0x02
#define QETH_IPA_QIPASSIST       0xb2
#define QETH_IPA_SETADAPTERPARMS 0xb8
#define QETH_IPA_SETADP_QUERY    0x00000001
#define QETH_IPA_SETADP_READ_MAC 0x00000002
#define QETH_IPA_SETADAPTER_CAP  0x00000400
#define QETH_LINK_TYPE_10GBE     0x10
#define QETH_SETADP_VIRTUAL_MAC  0x80

typedef enum QethPeerRole {
    QETH_ROLE_NONE,
    QETH_ROLE_READ,
    QETH_ROLE_WRITE,
    QETH_ROLE_DATA,
} QethPeerRole;

struct QethCcwDevice {
    CcwDevice parent_obj;
    SubchDev *peer[3];
    QethPeerRole role[3];
    uint8_t chpid;

    NICConf conf;
    NICState *nic;
    QEMUBH *tx_bh;

    S390QdioState qdio;
    SubchDev *data_sch;
    SubchDev *pending_read;
    uint8_t response[QETH_MAX_CTRL];
    size_t response_len;
    bool lan_online;
};

static int qeth_peer_index(QethCcwDevice *s, SubchDev *sch)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (s->peer[i] == sch) {
            return i;
        }
    }
    return -1;
}

static int qeth_unit_check(SubchDev *sch, uint8_t sense)
{
    sch->sense_data[0] = sense;
    sch->curr_status.scsw.dstat = SCSW_DSTAT_CHANNEL_END |
                                  SCSW_DSTAT_DEVICE_END |
                                  SCSW_DSTAT_UNIT_CHECK;
    sch->curr_status.scsw.ctrl &= ~SCSW_ACTL_START_PEND;
    sch->curr_status.scsw.ctrl &= ~SCSW_CTRL_MASK_STCTL;
    sch->curr_status.scsw.ctrl |= SCSW_STCTL_PRIMARY |
                                  SCSW_STCTL_SECONDARY |
                                  SCSW_STCTL_ALERT |
                                  SCSW_STCTL_STATUS_PEND;
    return -EIO;
}

static int qeth_read_ccw(SubchDev *sch, void *buf, size_t len)
{
    int ret = ccw_dstream_read_buf(&sch->cds, buf, len);

    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    return ret;
}

static int qeth_write_ccw(SubchDev *sch, const void *buf, size_t len)
{
    int ret = ccw_dstream_write_buf(&sch->cds, (void *)buf, len);

    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    return ret;
}

static int qeth_copy_response(SubchDev *sch, const void *buf, size_t len)
{
    size_t count = MIN(len, (size_t)ccw_dstream_avail(&sch->cds));

    return qeth_write_ccw(sch, buf, count);
}

static void qeth_deliver_response(QethCcwDevice *s)
{
    SubchDev *sch = s->pending_read;

    if (!sch || !s->response_len) {
        return;
    }
    s->pending_read = NULL;
    if (qeth_copy_response(sch, s->response, s->response_len)) {
        css_virtual_ccw_complete(sch, -EFAULT);
    } else {
        css_virtual_ccw_complete(sch, 0);
    }
    s->response_len = 0;
}

static void qeth_make_idx_response(QethCcwDevice *s, SubchDev *sch,
                                   uint8_t *buf, size_t len)
{
    int peer = qeth_peer_index(s, sch);
    uint16_t type;

    if (len < QETH_IDX_SIZE || peer < 0) {
        s->response_len = 0;
        return;
    }
    type = lduw_be_p(buf + 8);
    if (type == 0x1901) {
        s->role[peer] = QETH_ROLE_READ;
    } else if (type == 0x1501) {
        s->role[peer] = QETH_ROLE_WRITE;
    } else {
        s->response_len = 0;
        return;
    }

    memcpy(s->response, buf, QETH_IDX_SIZE);
    s->response[8] = 0x02;
    s->response[9] = 0;
    s->response[11] = 0xc0;
    stl_be_p(s->response + 12, 0x51454d55); /* "QEMU" */
    stw_be_p(s->response + 16, 0x0201);
    memcpy(s->response + 18, "QEMU", 4);
    s->response_len = QETH_IDX_SIZE;
}

static void qeth_make_mpc_response(QethCcwDevice *s, const uint8_t *buf,
                                   size_t len)
{
    if (len > QETH_MAX_CTRL) {
        s->response_len = 0;
        return;
    }
    memcpy(s->response, buf, len);
    s->response_len = len;

    switch (len) {
    case QETH_MPC_CM_ENABLE_SIZE:
        memcpy(s->response + 0x53, "QCMF", 4);
        break;
    case QETH_MPC_CM_SETUP_SIZE:
        memcpy(s->response + 0x5a, "QCMC", 4);
        break;
    case QETH_MPC_ULP_ENABLE_SIZE:
        memcpy(s->response + 0x53, "QULF", 4);
        stw_be_p(s->response + 0x57, 0);
        stw_be_p(s->response + 0x5f, 1500);
        break;
    case QETH_MPC_ULP_SETUP_SIZE:
        memcpy(s->response + 0x5a, "QULC", 4);
        break;
    case QETH_MPC_DM_ACT_SIZE:
        break;
    default:
        if (len >= 0x54 && s->response[0x18] == 0xc1) {
            uint8_t command = s->response[0x40];

            s->response[0x41] = 0;       /* host reply */
            stw_be_p(s->response + 0x44, 0);
            stl_be_p(s->response + 0x4c, 0);
            stl_be_p(s->response + 0x50, 0);
            if (command == QETH_IPA_QIPASSIST) {
                stl_be_p(s->response + 0x4c, QETH_IPA_SETADAPTER_CAP);
                stl_be_p(s->response + 0x50, QETH_IPA_SETADAPTER_CAP);
            } else if (command == QETH_IPA_SETADAPTERPARMS && len >= 0x6c) {
                uint32_t subcommand = ldl_be_p(s->response + 0x60);

                stw_be_p(s->response + 0x64, 0);
                if (subcommand == QETH_IPA_SETADP_QUERY && len >= 0x80) {
                    stl_be_p(s->response + 0x6c, 1);
                    s->response[0x70] = QETH_LINK_TYPE_10GBE;
                    stl_be_p(s->response + 0x74,
                             QETH_IPA_SETADP_READ_MAC);
                } else if (subcommand == QETH_IPA_SETADP_READ_MAC &&
                           len >= 0x7e) {
                    s->response[0x68] |= QETH_SETADP_VIRTUAL_MAC;
                    stl_be_p(s->response + 0x6c, 0);
                    stl_be_p(s->response + 0x70, ETH_ALEN);
                    stl_be_p(s->response + 0x74, 1);
                    memcpy(s->response + 0x78, s->conf.macaddr.a, ETH_ALEN);
                }
            }
            if (command == QETH_IPA_STARTLAN) {
                s->lan_online = true;
            } else if (command == QETH_IPA_STOPLAN) {
                s->lan_online = false;
            }
        }
        break;
    }
}

static void qeth_build_rcd(QethCcwDevice *s, SubchDev *sch, uint8_t out[96])
{
    int peer = qeth_peer_index(s, sch);

    memset(out, 0, 96);
    out[30] = s->chpid;
    out[31] = peer >= 0 ? s->peer[peer]->devno : 0;
    out[62] = s->chpid;
    out[63] = s->peer[0]->devno;
}

static int qeth_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    QethCcwDevice *s = sch->driver_data;
    uint8_t buf[QETH_MAX_CTRL];
    size_t len;

    switch (ccw.cmd_code) {
    case QETH_CMD_WRITE:
        len = MIN((size_t)ccw.count, sizeof(buf));
        if (qeth_read_ccw(sch, buf, len)) {
            return qeth_unit_check(sch, 0x80);
        }
        if (len >= 4 && ldl_be_p(buf) == 0x00008000) {
            qeth_make_idx_response(s, sch, buf, len);
        } else if (len >= 4 && ldl_be_p(buf) == 0x00e00000) {
            qeth_make_mpc_response(s, buf, len);
        } else {
            return qeth_unit_check(sch, 0x80);
        }
        qeth_deliver_response(s);
        return 0;
    case QETH_CMD_READ:
        if (s->response_len) {
            int ret = qeth_copy_response(sch, s->response, s->response_len);

            s->response_len = 0;
            return ret;
        }
        s->pending_read = sch;
        return CSS_CCW_PENDING;
    case QETH_CMD_RCD:
    {
        uint8_t rcd[96];

        qeth_build_rcd(s, sch, rcd);
        return qeth_copy_response(sch, rcd, sizeof(rcd));
    }
    case QETH_CMD_SII:
        return 0;
    case QETH_CMD_RNI:
        memset(buf, 0, 32);
        return qeth_copy_response(sch, buf, 32);
    case QETH_CMD_EQ:
        len = MIN((size_t)ccw.count, sizeof(buf));
        if (qeth_read_ccw(sch, buf, len) ||
            s390_qdio_establish(&s->qdio, buf, len)) {
            return qeth_unit_check(sch, 0x80);
        }
        s->data_sch = sch;
        s->role[qeth_peer_index(s, sch)] = QETH_ROLE_DATA;
        return 0;
    case QETH_CMD_AQ:
        if (!s->qdio.established || s->data_sch != sch) {
            return qeth_unit_check(sch, 0x80);
        }
        s->qdio.active = true;
        return CSS_CCW_PENDING;
    case QETH_CMD_NOP:
        return 0;
    case QETH_CMD_SENSE:
        len = MIN((size_t)ccw.count, sizeof(sch->sense_data));
        if (qeth_copy_response(sch, sch->sense_data, len)) {
            return -EFAULT;
        }
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
        return 0;
    case QETH_CMD_SENSE_ID:
        return -ENOSYS;
    default:
        return qeth_unit_check(sch, 0x80);
    }
}

static void qeth_cancel(SubchDev *sch)
{
    QethCcwDevice *s = sch->driver_data;

    if (s->pending_read == sch) {
        s->pending_read = NULL;
    }
    if (s->data_sch == sch) {
        s->qdio.active = false;
    }
}

static int qeth_siga(SubchDev *sch, uint8_t function, uint32_t output_mask,
                     uint32_t input_mask, uint64_t aob)
{
    QethCcwDevice *s = sch->driver_data;
    unsigned int i;

    if (sch != s->data_sch || !s->qdio.active) {
        return 1;
    }
    switch (function & 0x7f) {
    case 0:
    case 3:
        for (i = 0; i < s->qdio.output_count; i++) {
            if (output_mask & (0x80000000U >> i)) {
                s->qdio.output[i].mask = 0x80000000U >> i;
            }
        }
        qemu_bh_schedule(s->tx_bh);
        return 0;
    case 1:
        for (i = 0; i < s->qdio.input_count; i++) {
            if (output_mask & (0x80000000U >> i)) {
                s->qdio.input[i].mask = 0x80000000U >> i;
            }
        }
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        return 0;
    case 2:
        return 0;
    default:
        return 3;
    }
}

static void qeth_ssqd(SubchDev *sch, void *descriptor)
{
    uint8_t *d = descriptor;

    d[0] = 0xc0;
    stw_be_p(d + 2, sch->schid);
    d[4] = 0;
    d[6] = 0x64; /* SIGA input/output needed, auto-sync on output PCI */
    d[9] = 1;
    d[11] = 4;
}

static const S390QdioOps qeth_qdio_ops = {
    .siga = qeth_siga,
    .ssqd = qeth_ssqd,
};

static int qeth_read_sbal_record(uint64_t sbal, uint8_t **record,
                                 size_t *record_len, bool *pci)
{
    g_autofree uint8_t *buf = g_malloc(QETH_MAX_FRAME);
    size_t total = 0;
    int i;

    *pci = false;
    for (i = 0; i < S390_QDIO_MAX_ELEMENTS; i++) {
        uint8_t elem[16];
        uint32_t len;
        uint64_t addr;

        if (s390_qdio_read(sbal + i * 16, elem, sizeof(elem))) {
            return -EFAULT;
        }
        len = ldl_be_p(elem + 4);
        addr = ldq_be_p(elem + 8);
        if (len > QETH_MAX_FRAME - total ||
            (len && s390_qdio_read(addr, buf + total, len))) {
            return -EFAULT;
        }
        total += len;
        *pci |= elem[3] & S390_QDIO_SFLAGS_PCI_REQ;
        if (elem[0] & S390_QDIO_EFLAGS_LAST_ENTRY) {
            *record = g_steal_pointer(&buf);
            *record_len = total;
            return 0;
        }
    }
    return -EINVAL;
}

static void qeth_tx_bh(void *opaque)
{
    QethCcwDevice *s = opaque;
    unsigned int qn;
    bool interrupt = false;

    if (!s->qdio.active) {
        return;
    }
    for (qn = 0; qn < s->qdio.output_count; qn++) {
        S390QdioQueue *q = &s->qdio.output[qn];
        unsigned int drained;

        if (!q->mask) {
            continue;
        }
        for (drained = 0; drained < S390_QDIO_MAX_BUFFERS; drained++) {
            g_autofree uint8_t *record = NULL;
            uint64_t sbal;
            size_t len;
            uint8_t index;
            bool pci;

            if (s390_qdio_find_buffer(q, S390_QDIO_SLSB_OUTPUT_PRIMED,
                                      &index) ||
                s390_qdio_get_sbal(q, index, &sbal) ||
                qeth_read_sbal_record(sbal, &record, &len, &pci)) {
                break;
            }
            if (len >= 32 && record[0] == 0x02) {
                uint16_t pkt_len = lduw_be_p(record + 6);

                if (pkt_len <= len - 32) {
                    qemu_send_packet(qemu_get_queue(s->nic), record + 32,
                                     pkt_len);
                }
            }
            if (s390_qdio_set_state(q, index,
                                    S390_QDIO_SLSB_OUTPUT_EMPTY)) {
                break;
            }
            interrupt |= pci;
        }
    }
    if (interrupt) {
        css_inject_qdio_pci(s->data_sch);
    }
}

static bool qeth_can_receive(NetClientState *nc)
{
    QethCcwDevice *s = qemu_get_nic_opaque(nc);
    uint8_t index;

    return s->lan_online && s->qdio.active && s->qdio.input_count &&
           !s390_qdio_find_buffer(&s->qdio.input[0],
                                  S390_QDIO_SLSB_INPUT_EMPTY, &index);
}

static ssize_t qeth_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    QethCcwDevice *s = qemu_get_nic_opaque(nc);
    S390QdioQueue *q = &s->qdio.input[0];
    uint8_t header[32] = { 0 };
    uint8_t index;
    uint64_t sbal;
    size_t offset = 0;
    int i;

    if (len > UINT16_MAX ||
        s390_qdio_find_buffer(q, S390_QDIO_SLSB_INPUT_EMPTY, &index) ||
        s390_qdio_get_sbal(q, index, &sbal)) {
        return 0;
    }
    header[0] = 0x02;
    stw_be_p(header + 6, len);

    for (i = 0; i < S390_QDIO_MAX_ELEMENTS && offset < len + 32; i++) {
        uint8_t elem[16];
        uint32_t capacity, amount;
        uint64_t addr;
        size_t header_part;

        if (s390_qdio_read(sbal + i * 16, elem, sizeof(elem))) {
            goto error;
        }
        capacity = ldl_be_p(elem + 4);
        addr = ldq_be_p(elem + 8);
        amount = MIN((size_t)capacity, len + 32 - offset);
        header_part = offset < 32 ? MIN((size_t)amount, 32 - offset) : 0;
        if (header_part &&
            s390_qdio_write(addr, header + offset, header_part)) {
            goto error;
        }
        if (amount > header_part &&
            s390_qdio_write(addr + header_part,
                            buf + offset + header_part - 32,
                            amount - header_part)) {
            goto error;
        }
        memset(elem, 0, 4);
        if (offset == 0) {
            elem[0] |= 0x04;
        }
        offset += amount;
        if (offset == len + 32) {
            elem[0] = (elem[0] & ~0x0c) | 0x0c |
                      S390_QDIO_EFLAGS_LAST_ENTRY;
        } else {
            elem[0] = (elem[0] & ~0x0c) | 0x08;
        }
        stl_be_p(elem + 4, amount);
        if (s390_qdio_write(sbal + i * 16, elem, sizeof(elem))) {
            goto error;
        }
    }
    if (offset != len + 32 ||
        s390_qdio_set_state(q, index, S390_QDIO_SLSB_INPUT_PRIMED)) {
        goto error;
    }
    css_inject_qdio_pci(s->data_sch);
    return len;

error:
    s390_qdio_set_state(q, index, S390_QDIO_SLSB_ERROR);
    css_inject_qdio_pci(s->data_sch);
    return 0;
}

static NetClientInfo qeth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = qeth_can_receive,
    .receive = qeth_receive,
};

static void qeth_setup_peer(QethCcwDevice *s, SubchDev *sch)
{
    sch->driver_data = s;
    sch->id.reserved = 0xff;
    sch->id.cu_type = QETH_CU_TYPE;
    sch->id.cu_model = QETH_CU_MODEL;
    sch->id.dev_type = QETH_DEV_TYPE;
    sch->id.dev_model = QETH_DEV_MODEL;
    sch->id.ciw[0] = (CIW) { .type = 0, .command = QETH_CMD_RCD, .count = 96 };
    sch->id.ciw[1] = (CIW) { .type = 1, .command = QETH_CMD_SII, .count = 4 };
    sch->id.ciw[2] = (CIW) { .type = 2, .command = QETH_CMD_RNI, .count = 32 };
    sch->id.ciw[3] = (CIW) { .type = 3, .command = QETH_CMD_EQ, .count = 4096 };
    sch->id.ciw[4] = (CIW) { .type = 4, .command = QETH_CMD_AQ, .count = 0 };
    css_sch_build_virtual_schib(sch, s->chpid, QETH_CHPID_TYPE);
    sch->curr_status.pmcw.flags |= PMCW_FLAGS_MASK_QF;
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = qeth_ccw_cb;
    sch->ccw_cb_first = false;
    sch->cancel_cb = qeth_cancel;
    sch->disable_cb = qeth_cancel;
    sch->irb_cb = build_irb_virtual;
    sch->qdio_ops = &qeth_qdio_ops;
}

static void qeth_realize(DeviceState *dev, Error **errp)
{
    QethCcwDevice *s = QETH_CCW(dev);
    CcwDevice *cdev = CCW_DEVICE(dev);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    CssDevId id = cdev->devno;
    Error *local_err = NULL;
    unsigned int chpid;
    uint16_t base_devno = id.valid ? id.devid : 0;
    int i;

    if (id.valid && id.devid > UINT16_MAX - 2) {
        error_setg(errp, "qeth-ccw base devno leaves no room for three peers");
        return;
    }
    for (i = 0; i < 3; i++) {
        if (id.valid) {
            id.devid = base_devno + i;
        }
        s->peer[i] = css_create_sch(id, &local_err);
        if (!s->peer[i]) {
            goto fail;
        }
        if (i == 0) {
            id.cssid = s->peer[0]->cssid;
            id.ssid = s->peer[0]->ssid;
            id.devid = s->peer[0]->devno;
            id.valid = true;
            base_devno = s->peer[0]->devno;
        }
    }
    cdev->sch = s->peer[0];
    chpid = css_find_virtual_chpid(s->peer[0]->cssid, QETH_CHPID_TYPE);
    if (chpid > MAX_CHPID) {
        error_setg(&local_err, "No available CHPID for qeth-ccw");
        goto fail;
    }
    s->chpid = chpid;
    for (i = 0; i < 3; i++) {
        qeth_setup_peer(s, s->peer[i]);
    }
    if (!cdk->realize(cdev, &local_err)) {
        goto fail;
    }

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&qeth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    s->tx_bh = qemu_bh_new(qeth_tx_bh, s);
    for (i = 0; i < 3; i++) {
        css_generate_sch_crws(s->peer[i]->cssid, s->peer[i]->ssid,
                              s->peer[i]->schid, dev->hotplugged, 1);
    }
    return;

fail:
    error_propagate(errp, local_err);
    for (i = 0; i < 3; i++) {
        SubchDev *sch = s->peer[i];

        if (sch) {
            css_subch_assign(sch->cssid, sch->ssid, sch->schid, sch->devno,
                             NULL);
            g_free(sch);
            s->peer[i] = NULL;
        }
    }
    cdev->sch = NULL;
}

static void qeth_unrealize(DeviceState *dev)
{
    QethCcwDevice *s = QETH_CCW(dev);
    int i;

    if (s->tx_bh) {
        qemu_bh_delete(s->tx_bh);
        s->tx_bh = NULL;
    }
    if (s->nic) {
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
    for (i = 0; i < 3; i++) {
        SubchDev *sch = s->peer[i];

        if (sch) {
            css_subch_assign(sch->cssid, sch->ssid, sch->schid, sch->devno,
                             NULL);
            g_free(sch);
            s->peer[i] = NULL;
        }
    }
    CCW_DEVICE(dev)->sch = NULL;
}

static void qeth_reset_hold(Object *obj, ResetType type)
{
    QethCcwDevice *s = QETH_CCW(obj);
    int i;

    s->pending_read = NULL;
    s->response_len = 0;
    s->data_sch = NULL;
    s->lan_online = false;
    s390_qdio_reset(&s->qdio);
    memset(s->role, 0, sizeof(s->role));
    for (i = 0; i < 3; i++) {
        if (s->peer[i]) {
            css_reset_sch(s->peer[i]);
        }
    }
}

static const Property qeth_properties[] = {
    DEFINE_NIC_PROPERTIES(QethCcwDevice, conf),
};

static const VMStateDescription vmstate_qeth = {
    .name = TYPE_QETH_CCW,
    .unmigratable = 1,
};

static void qeth_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = qeth_realize;
    dc->unrealize = qeth_unrealize;
    dc->hotpluggable = false;
    dc->vmsd = &vmstate_qeth;
    device_class_set_props(dc, qeth_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    rc->phases.hold = qeth_reset_hold;
}

static const TypeInfo qeth_info = {
    .name = TYPE_QETH_CCW,
    .parent = TYPE_CCW_DEVICE,
    .instance_size = sizeof(QethCcwDevice),
    .class_init = qeth_class_init,
};

static void qeth_register_types(void)
{
    type_register_static(&qeth_info);
}

type_init(qeth_register_types)
