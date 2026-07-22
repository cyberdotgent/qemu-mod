/*
 * Emulated IBM 3590 A50 tape drive backed by an AWS image
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/s390x/3590-ccw.h"
#include "hw/s390x/css-bridge.h"
#include "hw/s390x/ipl.h"
#include "hw/s390x/tape-aws.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"
#include "trace.h"

#define TAPE3590_CU_TYPE          0x3590
#define TAPE3590_CU_MODEL         0x11
#define TAPE3590_DEV_TYPE         0x3590
#define TAPE3590_DEV_MODEL        0x60
#define TAPE3590_CHPID_TYPE       0x1b

#define TAPE_CMD_READ_IPL         0x02
#define TAPE_CMD_NOP              0x03
#define TAPE_CMD_SENSE            0x04
#define TAPE_CMD_READ_FORWARD     0x06
#define TAPE_CMD_REWIND           0x07
#define TAPE_CMD_READ_PREVIOUS    0x0a
#define TAPE_CMD_REWIND_UNLOAD    0x0f
#define TAPE_CMD_READ_BLOCK_ID    0x22
#define TAPE_CMD_BACKSPACE_BLOCK  0x27
#define TAPE_CMD_BACKSPACE_FILE   0x2f
#define TAPE_CMD_FSPACE_BLOCK     0x37
#define TAPE_CMD_FSPACE_FILE      0x3f
#define TAPE_CMD_LOCATE           0x4f
#define TAPE_CMD_READ_MED_CHAR    0x62
#define TAPE_CMD_RDC              0x64
#define TAPE_CMD_MEDIUM_SENSE     0xc2
#define TAPE_CMD_MODE_SET         0xdb
#define TAPE_CMD_SENSE_ID         0xe4

#define TAPE_SENSE_COMMAND_REJECT 0x80
#define TAPE_SENSE_INTERVENTION   0x40
#define TAPE_SENSE_EQUIPMENT      0x10
#define TAPE_SENSE_DATA_CHECK     0x08

struct Tape3590CcwDevice {
    CcwDevice parent_obj;
    BlockBackend *blk;
    AwsTape medium;
    uint8_t *record;
    uint32_t record_length;
    uint32_t record_offset;
    bool tray_open;
};

static void tape3590_cancel(SubchDev *sch)
{
    Tape3590CcwDevice *tape = sch->driver_data;

    tape->record_length = tape->record_offset = 0;
}

static void tape3590_change_media(void *opaque, bool load, Error **errp)
{
    Tape3590CcwDevice *tape = opaque;

    if (load) {
        if (!blk_is_inserted(tape->blk)) {
            error_setg(errp, "cannot close 3590 tape drive without a medium");
            return;
        }
        /*
         * Block I/O to removable media is unavailable while its tray is
         * reported open.  Close it before validating the AWS stream.
         */
        tape->tray_open = false;
        aws_tape_init(&tape->medium, tape->blk, errp);
        if (*errp) {
            tape->tray_open = true;
            return;
        }
    } else {
        tape->tray_open = true;
        aws_tape_rewind(&tape->medium);
    }
    tape->record_length = tape->record_offset = 0;
    if (CCW_DEVICE(tape)->sch) {
        css_generate_unsolicited_io_interrupt(CCW_DEVICE(tape)->sch,
            SCSW_DSTAT_ATTENTION | SCSW_DSTAT_DEVICE_END);
    }
}

static bool tape3590_is_tray_open(void *opaque)
{
    return ((Tape3590CcwDevice *)opaque)->tray_open;
}

static const BlockDevOps tape3590_block_ops = {
    .change_media_cb = tape3590_change_media,
    .is_tray_open = tape3590_is_tray_open,
};

static int tape3590_unit_check(Tape3590CcwDevice *tape, uint8_t sense)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    SCHIB *schib = &sch->curr_status;

    memset(sch->sense_data, 0, sizeof(sch->sense_data));
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

static int tape3590_unit_exception(Tape3590CcwDevice *tape)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    SCHIB *schib = &sch->curr_status;

    schib->scsw.dstat = SCSW_DSTAT_CHANNEL_END |
                        SCSW_DSTAT_DEVICE_END |
                        SCSW_DSTAT_UNIT_EXCEP;
    schib->scsw.ctrl &= ~SCSW_ACTL_START_PEND;
    schib->scsw.ctrl &= ~SCSW_CTRL_MASK_STCTL;
    schib->scsw.ctrl |= SCSW_STCTL_PRIMARY | SCSW_STCTL_SECONDARY |
                        SCSW_STCTL_STATUS_PEND;
    schib->scsw.cpa = sch->channel_prog + 8;
    return -EIO;
}

static void tape3590_set_length_status(Tape3590CcwDevice *tape,
                                       const CCW1 *ccw,
                                       uint32_t transferred,
                                       uint32_t available)
{
    bool suppress = (ccw->flags & CCW_FLAG_SLI) &&
                    !(ccw->flags & CCW_FLAG_DC);
    bool continues = (ccw->flags & CCW_FLAG_DC) && transferred < available;

    if (!suppress && !continues && ccw->count != available) {
        CCW_DEVICE(tape)->sch->curr_status.scsw.cstat |=
            SCSW_CSTAT_INCORR_LEN;
    }
}

static int tape3590_copy_response(Tape3590CcwDevice *tape, const CCW1 *ccw,
                                  const void *buf, size_t available)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    size_t length = MIN((size_t)ccw->count, available);
    int ret = ccw_dstream_write_buf(&sch->cds, (void *)buf, length);

    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    if (!ret) {
        tape3590_set_length_status(tape, ccw, length, available);
    }
    return ret;
}

static int tape3590_read(Tape3590CcwDevice *tape, const CCW1 *ccw,
                         bool previous)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    bool continuation = sch->last_cmd_valid &&
                        (sch->last_cmd.flags & CCW_FLAG_DC) &&
                        tape->record_offset < tape->record_length;
    AwsTapeResult result;
    size_t record_length;
    uint32_t available, length;

    if (!continuation) {
        if (previous) {
            result = aws_tape_backspace(&tape->medium);
            if (result != AWS_TAPE_OK) {
                return tape3590_unit_check(tape, TAPE_SENSE_DATA_CHECK);
            }
        }
        result = aws_tape_read(&tape->medium, tape->record,
                               AWS_TAPE_MAX_RECORD, &record_length);
        if (result == AWS_TAPE_MARK) {
            sch->curr_status.scsw.count = ccw->count;
            return tape3590_unit_exception(tape);
        }
        if (result != AWS_TAPE_OK) {
            return tape3590_unit_check(tape,
                    result == AWS_TAPE_IO_ERROR ? TAPE_SENSE_EQUIPMENT :
                                                 TAPE_SENSE_DATA_CHECK);
        }
        tape->record_length = record_length;
        tape->record_offset = 0;
        if (previous) {
            /* Read Previous leaves the medium before the returned block. */
            result = aws_tape_backspace(&tape->medium);
            if (result != AWS_TAPE_OK) {
                return tape3590_unit_check(tape, TAPE_SENSE_DATA_CHECK);
            }
        }
    }
    available = tape->record_length - tape->record_offset;
    length = MIN((uint32_t)ccw->count, available);
    if (ccw_dstream_write_buf(&sch->cds,
                              tape->record + tape->record_offset, length)) {
        return -EFAULT;
    }
    tape->record_offset += length;
    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    tape3590_set_length_status(tape, ccw, length, available);
    return 0;
}

static int tape3590_space(Tape3590CcwDevice *tape, bool forward,
                          bool to_mark)
{
    AwsTapeResult result;
    size_t length;

    do {
        if (forward) {
            result = aws_tape_read(&tape->medium, tape->record,
                                   AWS_TAPE_MAX_RECORD, &length);
        } else {
            result = aws_tape_backspace(&tape->medium);
            if (result == AWS_TAPE_OK && to_mark) {
                int64_t position = tape->medium.offset;
                uint32_t block_id = tape->medium.block_id;

                result = aws_tape_read(&tape->medium, tape->record,
                                       AWS_TAPE_MAX_RECORD, &length);
                tape->medium.offset = position;
                tape->medium.block_id = block_id;
            }
        }
        if (result != AWS_TAPE_OK && result != AWS_TAPE_MARK) {
            return tape3590_unit_check(tape, TAPE_SENSE_DATA_CHECK);
        }
    } while (to_mark && result != AWS_TAPE_MARK);
    return 0;
}

static int tape3590_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    Tape3590CcwDevice *tape = sch->driver_data;
    uint8_t buf[64] = { 0 };
    uint32_t block_id;
    AwsTapeResult result;

    trace_tape3590_ccw(sch->devno, ccw.cmd_code, ccw.cda, ccw.count,
                       ccw.flags, tape->medium.block_id);

    if (tape->tray_open && ccw.cmd_code != TAPE_CMD_SENSE &&
        ccw.cmd_code != TAPE_CMD_SENSE_ID) {
        return tape3590_unit_check(tape, TAPE_SENSE_INTERVENTION);
    }

    switch (ccw.cmd_code) {
    case TAPE_CMD_READ_IPL:
        return tape3590_read(tape, &ccw, false);
    case TAPE_CMD_READ_FORWARD:
        return tape3590_read(tape, &ccw, false);
    case TAPE_CMD_READ_PREVIOUS:
        return tape3590_read(tape, &ccw, true);
    case TAPE_CMD_NOP:
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    case TAPE_CMD_REWIND:
        aws_tape_rewind(&tape->medium);
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    case TAPE_CMD_REWIND_UNLOAD:
        aws_tape_rewind(&tape->medium);
        tape->tray_open = true;
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    case TAPE_CMD_BACKSPACE_BLOCK:
        sch->curr_status.scsw.count = ccw.count;
        return tape3590_space(tape, false, false);
    case TAPE_CMD_BACKSPACE_FILE:
        sch->curr_status.scsw.count = ccw.count;
        return tape3590_space(tape, false, true);
    case TAPE_CMD_FSPACE_BLOCK:
        sch->curr_status.scsw.count = ccw.count;
        return tape3590_space(tape, true, false);
    case TAPE_CMD_FSPACE_FILE:
        sch->curr_status.scsw.count = ccw.count;
        return tape3590_space(tape, true, true);
    case TAPE_CMD_READ_BLOCK_ID:
        stl_be_p(buf, tape->medium.block_id);
        stl_be_p(buf + 4, tape->medium.block_id);
        return tape3590_copy_response(tape, &ccw, buf, 8);
    case TAPE_CMD_LOCATE:
        if (ccw.count < 4 || ccw_dstream_read_buf(&sch->cds, buf, 4)) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        block_id = ldl_be_p(buf);
        result = aws_tape_locate(&tape->medium, block_id);
        sch->curr_status.scsw.count = ccw.count - 4;
        return result == AWS_TAPE_OK ? 0 :
               tape3590_unit_check(tape, TAPE_SENSE_DATA_CHECK);
    case TAPE_CMD_SENSE:
        memcpy(buf, sch->sense_data, 32);
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
        return tape3590_copy_response(tape, &ccw, buf, 32);
    case TAPE_CMD_SENSE_ID:
        buf[0] = 0xff;
        stw_be_p(buf + 1, TAPE3590_CU_TYPE);
        buf[3] = TAPE3590_CU_MODEL;
        stw_be_p(buf + 4, TAPE3590_DEV_TYPE);
        buf[6] = TAPE3590_DEV_MODEL;
        return tape3590_copy_response(tape, &ccw, buf, 7);
    case TAPE_CMD_RDC:
        stw_be_p(buf, TAPE3590_CU_TYPE);
        buf[2] = TAPE3590_CU_MODEL;
        stw_be_p(buf + 3, TAPE3590_DEV_TYPE);
        buf[5] = TAPE3590_DEV_MODEL;
        stl_be_p(buf + 6, 0x01805fe8); /* NTP, block-id, SIC, NOP, LWP */
        stl_be_p(buf + 12, 0x00800400); /* read-forward, medium-sense */
        buf[24] = 0x35; buf[25] = 0x90; buf[26] = 0x11;
        buf[27] = 0x35; buf[28] = 0x90; buf[29] = 0x60;
        buf[40] = 0x46; buf[41] = 0x83; buf[42] = 0x80;
        stl_be_p(buf + 43, AWS_TAPE_MAX_RECORD);
        stl_be_p(buf + 47, AWS_TAPE_MAX_RECORD);
        return tape3590_copy_response(tape, &ccw, buf, sizeof(buf));
    case TAPE_CMD_READ_MED_CHAR:
        return tape3590_copy_response(tape, &ccw, buf, sizeof(buf));
    case TAPE_CMD_MEDIUM_SENSE:
        buf[0] = 0x01;
        return tape3590_copy_response(tape, &ccw, buf, sizeof(buf));
    case TAPE_CMD_MODE_SET:
        if (!ccw.count || ccw_dstream_read_buf(&sch->cds, buf, 1)) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
        return 0;
    default:
        return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
    }
}

static void tape3590_reset_hold(Object *obj, ResetType type)
{
    Tape3590CcwDevice *tape = TAPE_3590_CCW(obj);
    Tape3590CcwDeviceClass *tc = TAPE_3590_CCW_GET_CLASS(tape);

    /*
     * A channel-subsystem reset clears buffered command state but does not
     * reposition the medium.  In particular, the load-normal reset at the
     * end of CCW IPL must preserve the position following the IPL records.
     */
    tape->record_length = tape->record_offset = 0;
    if (tc->parent_phases.hold) {
        tc->parent_phases.hold(obj, type);
    }
}

static void tape3590_realize(DeviceState *dev, Error **errp)
{
    Tape3590CcwDevice *tape = TAPE_3590_CCW(dev);
    CcwDevice *cdev = CCW_DEVICE(dev);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    SubchDev *sch;
    uint16_t chpid;
    uint64_t perm = BLK_PERM_CONSISTENT_READ;
    Error *local_err = NULL;

    if (!tape->blk) {
        int ret;

        tape->blk = blk_new(qemu_get_aio_context(), 0, BLK_PERM_ALL);
        ret = blk_attach_dev(tape->blk, dev);
        if (ret < 0) {
            error_setg_errno(errp, -ret,
                             "cannot create empty 3590 tape backend");
            blk_unref(tape->blk);
            tape->blk = NULL;
            return;
        }
        blk_unref(tape->blk);
    }
    if (blk_set_perm(tape->blk, perm, BLK_PERM_ALL, errp) < 0) {
        return;
    }
    tape->tray_open = !blk_is_inserted(tape->blk);
    if (!tape->tray_open) {
        aws_tape_init(&tape->medium, tape->blk, errp);
        if (*errp) {
            return;
        }
    }
    blk_set_dev_ops(tape->blk, &tape3590_block_ops, tape);
    tape->record = g_malloc(AWS_TAPE_MAX_RECORD);
    sch = css_create_sch(cdev->devno, errp);
    if (!sch) {
        goto fail_record;
    }
    sch->driver_data = tape;
    cdev->sch = sch;
    chpid = css_find_virtual_chpid(sch->cssid, TAPE3590_CHPID_TYPE);
    if (chpid > MAX_CHPID) {
        error_setg(&local_err, "No available CHPID for 3590-ccw");
        goto fail_sch;
    }
    sch->id.reserved = 0xff;
    sch->id.cu_type = TAPE3590_CU_TYPE;
    sch->id.cu_model = TAPE3590_CU_MODEL;
    sch->id.dev_type = TAPE3590_DEV_TYPE;
    sch->id.dev_model = TAPE3590_DEV_MODEL;
    css_sch_build_virtual_schib(sch, chpid, TAPE3590_CHPID_TYPE);
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = tape3590_ccw_cb;
    sch->cancel_cb = tape3590_cancel;
    sch->ccw_cb_first = true;
    sch->irb_cb = build_irb_virtual;
    if (!cdk->realize(cdev, &local_err)) {
        goto fail_sch;
    }
    return;

fail_sch:
    error_propagate(errp, local_err);
    css_subch_assign(sch->cssid, sch->ssid, sch->schid, sch->devno, NULL);
    cdev->sch = NULL;
    g_free(sch);
fail_record:
    g_clear_pointer(&tape->record, g_free);
}

static void tape3590_unrealize(DeviceState *dev)
{
    Tape3590CcwDevice *tape = TAPE_3590_CCW(dev);
    g_clear_pointer(&tape->record, g_free);
}

static const VMStateDescription vmstate_tape3590 = {
    .name = "3590-ccw",
    .unmigratable = 1,
};

static const Property tape3590_properties[] = {
    DEFINE_PROP_DRIVE("drive", Tape3590CcwDevice, blk),
    DEFINE_PROP_CCW_LOADPARM("loadparm", CcwDevice, loadparm),
};

static void tape3590_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CCWDeviceClass *cdk = CCW_DEVICE_CLASS(klass);
    Tape3590CcwDeviceClass *tc = TAPE_3590_CCW_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = tape3590_realize;
    dc->unrealize = tape3590_unrealize;
    dc->vmsd = &vmstate_tape3590;
    dc->hotpluggable = false;
    device_class_set_props(dc, tape3590_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    cdk->build_iplb = s390_ipl_build_ccw_iplb;
    resettable_class_set_parent_phases(rc, NULL, tape3590_reset_hold, NULL,
                                       &tc->parent_phases);
}

static const TypeInfo tape3590_info = {
    .name = TYPE_TAPE_3590_CCW,
    .parent = TYPE_CCW_DEVICE,
    .instance_size = sizeof(Tape3590CcwDevice),
    .class_size = sizeof(Tape3590CcwDeviceClass),
    .class_init = tape3590_class_init,
};

static void tape3590_register_types(void)
{
    type_register_static(&tape3590_info);
}
type_init(tape3590_register_types)
