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
#include "qemu/aio.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/block-backend.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"
#include "system/runstate.h"
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
#define TAPE_CMD_SENSE_PGID       0x34
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
#define TAPE_CMD_SET_PGID         0xaf
#define TAPE_CMD_UNASSIGN         0xc7
#define TAPE_CMD_MODE_SET         0xdb
#define TAPE_CMD_SENSE_ID         0xe4

#define TAPE_SENSE_COMMAND_REJECT 0x80
#define TAPE_SENSE_INTERVENTION   0x40
#define TAPE_SENSE_EQUIPMENT      0x10
#define TAPE_SENSE_DATA_CHECK     0x08
#define TAPE_SENSE_DEFERRED_CHECK 0x02
#define TAPE_SENSE_ASSIGNED_ELSEWHERE 0x01

#define TAPE_ERA_READ_DATA_CHECK  0x23
#define TAPE_ERA_WRITE_DATA_CHECK 0x25
#define TAPE_ERA_COMMAND_REJECT   0x27
#define TAPE_ERA_UNLOAD_COMPLETE  0x2b
#define TAPE_ERA_WRITE_PROTECTED  0x30
#define TAPE_ERA_TAPE_VOID        0x31
#define TAPE_ERA_END_OF_DATA      0x36
#define TAPE_ERA_PHYSICAL_EOT     0x38
#define TAPE_ERA_BACKWARD_AT_BOT  0x39
#define TAPE_ERA_DRIVE_NOT_READY  0x43
#define TAPE_ERA_ASSIGNED_ELSEWHERE 0x45
#define TAPE_ERA_VOLUME_FENCED    0x47

/*
 * 3590 sense: byte 2 bits 0-1 basic recovery action, bytes 4-5 the
 * reason/qualifier code (the 3480/3490 sense has the channel block number
 * in bytes 4-6 instead).  The codes a 3590 reports for the conditions this
 * drive raises, as the Linux tape_3590 driver interprets them.
 */
#define TAPE_BRA_PERMANENT        0
#define TAPE_BRA_CONTINUE         1
#define TAPE_RC_BACKWARD_AT_BOP   0x3010
#define TAPE_RC_END_OF_PARTITION  0x3012
#define TAPE_RC_END_OF_DATA       0x3020
#define TAPE_RC_UNLOAD_COMPLETE   0x3123
#define TAPE_RC_NOT_READY         0x4010

/* Sense byte 1 */
#define TAPE_SENSE1_LOCATE_FAILURE 0x80
#define TAPE_SENSE1_ONLINE         0x40
#define TAPE_SENSE1_BOT            0x08
#define TAPE_SENSE1_WRITE_PROTECT  0x02

typedef struct TapeIdentity {
    const char *name;
    uint16_t cu_type;
    uint8_t cu_model;
    uint16_t dev_type;
    uint8_t dev_model;
    uint8_t mdr;
    uint8_t obr;
    /*
     * Width of the logical block number in the block ID of Read Block ID
     * and Locate Block: 22 bits on the 3480/3490 family (bits 10-31, under
     * the wrap, segment and format bits), the whole 32 bits on a 3590.
     * The drives without block IDs use the 3590 commands as well.
     */
    uint8_t block_id_bits;
} TapeIdentity;

/*
 * Hercules tapedev.c DevInitTab and tapeccws.c TapeDevtypeList define
 * these supported tape identities and model pairings.
 */
static const TapeIdentity tape_identities[] = {
    { "3410", 0x3115, 0x01, 0x3410, 0x01, 0, 0, 32 },
    { "3411", 0x3115, 0x01, 0x3411, 0x01, 0, 0, 32 },
    { "3420", 0x3803, 0x02, 0x3420, 0x06, 0, 0, 32 },
    { "3422", 0x3422, 0x01, 0x3422, 0x01, 0, 0, 32 },
    { "3430", 0x3422, 0x01, 0x3430, 0x01, 0, 0, 32 },
    { "3480", 0x3480, 0x31, 0x3480, 0x31, 0x41, 0x80, 22 },
    { "3490", 0x3490, 0x50, 0x3490, 0x50, 0x42, 0x81, 22 },
    { "3590", 0x3590, 0x11, 0x3590, 0x60, 0x46, 0x83, 32 },
    { "8809", 0x8809, 0x01, 0x8809, 0x01, 0, 0, 32 },
    { "9347", 0x9347, 0x01, 0x9347, 0x01, 0, 0, 32 },
    { "9348", 0x9348, 0x01, 0x9348, 0x01, 0, 0, 32 },
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
    /* Path group ID of the (only) channel path; zero = ungrouped */
    uint8_t pgid[11];
    /* Medium length parsed on the migration destination, or -1 */
    int64_t local_length;
    /* A migrated position on a writable medium awaits validation */
    bool restore_pending;
    VMChangeStateEntry *vmstate_change;
    /* Limits of the medium (see tape-aws.h), 0 = largest possible */
    uint64_t capacity;
    uint32_t max_blocks;
    uint64_t eot_margin;
    uint32_t eot_margin_blocks;
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

/*
 * The permissions follow the medium, as with floppies: none while the drive
 * is unloaded, so that any medium can be inserted, and writing when a loaded
 * medium allows it.  The file-protect state the guest sees (sense byte 1,
 * write-protect unit checks) follows from that.
 */
static bool tape3590_update_perm(Tape3590CcwDevice *tape, bool loaded,
                                 Error **errp)
{
    uint64_t perm = 0;

    if (loaded) {
        perm = BLK_PERM_CONSISTENT_READ;
        if (blk_supports_write_perm(tape->blk)) {
            perm |= BLK_PERM_WRITE | BLK_PERM_RESIZE;
        }
    }
    return blk_set_perm(tape->blk, perm, BLK_PERM_ALL, errp) >= 0;
}

/* Unload: the guest's Rewind Unload, a failed restore, or the tray opens */
static void tape3590_unload(Tape3590CcwDevice *tape)
{
    tape->tray_open = true;
    aws_tape_rewind(&tape->medium);
    tape3590_update_perm(tape, false, &error_abort);
}

static void tape3590_change_media(void *opaque, bool load, Error **errp)
{
    Tape3590CcwDevice *tape = opaque;

    if (load) {
        if (!blk_is_inserted(tape->blk)) {
            error_setg(errp, "cannot close 3590 tape drive without a medium");
            return;
        }
        if (!tape3590_update_perm(tape, true, errp)) {
            tape3590_update_perm(tape, false, &error_abort);
            return;
        }
        /*
         * Block I/O to removable media is unavailable while its tray is
         * reported open.  Close it before validating the AWS stream.
         */
        tape->tray_open = false;
        aws_tape_init(&tape->medium, tape->blk, errp);
        if (*errp) {
            tape3590_unload(tape);
            return;
        }
    } else {
        tape3590_unload(tape);
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
    sch->sense_data[1] = TAPE_SENSE1_ONLINE;
    if (!tape->tray_open) {
        if (!blk_is_writable(tape->blk)) {
            sch->sense_data[1] |= TAPE_SENSE1_WRITE_PROTECT;
        }
        if (!tape->medium.block_id) {
            sch->sense_data[1] |= TAPE_SENSE1_BOT;
        }
        if (tape->identity->dev_type != 0x3590) {
            /* Bytes 4-6: channel logical block number, bits 8-31 */
            sch->sense_data[4] = tape->medium.block_id >> 16;
            sch->sense_data[5] = tape->medium.block_id >> 8;
            sch->sense_data[6] = tape->medium.block_id;
        }
    }
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

/* Add the 3590 reason code and basic recovery action to the sense */
static int tape3590_sense_rc(Tape3590CcwDevice *tape, int rc,
                             uint16_t rc_rqc, uint8_t bra)
{
    uint8_t *sense = CCW_DEVICE(tape)->sch->sense_data;

    if (tape->identity->dev_type == 0x3590) {
        sense[2] = (sense[2] & 0x3f) | (bra << 6);
        sense[4] = rc_rqc >> 8;
        sense[5] = rc_rqc;
    }
    return rc;
}

static int tape3590_unit_check(Tape3590CcwDevice *tape, uint8_t sense)
{
    return tape3590_unit_check_era(tape, sense, 0);
}

static int tape3590_read_result(Tape3590CcwDevice *tape,
                                AwsTapeResult result)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    int rc;

    switch (result) {
    case AWS_TAPE_EOT:
        /*
         * The end of the recorded data.  A drive reports the end-of-data
         * mark it finds after the last block written as data check with
         * ERA 36; a medium with nothing on it is a tape void (ERA 31).
         * Permanent error, OBR logging in sense byte 2.
         */
        rc = tape3590_unit_check_era(tape, TAPE_SENSE_DATA_CHECK,
                                     tape->medium.length ?
                                     TAPE_ERA_END_OF_DATA :
                                     TAPE_ERA_TAPE_VOID);
        sch->sense_data[2] = 0x10;
        return tape3590_sense_rc(tape, rc, TAPE_RC_END_OF_DATA,
                                 TAPE_BRA_PERMANENT);
    case AWS_TAPE_BOT:
        return tape3590_sense_rc(tape,
                                 tape3590_unit_check_era(tape, 0,
                                                TAPE_ERA_BACKWARD_AT_BOT),
                                 TAPE_RC_BACKWARD_AT_BOP, TAPE_BRA_PERMANENT);
    case AWS_TAPE_IO_ERROR:
        return tape3590_unit_check_era(tape, TAPE_SENSE_EQUIPMENT,
                                       TAPE_ERA_PHYSICAL_EOT);
    default:
        rc = tape3590_unit_check_era(tape, TAPE_SENSE_DATA_CHECK,
                                     TAPE_ERA_READ_DATA_CHECK);
        sch->sense_data[2] = 0x10;
        return rc;
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

/* Is this CCW the data-chained continuation of the previous one? */
static bool tape3590_data_chained(SubchDev *sch)
{
    return sch->last_cmd_valid && (sch->last_cmd.flags & CCW_FLAG_DC);
}

static int tape3590_read(Tape3590CcwDevice *tape, const CCW1 *ccw,
                         bool previous)
{
    SubchDev *sch = CCW_DEVICE(tape)->sch;
    AwsTapeResult result;
    size_t record_length;
    uint32_t available, length;

    /*
     * A data-chained continuation goes on with the block the first CCW
     * started, also when that block is exhausted: the device has ended the
     * transfer, and the next block is never read into the same chain.
     */
    if (!tape3590_data_chained(sch)) {
        tape->record_length = tape->record_offset = 0;
        if (previous) {
            /*
             * Read Previous transfers the block before the position in
             * forward order and leaves the position in front of it.  A
             * tape mark is spaced over (unit exception), BOT is ERA 39.
             */
            result = aws_tape_backspace(&tape->medium);
            if (result == AWS_TAPE_MARK) {
                sch->curr_status.scsw.count = ccw->count;
                return tape3590_unit_exception(tape);
            }
            if (result != AWS_TAPE_OK) {
                return tape3590_read_result(tape, result);
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
        if (previous) {
            result = aws_tape_backspace(&tape->medium);
            if (result != AWS_TAPE_OK) {
                return tape3590_read_result(tape, result);
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

/*
 * Forward/Backward Space Block move over one block or tape mark, with unit
 * exception for a tape mark.  Forward/Backward Space File move over blocks
 * up to and including the next tape mark, without unit exception; Backward
 * Space File leaves the position in front of (on the BOT side of) it.
 */
static int tape3590_space(Tape3590CcwDevice *tape, bool forward,
                          bool to_mark)
{
    AwsTapeResult result;

    do {
        result = forward ? aws_tape_skip(&tape->medium) :
                           aws_tape_backspace(&tape->medium);
        if (result != AWS_TAPE_OK && result != AWS_TAPE_MARK) {
            return tape3590_read_result(tape, result);
        }
    } while (to_mark && result != AWS_TAPE_MARK);
    if (!to_mark && result == AWS_TAPE_MARK) {
        return tape3590_unit_exception(tape);
    }
    return 0;
}

static int tape3590_write_result(Tape3590CcwDevice *tape,
                                 AwsTapeResult result)
{
    switch (result) {
    case AWS_TAPE_OK:
        /*
         * Logical end of tape: a write-type command that ends at or past
         * the early-warning point completes with unit exception.  The
         * block is written; the program ends the volume.
         */
        if (aws_tape_past_warning(&tape->medium)) {
            return tape3590_unit_exception(tape);
        }
        return 0;
    case AWS_TAPE_WRITE_PROTECTED:
        return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                       TAPE_ERA_WRITE_PROTECTED);
    case AWS_TAPE_NO_SPACE:
        /*
         * Physical end of tape: the end of the volume, or no more room on
         * the host.  Nothing of the block is on the medium.
         */
        return tape3590_sense_rc(tape,
                                 tape3590_unit_check_era(tape,
                                                TAPE_SENSE_EQUIPMENT,
                                                TAPE_ERA_PHYSICAL_EOT),
                                 TAPE_RC_END_OF_PARTITION,
                                 TAPE_BRA_PERMANENT);
    case AWS_TAPE_FENCED:
        return tape3590_unit_check_era(tape, TAPE_SENSE_EQUIPMENT |
                                       TAPE_SENSE_DEFERRED_CHECK,
                                       TAPE_ERA_VOLUME_FENCED);
    case AWS_TAPE_TOO_LARGE:
        return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                       TAPE_ERA_COMMAND_REJECT);
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
     * One Write command writes one block, whose data may come from several
     * data-chained CCWs: a CCW moves at most 65535 bytes, a block can have
     * 256 KiB.  Collect the data and write the block when the last CCW of
     * the chain has delivered its part.
     */
    if (!tape3590_data_chained(sch)) {
        tape->record_length = tape->record_offset = 0;
    }
    if (!ccw->count) {
        return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                       TAPE_ERA_COMMAND_REJECT);
    }
    if (ccw->count > AWS_TAPE_MAX_RECORD - tape->record_length) {
        tape->record_length = 0;
        return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                       TAPE_ERA_COMMAND_REJECT);
    }
    if (ccw_dstream_read_buf(&sch->cds, tape->record + tape->record_length,
                             ccw->count)) {
        tape->record_length = 0;
        return -EFAULT;
    }
    tape->record_length += ccw->count;
    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    if (ccw->flags & CCW_FLAG_DC) {
        return 0;
    }
    result = aws_tape_write(&tape->medium, tape->record, tape->record_length);
    tape->record_length = 0;
    if (result == AWS_TAPE_OK && tape->write_immediate) {
        result = aws_tape_sync(&tape->medium);
    }
    return tape3590_write_result(tape, result);
}

/*
 * Commands a drive completes also when it is not ready (3490E Hardware
 * Reference, command attributes): the sense and identification commands,
 * path groups and assignment, mode setting.  The operating system brings
 * an empty drive online with them.
 */
static bool tape3590_without_medium(uint8_t cmd)
{
    switch (cmd) {
    case TAPE_CMD_SENSE:
    case TAPE_CMD_SENSE_ID:
    case TAPE_CMD_SENSE_PGID:
    case TAPE_CMD_SET_PGID:
    case TAPE_CMD_ASSIGN:
    case TAPE_CMD_UNASSIGN:
    case TAPE_CMD_RDC:
    case TAPE_CMD_MODE_SET:
    case TAPE_CMD_READ_BLOCK_ID:
    case TAPE_CMD_WRITE_IMMEDIATE:
    case TAPE_CMD_MEDIUM_SENSE:
        return true;
    default:
        return false;
    }
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

    if (tape->tray_open && !tape3590_without_medium(ccw.cmd_code)) {
        return tape3590_sense_rc(tape,
                                 tape3590_unit_check_era(tape,
                                                TAPE_SENSE_INTERVENTION,
                                                TAPE_ERA_DRIVE_NOT_READY),
                                 TAPE_RC_NOT_READY, TAPE_BRA_PERMANENT);
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
        /*
         * The drive ends Rewind Unload with unit check once the cartridge
         * is out: ERA 2B (unload complete) on the 3480/3490, reason code
         * 3123 with "continue" on the 3590.  That ends the chain; a command
         * chained to it (Linux chains a No-Operation) would find the drive
         * not ready.
         */
        tape3590_unload(tape);
        sch->curr_status.scsw.count = ccw.count;
        return tape3590_sense_rc(tape,
                                 tape3590_unit_check_era(tape, 0,
                                                TAPE_ERA_UNLOAD_COMPLETE),
                                 TAPE_RC_UNLOAD_COMPLETE, TAPE_BRA_CONTINUE);
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
        result = aws_tape_sync(&tape->medium);
        return result == AWS_TAPE_OK ? 0 : tape3590_write_result(tape, result);
    case TAPE_CMD_READ_BLOCK_ID:
        stl_be_p(buf, tape->medium.block_id);
        stl_be_p(buf + 4, tape->medium.block_id);
        return tape3590_copy_response(tape, &ccw, buf, 8);
    case TAPE_CMD_LOCATE:
        if (ccw.count < 4 || ccw_dstream_read_buf(&sch->cds, buf, 4)) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        block_id = ldl_be_p(buf);
        if (tape->identity->block_id_bits < 32) {
            block_id &= (1u << tape->identity->block_id_bits) - 1;
        }
        result = aws_tape_locate(&tape->medium, block_id);
        sch->curr_status.scsw.count = ccw.count - 4;
        if (result == AWS_TAPE_NOT_FOUND) {
            /* The block is beyond the end of the data: locate failure. */
            int rc = tape3590_unit_check_era(tape, TAPE_SENSE_DATA_CHECK,
                                             TAPE_ERA_END_OF_DATA);

            sch->sense_data[1] |= TAPE_SENSE1_LOCATE_FAILURE;
            return tape3590_sense_rc(tape, rc, TAPE_RC_END_OF_DATA,
                                     TAPE_BRA_PERMANENT);
        }
        return result == AWS_TAPE_OK ? 0 :
               tape3590_read_result(tape, result);
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
        if (tape->tray_open) {
            return 0;
        }
        result = aws_tape_sync(&tape->medium);
        return result == AWS_TAPE_OK ? 0 : tape3590_write_result(tape, result);
    case TAPE_CMD_ASSIGN:
        if (ccw.count < sizeof(tape->partition_id) ||
            ccw_dstream_read_buf(&sch->cds, buf,
                                 sizeof(tape->partition_id))) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        /*
         * The drive is assigned to the ID given (all zeros: the path's
         * own).  Assigned to another ID: assigned elsewhere (sense byte 0
         * bit 7, ERA 45).
         */
        if (!buffer_is_zero(buf, sizeof(tape->partition_id)) &&
            !buffer_is_zero(tape->partition_id,
                            sizeof(tape->partition_id)) &&
            memcmp(buf, tape->partition_id, sizeof(tape->partition_id))) {
            return tape3590_unit_check_era(tape,
                                           TAPE_SENSE_ASSIGNED_ELSEWHERE,
                                           TAPE_ERA_ASSIGNED_ELSEWHERE);
        }
        if (!buffer_is_zero(buf, sizeof(tape->partition_id))) {
            memcpy(tape->partition_id, buf, sizeof(tape->partition_id));
        }
        sch->curr_status.scsw.count =
            ccw_dstream_residual_count(&sch->cds);
        return 0;
    case TAPE_CMD_UNASSIGN:
        if (ccw.count < sizeof(tape->partition_id) ||
            ccw_dstream_read_buf(&sch->cds, buf,
                                 sizeof(tape->partition_id))) {
            return tape3590_unit_check(tape, TAPE_SENSE_COMMAND_REJECT);
        }
        memset(tape->partition_id, 0, sizeof(tape->partition_id));
        sch->curr_status.scsw.count =
            ccw_dstream_residual_count(&sch->cds);
        return 0;
    case TAPE_CMD_SENSE_PGID:
        /*
         * Byte 0: path state, bits 0-1: 10 ungrouped, 11 grouped;
         * bytes 1-11: the path group ID.
         */
        buf[0] = buffer_is_zero(tape->pgid, sizeof(tape->pgid)) ? 0x80 : 0xc0;
        memcpy(buf + 1, tape->pgid, sizeof(tape->pgid));
        return tape3590_copy_response(tape, &ccw, buf, 12);
    case TAPE_CMD_SET_PGID:
        /*
         * Byte 0: function control (bit 0 multipath mode, bits 1-2:
         * 00 establish, 01 disband, 10 resign), bytes 1-11: the ID.  A
         * grouped path accepts only its own ID again.
         */
        if (ccw.count < 12 || ccw_dstream_read_buf(&sch->cds, buf, 12)) {
            return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                           TAPE_ERA_COMMAND_REJECT);
        }
        switch (buf[0] & 0x60) {
        case 0x00:
            if (buffer_is_zero(buf + 1, sizeof(tape->pgid)) ||
                (!buffer_is_zero(tape->pgid, sizeof(tape->pgid)) &&
                 memcmp(tape->pgid, buf + 1, sizeof(tape->pgid)))) {
                return tape3590_unit_check_era(tape,
                                               TAPE_SENSE_COMMAND_REJECT,
                                               TAPE_ERA_COMMAND_REJECT);
            }
            memcpy(tape->pgid, buf + 1, sizeof(tape->pgid));
            break;
        case 0x20:
        case 0x40:
            memset(tape->pgid, 0, sizeof(tape->pgid));
            break;
        default:
            return tape3590_unit_check_era(tape, TAPE_SENSE_COMMAND_REJECT,
                                           TAPE_ERA_COMMAND_REJECT);
        }
        sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
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
    /*
     * A system reset resets the path: it leaves its path group, and the
     * assignment made through it ends.  Otherwise a re-IPLed system, with
     * a new path group ID, could not group or assign the drive again.
     */
    memset(tape->pgid, 0, sizeof(tape->pgid));
    memset(tape->partition_id, 0, sizeof(tape->partition_id));
    if (tc->parent_phases.hold) {
        tc->parent_phases.hold(obj, type);
    }
}

static void tape3590_vm_state_change(void *opaque, bool running,
                                     RunState state);

/*
 * The volume ends at the most restrictive of: the blocks the block ID of
 * the drive can number, the optional max-blocks and capacity properties
 * (which can only lower it) and whatever room the host has.  Logical end
 * of tape is reported eot-margin bytes / eot-margin-blocks blocks before
 * the first two (automatic: 128 MiB or 8192 blocks, at most an eighth).
 */
static bool tape3590_set_limits(Tape3590CcwDevice *tape, Error **errp)
{
    uint32_t limit = tape->identity->block_id_bits < 32 ?
                     AWS_TAPE_MAX_BLOCKS_22 : AWS_TAPE_MAX_BLOCKS_32;
    uint32_t max_blocks = tape->max_blocks ?: limit;

    if (max_blocks > limit) {
        error_setg(errp, "max-blocks=%" PRIu32 " exceeds the %" PRIu32
                   " blocks the block ID of a %s can address",
                   tape->max_blocks, limit, tape->identity->name);
        return false;
    }
    if (tape->capacity > INT64_MAX) {
        error_setg(errp, "capacity too large");
        return false;
    }
    if (tape->eot_margin && (!tape->capacity ||
                             tape->eot_margin >= tape->capacity)) {
        error_setg(errp, "eot-margin needs a larger capacity");
        return false;
    }
    if (tape->eot_margin_blocks >= max_blocks) {
        error_setg(errp, "eot-margin-blocks must be below the %" PRIu32
                   "-block limit", max_blocks);
        return false;
    }
    aws_tape_set_limits(&tape->medium, tape->capacity, max_blocks,
                        tape->eot_margin, tape->eot_margin_blocks);
    return true;
}

static void tape3590_realize(DeviceState *dev, Error **errp)
{
    Tape3590CcwDevice *tape = TAPE_3590_CCW(dev);
    CcwDevice *cdev = CCW_DEVICE(dev);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    SubchDev *sch;
    uint16_t chpid;
    Error *local_err = NULL;

    tape->identity = tape3590_find_identity(tape->ident);
    if (!tape->identity) {
        error_setg(errp, "Invalid 3590 identity '%s'; expected one of "
                   "3410, 3411, 3420, 3422, 3430, 3480, 3490, 3590, "
                   "8809, 9347, 9348", tape->ident);
        return;
    }
    if (!tape3590_set_limits(tape, errp)) {
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
    tape->tray_open = !blk_is_inserted(tape->blk);
    if (!tape3590_update_perm(tape, !tape->tray_open, errp)) {
        return;
    }
    if (!tape->tray_open && runstate_check(RUN_STATE_INMIGRATE) &&
        blk_is_writable(tape->blk)) {
        /*
         * The source may still be writing this medium, so its tail can be
         * an incomplete record.  The incoming state supplies the position
         * and length, which post_load checks against the medium.
         */
        tape->medium.blk = tape->blk;
        tape->medium.length = -1;
    } else if (!tape->tray_open) {
        aws_tape_init(&tape->medium, tape->blk, errp);
        if (*errp) {
            return;
        }
    }
    blk_set_dev_ops(tape->blk, &tape3590_block_ops, tape);
    tape->vmstate_change = qemu_add_vm_change_state_handler(
        tape3590_vm_state_change, tape);
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
    qemu_del_vm_change_state_handler(tape->vmstate_change);
    tape->vmstate_change = NULL;
    g_clear_pointer(&tape->record, g_free);
    aws_tape_cleanup(&tape->medium);
}

static void tape3590_unrealize(DeviceState *dev)
{
    Tape3590CcwDevice *tape = TAPE_3590_CCW(dev);

    if (tape->vmstate_change) {
        qemu_del_vm_change_state_handler(tape->vmstate_change);
        tape->vmstate_change = NULL;
    }
    g_clear_pointer(&tape->record, g_free);
    aws_tape_cleanup(&tape->medium);
}

static void tape3590_unloaded_bh(void *opaque)
{
    Tape3590CcwDevice *tape = opaque;

    if (tape->tray_open) {
        tape3590_update_perm(tape, false, &error_abort);
    }
}

static int tape3590_pre_load(void *opaque)
{
    Tape3590CcwDevice *tape = opaque;

    tape->local_length = tape->tray_open ? -1 : tape->medium.length;
    return 0;
}

static int tape3590_post_load(void *opaque, int version_id)
{
    Tape3590CcwDevice *tape = opaque;
    SubchDev *sch = CCW_DEVICE(tape)->sch;

    if (sch->id.cu_type != tape->identity->cu_type ||
        sch->id.dev_type != tape->identity->dev_type ||
        sch->id.dev_model != tape->identity->dev_model) {
        error_report("3590-ccw %04x: migrated identity %04x/%04x-%02x "
                     "differs from ident=%s", sch->devno, sch->id.cu_type,
                     sch->id.dev_type, sch->id.dev_model,
                     tape->identity->name);
        return -EINVAL;
    }

    /* Channel programs run synchronously: no buffered record survives. */
    tape->record_length = tape->record_offset = 0;
    if (tape->tray_open) {
        /*
         * Unloaded: a remount repositions the medium at load point.  The
         * permissions are dropped from the main loop: loading the state
         * runs in a coroutine, where they cannot change.
         */
        aws_tape_rewind(&tape->medium);
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                tape3590_unloaded_bh, tape);
        return 0;
    }
    if (!blk_is_inserted(tape->blk)) {
        error_report("3590-ccw %04x: a loaded tape migrated, but the drive "
                     "has no medium", sch->devno);
        return -EINVAL;
    }
    /*
     * The medium is supplied by the management layer.  A read-only volume
     * must be the same image as on the source; a writable one may have
     * grown after the destination opened it.
     */
    if (!blk_is_writable(tape->blk) &&
        tape->local_length != tape->medium.length) {
        error_report("3590-ccw %04x: mounted read-only medium has %" PRId64
                     " bytes of AWS data, the migrated one %" PRId64,
                     sch->devno, tape->local_length, tape->medium.length);
        return -EINVAL;
    }
    /*
     * Checking the position reads the medium, which post_load must not
     * do: loadvm loads the state with the block layer drained, and the
     * destination of a migration sees a medium the source wrote to only
     * once the block layer reactivates it.  Check it when the VM runs.
     */
    tape->restore_pending = true;
    return 0;
}

static void tape3590_vm_state_change(void *opaque, bool running,
                                     RunState state)
{
    Tape3590CcwDevice *tape = opaque;
    Error *local_err = NULL;

    if (!running || !tape->restore_pending) {
        return;
    }
    tape->restore_pending = false;
    if (!blk_is_inserted(tape->blk) ||
        !aws_tape_restore(&tape->medium, tape->blk, &local_err)) {
        /*
         * Present the drive unloaded rather than reading or writing at a
         * position that does not exist on this medium.
         */
        error_reportf_err(local_err, "3590-ccw %04x: migrated position "
                          "unusable, medium unloaded: ",
                          CCW_DEVICE(tape)->sch->devno);
        tape3590_unload(tape);
    }
}

/*
 * The device state is the medium position (byte offset of the next AWS
 * header, AWS data length, logical block ID, length of the chunk before the
 * position) and the drive state (tray, write-immediate mode, assigned
 * partition).  Sense data, status and the channel program live in the
 * subchannel state.  The medium itself is not part of the state: the
 * destination must mount the same AWS image (shared storage or a copy
 * made by the management layer).
 */
static bool tape3590_pgid_needed(void *opaque)
{
    Tape3590CcwDevice *tape = opaque;

    return !buffer_is_zero(tape->pgid, sizeof(tape->pgid));
}

/* Sent only while the path is grouped: older streams stay loadable. */
static const VMStateDescription vmstate_tape3590_pgid = {
    .name = "3590-ccw/pgid",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = tape3590_pgid_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pgid, Tape3590CcwDevice, 11),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_tape3590 = {
    .name = "3590-ccw",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = tape3590_pre_load,
    .post_load = tape3590_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_CCW_DEVICE(parent_obj, Tape3590CcwDevice),
        VMSTATE_BOOL(tray_open, Tape3590CcwDevice),
        VMSTATE_BOOL(write_immediate, Tape3590CcwDevice),
        VMSTATE_UINT8_ARRAY(partition_id, Tape3590CcwDevice, 11),
        VMSTATE_INT64(medium.offset, Tape3590CcwDevice),
        VMSTATE_INT64(medium.length, Tape3590CcwDevice),
        VMSTATE_UINT32(medium.block_id, Tape3590CcwDevice),
        VMSTATE_UINT16(medium.previous_length, Tape3590CcwDevice),
        VMSTATE_BOOL(medium.fenced, Tape3590CcwDevice),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_tape3590_pgid,
        NULL
    },
};

static const Property tape3590_properties[] = {
    DEFINE_PROP_DRIVE("drive", Tape3590CcwDevice, blk),
    DEFINE_PROP_STRING("ident", Tape3590CcwDevice, ident),
    DEFINE_PROP_CCW_LOADPARM("loadparm", CcwDevice, loadparm),
    DEFINE_PROP_SIZE("capacity", Tape3590CcwDevice, capacity, 0),
    DEFINE_PROP_UINT32("max-blocks", Tape3590CcwDevice, max_blocks, 0),
    DEFINE_PROP_SIZE("eot-margin", Tape3590CcwDevice, eot_margin, 0),
    DEFINE_PROP_UINT32("eot-margin-blocks", Tape3590CcwDevice,
                       eot_margin_blocks, 0),
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
