/*
 * Emulated fixed-block architecture DASD
 *
 * The command set follows the IBM FBA DASD architecture and the behavior of
 * the Hercules FBA device handler.  The initial device identity is a generic
 * capacity IBM 9336 model 20 attached to a 6310 control unit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/s390x/css-bridge.h"
#include "hw/s390x/fba-ccw.h"
#include "hw/s390x/ipl.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"

#define FBA_BLOCK_SIZE             512
#define FBA_CU_TYPE                0x6310
#define FBA_CU_MODEL               0x01
#define FBA_DEV_TYPE               0x9336
#define FBA_DEV_MODEL              0x10
#define FBA_CHPID_TYPE             0x1b

#define FBA_CMD_READ_IPL           0x02
#define FBA_CMD_NOP                0x03
#define FBA_CMD_SENSE              0x04
#define FBA_CMD_UNCONDITIONAL_RSV  0x14
#define FBA_CMD_WRITE              0x41
#define FBA_CMD_READ               0x42
#define FBA_CMD_LOCATE             0x43
#define FBA_CMD_DEFINE_EXTENT      0x63
#define FBA_CMD_RDC                0x64
#define FBA_CMD_RELEASE            0x94
#define FBA_CMD_READ_RESET_LOG     0xa4
#define FBA_CMD_RESERVE            0xb4
#define FBA_CMD_SENSE_ID           0xe4

#define FBA_SENSE_COMMAND_REJECT   0x80
#define FBA_SENSE_EQUIPMENT_CHECK  0x10
#define FBA_SENSE_OVERRUN          0x04

#define FBA_MASK_WRITE_SHIFT       6
#define FBA_MASK_WRITE_MASK        0x03
#define FBA_MASK_WRITE_INHIBIT     0x01
#define FBA_MASK_RESERVED          0x20

#define FBA_LOCATE_RESERVED        0xe0
#define FBA_LOCATE_OP_MASK         0x0f
#define FBA_LOCATE_WRITE           0x01
#define FBA_LOCATE_READ_REPLICATED 0x02
#define FBA_LOCATE_FORMAT_DEFECTIVE 0x04
#define FBA_LOCATE_WRITE_VERIFY    0x05
#define FBA_LOCATE_READ            0x06

struct FBACcwDevice {
    CcwDevice parent_obj;

    BlockBackend *blk;
    uint64_t requested_blocks;
    uint32_t blocks;

    bool extent_defined;
    uint8_t file_mask;
    uint32_t extent_block;
    uint32_t extent_first;
    uint32_t extent_last;
    uint8_t locate_op;
    uint16_t locate_count;
    uint32_t locate_block;
    uint64_t byte_offset;
    bool ipl_positioned;
    bool reserved;

    BlockAIOCB *aiocb;
    QEMUIOVector qiov;
    uint8_t *io_buffer;
    uint32_t io_length;
    uint32_t guest_length;
    uint16_t consumed_blocks;
    bool io_is_read;
    bool completion_overrun;
};

static void fba_set_length_status(FBACcwDevice *fba, const CCW1 *ccw,
                                  uint32_t transferred, uint32_t available)
{
    CcwDevice *cdev = CCW_DEVICE(fba);
    bool suppress = (ccw->flags & CCW_FLAG_SLI) &&
                    !(ccw->flags & CCW_FLAG_DC);
    bool continues = (ccw->flags & CCW_FLAG_DC) && transferred < available;

    if (!suppress && !continues && ccw->count != available) {
        cdev->sch->curr_status.scsw.cstat |= SCSW_CSTAT_INCORR_LEN;
    }
}

static int fba_unit_check(FBACcwDevice *fba, uint8_t sense)
{
    SubchDev *sch = CCW_DEVICE(fba)->sch;
    SCHIB *schib = &sch->curr_status;

    sch->sense_data[0] = sense;
    schib->scsw.dstat = SCSW_DSTAT_CHANNEL_END |
                        SCSW_DSTAT_DEVICE_END |
                        SCSW_DSTAT_UNIT_CHECK;
    schib->scsw.ctrl &= ~SCSW_ACTL_START_PEND;
    schib->scsw.ctrl &= ~SCSW_CTRL_MASK_STCTL;
    schib->scsw.ctrl |= SCSW_STCTL_PRIMARY | SCSW_STCTL_SECONDARY |
                        SCSW_STCTL_ALERT | SCSW_STCTL_STATUS_PEND;
    schib->scsw.cpa = sch->channel_prog + 8;
    return -EIO;
}

static int fba_read_guest(FBACcwDevice *fba, void *buf, size_t len)
{
    SubchDev *sch = CCW_DEVICE(fba)->sch;
    int ret = ccw_dstream_read_buf(&sch->cds, buf, len);

    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    return ret;
}

static int fba_write_guest(FBACcwDevice *fba, const void *buf, size_t len)
{
    SubchDev *sch = CCW_DEVICE(fba)->sch;
    int ret = ccw_dstream_write_buf(&sch->cds, (void *)buf, len);

    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    return ret;
}

static int fba_copy_response(FBACcwDevice *fba, const CCW1 *ccw,
                             const void *buf, size_t available)
{
    size_t len = MIN((size_t)ccw->count, available);
    int ret = fba_write_guest(fba, buf, len);

    if (ret) {
        return ret;
    }
    fba_set_length_status(fba, ccw, len, available);
    return 0;
}

static void fba_free_request(FBACcwDevice *fba)
{
    fba->aiocb = NULL;
    qemu_iovec_destroy(&fba->qiov);
    g_free(fba->io_buffer);
    fba->io_buffer = NULL;
    fba->io_length = 0;
    fba->guest_length = 0;
    fba->consumed_blocks = 0;
}

static void fba_aio_complete(void *opaque, int ret)
{
    FBACcwDevice *fba = opaque;
    SubchDev *sch = CCW_DEVICE(fba)->sch;
    int completion = 0;

    fba->aiocb = NULL;
    if (ret < 0) {
        completion = fba_unit_check(fba, FBA_SENSE_EQUIPMENT_CHECK);
    } else if (fba->io_is_read &&
               fba_write_guest(fba, fba->io_buffer, fba->guest_length)) {
        completion = -EFAULT;
    } else {
        fba->locate_count -= fba->consumed_blocks;
        fba->byte_offset += fba->io_is_read ? fba->guest_length
                                            : fba->io_length;
        if (fba->completion_overrun) {
            completion = fba_unit_check(fba, FBA_SENSE_OVERRUN);
        }
    }
    fba_free_request(fba);
    css_virtual_ccw_complete(sch, completion);
}

static int fba_start_io(FBACcwDevice *fba, CCW1 *ccw, bool read)
{
    uint32_t max_bytes = (uint32_t)fba->locate_count * FBA_BLOCK_SIZE;
    uint32_t guest_len = MIN((uint32_t)ccw->count, max_bytes);
    uint32_t blocks;
    uint32_t io_len;
    int ret;

    if (!read && !blk_is_writable(fba->blk)) {
        return fba_unit_check(fba, FBA_SENSE_EQUIPMENT_CHECK);
    }

    /* A data-chained CCW may not end part way through a physical block. */
    fba->completion_overrun = (ccw->flags & CCW_FLAG_DC) &&
                              (guest_len % FBA_BLOCK_SIZE);
    if (fba->completion_overrun) {
        guest_len -= guest_len % FBA_BLOCK_SIZE;
    }

    if (read) {
        blocks = DIV_ROUND_UP(guest_len, FBA_BLOCK_SIZE);
        io_len = guest_len;
    } else if (!guest_len && !(ccw->flags & CCW_FLAG_DC)) {
        blocks = fba->locate_count;
        io_len = blocks * FBA_BLOCK_SIZE;
    } else {
        blocks = DIV_ROUND_UP(guest_len, FBA_BLOCK_SIZE);
        io_len = blocks * FBA_BLOCK_SIZE;
    }

    CCW_DEVICE(fba)->sch->curr_status.scsw.count = ccw->count - guest_len;
    if (!fba->completion_overrun) {
        fba_set_length_status(fba, ccw, guest_len, max_bytes);
    }

    if (!io_len) {
        if (fba->completion_overrun) {
            return fba_unit_check(fba, FBA_SENSE_OVERRUN);
        }
        return 0;
    }

    fba->io_buffer = blk_blockalign(fba->blk, io_len);
    fba->io_length = io_len;
    fba->guest_length = guest_len;
    fba->consumed_blocks = blocks;
    fba->io_is_read = read;
    qemu_iovec_init(&fba->qiov, 1);
    qemu_iovec_add(&fba->qiov, fba->io_buffer, io_len);

    if (!read) {
        memset(fba->io_buffer, 0, io_len);
        ret = fba_read_guest(fba, fba->io_buffer, guest_len);
        if (ret) {
            fba_free_request(fba);
            return ret;
        }
        fba->aiocb = blk_aio_pwritev(fba->blk, fba->byte_offset,
                                     &fba->qiov, 0, fba_aio_complete, fba);
    } else {
        fba->aiocb = blk_aio_preadv(fba->blk, fba->byte_offset,
                                    &fba->qiov, 0, fba_aio_complete, fba);
    }
    return CSS_CCW_PENDING;
}

static int fba_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    FBACcwDevice *fba = sch->driver_data;
    uint8_t buf[32] = { 0 };
    uint8_t previous = sch->last_cmd_valid ? sch->last_cmd.cmd_code : 0;
    bool continuation = sch->last_cmd_valid &&
                        (sch->last_cmd.flags & CCW_FLAG_DC);
    uint32_t first, last, extent_block;
    uint16_t count;
    uint8_t op, replication;
    bool after_read_ipl = false;

    if (!sch->last_cmd_valid) {
        after_read_ipl = fba->ipl_positioned && ccw.cmd_code == FBA_CMD_READ;
        fba->ipl_positioned = false;
        if (!after_read_ipl) {
            fba->extent_defined = false;
        }
    }

    switch (ccw.cmd_code) {
    case FBA_CMD_READ_IPL:
    {
        int ret;

        if (sch->last_cmd_valid && previous != FBA_CMD_READ_IPL) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        if (ccw.flags & CCW_FLAG_DC) {
            return fba_unit_check(fba, FBA_SENSE_OVERRUN);
        }
        fba->file_mask = 0;
        fba->extent_block = 0;
        fba->extent_first = 0;
        fba->extent_last = fba->blocks - 1;
        fba->extent_defined = true;
        fba->locate_count = 1;
        fba->byte_offset = 0;
        ret = fba_start_io(fba, &ccw, true);
        /*
         * Read IPL positions within block zero; it does not consume the
         * block for the command-chained IPL channel program.
         */
        if (ret == CSS_CCW_PENDING) {
            fba->consumed_blocks = 0;
            fba->ipl_positioned = true;
        }
        return ret;
    }

    case FBA_CMD_NOP:
        sch->curr_status.scsw.count = ccw.count;
        return 0;

    case FBA_CMD_WRITE:
        if (previous != FBA_CMD_LOCATE &&
            !(continuation && previous == FBA_CMD_WRITE)) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        op = fba->locate_op & FBA_LOCATE_OP_MASK;
        if (op != FBA_LOCATE_WRITE && op != FBA_LOCATE_WRITE_VERIFY) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        return fba_start_io(fba, &ccw, false);

    case FBA_CMD_READ:
        if (!after_read_ipl && previous != FBA_CMD_LOCATE &&
            previous != FBA_CMD_READ_IPL &&
            !(continuation && previous == FBA_CMD_READ)) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        op = fba->locate_op & FBA_LOCATE_OP_MASK;
        if (!after_read_ipl && previous != FBA_CMD_READ_IPL &&
            op != FBA_LOCATE_READ &&
            op != FBA_LOCATE_READ_REPLICATED) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        return fba_start_io(fba, &ccw, true);

    case FBA_CMD_LOCATE:
        if (ccw.count < 8 || !fba->extent_defined) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        if (fba_read_guest(fba, buf, 8)) {
            return -EFAULT;
        }
        op = buf[0];
        replication = buf[1];
        count = lduw_be_p(buf + 2);
        first = ldl_be_p(buf + 4);
        if (op & FBA_LOCATE_RESERVED) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        switch (op & FBA_LOCATE_OP_MASK) {
        case FBA_LOCATE_WRITE:
        case FBA_LOCATE_WRITE_VERIFY:
            if (((fba->file_mask >> FBA_MASK_WRITE_SHIFT) &
                 FBA_MASK_WRITE_MASK) == FBA_MASK_WRITE_INHIBIT) {
                return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
            }
            break;
        case FBA_LOCATE_READ:
            break;
        case FBA_LOCATE_READ_REPLICATED:
            if (!replication || !count || replication % count) {
                return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
            }
            break;
        case FBA_LOCATE_FORMAT_DEFECTIVE:
            if (((fba->file_mask >> FBA_MASK_WRITE_SHIFT) &
                 FBA_MASK_WRITE_MASK) == 0) {
                return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
            }
            break;
        default:
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        if (!count || first < fba->extent_first ||
            first > fba->extent_last ||
            count > fba->extent_last - first + 1) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        fba->locate_op = op;
        fba->locate_count = count;
        fba->locate_block = first;
        fba->byte_offset = ((uint64_t)fba->extent_block + first -
                            fba->extent_first) * FBA_BLOCK_SIZE;
        sch->curr_status.scsw.count = ccw.count - 8;
        return 0;

    case FBA_CMD_DEFINE_EXTENT:
        if (ccw.count < 16 || fba->extent_defined) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        if (fba_read_guest(fba, buf, 16)) {
            return -EFAULT;
        }
        if (buf[0] & FBA_MASK_RESERVED) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        extent_block = ldl_be_p(buf + 4);
        first = ldl_be_p(buf + 8);
        last = ldl_be_p(buf + 12);
        if (last < first || extent_block >= fba->blocks ||
            (uint64_t)last - first >= fba->blocks - extent_block) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        fba->file_mask = buf[0];
        fba->extent_block = extent_block;
        fba->extent_first = first;
        fba->extent_last = last;
        fba->extent_defined = true;
        sch->curr_status.scsw.count = ccw.count - 16;
        return 0;

    case FBA_CMD_RDC:
        buf[0] = 0x30;
        buf[1] = 0x08;
        buf[2] = 0x21;
        buf[3] = 0x11;
        stw_be_p(buf + 4, FBA_BLOCK_SIZE);
        stl_be_p(buf + 6, 111);
        stl_be_p(buf + 10, 777);
        stl_be_p(buf + 14, fba->blocks);
        return fba_copy_response(fba, &ccw, buf, sizeof(buf));

    case FBA_CMD_RELEASE:
        if (fba->extent_defined) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        fba->reserved = false;
        /* Reserve and release return the same data as Basic Sense. */
        /* fall through */
    case FBA_CMD_SENSE:
        memcpy(buf, sch->sense_data, sizeof(buf));
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
        return fba_copy_response(fba, &ccw, buf, 24);

    case FBA_CMD_RESERVE:
        if (fba->extent_defined) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        fba->reserved = true;
        memcpy(buf, sch->sense_data, sizeof(buf));
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
        return fba_copy_response(fba, &ccw, buf, 24);

    case FBA_CMD_UNCONDITIONAL_RSV:
        if (sch->last_cmd_valid) {
            return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
        }
        fba->reserved = true;
        memcpy(buf, sch->sense_data, sizeof(buf));
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
        return fba_copy_response(fba, &ccw, buf, 24);

    case FBA_CMD_SENSE_ID:
        buf[0] = 0xff;
        stw_be_p(buf + 1, FBA_CU_TYPE);
        buf[3] = FBA_CU_MODEL;
        stw_be_p(buf + 4, FBA_DEV_TYPE);
        buf[6] = FBA_DEV_MODEL;
        return fba_copy_response(fba, &ccw, buf, 7);

    case FBA_CMD_READ_RESET_LOG:
        return fba_copy_response(fba, &ccw, buf, 24);

    default:
        return fba_unit_check(fba, FBA_SENSE_COMMAND_REJECT);
    }
}

static void fba_cancel(SubchDev *sch)
{
    FBACcwDevice *fba = sch->driver_data;

    if (fba->aiocb) {
        blk_aio_cancel(fba->aiocb);
    }
}

static void fba_reset_hold(Object *obj, ResetType type)
{
    FBACcwDevice *fba = FBA_CCW(obj);
    FBACcwDeviceClass *fbc = FBA_CCW_GET_CLASS(fba);

    if (fba->aiocb) {
        blk_aio_cancel(fba->aiocb);
    }
    fba->extent_defined = false;
    fba->ipl_positioned = false;
    fba->reserved = false;

    if (fbc->parent_phases.hold) {
        fbc->parent_phases.hold(obj, type);
    }
}

static int fba_pre_save(void *opaque)
{
    FBACcwDevice *fba = opaque;

    blk_drain(fba->blk);
    return 0;
}

static void fba_realize(DeviceState *dev, Error **errp)
{
    FBACcwDevice *fba = FBA_CCW(dev);
    CcwDevice *cdev = CCW_DEVICE(dev);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    SubchDev *sch;
    uint16_t chpid;
    int64_t length;
    uint64_t blocks;
    uint64_t perm;
    Error *local_err = NULL;

    if (!fba->blk) {
        error_setg(errp, "fba-ccw requires a block backend via drive=");
        return;
    }
    length = blk_getlength(fba->blk);
    if (length < 0) {
        error_setg_errno(errp, -length, "cannot determine FBA backend size");
        return;
    }
    if (!length || length % FBA_BLOCK_SIZE) {
        error_setg(errp, "FBA backend size must be a non-zero multiple of %u",
                   FBA_BLOCK_SIZE);
        return;
    }
    blocks = length / FBA_BLOCK_SIZE;
    if (fba->requested_blocks) {
        if (fba->requested_blocks > blocks) {
            error_setg(errp, "blocks exceeds FBA backend capacity");
            return;
        }
        blocks = fba->requested_blocks;
    }
    if (blocks > UINT32_MAX) {
        error_setg(errp,
                   "FBA capacity exceeds the 32-bit architectural block count");
        return;
    }
    fba->blocks = blocks;

    perm = BLK_PERM_CONSISTENT_READ;
    if (blk_supports_write_perm(fba->blk)) {
        perm |= BLK_PERM_WRITE;
    }
    if (blk_set_perm(fba->blk, perm, BLK_PERM_ALL, errp) < 0) {
        return;
    }

    sch = css_create_sch(cdev->devno, errp);
    if (!sch) {
        return;
    }
    sch->driver_data = fba;
    cdev->sch = sch;
    chpid = css_find_virtual_chpid(sch->cssid, FBA_CHPID_TYPE);
    if (chpid > MAX_CHPID) {
        error_setg(&local_err, "No available CHPID for fba-ccw");
        goto fail;
    }

    sch->id.reserved = 0xff;
    sch->id.cu_type = FBA_CU_TYPE;
    sch->id.cu_model = FBA_CU_MODEL;
    sch->id.dev_type = FBA_DEV_TYPE;
    sch->id.dev_model = FBA_DEV_MODEL;
    css_sch_build_virtual_schib(sch, chpid, FBA_CHPID_TYPE);
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = fba_ccw_cb;
    sch->ccw_cb_first = true;
    sch->cancel_cb = fba_cancel;
    sch->disable_cb = fba_cancel;
    sch->irb_cb = build_irb_virtual;

    if (!cdk->realize(cdev, &local_err)) {
        goto fail;
    }
    return;

fail:
    error_propagate(errp, local_err);
    css_subch_assign(sch->cssid, sch->ssid, sch->schid, sch->devno, NULL);
    cdev->sch = NULL;
    g_free(sch);
}

static void fba_unrealize(DeviceState *dev)
{
    FBACcwDevice *fba = FBA_CCW(dev);

    if (fba->aiocb) {
        blk_aio_cancel(fba->aiocb);
    }
}

static const Property fba_properties[] = {
    DEFINE_PROP_DRIVE("drive", FBACcwDevice, blk),
    DEFINE_PROP_UINT64("blocks", FBACcwDevice, requested_blocks, 0),
    DEFINE_PROP_CCW_LOADPARM("loadparm", CcwDevice, loadparm),
};

static const VMStateDescription vmstate_fba_ccw = {
    .name = TYPE_FBA_CCW,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = fba_pre_save,
    .fields = (const VMStateField[]) {
        VMSTATE_CCW_DEVICE(parent_obj, FBACcwDevice),
        VMSTATE_BOOL(extent_defined, FBACcwDevice),
        VMSTATE_UINT8(file_mask, FBACcwDevice),
        VMSTATE_UINT32(extent_block, FBACcwDevice),
        VMSTATE_UINT32(extent_first, FBACcwDevice),
        VMSTATE_UINT32(extent_last, FBACcwDevice),
        VMSTATE_UINT8(locate_op, FBACcwDevice),
        VMSTATE_UINT16(locate_count, FBACcwDevice),
        VMSTATE_UINT32(locate_block, FBACcwDevice),
        VMSTATE_UINT64(byte_offset, FBACcwDevice),
        VMSTATE_BOOL(ipl_positioned, FBACcwDevice),
        VMSTATE_BOOL(reserved, FBACcwDevice),
        VMSTATE_END_OF_LIST()
    },
};

static void fba_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CCWDeviceClass *cdk = CCW_DEVICE_CLASS(klass);
    FBACcwDeviceClass *fbc = FBA_CCW_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = fba_realize;
    dc->unrealize = fba_unrealize;
    dc->hotpluggable = false;
    dc->vmsd = &vmstate_fba_ccw;
    device_class_set_props(dc, fba_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    cdk->build_iplb = s390_ipl_build_ccw_iplb;
    resettable_class_set_parent_phases(rc, NULL, fba_reset_hold, NULL,
                                       &fbc->parent_phases);
}

static const TypeInfo fba_info = {
    .name = TYPE_FBA_CCW,
    .parent = TYPE_CCW_DEVICE,
    .instance_size = sizeof(FBACcwDevice),
    .class_size = sizeof(FBACcwDeviceClass),
    .class_init = fba_class_init,
};

static void fba_register_types(void)
{
    type_register_static(&fba_info);
}

type_init(fba_register_types)
