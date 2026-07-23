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
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "system/block-backend.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "trace.h"

#define TAPE3590_CHPID_TYPE       0x1b
#define TAPE3590_AUTO_DEVNO       0x0580

#define TAPE_CMD_WRITE            0x01
#define TAPE_CMD_READ_IPL         0x02
#define TAPE_CMD_NOP              0x03
#define TAPE_CMD_SENSE            0x04
#define TAPE_CMD_READ_FORWARD     0x06
#define TAPE_CMD_REWIND           0x07
#define TAPE_CMD_READ_PREVIOUS    0x0a
#define TAPE_CMD_REWIND_UNLOAD    0x0f
#define TAPE_CMD_ERASE_GAP        0x17
#define TAPE_CMD_WRITE_MARK       0x1f
#define TAPE_CMD_READ_BLOCK_ID    0x22
#define TAPE_CMD_BACKSPACE_BLOCK  0x27
#define TAPE_CMD_BACKSPACE_FILE   0x2f
#define TAPE_CMD_FSPACE_BLOCK     0x37
#define TAPE_CMD_FSPACE_FILE      0x3f
#define TAPE_CMD_SYNCHRONIZE      0x43
#define TAPE_CMD_LOCATE           0x4f
#define TAPE_CMD_READ_MED_CHAR    0x62
#define TAPE_CMD_RDC              0x64
#define TAPE_CMD_MEDIUM_SENSE     0xc2
#define TAPE_CMD_WRITE_IMMEDIATE  0xc3
#define TAPE_CMD_ASSIGN           0xb7
#define TAPE_CMD_MODE_SET         0xdb
#define TAPE_CMD_SENSE_ID         0xe4

#define TAPE_SENSE_COMMAND_REJECT 0x80
#define TAPE_SENSE_INTERVENTION   0x40
#define TAPE_SENSE_EQUIPMENT      0x10
#define TAPE_SENSE_DATA_CHECK     0x08
#define TAPE_SENSE_DEFERRED_CHECK 0x02

#define TAPE_ERA_READ_DATA_CHECK  0x23
#define TAPE_ERA_WRITE_DATA_CHECK 0x25
#define TAPE_ERA_COMMAND_REJECT   0x27
#define TAPE_ERA_WRITE_PROTECTED  0x30
#define TAPE_ERA_TAPE_VOID        0x31
#define TAPE_ERA_PHYSICAL_EOT     0x38
#define TAPE_ERA_BACKWARD_AT_BOT  0x39
#define TAPE_ERA_VOLUME_FENCED    0x47

typedef struct TapeIdentity {
    const char *name;
    uint16_t cu_type;
    uint8_t cu_model;
    uint16_t dev_type;
    uint8_t dev_model;
    uint8_t mdr;
    uint8_t obr;
} TapeIdentity;

/*
 * Hercules tapedev.c DevInitTab and tapeccws.c TapeDevtypeList define
 * these supported tape identities and model pairings.
 */
static const TapeIdentity tape_identities[] = {
    { "3410", 0x3115, 0x01, 0x3410, 0x01 },
    { "3411", 0x3115, 0x01, 0x3411, 0x01 },
    { "3420", 0x3803, 0x02, 0x3420, 0x06 },
    { "3422", 0x3422, 0x01, 0x3422, 0x01 },
    { "3430", 0x3422, 0x01, 0x3430, 0x01 },
    { "3480", 0x3480, 0x31, 0x3480, 0x31, 0x41, 0x80 },
    { "3490", 0x3490, 0x50, 0x3490, 0x50, 0x42, 0x81 },
    { "3590", 0x3590, 0x11, 0x3590, 0x60, 0x46, 0x83 },
    { "8809", 0x8809, 0x01, 0x8809, 0x01 },
    { "9347", 0x9347, 0x01, 0x9347, 0x01 },
    { "9348", 0x9348, 0x01, 0x9348, 0x01 },
};

struct Tape3590CcwDevice {
    CcwDevice parent_obj;
    BlockBackend *blk;
    char *ident;
    const TapeIdentity *identity;
    AwsTape medium;
    uint8_t *record;
    uint32_t record_length;
    uint32_t record_offset;
    bool tray_open;
    bool write_immediate;
    uint8_t partition_id[11];
};

static const TapeIdentity *tape3590_find_identity(const char *name)
{
    size_t i;

    name = name ?: "3590";
    for (i = 0; i < ARRAY_SIZE(tape_identities); i++) {
        if (!strcmp(name, tape_identities[i].name)) {
            return &tape_identities[i];
        }
    }
    return NULL;
}

static void tape3590_cancel(SubchDev *sch)
{
    Tape3590CcwDevice *tape = sch->driver_data;

    tape->record_length = tape->record_offset = 0;
    tape->write_immediate = false;
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

static int tape3590_unit_check_era(Tape3590CcwDevice *tape, uint8_t sense,
                                   uint8_t era)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    SCHIB *schib = &sch->curr_status;

    memset(sch->sense_data, 0, sizeof(sch->sense_data));
    sch->sense_data[0] = sense;
    sch->sense_data[3] = era;
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

static int tape3590_unit_check(Tape3590CcwDevice *tape, uint8_t sense)
{
    return tape3590_unit_check_era(tape, sense, 0);
}

static int tape3590_read_result(Tape3590CcwDevice *tape,
                                AwsTapeResult result)
{
    switch (result) {
    case AWS_TAPE_EOT:
        /*
         * Hercules' AWS backend reports a header read at physical EOF as
         * EMPTYTAPE.  For a 3590 that is data check, permanent-error/OBR
         * logging in sense byte 2, and ERA 31 (tape void).
         */
        {
            int rc = tape3590_unit_check_era(tape, TAPE_SENSE_DATA_CHECK,
                                              TAPE_ERA_TAPE_VOID);

            CCW_DEVICE(tape)->sch->sense_data[2] = 0x10;
            return rc;
        }
    case AWS_TAPE_BOT:
        return tape3590_unit_check_era(tape, 0,
                                       TAPE_ERA_BACKWARD_AT_BOT);
    case AWS_TAPE_IO_ERROR:
        return tape3590_unit_check_era(tape, TAPE_SENSE_EQUIPMENT,
                                       TAPE_ERA_PHYSICAL_EOT);
    default:
        {
            int rc = tape3590_unit_check_era(tape, TAPE_SENSE_DATA_CHECK,
                                              TAPE_ERA_READ_DATA_CHECK);

            CCW_DEVICE(tape)->sch->sense_data[2] = 0x10;
            return rc;
        }
    }
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

static void tape3590_unsolicited_sense(Tape3590CcwDevice *tape, uint8_t *buf)
{
    uint16_t devno = CCW_DEVICE(tape)->sch->devno;

    /*
     * Hyperion's 3480-family format-20 drive/CU information, with the
     * 3590 software-recovery byte added by build_sense_3590().
     */
    buf[1] = 0x40; /* online */
    if (!blk_is_writable(tape->blk)) {
        buf[1] |= 0x02; /* file protected */
    }
    if (tape->medium.block_id == 0) {
        buf[1] |= 0x08; /* beginning of tape */
    }
    buf[2] = 0x20; /* reporting channel A, no log/recovery required */
    buf[7] = 0x20; /* format-20 drive and CU information */
    buf[25] = 0x06; /* IDRC installed and upgraded buffer */
    buf[27] = 0xec; /* 3490-compatible model code plus serial marker */
    buf[28] = devno >> 12;
    buf[29] = devno >> 4;
    buf[30] = (devno & 0x0f) | ((devno & 0x0f) << 4);
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
            return tape3590_read_result(tape, result);
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
                AwsTape position = tape->medium;

                result = aws_tape_read(&tape->medium, tape->record,
                                       AWS_TAPE_MAX_RECORD, &length);
                tape->medium = position;
            }
        }
        if (result != AWS_TAPE_OK && result != AWS_TAPE_MARK) {
            return tape3590_read_result(tape, result);
        }
    } while (to_mark && result != AWS_TAPE_MARK);
    return 0;
}

static int tape3590_write_result(Tape3590CcwDevice *tape,
                                 AwsTapeResult result)
{
    switch (result) {
    case AWS_TAPE_OK:
        return 0;
    case AWS_TAPE_WRITE_PROTECTED:
        return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                       TAPE_ERA_WRITE_PROTECTED);
    case AWS_TAPE_NO_SPACE:
        return tape3590_unit_check_era(tape, TAPE_SENSE_EQUIPMENT,
                                       TAPE_ERA_PHYSICAL_EOT);
    case AWS_TAPE_FENCED:
        return tape3590_unit_check_era(tape, TAPE_SENSE_EQUIPMENT |
                                       TAPE_SENSE_DEFERRED_CHECK,
                                       TAPE_ERA_VOLUME_FENCED);
    default:
        return tape3590_unit_check_era(tape, TAPE_SENSE_DATA_CHECK,
                                       TAPE_ERA_WRITE_DATA_CHECK);
    }
}

static int tape3590_write(Tape3590CcwDevice *tape, const CCW1 *ccw)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    AwsTapeResult result;

    /*
     * Hercules rejects data chaining for writes.  Besides matching that
     * behavior, keeping a record within one CCW makes overwrite-and-truncate
     * failure handling unambiguous.
     */
    if ((ccw->flags & CCW_FLAG_DC) || !ccw->count) {
        return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                       TAPE_ERA_COMMAND_REJECT);
    }
    if (ccw_dstream_read_buf(&sch->cds, tape->record, ccw->count)) {
        return -EFAULT;
    }
    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    result = aws_tape_write(&tape->medium, tape->record, ccw->count);
    if (result == AWS_TAPE_OK && tape->write_immediate) {
        result = aws_tape_sync(&tape->medium);
    }
    return tape3590_write_result(tape, result);
}

static int tape3590_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    Tape3590CcwDevice *tape = sch->driver_data;
    uint8_t buf[256] = { 0 };
    uint32_t block_id;
    AwsTapeResult result;

    if (!sch->last_cmd_valid) {
        tape->write_immediate = false;
    }
    trace_tape3590_ccw(sch->devno, ccw.cmd_code, ccw.cda, ccw.count,
                       ccw.flags, tape->medium.block_id);

    if (tape->tray_open && ccw.cmd_code != TAPE_CMD_SENSE &&
        ccw.cmd_code != TAPE_CMD_SENSE_ID) {
        return tape3590_unit_check(tape, TAPE_SENSE_INTERVENTION);
    }

    switch (ccw.cmd_code) {
    case TAPE_CMD_WRITE:
        return tape3590_write(tape, &ccw);
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
    case TAPE_CMD_ERASE_GAP:
        sch->curr_status.scsw.count = ccw.count;
        result = aws_tape_erase_gap(&tape->medium);
        if (result == AWS_TAPE_OK && tape->write_immediate) {
            result = aws_tape_sync(&tape->medium);
        }
        return tape3590_write_result(tape, result);
    case TAPE_CMD_WRITE_MARK:
        sch->curr_status.scsw.count = ccw.count;
        result = aws_tape_write_mark(&tape->medium);
        if (result == AWS_TAPE_OK && tape->write_immediate) {
            result = aws_tape_sync(&tape->medium);
        }
        return tape3590_write_result(tape, result);
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
    case TAPE_CMD_SYNCHRONIZE:
        sch->curr_status.scsw.count = ccw.count;
        return tape3590_write_result(tape, aws_tape_sync(&tape->medium));
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
        if (buffer_is_zero(sch->sense_data, 32)) {
            tape3590_unsolicited_sense(tape, buf);
        } else {
            memcpy(buf, sch->sense_data, 32);
        }
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
        return tape3590_copy_response(tape, &ccw, buf, 32);
    case TAPE_CMD_SENSE_ID:
        buf[0] = 0xff;
        stw_be_p(buf + 1, tape->identity->cu_type);
        buf[3] = tape->identity->cu_model;
        stw_be_p(buf + 4, tape->identity->dev_type);
        buf[6] = tape->identity->dev_model;
        return tape3590_copy_response(tape, &ccw, buf, 7);
    case TAPE_CMD_RDC:
        stw_be_p(buf, tape->identity->cu_type);
        buf[2] = tape->identity->cu_model;
        stw_be_p(buf + 3, tape->identity->dev_type);
        buf[5] = tape->identity->dev_model;
        /*
         * Match the A50/B1A feature words defined by the 3590 hardware
         * profile: NTP, 32-bit block IDs, SIC, channel-path NOP, logical
         * write protect, ACL and IDR; Read Forward, two-block DCE data and
         * Medium Sense in the second word.
         */
        stl_be_p(buf + 6, 0x01004ec0);
        stl_be_p(buf + 12, 0x00900400);
        stw_be_p(buf + 24, tape->identity->cu_type);
        buf[26] = tape->identity->cu_model;
        stw_be_p(buf + 27, tape->identity->dev_type);
        buf[29] = tape->identity->dev_model;
        buf[40] = tape->identity->mdr;
        buf[41] = tape->identity->obr;
        buf[42] = tape->identity->dev_type == 0x3590 ? 0x80 : 0x00;
        stl_be_p(buf + 43, AWS_TAPE_MAX_RECORD);
        stl_be_p(buf + 47, AWS_TAPE_MAX_RECORD);
        return tape3590_copy_response(tape, &ccw, buf, 64);
    case TAPE_CMD_READ_MED_CHAR:
        return tape3590_copy_response(tape, &ccw, buf, sizeof(buf));
    case TAPE_CMD_MEDIUM_SENSE:
        buf[0] = 0x01;
        return tape3590_copy_response(tape, &ccw, buf, 128);
    case TAPE_CMD_WRITE_IMMEDIATE:
        tape->write_immediate = true;
        sch->curr_status.scsw.count = ccw.count;
        return tape3590_write_result(tape, aws_tape_sync(&tape->medium));
    case TAPE_CMD_ASSIGN:
        if (ccw.count < sizeof(tape->partition_id) ||
            ccw_dstream_read_buf(&sch->cds, buf,
                                 sizeof(tape->partition_id))) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        if (!buffer_is_zero(buf, sizeof(tape->partition_id)) &&
            memcmp(buf, tape->partition_id, sizeof(tape->partition_id))) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        if (!buffer_is_zero(buf, sizeof(tape->partition_id))) {
            memcpy(tape->partition_id, buf, sizeof(tape->partition_id));
        }
        sch->curr_status.scsw.count =
            ccw_dstream_residual_count(&sch->cds);
        return 0;
    case TAPE_CMD_MODE_SET:
        if (!ccw.count || ccw_dstream_read_buf(&sch->cds, buf, 1)) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        if (buf[0] & 0x20) {
            tape->write_immediate = true;
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
    tape->write_immediate = false;
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

    tape->identity = tape3590_find_identity(tape->ident);
    if (!tape->identity) {
        error_setg(errp, "Invalid 3590 identity '%s'; expected one of "
                   "3410, 3411, 3420, 3422, 3430, 3480, 3490, 3590, "
                   "8809, 9347, 9348", tape->ident);
        return;
    }

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
    if (blk_supports_write_perm(tape->blk)) {
        perm |= BLK_PERM_WRITE | BLK_PERM_RESIZE;
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
    sch = css_create_sch_at(cdev->devno, TAPE3590_AUTO_DEVNO, errp);
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
    sch->id.cu_type = tape->identity->cu_type;
    sch->id.cu_model = tape->identity->cu_model;
    sch->id.dev_type = tape->identity->dev_type;
    sch->id.dev_model = tape->identity->dev_model;
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
    DEFINE_PROP_STRING("ident", Tape3590CcwDevice, ident),
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
