/*
 * Emulated OSA-Express QETH adapter (OSD)
 *
 * The adapter presents consecutive peer CCW subchannels: read and write
 * subchannels carrying the IDX, MPC (CM/ULP/DM) and IPA control protocols,
 * and one or more data subchannels, each carrying the format-0 QDIO data
 * path (with conventional PCI interruptions) of one ULP connection.  All
 * connections share one QEMU network backend.
 *
 * Both QETH modes are supported, selected by the guest per connection in
 * ULP_ENABLE:
 *  - layer 2: the guest exchanges Ethernet frames (qeth header type 2);
 *  - layer 3: the guest exchanges IP packets (qeth header type 1) and the
 *    adapter owns the link layer: it registers the guest's IP addresses
 *    (SETIP), answers and resolves ARP on the guest's behalf, maps
 *    broadcast and multicast destinations, and adds or strips the Ethernet
 *    header.  For IPv6 the adapter also answers and resolves neighbour
 *    discovery for guests that hand it plain IPv6 packets, while guests
 *    that resolve neighbours themselves pass complete Ethernet frames.
 *
 * Protocol layouts follow the Linux qeth driver and the architecture as
 * observed on working OSA implementations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/s390x/css-bridge.h"
#include "hw/s390x/qeth-ccw.h"
#include "net/checksum.h"
#include "net/eth.h"
#include "net/net.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "system/runstate.h"
#include "trace.h"

#define QETH_CU_TYPE             0x1731
#define QETH_CU_MODEL            0x01
#define QETH_DEV_TYPE            0x1732
#define QETH_DEV_MODEL           0x01
#define QETH_CHPID_TYPE          0x11
#define QETH_AUTO_DEVNO          0x0400
#define QETH_MAX_DATA            8      /* data devices per adapter */
#define QETH_MAX_PEERS           (2 + QETH_MAX_DATA)

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

/* Self-description */
#define QETH_RCD_SIZE            128
#define QETH_RNI_SIZE            64
#define QETH_CULA                0x00
#define QETH_LINK_ADDRESS        0x0d
#define QETH_NED_FLAGS           0xd0   /* NED, serial number valid */
#define QETH_NED_TOKEN           0x20
#define QETH_NED_TYPE_DEVICE     0x01
#define QETH_NED_TYPE_CU         0x02
#define QETH_NED_CLASS_COMM      0x06
#define QETH_NEQ_GENERAL         0x80
#define QETH_CIW_VALID           0x40

#define QETH_MAX_CTRL            4096
#define QETH_MAX_FRAME           65536
#define QETH_HDR_SIZE            32
#define QETH_RESPONSES_MAX       16

/* IDX */
#define QETH_IDX_SIZE            0x22
#define QETH_IDX_REQ             0x00008000
#define QETH_IDX_TYPE_READ       0x1901
#define QETH_IDX_TYPE_WRITE      0x1501

/*
 * Adapter tokens ("QEMU", "QEM1", "QEM2" in ASCII; per-connection ULP
 * tokens are "QF" / "QC" followed by the connection index)
 */
#define QETH_TOKEN_ISSUER        0x51454d55
#define QETH_TOKEN_CM_FILTER     0x51454d31
#define QETH_TOKEN_CM_CONN       0x51454d32
#define QETH_TOKEN_ULP_FILTER    0x51460000
#define QETH_TOKEN_ULP_CONN      0x51430000

/* MPC transport header, request/response header, PDU header */
#define MPC_TH_FIRST4            0x00e00000
#define MPC_TH_SIZE              0x14
#define MPC_RRH_SIZE_SHORT       0x1c
#define MPC_PH_SIZE              0x08
#define MPC_PUK_SIZE             0x0c
#define MPC_RRH_TYPE_CM          0x81
#define MPC_RRH_TYPE_ULP         0x41
#define MPC_RRH_TYPE_IPA         0xc1
#define MPC_PROTO_ULP            0x02
#define MPC_PROTO_UNKNOWN        0x7e
#define MPC_PROT_LAYER2          0x08
#define MPC_PUK_WHAT_41          0x41
#define MPC_PUK_WHAT_43          0x43
#define MPC_PUK_ENABLE           0x02
#define MPC_PUK_DISABLE          0x03
#define MPC_PUK_SETUP            0x04
#define MPC_PUK_TAKEDOWN         0x05
#define MPC_PUK_CONFIRM          0x06
#define MPC_PUK_ACTIVE           0x60
#define MPC_TOKEN_X5             0x05

/* IPA */
#define IPA_HDR_SIZE             0x14
#define IPA_HDR_SIZE_SHORT       0x10
#define IPA_INITIATOR_HOST       0x00
#define IPA_PROT_IPV4            4
#define IPA_PROT_IPV6            6

#define IPA_CMD_STARTLAN         0x01
#define IPA_CMD_STOPLAN          0x02
#define IPA_CMD_SETVMAC          0x21
#define IPA_CMD_DELVMAC          0x22
#define IPA_CMD_SETGMAC          0x23
#define IPA_CMD_DELGMAC          0x24
#define IPA_CMD_SETVLAN          0x25
#define IPA_CMD_DELVLAN          0x26
#define IPA_CMD_SETIP            0xb1
#define IPA_CMD_QIPASSIST        0xb2
#define IPA_CMD_SETASSPARMS      0xb3
#define IPA_CMD_SETIPM           0xb4
#define IPA_CMD_DELIPM           0xb5
#define IPA_CMD_SETRTG           0xb6
#define IPA_CMD_DELIP            0xb7
#define IPA_CMD_SETADAPTERPARMS  0xb8
#define IPA_CMD_SET_DIAG_ASS     0xb9
#define IPA_CMD_CREATE_ADDR      0xc3

#define IPA_RC_SUCCESS           0x0000
#define IPA_RC_NOTSUPP           0x0001
#define IPA_RC_UNREGISTERED_ADDR 0x0011
#define IPA_RC_INVALID_IP_VERSION 0x0020
#define IPA_RC_IP_ADDR_TABLE_FULL 0xe006
#define IPA_RC_IP_ADDR_ALREADY_USED 0xe00a
#define IPA_RC_UNSUPPORTED_SUBCMD 0xe00e

#define IPA_ARP_PROCESSING       0x00000001
#define IPA_FILTERING            0x00000010
#define IPA_IPV6                 0x00000020
#define IPA_MULTICASTING         0x00000040
#define IPA_QUERY_ARP_COUNTERS   0x00000100
#define IPA_SETADAPTERPARMS      0x00000400
#define IPA_PASSTHRU             0x00001000

#define IPA_ASSISTS_V4           (IPA_ARP_PROCESSING | IPA_FILTERING | \
                                  IPA_IPV6 | IPA_MULTICASTING | \
                                  IPA_QUERY_ARP_COUNTERS | \
                                  IPA_SETADAPTERPARMS | IPA_PASSTHRU)
#define IPA_ASSISTS_V6           (IPA_IPV6 | IPA_MULTICASTING | \
                                  IPA_SETADAPTERPARMS | IPA_PASSTHRU)
#define IPA_ASSISTS_L2           IPA_SETADAPTERPARMS

#define IPA_ASS_START            0x0001
#define IPA_ASS_STOP             0x0002

#define IPA_SETDELIP_DEFAULT     0x00
#define IPA_SETIP_VIPA           0x01
#define IPA_SETIP_TAKEOVER       0x02

#define IPA_SETADP_QUERY         0x00000001
#define IPA_SETADP_ALTER_MAC     0x00000002
#define IPA_SETADP_CARD_INFO     0x00000400
#define IPA_SETADP_PROMISC       0x00000800
#define IPA_SETADP_ACCESS_CTRL   0x00010000
#define IPA_SETADP_SUPPORTED     (IPA_SETADP_QUERY | IPA_SETADP_ALTER_MAC | \
                                  IPA_SETADP_CARD_INFO | IPA_SETADP_PROMISC | \
                                  IPA_SETADP_ACCESS_CTRL)
#define IPA_SETADP_HDR_SIZE      24     /* hw_cmds caps + subcommand header */
#define IPA_SETADP_VIRTUAL_MAC   0x80

#define QETH_LINK_TYPE_10GBE     0x10
#define QETH_MTU_LAYER2          1500
#define QETH_MTU_LAYER3          1492

/* qeth header */
#define QETH_HDR_LAYER3          0x01
#define QETH_HDR_LAYER2          0x02
#define QETH_HDR3_IPV6           0x80
#define QETH_HDR3_PASSTHRU       0x10
#define QETH_HDR3_CAST_MASK      0x07
#define QETH_CAST_MULTICAST      0x04
#define QETH_CAST_BROADCAST      0x05
#define QETH_CAST_UNICAST        0x06

/* Layer-3 link-layer emulation */
#define QETH_MAX_IP4             32
#define QETH_MAX_IP6             16
#define QETH_MAX_MC              64
#define QETH_ARP_ENTRIES         64
#define QETH_PENDING_MAX         32
#define QETH_ARP_RETRY_MS        1000
#define QETH_ND_ENTRIES          64

#define ETH_HDR                  14
#define ETHERTYPE_IPV4           0x0800
#define ETHERTYPE_ARP            0x0806
#define ETHERTYPE_IPV6           0x86dd
#define ARP_PACKET               28
#define ARP_REQUEST              1
#define ARP_REPLY                2
#define IP6_HDR                  40
#define IP6_NEXT_ICMP6           58
#define ICMP6_ND_SOLICIT         135
#define ICMP6_ND_ADVERT          136
#define ICMP6_ND_SIZE            24     /* type .. target address */
#define ICMP6_ND_OPT_SRC_LL      1
#define ICMP6_ND_OPT_TGT_LL      2
#define ICMP6_NA_SOLICITED       0x40
#define ICMP6_NA_OVERRIDE        0x20

typedef enum QethPeerRole {
    QETH_ROLE_NONE,
    QETH_ROLE_READ,
    QETH_ROLE_WRITE,
    QETH_ROLE_DATA,
} QethPeerRole;

/* Address table entries are owned by the connection that registered them */
typedef struct QethIp4 {
    uint32_t addr;
    uint32_t flags;
    int conn;
} QethIp4;

typedef struct QethIp6 {
    uint8_t addr[16];
    int conn;
} QethIp6;

typedef struct QethMc {
    uint8_t mac[ETH_ALEN];
    int conn;
} QethMc;

/*
 * A ULP connection: one guest interface on the adapter, set up by
 * ULP_ENABLE / ULP_SETUP / DM_ACT on the control subchannels and carried
 * by its own data subchannel and QDIO queues.  A guest may run several
 * connections over one adapter, for example separate IPv4 and IPv6
 * interfaces, each with its own data device.
 */
typedef struct QethConn {
    bool in_use;
    bool activated;             /* its queues have been active */
    int data_peer;              /* index into peer[], or -1 */
    uint32_t guest_ulp_filter;
    uint32_t guest_ulp_conn;
    bool layer3;
    bool lan_online;
    uint32_t assists_enabled[2];    /* IPv4, IPv6 */
    S390QdioState qdio;
} QethConn;

typedef struct QethArpEntry {
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    int64_t used;
} QethArpEntry;

typedef struct QethNdEntry {
    uint8_t ip[16];
    uint8_t mac[ETH_ALEN];
    int64_t used;
} QethNdEntry;

typedef struct QethPending {
    bool v6;
    uint32_t ip;
    uint8_t ip6[16];
    size_t len;
    uint8_t *frame;     /* Ethernet frame, destination MAC not yet set */
} QethPending;

struct QethCcwDevice {
    CcwDevice parent_obj;
    SubchDev *peer[QETH_MAX_PEERS];
    uint8_t role[QETH_MAX_PEERS];       /* QethPeerRole */
    unsigned int nr_peers;
    uint8_t data_devices;
    uint8_t chpid;
    uint16_t chpid_prop;

    NICConf conf;
    NICState *nic;
    QEMUBH *tx_bh;
    VMChangeStateEntry *vmstate_change;

    /* Control responses queued per subchannel, and outstanding READs */
    GQueue responses[QETH_MAX_PEERS];
    bool read_pending[QETH_MAX_PEERS];
    uint32_t sii;

    /* MPC connection state */
    uint32_t guest_issuer_token;
    uint32_t guest_cm_filter;
    uint32_t guest_cm_conn;
    uint32_t ulp_seq;
    QethConn conn[QETH_MAX_DATA];
    int last_enabled;

    /* Layer-3 address tables and link-layer emulation */
    QethIp4 ip4[QETH_MAX_IP4];
    unsigned int nr_ip4;
    QethIp6 ip6[QETH_MAX_IP6];
    unsigned int nr_ip6;
    QethMc mc[QETH_MAX_MC];
    unsigned int nr_mc;
    QethArpEntry arp[QETH_ARP_ENTRIES];
    GQueue pending;
    int64_t arp_last_request;
    uint32_t arp_last_ip;
    QethNdEntry nd[QETH_ND_ENTRIES];
    int64_t nd_last_request;
    uint8_t nd_last_ip[16];

    /* Queued control responses, serialized for migration */
    uint8_t *migration_responses;
    uint32_t migration_responses_len;
};

static int qeth_peer_index(QethCcwDevice *s, SubchDev *sch)
{
    int i;

    for (i = 0; i < s->nr_peers; i++) {
        if (s->peer[i] == sch) {
            return i;
        }
    }
    return -1;
}

static int qeth_peer_with_role(QethCcwDevice *s, QethPeerRole role)
{
    int i;

    for (i = 0; i < s->nr_peers; i++) {
        if (s->role[i] == role) {
            return i;
        }
    }
    return -1;
}

/* ULP connections */

static int qeth_conn_index(QethCcwDevice *s, QethConn *c)
{
    return c - s->conn;
}

static unsigned int qeth_nr_conns(QethCcwDevice *s)
{
    return s->nr_peers - 2;
}

static QethConn *qeth_conn_by_peer(QethCcwDevice *s, int peer)
{
    unsigned int i;

    for (i = 0; peer >= 0 && i < qeth_nr_conns(s); i++) {
        if (s->conn[i].in_use && s->conn[i].data_peer == peer) {
            return &s->conn[i];
        }
    }
    return NULL;
}

/* Connection named by one of the adapter's per-connection tokens */
static QethConn *qeth_conn_by_token(QethCcwDevice *s, uint32_t token,
                                    uint32_t base)
{
    uint32_t i = token - base;

    if (i < qeth_nr_conns(s) && s->conn[i].in_use) {
        return &s->conn[i];
    }
    return NULL;
}

static SubchDev *qeth_conn_sch(QethCcwDevice *s, QethConn *c)
{
    return c->data_peer >= 0 ? s->peer[c->data_peer] : NULL;
}

static void qeth_trace_data(SubchDev *sch, const char *dir,
                            const uint8_t *buf, size_t len)
{
    size_t off;

    if (!trace_event_get_state_backends(TRACE_QETH_CTRL_DATA)) {
        return;
    }
    for (off = 0; off < len; off += 32) {
        char hex[32 * 2 + 1];
        size_t i, n = MIN(len - off, (size_t)32);

        for (i = 0; i < n; i++) {
            snprintf(hex + i * 2, 3, "%02x", buf[off + i]);
        }
        trace_qeth_ctrl_data(sch->devno, dir, off, hex);
    }
}

static void qeth_unhandled(SubchDev *sch, const char *what, uint32_t value)
{
    trace_qeth_unhandled(sch ? sch->devno : 0, what, value);
}

static int qeth_unit_check(SubchDev *sch, uint8_t cmd, uint8_t sense)
{
    trace_qeth_unit_check(sch->devno, cmd, sense);
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

    qeth_trace_data(sch, "to-guest", buf, count);
    return qeth_write_ccw(sch, buf, count);
}

/*
 * Control responses.  IDX responses are read back on the subchannel that
 * issued the request; MPC and IPA responses arrive on the read subchannel.
 */
static void qeth_queue_response(QethCcwDevice *s, int peer,
                                const uint8_t *buf, size_t len)
{
    GByteArray *rsp;
    SubchDev *sch;

    if (peer < 0) {
        qeth_unhandled(NULL, "response without read subchannel", len);
        return;
    }
    if (g_queue_get_length(&s->responses[peer]) >= QETH_RESPONSES_MAX) {
        qeth_unhandled(s->peer[peer], "response queue full", len);
        return;
    }
    rsp = g_byte_array_sized_new(len);
    g_byte_array_append(rsp, buf, len);
    g_queue_push_tail(&s->responses[peer], rsp);

    if (!s->read_pending[peer]) {
        return;
    }
    sch = s->peer[peer];
    s->read_pending[peer] = false;
    rsp = g_queue_pop_head(&s->responses[peer]);
    css_virtual_ccw_complete(sch, qeth_copy_response(sch, rsp->data, rsp->len) ?
                             -EFAULT : 0);
    g_byte_array_unref(rsp);
}

static void qeth_flush_responses(QethCcwDevice *s)
{
    int i;

    for (i = 0; i < QETH_MAX_PEERS; i++) {
        GByteArray *rsp;

        while ((rsp = g_queue_pop_head(&s->responses[i]))) {
            g_byte_array_unref(rsp);
        }
        s->read_pending[i] = false;
    }
}

/*
 * IDX ACTIVATE: the read and write subchannels identify themselves and
 * list the data device(s).  The response echoes the request with a
 * positive response code in the low two bits of byte 8 (the high bits keep
 * the request type, which guests check), no port name required, the
 * adapter's issuer token, function level 0x0201 and a microcode level.
 */
static void qeth_process_idx(QethCcwDevice *s, SubchDev *sch,
                             const uint8_t *buf, size_t len)
{
    uint8_t rsp[QETH_MAX_CTRL];
    int peer = qeth_peer_index(s, sch);
    uint16_t type;

    if (len < QETH_IDX_SIZE || peer < 0) {
        qeth_unhandled(sch, "short idx", len);
        return;
    }
    type = lduw_be_p(buf + 8);
    trace_qeth_idx(sch->devno, type, buf[11], ldl_be_p(buf + 12));
    if (type == QETH_IDX_TYPE_READ) {
        s->role[peer] = QETH_ROLE_READ;
    } else if (type == QETH_IDX_TYPE_WRITE) {
        s->role[peer] = QETH_ROLE_WRITE;
        s->guest_issuer_token = ldl_be_p(buf + 12);
    } else {
        qeth_unhandled(sch, "idx type", type);
        return;
    }

    memcpy(rsp, buf, len);
    rsp[8] = (buf[8] & ~0x03) | 0x02;
    rsp[11] = 0xc0;
    stl_be_p(rsp + 12, QETH_TOKEN_ISSUER);
    stw_be_p(rsp + 16, 0x0201);
    memcpy(rsp + 18, "QEMU", 4);
    qeth_queue_response(s, peer, rsp, len);
}

/* MPC protocol data units */

static const uint8_t *qeth_find_pus(const uint8_t *puk, size_t len,
                                    uint8_t type, size_t *pus_len)
{
    size_t off = MIN((size_t)lduw_be_p(puk), len);

    while (off + 4 <= len) {
        size_t l = lduw_be_p(puk + off);

        if (l < 4 || off + l > len) {
            break;
        }
        if (puk[off + 3] == type) {
            *pus_len = l;
            return puk + off;
        }
        off += l;
    }
    return NULL;
}

static size_t qeth_pus_token(uint8_t *out, uint8_t type, uint32_t token)
{
    out[0] = 0;
    out[1] = 9;
    out[2] = 0x04;
    out[3] = type;
    out[4] = MPC_TOKEN_X5;
    stl_be_p(out + 5, token);
    return 9;
}

static size_t qeth_pus_filter(uint8_t *out, uint8_t proto, uint32_t token)
{
    out[0] = 0;
    out[1] = 0x0b;
    out[2] = 0x04;
    out[3] = 0x01;
    out[4] = proto;
    out[5] = 0x04;
    out[6] = MPC_TOKEN_X5;
    stl_be_p(out + 7, token);
    return 0x0b;
}

/*
 * Build a CM/ULP/DM response PDU: transport header, short request/response
 * header, one PDU header and a PUK with the given structured fields.
 */
static void qeth_mpc_reply(QethCcwDevice *s, uint8_t rrh_type,
                           uint8_t rrh_proto, uint32_t token,
                           uint32_t seq, uint32_t ackseq,
                           uint8_t puk_what, uint8_t puk_type,
                           const uint8_t *pus, size_t pus_len)
{
    uint8_t out[QETH_MAX_CTRL] = { 0 };
    const size_t hdr = MPC_TH_SIZE + MPC_RRH_SIZE_SHORT + MPC_PH_SIZE;
    size_t data_len = MPC_PUK_SIZE + pus_len;
    uint8_t *rrh = out + MPC_TH_SIZE;
    uint8_t *ph = rrh + MPC_RRH_SIZE_SHORT;
    uint8_t *puk = out + hdr;

    stl_be_p(out, MPC_TH_FIRST4);
    stl_be_p(out + 8, MPC_TH_SIZE);
    stl_be_p(out + 0x0c, hdr + data_len);
    stw_be_p(out + 0x10, 0x0ffc);
    stw_be_p(out + 0x12, 1);

    rrh[4] = rrh_type;
    rrh[5] = rrh_proto;
    stw_be_p(rrh + 6, 1);
    stl_be_p(rrh + 8, seq);
    stl_be_p(rrh + 0x0c, ackseq);
    stw_be_p(rrh + 0x10, MPC_RRH_SIZE_SHORT);
    stw_be_p(rrh + 0x12, data_len);
    stl_be_p(rrh + 0x14, data_len << 8 | MPC_TOKEN_X5);
    stl_be_p(rrh + 0x18, token);

    stl_be_p(ph, 0x01000000 | data_len);
    stl_be_p(ph + 4, hdr);

    stw_be_p(puk, MPC_PUK_SIZE);
    puk[2] = puk_what;
    puk[3] = puk_type;
    stw_be_p(puk + 4, pus_len);
    memcpy(puk + MPC_PUK_SIZE, pus, pus_len);

    qeth_queue_response(s, qeth_peer_with_role(s, QETH_ROLE_READ), out,
                        hdr + data_len);
}

/* Drop the addresses a connection registered */
static void qeth_forget_addrs(QethCcwDevice *s, int conn)
{
    unsigned int i;

    for (i = 0; i < s->nr_ip4;) {
        if (s->ip4[i].conn == conn) {
            s->ip4[i] = s->ip4[--s->nr_ip4];
        } else {
            i++;
        }
    }
    for (i = 0; i < s->nr_ip6;) {
        if (s->ip6[i].conn == conn) {
            s->ip6[i] = s->ip6[--s->nr_ip6];
        } else {
            i++;
        }
    }
    for (i = 0; i < s->nr_mc;) {
        if (s->mc[i].conn == conn) {
            s->mc[i] = s->mc[--s->nr_mc];
        } else {
            i++;
        }
    }
}

static void qeth_conn_release(QethCcwDevice *s, QethConn *c)
{
    qeth_forget_addrs(s, qeth_conn_index(s, c));
    s390_qdio_reset(&c->qdio);
    *c = (QethConn) { .data_peer = -1 };
}

/*
 * ULP_ENABLE starts a connection: use a free slot, else one whose queues
 * were active and have been stopped, else one still being set up.
 */
static QethConn *qeth_conn_alloc(QethCcwDevice *s)
{
    unsigned int i, n = qeth_nr_conns(s);
    QethConn *c = NULL;

    for (i = 0; !c && i < n; i++) {
        if (!s->conn[i].in_use) {
            c = &s->conn[i];
        }
    }
    for (i = 0; !c && i < n; i++) {
        if (s->conn[i].activated && !s->conn[i].qdio.active) {
            c = &s->conn[i];
        }
    }
    for (i = 0; !c && i < n; i++) {
        if (!s->conn[i].qdio.active) {
            c = &s->conn[i];
        }
    }
    if (!c) {
        return NULL;
    }
    qeth_conn_release(s, c);
    c->in_use = true;
    s->last_enabled = qeth_conn_index(s, c);
    return c;
}

/* Connection named by a token structured field of a ULP PUK */
static QethConn *qeth_conn_from_pus(QethCcwDevice *s, const uint8_t *puk,
                                    size_t len)
{
    static const uint8_t types[] = { 0x04, 0x05, 0x08 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(types); i++) {
        const uint8_t *p;
        QethConn *c;
        size_t l;

        p = qeth_find_pus(puk, len, types[i], &l);
        if (!p || l < 9) {
            continue;
        }
        c = qeth_conn_by_token(s, ldl_be_p(p + 5), QETH_TOKEN_ULP_CONN);
        if (!c) {
            c = qeth_conn_by_token(s, ldl_be_p(p + 5), QETH_TOKEN_ULP_FILTER);
        }
        if (c) {
            return c;
        }
    }
    return NULL;
}

/*
 * Data subchannel named in ULP_SETUP: device number, then control-unit
 * logical address and unit address of the data device.
 */
static int qeth_peer_for_cua(QethCcwDevice *s, const uint8_t *pus, size_t l)
{
    unsigned int i;

    if (l < 8) {
        return -1;
    }
    for (i = 0; i < s->nr_peers; i++) {
        if (s->peer[i]->devno == lduw_be_p(pus + 4)) {
            return i;
        }
    }
    for (i = 0; i < s->nr_peers; i++) {
        if ((s->peer[i]->devno & 0xff) == pus[7]) {
            return i;
        }
    }
    return -1;
}

static void qeth_process_puk(QethCcwDevice *s, SubchDev *sch,
                             const uint8_t *rrh, const uint8_t *puk,
                             size_t len)
{
    uint8_t pus[256];
    size_t n = 0, l;
    const uint8_t *p;
    uint32_t req_seq = ldl_be_p(rrh + 8);
    uint8_t what = puk[2], type = puk[3];
    QethConn *c;

    trace_qeth_mpc(rrh[4], rrh[5], what, type, len);

    if (what == MPC_PUK_WHAT_43 && type == MPC_PUK_ACTIVE) {
        /* DM_ACT names the connection by the adapter's token */
        c = qeth_conn_from_pus(s, puk, len);
        if (!c) {
            goto bad;
        }
        trace_qeth_conn(qeth_conn_index(s, c), "activate", 0);
        n += qeth_pus_token(pus + n, 0x04, c->guest_ulp_conn);
        qeth_mpc_reply(s, MPC_RRH_TYPE_ULP, MPC_PROTO_ULP, s->guest_cm_conn,
                       ++s->ulp_seq, req_seq, MPC_PUK_WHAT_43,
                       MPC_PUK_ACTIVE, pus, n);
        return;
    }
    if (type == MPC_PUK_TAKEDOWN || type == MPC_PUK_DISABLE) {
        if (rrh[4] == MPC_RRH_TYPE_ULP) {
            c = qeth_conn_from_pus(s, puk, len);
            if (c) {
                trace_qeth_conn(qeth_conn_index(s, c), "release", type);
                qeth_conn_release(s, c);
            }
        }
        return;     /* no response */
    }

    if (rrh[4] == MPC_RRH_TYPE_CM && type == MPC_PUK_ENABLE) {
        p = qeth_find_pus(puk, len, 0x01, &l);
        if (!p || l < 0x0b) {
            goto bad;
        }
        s->guest_cm_filter = ldl_be_p(p + 7);
        n += qeth_pus_filter(pus + n, MPC_PROTO_UNKNOWN, QETH_TOKEN_CM_FILTER);
        pus[n + 1] = 0x0c;              /* PUS 02: eight null bytes */
        pus[n + 2] = 0x04;
        pus[n + 3] = 0x02;
        memset(pus + n + 4, 0, 8);
        n += 0x0c;
        qeth_mpc_reply(s, MPC_RRH_TYPE_CM, MPC_PROTO_UNKNOWN,
                       s->guest_issuer_token, 0, 0, MPC_PUK_WHAT_41,
                       MPC_PUK_ENABLE, pus, n);
    } else if (rrh[4] == MPC_RRH_TYPE_CM && type == MPC_PUK_SETUP) {
        p = qeth_find_pus(puk, len, 0x04, &l);
        if (!p || l < 9) {
            goto bad;
        }
        s->guest_cm_conn = ldl_be_p(p + 5);
        n += qeth_pus_token(pus + n, 0x04, s->guest_cm_conn);
        n += qeth_pus_token(pus + n, 0x08, QETH_TOKEN_CM_CONN);
        stl_be_p(pus + n, 0x00080407);  /* PUS 07 */
        stl_be_p(pus + n + 4, 0x40000000);
        n += 8;
        qeth_mpc_reply(s, MPC_RRH_TYPE_CM, MPC_PROTO_UNKNOWN,
                       s->guest_issuer_token, 0, 0, MPC_PUK_WHAT_41,
                       MPC_PUK_CONFIRM, pus, n);
    } else if (rrh[4] == MPC_RRH_TYPE_ULP && type == MPC_PUK_ENABLE) {
        const uint8_t *pus0a;
        size_t l0a, rsp_l0a;

        p = qeth_find_pus(puk, len, 0x01, &l);
        pus0a = qeth_find_pus(puk, len, 0x0a, &l0a);
        if (!p || l < 0x0b || !pus0a || l0a < 0x14 || l0a > 64) {
            goto bad;
        }
        c = qeth_conn_alloc(s);
        if (!c) {
            qeth_unhandled(sch, "no free ulp connection", qeth_nr_conns(s));
            return;
        }
        /* Layer 2 is protocol 0x08; TCP/IP protocols are layer 3 */
        c->layer3 = p[4] != MPC_PROT_LAYER2;
        c->guest_ulp_filter = ldl_be_p(p + 7);
        trace_qeth_conn(qeth_conn_index(s, c), "enable", p[4]);
        trace_qeth_l3("ulp-enable", c->layer3 ? "layer 3" : "layer 2");
        /* The response repeats the requested protocol (layer 2 or 3) */
        n += qeth_pus_filter(pus + n, p[4],
                             QETH_TOKEN_ULP_FILTER + qeth_conn_index(s, c));
        rsp_l0a = MAX(l0a, (size_t)0x18);
        memset(pus + n, 0, rsp_l0a);
        memcpy(pus + n, pus0a, l0a);
        stw_be_p(pus + n, rsp_l0a);
        stw_be_p(pus + n + 8, c->layer3 ? QETH_MTU_LAYER3 : QETH_MTU_LAYER2);
        pus[n + 0x14] = QETH_LINK_TYPE_10GBE;
        n += rsp_l0a;
        qeth_mpc_reply(s, MPC_RRH_TYPE_ULP, MPC_PROTO_ULP, s->guest_cm_conn,
                       ++s->ulp_seq, req_seq, MPC_PUK_WHAT_41,
                       MPC_PUK_ENABLE, pus, n);
    } else if (rrh[4] == MPC_RRH_TYPE_ULP && type == MPC_PUK_SETUP) {
        const uint8_t *pus06, *pus0b;
        size_t l06, l0b;

        p = qeth_find_pus(puk, len, 0x04, &l);
        pus06 = qeth_find_pus(puk, len, 0x06, &l06);
        pus0b = qeth_find_pus(puk, len, 0x0b, &l0b);
        if (!p || l < 9 || !pus06 || l06 < 6 || !pus0b || l0b > 64) {
            goto bad;
        }
        /* The filter token names the connection, PUS 0B its data device */
        c = qeth_conn_from_pus(s, puk, len);
        if (!c && s->conn[s->last_enabled].in_use) {
            c = &s->conn[s->last_enabled];
        }
        if (!c) {
            goto bad;
        }
        c->guest_ulp_conn = ldl_be_p(p + 5);
        c->data_peer = qeth_peer_for_cua(s, pus0b, l0b);
        trace_qeth_conn(qeth_conn_index(s, c), "setup",
                        l0b >= 8 ? ldl_be_p(pus0b + 4) : 0);
        n += qeth_pus_token(pus + n, 0x04, c->guest_ulp_conn);
        n += qeth_pus_token(pus + n, 0x08,
                            QETH_TOKEN_ULP_CONN + qeth_conn_index(s, c));
        stl_be_p(pus + n, 0x00080407);  /* PUS 07 echoes PUS 06 */
        memcpy(pus + n + 4, pus06 + 4, 2);
        stw_be_p(pus + n + 6, 0);
        n += 8;
        memcpy(pus + n, pus0b, l0b);
        n += l0b;
        qeth_mpc_reply(s, MPC_RRH_TYPE_ULP, MPC_PROTO_ULP, s->guest_cm_conn,
                       ++s->ulp_seq, req_seq, MPC_PUK_WHAT_41,
                       MPC_PUK_CONFIRM, pus, n);
    } else {
        qeth_unhandled(sch, "mpc puk", rrh[4] << 16 | what << 8 | type);
    }
    return;

bad:
    qeth_unhandled(sch, "malformed mpc puk", rrh[4] << 16 | what << 8 | type);
}

/* Layer-3 address tables */

static int qeth_find_ip4(QethCcwDevice *s, uint32_t addr)
{
    unsigned int i;

    for (i = 0; i < s->nr_ip4; i++) {
        if (s->ip4[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

static int qeth_find_ip6(QethCcwDevice *s, const uint8_t *addr)
{
    unsigned int i;

    for (i = 0; i < s->nr_ip6; i++) {
        if (!memcmp(s->ip6[i].addr, addr, 16)) {
            return i;
        }
    }
    return -1;
}

static int qeth_find_mc(QethCcwDevice *s, int conn, const uint8_t *mac)
{
    unsigned int i;

    for (i = 0; i < s->nr_mc; i++) {
        if (s->mc[i].conn == conn && !memcmp(s->mc[i].mac, mac, ETH_ALEN)) {
            return i;
        }
    }
    return -1;
}

/* Connections (as a bit mask) that registered a multicast MAC address */
static uint32_t qeth_mc_conns(QethCcwDevice *s, const uint8_t *mac)
{
    uint32_t mask = 0;
    unsigned int i;

    for (i = 0; i < s->nr_mc; i++) {
        if (!memcmp(s->mc[i].mac, mac, ETH_ALEN)) {
            mask |= 1u << s->mc[i].conn;
        }
    }
    return mask;
}

static void qeth_ip4_str(uint32_t ip, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff,
             (ip >> 8) & 0xff, ip & 0xff);
}

static void qeth_send_arp(QethCcwDevice *s, uint16_t op, uint32_t sender_ip,
                          const uint8_t *target_mac, uint32_t target_ip)
{
    static const uint8_t broadcast[ETH_ALEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    uint8_t f[ETH_HDR + ARP_PACKET];
    const uint8_t *dst = op == ARP_REPLY ? target_mac : broadcast;

    memcpy(f, dst, ETH_ALEN);
    memcpy(f + 6, s->conf.macaddr.a, ETH_ALEN);
    stw_be_p(f + 12, ETHERTYPE_ARP);
    stw_be_p(f + 14, 1);                    /* Ethernet */
    stw_be_p(f + 16, ETHERTYPE_IPV4);
    f[18] = ETH_ALEN;
    f[19] = 4;
    stw_be_p(f + 20, op);
    memcpy(f + 22, s->conf.macaddr.a, ETH_ALEN);
    stl_be_p(f + 28, sender_ip);
    if (op == ARP_REPLY) {
        memcpy(f + 32, target_mac, ETH_ALEN);
    } else {
        memset(f + 32, 0, ETH_ALEN);
    }
    stl_be_p(f + 38, target_ip);
    qemu_send_packet(qemu_get_queue(s->nic), f, sizeof(f));
}

static QethArpEntry *qeth_arp_lookup(QethCcwDevice *s, uint32_t ip)
{
    int i;

    for (i = 0; i < QETH_ARP_ENTRIES; i++) {
        if (s->arp[i].used && s->arp[i].ip == ip) {
            s->arp[i].used = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            return &s->arp[i];
        }
    }
    return NULL;
}

static void qeth_flush_pending(QethCcwDevice *s, uint32_t ip,
                               const uint8_t *mac)
{
    GList *l = s->pending.head;

    while (l) {
        GList *next = l->next;
        QethPending *p = l->data;

        if (!p->v6 && p->ip == ip) {
            memcpy(p->frame, mac, ETH_ALEN);
            qemu_send_packet(qemu_get_queue(s->nic), p->frame, p->len);
            g_queue_delete_link(&s->pending, l);
            g_free(p->frame);
            g_free(p);
        }
        l = next;
    }
}

static void qeth_arp_learn(QethCcwDevice *s, uint32_t ip, const uint8_t *mac)
{
    QethArpEntry *e = qeth_arp_lookup(s, ip);
    char ipstr[16];
    int i;

    if (!e) {
        e = &s->arp[0];
        for (i = 0; i < QETH_ARP_ENTRIES; i++) {
            if (s->arp[i].used < e->used) {
                e = &s->arp[i];
            }
        }
        e->ip = ip;
        qeth_ip4_str(ip, ipstr, sizeof(ipstr));
        trace_qeth_l3("arp learn", ipstr);
    }
    memcpy(e->mac, mac, ETH_ALEN);
    e->used = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    qeth_flush_pending(s, ip, mac);
}

static void qeth_arp_reset(QethCcwDevice *s)
{
    QethPending *p;

    memset(s->arp, 0, sizeof(s->arp));
    memset(s->nd, 0, sizeof(s->nd));
    while ((p = g_queue_pop_head(&s->pending))) {
        g_free(p->frame);
        g_free(p);
    }
    s->arp_last_request = 0;
    s->arp_last_ip = 0;
    s->nd_last_request = 0;
    memset(s->nd_last_ip, 0, sizeof(s->nd_last_ip));
}

/*
 * IPv6 neighbour discovery on the guest's behalf.  The adapter keeps a
 * neighbour cache for plain (non-passthru) IPv6 packets from the guest,
 * learns it from solicitations and advertisements, answers solicitations
 * for the guest's registered addresses with its own MAC address and
 * solicits unknown next hops itself.
 */

static void qeth_ip6_str(const uint8_t *ip, char *buf, size_t len)
{
    snprintf(buf, len, "%x:%x:%x:%x:%x:%x:%x:%x",
             lduw_be_p(ip), lduw_be_p(ip + 2), lduw_be_p(ip + 4),
             lduw_be_p(ip + 6), lduw_be_p(ip + 8), lduw_be_p(ip + 10),
             lduw_be_p(ip + 12), lduw_be_p(ip + 14));
}

static bool qeth_ip6_unspecified(const uint8_t *ip)
{
    static const uint8_t zero[16];

    return !memcmp(ip, zero, 16);
}

/* Ethernet multicast address of an IPv6 multicast group */
static void qeth_ip6_mc_mac(const uint8_t *group, uint8_t *mac)
{
    mac[0] = 0x33;
    mac[1] = 0x33;
    memcpy(mac + 2, group + 12, 4);
}

/* Solicited-node multicast group of an address */
static void qeth_ip6_solicited_node(const uint8_t *ip, uint8_t *group)
{
    static const uint8_t prefix[13] = {
        0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0xff
    };

    memcpy(group, prefix, sizeof(prefix));
    memcpy(group + 13, ip + 13, 3);
}

/*
 * Send an ICMPv6 neighbour solicitation or advertisement carrying the
 * adapter's MAC address in the source or target link-layer option.
 */
static void qeth_send_nd(QethCcwDevice *s, uint8_t type, uint8_t flags,
                         const uint8_t *src_ip, const uint8_t *dst_ip,
                         const uint8_t *dst_mac, const uint8_t *target)
{
    uint8_t f[ETH_HDR + IP6_HDR + ICMP6_ND_SIZE + 8] = { 0 };
    uint8_t *ip6 = f + ETH_HDR;
    uint8_t *icmp = ip6 + IP6_HDR;
    const size_t icmp_len = ICMP6_ND_SIZE + 8;
    uint32_t sum;

    memcpy(f, dst_mac, ETH_ALEN);
    memcpy(f + 6, s->conf.macaddr.a, ETH_ALEN);
    stw_be_p(f + 12, ETHERTYPE_IPV6);
    ip6[0] = 0x60;
    stw_be_p(ip6 + 4, icmp_len);
    ip6[6] = IP6_NEXT_ICMP6;
    ip6[7] = 255;                           /* hop limit */
    memcpy(ip6 + 8, src_ip, 16);
    memcpy(ip6 + 24, dst_ip, 16);
    icmp[0] = type;
    icmp[4] = flags;
    memcpy(icmp + 8, target, 16);
    icmp[24] = type == ICMP6_ND_SOLICIT ? ICMP6_ND_OPT_SRC_LL :
                                          ICMP6_ND_OPT_TGT_LL;
    icmp[25] = 1;                           /* in units of 8 bytes */
    memcpy(icmp + 26, s->conf.macaddr.a, ETH_ALEN);

    /* Pseudo-header: addresses, upper-layer length, next header */
    sum = net_checksum_add(32, ip6 + 8) + icmp_len + IP6_NEXT_ICMP6 +
          net_checksum_add(icmp_len, icmp);
    stw_be_p(icmp + 2, net_checksum_finish(sum));
    qemu_send_packet(qemu_get_queue(s->nic), f, sizeof(f));
}

static void qeth_send_ns(QethCcwDevice *s, const uint8_t *src_ip,
                         const uint8_t *target)
{
    uint8_t group[16], mac[ETH_ALEN];

    qeth_ip6_solicited_node(target, group);
    qeth_ip6_mc_mac(group, mac);
    qeth_send_nd(s, ICMP6_ND_SOLICIT, 0, src_ip, group, mac, target);
}

/* Advertise a registered address, to a solicitor or to all nodes */
static void qeth_send_na(QethCcwDevice *s, const uint8_t *target,
                         const uint8_t *dst_ip, const uint8_t *dst_mac)
{
    static const uint8_t all_nodes[16] = {
        0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01
    };
    uint8_t mac[ETH_ALEN];

    if (!dst_ip) {
        qeth_ip6_mc_mac(all_nodes, mac);
        qeth_send_nd(s, ICMP6_ND_ADVERT, ICMP6_NA_OVERRIDE, target,
                     all_nodes, mac, target);
        return;
    }
    qeth_send_nd(s, ICMP6_ND_ADVERT, ICMP6_NA_SOLICITED | ICMP6_NA_OVERRIDE,
                 target, dst_ip, dst_mac, target);
}

static QethNdEntry *qeth_nd_lookup(QethCcwDevice *s, const uint8_t *ip)
{
    int i;

    for (i = 0; i < QETH_ND_ENTRIES; i++) {
        if (s->nd[i].used && !memcmp(s->nd[i].ip, ip, 16)) {
            s->nd[i].used = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            return &s->nd[i];
        }
    }
    return NULL;
}

static void qeth_nd_learn(QethCcwDevice *s, const uint8_t *ip,
                          const uint8_t *mac)
{
    QethNdEntry *e;
    GList *l;
    char ipstr[40];
    int i;

    if (qeth_ip6_unspecified(ip) || ip[0] == 0xff || (mac[0] & 1)) {
        return;
    }
    e = qeth_nd_lookup(s, ip);
    if (!e) {
        e = &s->nd[0];
        for (i = 0; i < QETH_ND_ENTRIES; i++) {
            if (s->nd[i].used < e->used) {
                e = &s->nd[i];
            }
        }
        memcpy(e->ip, ip, 16);
        qeth_ip6_str(ip, ipstr, sizeof(ipstr));
        trace_qeth_l3("nd learn", ipstr);
    }
    memcpy(e->mac, mac, ETH_ALEN);
    e->used = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    for (l = s->pending.head; l;) {
        GList *next = l->next;
        QethPending *p = l->data;

        if (p->v6 && !memcmp(p->ip6, ip, 16)) {
            memcpy(p->frame, mac, ETH_ALEN);
            qemu_send_packet(qemu_get_queue(s->nic), p->frame, p->len);
            g_queue_delete_link(&s->pending, l);
            g_free(p->frame);
            g_free(p);
        }
        l = next;
    }
}

static void qeth_l3_reset(QethCcwDevice *s)
{
    s->nr_ip4 = 0;
    s->nr_ip6 = 0;
    s->nr_mc = 0;
    qeth_arp_reset(s);
}

/* IPA commands */

static uint32_t qeth_assists_supported(QethConn *c, uint16_t prot)
{
    if (!c->layer3) {
        return IPA_ASSISTS_L2;
    }
    return prot == IPA_PROT_IPV6 ? IPA_ASSISTS_V6 : IPA_ASSISTS_V4;
}

static uint32_t *qeth_assists_enabled(QethConn *c, uint16_t prot)
{
    return &c->assists_enabled[prot == IPA_PROT_IPV6];
}

static uint16_t qeth_ipa_setip(QethCcwDevice *s, int conn, uint16_t prot,
                               const uint8_t *data, size_t avail, bool set,
                               int *data_size)
{
    char ipstr[16];

    if (prot == IPA_PROT_IPV4) {
        uint32_t addr, flags;
        int i;

        if (avail < 12) {
            return IPA_RC_UNSUPPORTED_SUBCMD;
        }
        addr = ldl_be_p(data);
        flags = ldl_be_p(data + 8);
        *data_size = 12;
        qeth_ip4_str(addr, ipstr, sizeof(ipstr));
        trace_qeth_l3(set ? "setip" : "delip", ipstr);
        i = qeth_find_ip4(s, addr);
        if (!set) {
            if (i < 0 || s->ip4[i].conn != conn) {
                return IPA_RC_UNREGISTERED_ADDR;
            }
            s->ip4[i] = s->ip4[--s->nr_ip4];
            return IPA_RC_SUCCESS;
        }
        if (flags != IPA_SETDELIP_DEFAULT && flags != IPA_SETIP_VIPA &&
            flags != IPA_SETIP_TAKEOVER) {
            return IPA_RC_UNSUPPORTED_SUBCMD;
        }
        if (i >= 0) {
            return IPA_RC_IP_ADDR_ALREADY_USED;
        }
        if (s->nr_ip4 == QETH_MAX_IP4) {
            return IPA_RC_IP_ADDR_TABLE_FULL;
        }
        s->ip4[s->nr_ip4++] = (QethIp4) {
            .addr = addr, .flags = flags, .conn = conn
        };
        if (!(flags & IPA_SETIP_VIPA) && s->nic) {
            /* Announce the address (gratuitous ARP) */
            qeth_send_arp(s, ARP_REQUEST, addr, NULL, addr);
        }
        return IPA_RC_SUCCESS;
    }
    if (prot == IPA_PROT_IPV6) {
        char ip6str[40];
        int i;

        if (avail < 36) {
            return IPA_RC_UNSUPPORTED_SUBCMD;
        }
        *data_size = 36;
        qeth_ip6_str(data, ip6str, sizeof(ip6str));
        trace_qeth_l3(set ? "setip" : "delip", ip6str);
        i = qeth_find_ip6(s, data);
        if (!set) {
            if (i < 0 || s->ip6[i].conn != conn) {
                return IPA_RC_UNREGISTERED_ADDR;
            }
            s->ip6[i] = s->ip6[--s->nr_ip6];
            return IPA_RC_SUCCESS;
        }
        if (i >= 0) {
            return IPA_RC_IP_ADDR_ALREADY_USED;
        }
        if (s->nr_ip6 == QETH_MAX_IP6) {
            return IPA_RC_IP_ADDR_TABLE_FULL;
        }
        memcpy(s->ip6[s->nr_ip6].addr, data, 16);
        s->ip6[s->nr_ip6++].conn = conn;
        if (s->nic) {
            /* Announce the address (unsolicited neighbour advertisement) */
            qeth_send_na(s, data, NULL, NULL);
        }
        return IPA_RC_SUCCESS;
    }
    return IPA_RC_INVALID_IP_VERSION;
}

static uint16_t qeth_ipa_setadp(QethCcwDevice *s, uint8_t *sap, size_t avail,
                                int *data_size)
{
    uint32_t cmd;
    uint8_t *data = sap + IPA_SETADP_HDR_SIZE;
    uint16_t rc = IPA_RC_SUCCESS;
    size_t len = 0;

    if (avail < IPA_SETADP_HDR_SIZE) {
        return IPA_RC_UNSUPPORTED_SUBCMD;
    }
    cmd = ldl_be_p(sap + 12);
    stl_be_p(sap, IPA_SETADP_SUPPORTED);
    switch (cmd) {
    case IPA_SETADP_QUERY:
        len = 20;
        memset(data, 0, len);
        stl_be_p(data, 1);                      /* LAN types */
        data[4] = QETH_LINK_TYPE_10GBE;
        stl_be_p(data + 8, IPA_SETADP_SUPPORTED);
        break;
    case IPA_SETADP_ALTER_MAC:
        if (avail < IPA_SETADP_HDR_SIZE + 12 || ldl_be_p(data) != 0) {
            rc = IPA_RC_UNSUPPORTED_SUBCMD;     /* only READ_MAC */
            break;
        }
        len = 18;
        stl_be_p(data + 4, ETH_ALEN);
        stl_be_p(data + 8, 1);
        memcpy(data + 12, s->conf.macaddr.a, ETH_ALEN);
        sap[20] |= IPA_SETADP_VIRTUAL_MAC;
        break;
    case IPA_SETADP_CARD_INFO:
        len = 12;
        memset(data, 0, len);
        data[0] = 0x91;                         /* 10G fibre */
        stw_be_p(data + 2, 0x0003);             /* full duplex */
        stl_be_p(data + 4, 0x00000008);         /* 10 Gb/s */
        break;
    case IPA_SETADP_PROMISC:
    case IPA_SETADP_ACCESS_CTRL:
        break;
    default:
        rc = IPA_RC_UNSUPPORTED_SUBCMD;
        break;
    }
    stw_be_p(sap + 16, rc);
    if (rc == IPA_RC_SUCCESS && len) {
        stw_be_p(sap + 8, IPA_SETADP_HDR_SIZE - 8 + len);
        *data_size = IPA_SETADP_HDR_SIZE + len;
    }
    return rc;
}

static void qeth_process_ipa(QethCcwDevice *s, SubchDev *sch,
                             const uint8_t *req, size_t len,
                             uint32_t offrrh, uint32_t offph,
                             uint32_t offdata, uint32_t lendata)
{
    uint8_t out[QETH_MAX_CTRL];
    uint8_t *ipa = out + offdata;
    uint8_t *data = ipa + IPA_HDR_SIZE;
    size_t avail = lendata > IPA_HDR_SIZE ? lendata - IPA_HDR_SIZE : 0;
    size_t room = sizeof(out) - offdata - IPA_HDR_SIZE;
    int ipa_size = IPA_HDR_SIZE, data_size = -1;
    uint16_t prot, rc = IPA_RC_SUCCESS;
    uint32_t sub = 0;
    uint8_t cmd;
    QethConn *c;
    int ci;

    if (lendata < IPA_HDR_SIZE_SHORT || offdata + lendata > len) {
        qeth_unhandled(sch, "short ipa", lendata);
        return;
    }
    /* IPA commands name their connection by the adapter's token */
    c = qeth_conn_by_token(s, ldl_be_p(req + offrrh + 0x18),
                           QETH_TOKEN_ULP_CONN);
    if (!c) {
        c = &s->conn[s->last_enabled];
        qeth_unhandled(sch, "ipa for unknown connection",
                       ldl_be_p(req + offrrh + 0x18));
    }
    ci = qeth_conn_index(s, c);
    memset(out, 0, sizeof(out));
    memcpy(out, req, len);
    cmd = ipa[0];
    prot = lduw_be_p(ipa + 10);

    switch (cmd) {
    case IPA_CMD_STARTLAN:
        c->lan_online = true;
        *qeth_assists_enabled(c, IPA_PROT_IPV4) |= IPA_SETADAPTERPARMS;
        *qeth_assists_enabled(c, IPA_PROT_IPV6) |= IPA_SETADAPTERPARMS;
        ipa_size = IPA_HDR_SIZE_SHORT;
        data_size = 0;
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    case IPA_CMD_STOPLAN:
        c->lan_online = false;
        break;
    case IPA_CMD_QIPASSIST:
        *qeth_assists_enabled(c, prot) |= IPA_SETADAPTERPARMS;
        data_size = 0;
        break;
    case IPA_CMD_SETASSPARMS:
    {
        uint32_t assist;
        uint16_t sas_len;

        if (avail < 12) {
            rc = IPA_RC_UNSUPPORTED_SUBCMD;
            break;
        }
        assist = ldl_be_p(data);
        sas_len = lduw_be_p(data + 4);
        sub = assist << 16 | lduw_be_p(data + 6);
        if (!(assist & qeth_assists_supported(c, prot))) {
            rc = IPA_RC_NOTSUPP;
            break;
        }
        switch (lduw_be_p(data + 6)) {
        case IPA_ASS_START:
            *qeth_assists_enabled(c, prot) |= assist;
            break;
        case IPA_ASS_STOP:
            *qeth_assists_enabled(c, prot) &= ~assist;
            break;
        case 0x0003: case 0x0004: case 0x0005: case 0x0006: case 0x0008:
            break;
        default:
            rc = IPA_RC_UNSUPPORTED_SUBCMD;
            break;
        }
        if (rc == IPA_RC_SUCCESS) {
            stw_be_p(data + 8, IPA_RC_SUCCESS);
            data_size = MIN((size_t)sas_len + 4, room);
        }
        break;
    }
    case IPA_CMD_SETIP:
    case IPA_CMD_DELIP:
        rc = qeth_ipa_setip(s, ci, prot, data, avail, cmd == IPA_CMD_SETIP,
                            &data_size);
        break;
    case IPA_CMD_SETIPM:
    case IPA_CMD_DELIPM:
        if (avail >= 6) {
            int i = qeth_find_mc(s, ci, data);

            if (cmd == IPA_CMD_SETIPM && i < 0 && s->nr_mc < QETH_MAX_MC) {
                memcpy(s->mc[s->nr_mc].mac, data, ETH_ALEN);
                s->mc[s->nr_mc++].conn = ci;
            } else if (cmd == IPA_CMD_DELIPM && i >= 0) {
                s->mc[i] = s->mc[--s->nr_mc];
            }
        }
        if (cmd == IPA_CMD_SETIPM) {
            data_size = MIN((size_t)24, room);
        }
        break;
    case IPA_CMD_SETRTG:
        data[0] = 0;
        data_size = 1;
        break;
    case IPA_CMD_SETADAPTERPARMS:
        sub = avail >= 16 ? ldl_be_p(data + 12) : 0;
        if (room < IPA_SETADP_HDR_SIZE + 20) {
            rc = IPA_RC_UNSUPPORTED_SUBCMD;
            break;
        }
        rc = qeth_ipa_setadp(s, data, MAX(avail, (size_t)IPA_SETADP_HDR_SIZE),
                             &data_size);
        break;
    case IPA_CMD_SET_DIAG_ASS:
        break;
    case IPA_CMD_CREATE_ADDR:
        memcpy(data, s->conf.macaddr.a, ETH_ALEN);
        data[0] |= 0x02;
        data[6] = 0xff;
        data[7] = 0xfe;
        data_size = 8;
        break;
    case IPA_CMD_SETVMAC:
    case IPA_CMD_DELVMAC:
    case IPA_CMD_SETGMAC:
    case IPA_CMD_DELGMAC:
    case IPA_CMD_SETVLAN:
    case IPA_CMD_DELVLAN:
        break;
    default:
        qeth_unhandled(sch, "ipa command", cmd);
        rc = IPA_RC_NOTSUPP;
        break;
    }
    trace_qeth_ipa(ci, cmd, prot, lduw_be_p(ipa + 2), sub, rc);

    ipa[1] = IPA_INITIATOR_HOST;
    stw_be_p(ipa + 4, rc);
    stl_be_p(ipa + 12, qeth_assists_supported(c, prot));
    *qeth_assists_enabled(c, prot) &= qeth_assists_supported(c, prot);
    if (lendata >= IPA_HDR_SIZE && ipa_size == IPA_HDR_SIZE) {
        stl_be_p(ipa + 16, *qeth_assists_enabled(c, prot));
    }

    /* Adjust the PDU lengths when the response is shorter or longer */
    if (ipa_size != IPA_HDR_SIZE || data_size >= 0) {
        size_t new_data = ipa_size + (data_size >= 0 ? data_size : avail);
        size_t new_len = offdata + new_data;

        if (new_len > sizeof(out)) {
            qeth_unhandled(sch, "ipa response too long", new_len);
            return;
        }
        stl_be_p(out + 0x0c, new_len);
        stw_be_p(out + offrrh + 0x12, new_data);
        stl_be_p(out + offrrh + 0x14,
                 new_data << 8 | out[offrrh + 0x17]);
        stl_be_p(out + offrrh + offph,
                 (out[offrrh + offph] << 24) | new_data);
        len = new_len;
    }
    stl_be_p(out + 4, 0xffffffff);
    stw_be_p(out + 0x10, 0x0ffc);
    out[offrrh + 5] = MPC_PROTO_UNKNOWN;
    stl_be_p(out + offrrh + 0x18, c->guest_ulp_conn);

    qeth_queue_response(s, qeth_peer_with_role(s, QETH_ROLE_READ), out, len);
}

/*
 * MPC control traffic written to the write subchannel: transport header,
 * request/response header and a PDU header locating the payload, which is
 * either a CM/ULP/DM PUK with structured fields or an IPA command.
 */
static void qeth_process_mpc(QethCcwDevice *s, SubchDev *sch,
                             const uint8_t *req, size_t len)
{
    uint32_t offrrh, offdata, lendata;
    uint16_t offph;
    const uint8_t *rrh;

    if (len < MPC_TH_SIZE) {
        goto bad;
    }
    offrrh = ldl_be_p(req + 8);
    if (offrrh > len || len - offrrh < MPC_RRH_SIZE_SHORT) {
        goto bad;
    }
    rrh = req + offrrh;
    offph = lduw_be_p(rrh + 0x10);
    if (offph > len - offrrh || len - offrrh - offph < MPC_PH_SIZE) {
        goto bad;
    }
    lendata = ldl_be_p(rrh + offph) & 0x00ffffff;
    offdata = ldl_be_p(rrh + offph + 4);
    if (offdata > len || lendata > len - offdata) {
        goto bad;
    }
    if (rrh[4] == MPC_RRH_TYPE_IPA) {
        qeth_process_ipa(s, sch, req, len, offrrh, offph, offdata, lendata);
        return;
    }
    if (lendata < MPC_PUK_SIZE) {
        goto bad;
    }
    qeth_process_puk(s, sch, rrh, req + offdata, lendata);
    return;

bad:
    qeth_unhandled(sch, "malformed mpc", len);
}

/* Self-description */

/*
 * Self-describing component text: type, model, manufacturer, plant and
 * sequence number, all in EBCDIC (26 bytes).
 */
static void qeth_sdc(uint8_t *out, const char *type, const char *model,
                     const char *serial)
{
    char text[27];
    int i;

    snprintf(text, sizeof(text), "%-6.6s%-3.3sIBMZZ%-12.12s", type, model,
             serial);
    for (i = 0; i < 26; i++) {
        uint8_t c = text[i];

        out[i] = c >= '0' && c <= '9' ? 0xf0 + (c - '0') :
                 c >= 'A' && c <= 'I' ? 0xc1 + (c - 'A') :
                 c >= 'J' && c <= 'R' ? 0xd1 + (c - 'J') :
                 c >= 'S' && c <= 'Z' ? 0xe2 + (c - 'S') : 0x40;
    }
}

/*
 * Read Configuration Data: device NED, control-unit NED, token NED and the
 * general NEQ, with the control-unit logical address (CULA) in the
 * control-unit NED tag and in the general NEQ interface identifier.
 */
static void qeth_build_rcd(QethCcwDevice *s, SubchDev *sch,
                           uint8_t out[QETH_RCD_SIZE])
{
    memset(out, 0, QETH_RCD_SIZE);

    out[0] = QETH_NED_FLAGS;                /* device NED */
    out[1] = QETH_NED_TYPE_DEVICE;
    out[2] = QETH_NED_CLASS_COMM;
    qeth_sdc(out + 4, "001732", "001", "000000000001");
    out[30] = s->chpid;
    out[31] = sch->devno;

    out[32] = QETH_NED_FLAGS;               /* control-unit NED */
    out[33] = QETH_NED_TYPE_CU;
    qeth_sdc(out + 36, "001730", "001", "000000000002");
    out[62] = s->chpid;
    out[63] = QETH_CULA;

    out[64] = QETH_NED_FLAGS | QETH_NED_TOKEN;  /* token NED */
    out[66] = QETH_NED_CLASS_COMM;
    qeth_sdc(out + 68, "001730", "002", "000000000003");
    out[94] = s->chpid;
    out[95] = QETH_CULA;

    out[96] = QETH_NEQ_GENERAL;             /* general NEQ */
    out[98] = s->chpid;
    out[99] = QETH_CULA;
}

/*
 * Read Node Identifier: node descriptor of the adapter followed by a
 * model-dependent node qualifier carrying the CHPID and the CULA.  A
 * non-zero node selector in the preceding Set Interface Identifier asks
 * for the node descriptor only.
 */
static size_t qeth_build_rni(QethCcwDevice *s, uint8_t out[QETH_RNI_SIZE])
{
    memset(out, 0, QETH_RNI_SIZE);
    out[2] = QETH_NED_CLASS_COMM;
    out[3] = 0xff;
    qeth_sdc(out + 4, "001730", "001", "000000000002");
    out[30] = s->chpid;
    if (s->sii & 0x00030000) {
        return 32;
    }
    out[32] = 0x20;                         /* model-dependent NQ */
    out[34] = s->chpid;
    out[35] = QETH_CULA;
    return QETH_RNI_SIZE;
}

/*
 * Connection whose queues a data subchannel establishes: the one set up
 * for it in ULP_SETUP, else a connection without a data device yet.
 */
static QethConn *qeth_conn_for_data(QethCcwDevice *s, int peer)
{
    QethConn *c = qeth_conn_by_peer(s, peer);
    unsigned int i;

    for (i = 0; !c && i < qeth_nr_conns(s); i++) {
        if (s->conn[i].in_use && s->conn[i].data_peer < 0) {
            c = &s->conn[i];
        }
    }
    return c;
}

static int qeth_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    QethCcwDevice *s = sch->driver_data;
    int peer = qeth_peer_index(s, sch);
    uint8_t buf[QETH_MAX_CTRL];
    QethConn *c;
    size_t len;

    trace_qeth_ccw(sch->devno, ccw.cmd_code, ccw.count, ccw.flags);
    switch (ccw.cmd_code) {
    case QETH_CMD_WRITE:
        len = MIN((size_t)ccw.count, sizeof(buf));
        if (qeth_read_ccw(sch, buf, len)) {
            return qeth_unit_check(sch, ccw.cmd_code, 0x80);
        }
        qeth_trace_data(sch, "from-guest", buf, len);
        if (len >= 4 && ldl_be_p(buf) == QETH_IDX_REQ) {
            qeth_process_idx(s, sch, buf, len);
        } else if (len >= 4 && ldl_be_p(buf) == MPC_TH_FIRST4) {
            qeth_process_mpc(s, sch, buf, len);
        } else {
            qeth_unhandled(sch, "control write", len >= 4 ? ldl_be_p(buf) : 0);
            return qeth_unit_check(sch, ccw.cmd_code, 0x80);
        }
        return 0;
    case QETH_CMD_READ:
    {
        GByteArray *rsp = g_queue_pop_head(&s->responses[peer]);
        int ret;

        if (!rsp) {
            s->read_pending[peer] = true;
            return CSS_CCW_PENDING;
        }
        ret = qeth_copy_response(sch, rsp->data, rsp->len);
        g_byte_array_unref(rsp);
        return ret;
    }
    case QETH_CMD_RCD:
    {
        uint8_t rcd[QETH_RCD_SIZE];

        qeth_build_rcd(s, sch, rcd);
        return qeth_copy_response(sch, rcd, sizeof(rcd));
    }
    case QETH_CMD_SII:
        len = MIN((size_t)ccw.count, sizeof(buf));
        if (len < 4 || qeth_read_ccw(sch, buf, len)) {
            return qeth_unit_check(sch, ccw.cmd_code, 0x80);
        }
        qeth_trace_data(sch, "from-guest", buf, len);
        s->sii = ldl_be_p(buf);
        return 0;
    case QETH_CMD_RNI:
    {
        uint8_t rni[QETH_RNI_SIZE];

        len = qeth_build_rni(s, rni);
        s->sii = 0;
        return qeth_copy_response(sch, rni, len);
    }
    case QETH_CMD_EQ:
        len = MIN((size_t)ccw.count, sizeof(buf));
        if (qeth_read_ccw(sch, buf, len)) {
            return qeth_unit_check(sch, ccw.cmd_code, 0x80);
        }
        qeth_trace_data(sch, "from-guest", buf, len);
        c = qeth_conn_for_data(s, peer);
        if (!c || s390_qdio_establish(&c->qdio, buf, len)) {
            return qeth_unit_check(sch, ccw.cmd_code, 0x80);
        }
        c->data_peer = peer;
        s->role[peer] = QETH_ROLE_DATA;
        trace_qeth_conn(qeth_conn_index(s, c), "establish", sch->devno);
        return 0;
    case QETH_CMD_AQ:
        c = qeth_conn_by_peer(s, peer);
        if (!c || !c->qdio.established) {
            return qeth_unit_check(sch, ccw.cmd_code, 0x80);
        }
        c->qdio.active = true;
        c->activated = true;
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
        return qeth_unit_check(sch, ccw.cmd_code, 0x80);
    }
}

static void qeth_cancel(SubchDev *sch)
{
    QethCcwDevice *s = sch->driver_data;
    int peer = qeth_peer_index(s, sch);
    QethConn *c = qeth_conn_by_peer(s, peer);

    if (peer >= 0) {
        s->read_pending[peer] = false;
    }
    if (c) {
        c->qdio.active = false;
    }
}

static int qeth_siga(SubchDev *sch, uint8_t function, uint32_t output_mask,
                     uint32_t input_mask, uint64_t aob)
{
    QethCcwDevice *s = sch->driver_data;
    QethConn *c = qeth_conn_by_peer(s, qeth_peer_index(s, sch));
    unsigned int i;

    trace_qeth_siga(sch->devno, function, output_mask, input_mask,
                    !c || !c->qdio.active ? 1 : 0);
    if (!c || !c->qdio.active) {
        return 1;
    }
    switch (function & 0x7f) {
    case 0:
    case 3:
        for (i = 0; i < c->qdio.output_count; i++) {
            if (output_mask & (0x80000000U >> i)) {
                c->qdio.output[i].mask = 0x80000000U >> i;
            }
        }
        qemu_bh_schedule(s->tx_bh);
        return 0;
    case 1:
        for (i = 0; i < c->qdio.input_count; i++) {
            if (output_mask & (0x80000000U >> i)) {
                c->qdio.input[i].mask = 0x80000000U >> i;
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

    d[0] = 0xc0;    /* QDIO capable, descriptor valid */
    stw_be_p(d + 2, sch->schid);
    d[4] = 0;
    /* SIGA input/output needed, auto-sync on output PCI */
    d[6] = 0x80 | 0x64;
    d[9] = 1;
    d[11] = 4;
    stw_be_p(d + 14, 0x6000);
}

static const S390QdioOps qeth_qdio_ops = {
    .siga = qeth_siga,
    .ssqd = qeth_ssqd,
};

/* Data path */

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

static bool qeth_ip4_multicast(uint32_t ip)
{
    return (ip >> 28) == 0xe;
}

/*
 * Layer-3 IPv6 transmit of a plain IPv6 packet (the frame has its source
 * MAC and type set): multicast maps to 33:33:xx:xx:xx:xx; unicast goes to
 * the next hop from the qeth header, or the destination, resolved through
 * the neighbour cache.  Unresolved packets wait for the advertisement.
 */
static void qeth_l3_transmit6(QethCcwDevice *s, const uint8_t *hdr,
                              const uint8_t *pkt, uint8_t *frame, size_t len)
{
    const uint8_t *dst = pkt + 24, *src = pkt + 8;
    const uint8_t *nexthop = hdr + 16;
    QethNdEntry *e;
    QethPending *p;
    char ipstr[40];
    int64_t now;

    if (dst[0] == 0xff) {
        qeth_ip6_mc_mac(dst, frame);
        qemu_send_packet(qemu_get_queue(s->nic), frame, len);
        g_free(frame);
        return;
    }
    if (qeth_ip6_unspecified(nexthop) || nexthop[0] == 0xff) {
        nexthop = dst;
    }
    e = qeth_nd_lookup(s, nexthop);
    if (e) {
        memcpy(frame, e->mac, ETH_ALEN);
        qemu_send_packet(qemu_get_queue(s->nic), frame, len);
        g_free(frame);
        return;
    }

    /* Queue until the next hop is resolved */
    if (g_queue_get_length(&s->pending) >= QETH_PENDING_MAX) {
        p = g_queue_pop_head(&s->pending);
        g_free(p->frame);
        g_free(p);
    }
    p = g_new0(QethPending, 1);
    p->v6 = true;
    memcpy(p->ip6, nexthop, 16);
    p->len = len;
    p->frame = frame;
    g_queue_push_tail(&s->pending, p);

    now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (memcmp(s->nd_last_ip, nexthop, 16) ||
        now - s->nd_last_request >= QETH_ARP_RETRY_MS) {
        memcpy(s->nd_last_ip, nexthop, 16);
        s->nd_last_request = now;
        if (qeth_ip6_unspecified(src) && s->nr_ip6) {
            src = s->ip6[0].addr;
        }
        qeth_ip6_str(nexthop, ipstr, sizeof(ipstr));
        trace_qeth_l3("nd solicit", ipstr);
        qeth_send_ns(s, src, nexthop);
    }
}

/*
 * Layer-3 transmit: build the Ethernet frame for an IP packet.  Unicast
 * destinations are resolved through the adapter's ARP cache; unresolved
 * packets wait for the ARP reply.
 */
static void qeth_l3_transmit(QethCcwDevice *s, const uint8_t *hdr,
                             const uint8_t *pkt, size_t len)
{
    g_autofree uint8_t *f = NULL;
    uint8_t cast = hdr[1] & QETH_HDR3_CAST_MASK;
    bool v6 = (hdr[1] & QETH_HDR3_IPV6) || (len && (pkt[0] >> 4) == 6);
    uint32_t dst, nexthop;
    QethArpEntry *e;
    QethPending *p;
    char ipstr[16];
    int64_t now;

    if (hdr[1] & QETH_HDR3_PASSTHRU) {
        if (len >= ETH_HDR) {
            qemu_send_packet(qemu_get_queue(s->nic), pkt, len);
        }
        return;
    }
    if (len < (v6 ? 40 : 20)) {
        trace_qeth_l3("tx drop", "short packet");
        return;
    }
    f = g_malloc(ETH_HDR + len);
    memcpy(f + 6, s->conf.macaddr.a, ETH_ALEN);
    stw_be_p(f + 12, v6 ? ETHERTYPE_IPV6 : ETHERTYPE_IPV4);
    memcpy(f + ETH_HDR, pkt, len);

    if (v6) {
        qeth_l3_transmit6(s, hdr, pkt, g_steal_pointer(&f), ETH_HDR + len);
        return;
    }

    dst = ldl_be_p(pkt + 16);
    if (cast == QETH_CAST_BROADCAST || dst == 0xffffffff) {
        memset(f, 0xff, ETH_ALEN);
    } else if (cast == QETH_CAST_MULTICAST || qeth_ip4_multicast(dst)) {
        f[0] = 0x01;
        f[1] = 0x00;
        f[2] = 0x5e;
        f[3] = (dst >> 16) & 0x7f;
        f[4] = dst >> 8;
        f[5] = dst;
    } else {
        nexthop = ldl_be_p(hdr + 0x1c);
        if (!nexthop) {
            nexthop = dst;
        }
        e = qeth_arp_lookup(s, nexthop);
        if (!e) {
            /* Queue until the next hop is resolved */
            if (g_queue_get_length(&s->pending) >= QETH_PENDING_MAX) {
                p = g_queue_pop_head(&s->pending);
                g_free(p->frame);
                g_free(p);
            }
            p = g_new(QethPending, 1);
            p->ip = nexthop;
            p->len = ETH_HDR + len;
            p->frame = g_steal_pointer(&f);
            g_queue_push_tail(&s->pending, p);
            now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            if (s->arp_last_ip != nexthop ||
                now - s->arp_last_request >= QETH_ARP_RETRY_MS) {
                s->arp_last_ip = nexthop;
                s->arp_last_request = now;
                qeth_ip4_str(nexthop, ipstr, sizeof(ipstr));
                trace_qeth_l3("arp request", ipstr);
                qeth_send_arp(s, ARP_REQUEST, ldl_be_p(pkt + 12), NULL,
                              nexthop);
            }
            return;
        }
        memcpy(f, e->mac, ETH_ALEN);
    }
    qemu_send_packet(qemu_get_queue(s->nic), f, ETH_HDR + len);
}

/*
 * Drain an output buffer.  A buffer holds one or more packets, each led by
 * a 32-byte qeth header whose type selects layer-2 frames or layer-3 IP
 * packets.
 */
static void qeth_tx_buffer(QethCcwDevice *s, unsigned int queue,
                           unsigned int index, const uint8_t *buf,
                           size_t len)
{
    size_t off = 0;

    while (len - off >= QETH_HDR_SIZE) {
        const uint8_t *hdr = buf + off;
        size_t pkt_len;

        if (hdr[0] == QETH_HDR_LAYER2) {
            pkt_len = lduw_be_p(hdr + 6);
        } else if (hdr[0] == QETH_HDR_LAYER3) {
            pkt_len = lduw_be_p(hdr + 8);
        } else {
            trace_qeth_tx(queue, index, len, hdr[0], 0);
            return;
        }
        trace_qeth_tx(queue, index, len, hdr[0], pkt_len);
        if (pkt_len > len - off - QETH_HDR_SIZE) {
            return;
        }
        if (hdr[0] == QETH_HDR_LAYER2) {
            qemu_send_packet(qemu_get_queue(s->nic), hdr + QETH_HDR_SIZE,
                             pkt_len);
        } else {
            qeth_l3_transmit(s, hdr, hdr + QETH_HDR_SIZE, pkt_len);
        }
        off += QETH_HDR_SIZE + pkt_len;
    }
}

static void qeth_tx_conn(QethCcwDevice *s, QethConn *c)
{
    unsigned int qn;
    bool interrupt = false;

    for (qn = 0; qn < c->qdio.output_count; qn++) {
        S390QdioQueue *q = &c->qdio.output[qn];
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
            qeth_tx_buffer(s, qn, index, record, len);
            if (s390_qdio_set_state(q, index,
                                    S390_QDIO_SLSB_OUTPUT_EMPTY)) {
                break;
            }
            interrupt |= pci;
        }
    }
    if (interrupt) {
        css_inject_qdio_pci(qeth_conn_sch(s, c));
    }
}

static void qeth_tx_bh(void *opaque)
{
    QethCcwDevice *s = opaque;
    unsigned int i;

    for (i = 0; i < qeth_nr_conns(s); i++) {
        if (s->conn[i].in_use && s->conn[i].qdio.active) {
            qeth_tx_conn(s, &s->conn[i]);
        }
    }
}

static bool qeth_can_receive(NetClientState *nc)
{
    QethCcwDevice *s = qemu_get_nic_opaque(nc);
    unsigned int i;
    uint8_t index;

    for (i = 0; i < qeth_nr_conns(s); i++) {
        QethConn *c = &s->conn[i];

        if (c->in_use && c->lan_online && c->qdio.active &&
            c->qdio.input_count &&
            !s390_qdio_find_buffer(&c->qdio.input[0],
                                   S390_QDIO_SLSB_INPUT_EMPTY, &index)) {
            return true;
        }
    }
    return false;
}

static bool qeth_conn_online(QethConn *c)
{
    return c->in_use && c->lan_online && c->qdio.active &&
           c->qdio.input_count && c->data_peer >= 0;
}

/*
 * Place one packet with its qeth header into the connection's next empty
 * input buffer.  Returns false when no input buffer is free (the packet
 * is retried later); packets for connections that are not running are
 * dropped.
 */
static bool qeth_rx_deliver(QethCcwDevice *s, QethConn *c,
                            const uint8_t *header, const uint8_t *buf,
                            size_t len)
{
    S390QdioQueue *q = &c->qdio.input[0];
    uint8_t index;
    uint64_t sbal;
    size_t offset = 0;
    int i;

    if (!qeth_conn_online(c) || len > UINT16_MAX) {
        return true;
    }
    if (s390_qdio_find_buffer(q, S390_QDIO_SLSB_INPUT_EMPTY, &index) ||
        s390_qdio_get_sbal(q, index, &sbal)) {
        return false;
    }

    for (i = 0; i < S390_QDIO_MAX_ELEMENTS && offset < len + QETH_HDR_SIZE;
         i++) {
        uint8_t elem[16];
        uint32_t capacity, amount;
        uint64_t addr;
        size_t header_part;

        if (s390_qdio_read(sbal + i * 16, elem, sizeof(elem))) {
            goto error;
        }
        capacity = ldl_be_p(elem + 4);
        addr = ldq_be_p(elem + 8);
        amount = MIN((size_t)capacity, len + QETH_HDR_SIZE - offset);
        header_part = offset < QETH_HDR_SIZE ?
                      MIN((size_t)amount, QETH_HDR_SIZE - offset) : 0;
        if (header_part &&
            s390_qdio_write(addr, header + offset, header_part)) {
            goto error;
        }
        if (amount > header_part &&
            s390_qdio_write(addr + header_part,
                            buf + offset + header_part - QETH_HDR_SIZE,
                            amount - header_part)) {
            goto error;
        }
        memset(elem, 0, 4);
        if (offset == 0) {
            elem[0] |= 0x04;
        }
        offset += amount;
        if (offset == len + QETH_HDR_SIZE) {
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
    if (offset != len + QETH_HDR_SIZE ||
        s390_qdio_set_state(q, index, S390_QDIO_SLSB_INPUT_PRIMED)) {
        goto error;
    }
    css_inject_qdio_pci(qeth_conn_sch(s, c));
    return true;

error:
    s390_qdio_set_state(q, index, S390_QDIO_SLSB_ERROR);
    css_inject_qdio_pci(qeth_conn_sch(s, c));
    return true;
}

static void qeth_l3_rx_arp(QethCcwDevice *s, const uint8_t *arp, size_t len)
{
    uint32_t sender_ip, target_ip;
    uint16_t op;
    int i;

    if (len < ARP_PACKET || lduw_be_p(arp) != 1 ||
        lduw_be_p(arp + 2) != ETHERTYPE_IPV4 || arp[4] != ETH_ALEN ||
        arp[5] != 4) {
        return;
    }
    op = lduw_be_p(arp + 6);
    sender_ip = ldl_be_p(arp + 14);
    target_ip = ldl_be_p(arp + 24);
    i = qeth_find_ip4(s, target_ip);

    if (sender_ip && (op == ARP_REPLY || i >= 0 ||
                      qeth_arp_lookup(s, sender_ip))) {
        qeth_arp_learn(s, sender_ip, arp + 8);
    }
    if (op == ARP_REQUEST && i >= 0 && sender_ip != target_ip) {
        qeth_send_arp(s, ARP_REPLY, target_ip, arp + 8, sender_ip);
    }
}

/*
 * Connections (as a bit mask) that want an IPv6 multicast frame without
 * registering it: all nodes, and the solicited-node groups of their
 * registered addresses.
 */
static uint32_t qeth_ip6_mc_conns(QethCcwDevice *s, const uint8_t *mac)
{
    static const uint8_t all_nodes[ETH_ALEN] = { 0x33, 0x33, 0, 0, 0, 1 };
    bool solicited = mac[0] == 0x33 && mac[1] == 0x33 && mac[2] == 0xff;
    uint32_t mask = 0;
    unsigned int i;

    for (i = 0; i < s->nr_ip6; i++) {
        if (!memcmp(mac, all_nodes, ETH_ALEN) ||
            (solicited && !memcmp(s->ip6[i].addr + 13, mac + 3, 3))) {
            mask |= 1u << s->ip6[i].conn;
        }
    }
    return mask;
}

/* Connections (as a bit mask) with IPv4 addresses, for broadcasts */
static uint32_t qeth_ip4_conns(QethCcwDevice *s)
{
    uint32_t mask = 0;
    unsigned int i;

    for (i = 0; i < s->nr_ip4; i++) {
        mask |= 1u << s->ip4[i].conn;
    }
    return mask;
}

/*
 * Received neighbour discovery: learn link-layer addresses from
 * solicitations and advertisements, and answer solicitations for the
 * guest's addresses.  The packet is still delivered to the guest.
 */
static void qeth_l3_rx_nd(QethCcwDevice *s, const uint8_t *frame,
                          const uint8_t *pkt, size_t len)
{
    const uint8_t *icmp = pkt + IP6_HDR, *src = pkt + 8;
    const uint8_t *target = icmp + 8, *ll = NULL;
    size_t off;
    char ipstr[40];

    if (len < IP6_HDR + ICMP6_ND_SIZE || pkt[7] != 255 ||
        (icmp[0] != ICMP6_ND_SOLICIT && icmp[0] != ICMP6_ND_ADVERT)) {
        return;
    }
    for (off = IP6_HDR + ICMP6_ND_SIZE; off + 8 <= len && pkt[off + 1];
         off += pkt[off + 1] * 8) {
        if (pkt[off] == (icmp[0] == ICMP6_ND_SOLICIT ? ICMP6_ND_OPT_SRC_LL :
                                                       ICMP6_ND_OPT_TGT_LL)) {
            ll = pkt + off + 2;
        }
    }

    if (icmp[0] == ICMP6_ND_ADVERT) {
        if (qeth_find_ip6(s, target) < 0) {
            qeth_nd_learn(s, target, ll ? ll : frame + 6);
        }
        return;
    }
    if (!qeth_ip6_unspecified(src)) {
        qeth_nd_learn(s, src, ll ? ll : frame + 6);
    }
    if (qeth_find_ip6(s, target) < 0) {
        return;
    }
    qeth_ip6_str(target, ipstr, sizeof(ipstr));
    trace_qeth_l3("nd advertise", ipstr);
    if (qeth_ip6_unspecified(src)) {
        qeth_send_na(s, target, NULL, NULL);    /* duplicate detection */
    } else {
        qeth_send_na(s, target, src, ll ? ll : frame + 6);
    }
}

/*
 * Layer-3 receive: the adapter answers ARP itself, delivers IP packets for
 * the registered addresses, broadcast and registered multicast groups with
 * a type-1 qeth header to the connections that own them, and drops
 * everything else.
 */
static ssize_t qeth_l3_receive(QethCcwDevice *s, const uint8_t *buf,
                               size_t len)
{
    static const uint8_t broadcast[ETH_ALEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    uint8_t header[QETH_HDR_SIZE] = { 0 };
    const uint8_t *pkt = buf + ETH_HDR;
    size_t pkt_len = len - ETH_HDR;
    bool retry = false, delivered = false;
    uint32_t targets;
    unsigned int i;
    uint16_t type;
    uint8_t cast;
    int entry;

    if (len < ETH_HDR) {
        return len;
    }
    type = lduw_be_p(buf + 12);
    if (!memcmp(buf, broadcast, ETH_ALEN)) {
        cast = QETH_CAST_BROADCAST;
    } else if (buf[0] & 1) {
        cast = QETH_CAST_MULTICAST;
    } else if (!memcmp(buf, s->conf.macaddr.a, ETH_ALEN)) {
        cast = QETH_CAST_UNICAST;
    } else {
        return len;
    }

    switch (type) {
    case ETHERTYPE_ARP:
        qeth_l3_rx_arp(s, pkt, pkt_len);
        return len;
    case ETHERTYPE_IPV4:
    {
        uint32_t dst;

        if (pkt_len < 20 || (pkt[0] >> 4) != 4) {
            return len;
        }
        pkt_len = MIN(pkt_len, (size_t)lduw_be_p(pkt + 2));
        dst = ldl_be_p(pkt + 16);
        if (cast == QETH_CAST_MULTICAST) {
            targets = qeth_mc_conns(s, buf);
        } else if (cast == QETH_CAST_UNICAST) {
            entry = qeth_find_ip4(s, dst);
            targets = entry < 0 ? 0 : 1u << s->ip4[entry].conn;
        } else {
            targets = qeth_ip4_conns(s);
        }
        if (dst == 0xffffffff) {
            cast = QETH_CAST_BROADCAST;
        }
        break;
    }
    case ETHERTYPE_IPV6:
        if (pkt_len < IP6_HDR || (pkt[0] >> 4) != 6) {
            return len;
        }
        pkt_len = MIN(pkt_len, (size_t)lduw_be_p(pkt + 4) + IP6_HDR);
        if (cast == QETH_CAST_MULTICAST) {
            targets = qeth_mc_conns(s, buf) | qeth_ip6_mc_conns(s, buf);
        } else if (cast == QETH_CAST_UNICAST) {
            entry = qeth_find_ip6(s, pkt + 24);
            targets = entry < 0 ? 0 : 1u << s->ip6[entry].conn;
        } else {
            targets = 0;
        }
        if (!targets) {
            return len;
        }
        if (pkt[6] == IP6_NEXT_ICMP6) {
            qeth_l3_rx_nd(s, buf, pkt, pkt_len);
        }
        cast |= QETH_HDR3_IPV6;
        break;
    default:
        return len;
    }

    header[0] = QETH_HDR_LAYER3;
    header[1] = cast;
    stw_be_p(header + 8, pkt_len);
    for (i = 0; i < qeth_nr_conns(s); i++) {
        if (!(targets & (1u << i)) || !s->conn[i].layer3) {
            continue;
        }
        if (qeth_rx_deliver(s, &s->conn[i], header, pkt, pkt_len)) {
            delivered = true;
        } else {
            retry = true;
        }
    }
    /* Retry later only if no connection has taken the packet */
    return retry && !delivered ? 0 : len;
}

static ssize_t qeth_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    QethCcwDevice *s = qemu_get_nic_opaque(nc);
    uint8_t header[QETH_HDR_SIZE] = { 0 };
    QethConn *l2 = NULL;
    bool l3 = false;
    unsigned int i;
    ssize_t ret = len;

    for (i = 0; i < qeth_nr_conns(s); i++) {
        QethConn *c = &s->conn[i];

        if (c->in_use && c->layer3) {
            l3 = true;
        } else if (!l2 && qeth_conn_online(c)) {
            l2 = c;
        }
    }
    if (l2) {
        header[0] = QETH_HDR_LAYER2;
        stw_be_p(header + 6, len);
        ret = qeth_rx_deliver(s, l2, header, buf, len) ? len : 0;
        trace_qeth_rx(len, QETH_HDR_LAYER2, ret);
    }
    if (l3 && ret) {
        ret = qeth_l3_receive(s, buf, len);
        trace_qeth_rx(len, QETH_HDR_LAYER3, ret);
    }
    return ret;
}

/*
 * Resume the data and control paths when the VM starts running, in
 * particular after migration: transmit what the guest primed while the
 * backend could not run, retry received packets that waited for input
 * buffers and answer an outstanding READ from the restored responses.
 */
static void qeth_vm_state_change(void *opaque, bool running, RunState state)
{
    QethCcwDevice *s = opaque;
    unsigned int i;

    if (!running) {
        return;
    }
    for (i = 0; i < s->nr_peers; i++) {
        GByteArray *rsp;
        SubchDev *sch = s->peer[i];

        if (!s->read_pending[i] || g_queue_is_empty(&s->responses[i])) {
            continue;
        }
        s->read_pending[i] = false;
        rsp = g_queue_pop_head(&s->responses[i]);
        css_virtual_ccw_complete(sch,
            qeth_copy_response(sch, rsp->data, rsp->len) ? -EFAULT : 0);
        g_byte_array_unref(rsp);
    }
    for (i = 0; i < qeth_nr_conns(s); i++) {
        if (s->conn[i].in_use && s->conn[i].qdio.active) {
            qemu_bh_schedule(s->tx_bh);
            if (s->nic) {
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
            break;
        }
    }
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
    sch->id.ciw[0] = (CIW) { .type = QETH_CIW_VALID | 0,
                             .command = QETH_CMD_RCD, .count = QETH_RCD_SIZE };
    sch->id.ciw[1] = (CIW) { .type = QETH_CIW_VALID | 1,
                             .command = QETH_CMD_SII, .count = 4 };
    sch->id.ciw[2] = (CIW) { .type = QETH_CIW_VALID | 2,
                             .command = QETH_CMD_RNI, .count = QETH_RNI_SIZE };
    sch->id.ciw[3] = (CIW) { .type = QETH_CIW_VALID | 3,
                             .command = QETH_CMD_EQ, .count = 4096 };
    sch->id.ciw[4] = (CIW) { .type = QETH_CIW_VALID | 4,
                             .command = QETH_CMD_AQ, .count = 0 };
    css_sch_build_virtual_schib(sch, s->chpid, QETH_CHPID_TYPE);
    /*
     * Channel-subsystem addressing of the adapter: unit address from the
     * device number and a valid full link address on the single path whose
     * low byte is the control-unit logical address.  Guests that look up
     * the control-unit logical address of a QDIO device through the
     * subchannel description refuse the device without a valid link
     * address.
     */
    sch->topology = (CssDeviceTopology) {
        .valid = true,
        .unit_address = sch->devno,
        .fla_valid_mask = 0x80,
        .fla = { QETH_LINK_ADDRESS << 8 | QETH_CULA },
        .path_cu_number = { ((sch->devno & 0x00f0) << 4) | s->chpid },
    };
    sch->curr_status.pmcw.flags |= PMCW_FLAGS_MASK_QF;
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = qeth_ccw_cb;
    sch->ccw_cb_first = false;
    sch->cancel_cb = qeth_cancel;
    sch->disable_cb = qeth_cancel;
    sch->irb_cb = build_irb_virtual;
    sch->qdio_ops = &qeth_qdio_ops;
    /* Outstanding READs and active queues are resumed after migration. */
    sch->ccw_async_migratable = true;
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

    if (s->data_devices < 1 || s->data_devices > QETH_MAX_DATA) {
        error_setg(errp, "qeth-ccw data-devices must be between 1 and %d",
                   QETH_MAX_DATA);
        return;
    }
    s->nr_peers = 2 + s->data_devices;
    if (id.valid && id.devid > UINT16_MAX - (s->nr_peers - 1)) {
        error_setg(errp, "qeth-ccw base devno leaves no room for %u devices",
                   s->nr_peers);
        return;
    }
    for (i = 0; i < QETH_MAX_PEERS; i++) {
        g_queue_init(&s->responses[i]);
    }
    for (i = 0; i < QETH_MAX_DATA; i++) {
        s->conn[i].data_peer = -1;
    }
    g_queue_init(&s->pending);
    for (i = 0; i < s->nr_peers; i++) {
        if (id.valid) {
            id.devid = base_devno + i;
        }
        s->peer[i] = i == 0 ?
            css_create_sch_at(id, QETH_AUTO_DEVNO, &local_err) :
            css_create_sch(id, &local_err);
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
    chpid = s->chpid_prop == UINT16_MAX ? base_devno >> 8 : s->chpid_prop;
    if (chpid > MAX_CHPID) {
        error_setg(&local_err, "No available CHPID for qeth-ccw");
        goto fail;
    }
    s->chpid = chpid;
    for (i = 0; i < s->nr_peers; i++) {
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
    s->vmstate_change = qemu_add_vm_change_state_handler(qeth_vm_state_change,
                                                         s);
    for (i = 0; i < s->nr_peers; i++) {
        css_generate_sch_crws(s->peer[i]->cssid, s->peer[i]->ssid,
                              s->peer[i]->schid, dev->hotplugged, 1);
    }
    return;

fail:
    error_propagate(errp, local_err);
    for (i = 0; i < s->nr_peers; i++) {
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

    qeth_flush_responses(s);
    qeth_arp_reset(s);
    if (s->vmstate_change) {
        qemu_del_vm_change_state_handler(s->vmstate_change);
        s->vmstate_change = NULL;
    }
    if (s->tx_bh) {
        qemu_bh_delete(s->tx_bh);
        s->tx_bh = NULL;
    }
    if (s->nic) {
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
    for (i = 0; i < s->nr_peers; i++) {
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

    qeth_flush_responses(s);
    s->ulp_seq = 0;
    for (i = 0; i < QETH_MAX_DATA; i++) {
        qeth_conn_release(s, &s->conn[i]);
    }
    s->last_enabled = 0;
    qeth_l3_reset(s);
    memset(s->role, 0, sizeof(s->role));
    for (i = 0; i < s->nr_peers; i++) {
        if (s->peer[i]) {
            css_reset_sch(s->peer[i]);
        }
    }
}

static const Property qeth_properties[] = {
    DEFINE_NIC_PROPERTIES(QethCcwDevice, conf),
    DEFINE_PROP_UINT16("chpid", QethCcwDevice, chpid_prop, UINT16_MAX),
    DEFINE_PROP_UINT8("data-devices", QethCcwDevice, data_devices, 1),
};

/* Migration */

static const VMStateDescription vmstate_qeth_conn = {
    .name = "qeth-ccw/conn",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(in_use, QethConn),
        VMSTATE_BOOL(activated, QethConn),
        VMSTATE_INT32(data_peer, QethConn),
        VMSTATE_UINT32(guest_ulp_filter, QethConn),
        VMSTATE_UINT32(guest_ulp_conn, QethConn),
        VMSTATE_BOOL(layer3, QethConn),
        VMSTATE_BOOL(lan_online, QethConn),
        VMSTATE_UINT32_ARRAY(assists_enabled, QethConn, 2),
        VMSTATE_STRUCT(qdio, QethConn, 1, vmstate_s390_qdio, S390QdioState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_qeth_ip4 = {
    .name = "qeth-ccw/ip4",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(addr, QethIp4),
        VMSTATE_UINT32(flags, QethIp4),
        VMSTATE_INT32(conn, QethIp4),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_qeth_ip6 = {
    .name = "qeth-ccw/ip6",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(addr, QethIp6, 16),
        VMSTATE_INT32(conn, QethIp6),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_qeth_mc = {
    .name = "qeth-ccw/mc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(mac, QethMc, ETH_ALEN),
        VMSTATE_INT32(conn, QethMc),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_qeth_arp = {
    .name = "qeth-ccw/arp",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ip, QethArpEntry),
        VMSTATE_UINT8_ARRAY(mac, QethArpEntry, ETH_ALEN),
        VMSTATE_INT64(used, QethArpEntry),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_qeth_nd = {
    .name = "qeth-ccw/nd",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ip, QethNdEntry, 16),
        VMSTATE_UINT8_ARRAY(mac, QethNdEntry, ETH_ALEN),
        VMSTATE_INT64(used, QethNdEntry),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * Queued control responses: per peer subchannel a count byte, then each
 * response as a 16-bit big-endian length and its data.
 */
static int qeth_pre_save(void *opaque)
{
    QethCcwDevice *s = opaque;
    g_autoptr(GByteArray) blob = g_byte_array_new();
    unsigned int i;

    for (i = 0; i < s->nr_peers; i++) {
        uint8_t count = g_queue_get_length(&s->responses[i]);
        GList *l;

        g_byte_array_append(blob, &count, 1);
        for (l = s->responses[i].head; l; l = l->next) {
            GByteArray *rsp = l->data;
            uint8_t len[2];

            stw_be_p(len, rsp->len);
            g_byte_array_append(blob, len, 2);
            g_byte_array_append(blob, rsp->data, rsp->len);
        }
    }
    g_free(s->migration_responses);
    s->migration_responses_len = blob->len;
    s->migration_responses = (uint8_t *)g_byte_array_free(
        g_steal_pointer(&blob), false);
    return 0;
}

static void qeth_post_save(void *opaque)
{
    QethCcwDevice *s = opaque;

    g_clear_pointer(&s->migration_responses, g_free);
    s->migration_responses_len = 0;
}

static bool qeth_load_responses(QethCcwDevice *s)
{
    const uint8_t *p = s->migration_responses;
    size_t left = s->migration_responses_len;
    unsigned int i, n;

    for (i = 0; i < s->nr_peers; i++) {
        if (left < 1 || p[0] > QETH_RESPONSES_MAX) {
            return false;
        }
        n = *p++;
        left--;
        while (n--) {
            GByteArray *rsp;
            size_t len;

            if (left < 2) {
                return false;
            }
            len = lduw_be_p(p);
            p += 2;
            left -= 2;
            if (len > left || len > QETH_MAX_CTRL) {
                return false;
            }
            rsp = g_byte_array_sized_new(len);
            g_byte_array_append(rsp, p, len);
            g_queue_push_tail(&s->responses[i], rsp);
            p += len;
            left -= len;
        }
    }
    return left == 0;
}

static int qeth_pre_load(void *opaque)
{
    QethCcwDevice *s = opaque;

    /* The incoming state replaces whatever the destination set up. */
    qeth_flush_responses(s);
    qeth_arp_reset(s);
    return 0;
}

static int qeth_post_load(void *opaque, int version_id)
{
    QethCcwDevice *s = opaque;
    unsigned int i, n = qeth_nr_conns(s);
    bool ok;

    ok = qeth_load_responses(s);
    g_clear_pointer(&s->migration_responses, g_free);
    s->migration_responses_len = 0;
    if (!ok) {
        error_report("qeth-ccw: malformed control responses in the "
                     "migration stream");
        return -EINVAL;
    }

    if (s->nr_ip4 > QETH_MAX_IP4 || s->nr_ip6 > QETH_MAX_IP6 ||
        s->nr_mc > QETH_MAX_MC || s->last_enabled < 0 ||
        s->last_enabled >= QETH_MAX_DATA) {
        return -EINVAL;
    }
    for (i = 0; i < s->nr_peers; i++) {
        if (s->role[i] > QETH_ROLE_DATA) {
            return -EINVAL;
        }
        /* A READ is only outstanding while its channel program is. */
        if (!s->peer[i]->ccw_async_pending) {
            s->read_pending[i] = false;
        }
    }
    for (i = n; i < QETH_MAX_PEERS; i++) {
        s->read_pending[i] = false;
    }
    for (i = 0; i < QETH_MAX_DATA; i++) {
        QethConn *c = &s->conn[i];

        if (c->data_peer < -1 || c->data_peer >= (int)s->nr_peers ||
            (i >= n && c->in_use)) {
            return -EINVAL;
        }
    }
    for (i = 0; i < s->nr_ip4; i++) {
        if (s->ip4[i].conn < 0 || s->ip4[i].conn >= n) {
            return -EINVAL;
        }
    }
    for (i = 0; i < s->nr_ip6; i++) {
        if (s->ip6[i].conn < 0 || s->ip6[i].conn >= n) {
            return -EINVAL;
        }
    }
    for (i = 0; i < s->nr_mc; i++) {
        if (s->mc[i].conn < 0 || s->mc[i].conn >= n) {
            return -EINVAL;
        }
    }
    return 0;
}

/*
 * The adapter state: the peer subchannels (including outstanding READs on
 * the control subchannels and the active-queues channel program of each
 * data subchannel), the IDX roles, the MPC tokens and sequence numbers,
 * every ULP connection with its mode, LAN state, assists and QDIO queue
 * descriptors, the layer-3 address tables and ARP/ND caches, and queued
 * control responses.  QDIO buffers and their states are guest storage.
 * Frames held back for ARP or ND resolution are not migrated; the guest
 * retransmits them.
 */
static const VMStateDescription vmstate_qeth = {
    .name = TYPE_QETH_CCW,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = qeth_pre_save,
    .post_save = qeth_post_save,
    .pre_load = qeth_pre_load,
    .post_load = qeth_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_EQUAL(data_devices, QethCcwDevice),
        VMSTATE_ARRAY_OF_POINTER_TO_STRUCT(peer, QethCcwDevice,
                                           QETH_MAX_PEERS, 1,
                                           vmstate_subch_dev, SubchDev),
        VMSTATE_UINT8_ARRAY(role, QethCcwDevice, QETH_MAX_PEERS),
        VMSTATE_BOOL_ARRAY(read_pending, QethCcwDevice, QETH_MAX_PEERS),
        VMSTATE_UINT32(sii, QethCcwDevice),
        VMSTATE_UINT32(guest_issuer_token, QethCcwDevice),
        VMSTATE_UINT32(guest_cm_filter, QethCcwDevice),
        VMSTATE_UINT32(guest_cm_conn, QethCcwDevice),
        VMSTATE_UINT32(ulp_seq, QethCcwDevice),
        VMSTATE_STRUCT_ARRAY(conn, QethCcwDevice, QETH_MAX_DATA, 1,
                             vmstate_qeth_conn, QethConn),
        VMSTATE_INT32(last_enabled, QethCcwDevice),
        VMSTATE_STRUCT_ARRAY(ip4, QethCcwDevice, QETH_MAX_IP4, 1,
                             vmstate_qeth_ip4, QethIp4),
        VMSTATE_UINT32(nr_ip4, QethCcwDevice),
        VMSTATE_STRUCT_ARRAY(ip6, QethCcwDevice, QETH_MAX_IP6, 1,
                             vmstate_qeth_ip6, QethIp6),
        VMSTATE_UINT32(nr_ip6, QethCcwDevice),
        VMSTATE_STRUCT_ARRAY(mc, QethCcwDevice, QETH_MAX_MC, 1,
                             vmstate_qeth_mc, QethMc),
        VMSTATE_UINT32(nr_mc, QethCcwDevice),
        VMSTATE_STRUCT_ARRAY(arp, QethCcwDevice, QETH_ARP_ENTRIES, 1,
                             vmstate_qeth_arp, QethArpEntry),
        VMSTATE_INT64(arp_last_request, QethCcwDevice),
        VMSTATE_UINT32(arp_last_ip, QethCcwDevice),
        VMSTATE_STRUCT_ARRAY(nd, QethCcwDevice, QETH_ND_ENTRIES, 1,
                             vmstate_qeth_nd, QethNdEntry),
        VMSTATE_INT64(nd_last_request, QethCcwDevice),
        VMSTATE_UINT8_ARRAY(nd_last_ip, QethCcwDevice, 16),
        VMSTATE_UINT32(migration_responses_len, QethCcwDevice),
        VMSTATE_VBUFFER_ALLOC_UINT32(migration_responses, QethCcwDevice, 1,
                                     NULL, migration_responses_len),
        VMSTATE_END_OF_LIST()
    }
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
