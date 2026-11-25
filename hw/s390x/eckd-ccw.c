/*
 * Emulated IBM 2107 storage control with 3390 ECKD DASD
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/s390x/ckd-image.h"
#include "hw/s390x/css-bridge.h"
#include "hw/s390x/ebcdic.h"
#include "hw/s390x/eckd-ccw.h"
#include "hw/s390x/ipl.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/coroutine.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "system/block-backend.h"
#include "system/runstate.h"
#include "trace.h"

#define ECKD_AUTO_DEVNO 0x0300
#define ECKD_CU_TYPE 0x2107
#define ECKD_CU_MODEL 0xe8
#define ECKD_DEV_TYPE 0x3390
#define ECKD_CHPID_TYPE 0x1f
#define ECKD_TRACK_SIZE 56832
#define ECKD_MAX_RECORD_SIZE (8 + UINT8_MAX + UINT16_MAX)

#define CMD_WRITE 0x05
#define CMD_READ 0x06
#define CMD_SEEK 0x07
#define CMD_DIAG_WRITE_HA 0x09
#define CMD_DIAG_READ_HA 0x0a
#define CMD_SEEK_CYLINDER 0x0b
#define CMD_WRITE_KD 0x0d
#define CMD_READ_KD 0x0e
#define CMD_ERASE 0x11
#define CMD_READ_COUNT 0x12
#define CMD_WRITE_R0 0x15
#define CMD_READ_R0 0x16
#define CMD_WRITE_HA 0x19
#define CMD_READ_HA 0x1a
#define CMD_SEEK_HEAD 0x1b
#define CMD_SET_FILE_MASK 0x1f
#define CMD_READ_SECTOR 0x22
#define CMD_SET_SECTOR 0x23
#define CMD_PSF 0x27
#define CMD_SEARCH_KEY_EQ 0x29
#define CMD_SEARCH_ID_EQ 0x31
#define CMD_SNID 0x34
#define CMD_SEARCH_HA_EQ 0x39
#define CMD_RSSD 0x3e
#define CMD_LOCATE 0x47
#define CMD_SEARCH_KEY_HIGH 0x49
#define CMD_LOCATE_EXT 0x4b
#define CMD_SUSPEND_MPR 0x5b
#define CMD_SEARCH_ID_HIGH 0x51
#define CMD_SNSS 0x54
#define CMD_SEARCH_KEY_EQ_HIGH 0x69
#define CMD_DEFINE_EXTENT 0x63
#define CMD_SEARCH_ID_EQ_HIGH 0x71
#define CMD_RDC 0x64
#define CMD_WRITE_MT 0x85
#define CMD_READ_MT 0x86
#define CMD_WRITE_KD_MT 0x8d
#define CMD_READ_KD_MT 0x8e
#define CMD_READ_COUNT_MT 0x92
#define CMD_RELEASE 0x94
#define CMD_WRITE_FULL_TRACK 0x95
#define CMD_WRITE_CKD_MT 0x9d
#define CMD_READ_CKD_MT 0x9e
#define CMD_READ_HA_MT 0x9a
#define CMD_WRITE_TRACK_DATA 0xa5
#define CMD_READ_TRACK_DATA 0xa6
#define CMD_WRITE_DATA_SPECIAL_MT 0xb5
#define CMD_READ_DATA_SPECIAL_MT 0xb6
#define CMD_SET_PGID 0xaf
#define CMD_RESERVE 0xb4
#define CMD_READ_TRACK 0xde
#define CMD_PREFIX 0xe7
#define CMD_PREFIX_READ 0xea
#define CMD_RSCK 0xf9
#define CMD_RCD 0xfa

#define SENSE_COMMAND_REJECT 0x80
#define SENSE_EQUIPMENT_CHECK 0x10
#define SENSE_DATA_CHECK 0x08
#define SENSE_OVERRUN 0x04
#define SENSE1_INVALID_TRACK 0x40
#define SENSE1_END_CYLINDER 0x20
#define SENSE1_NO_RECORD 0x08
#define SENSE1_FILE_PROTECTED 0x04
/* File mask (Define Extent byte 0 / Set File Mask) seek control */
#define FILE_MASK_SEEK_CTL        0x18
#define FILE_MASK_SEEK_ALL        0x00  /* allow all seeks and recalibrate */
#define FILE_MASK_SEEK_CYL_HEAD   0x08  /* allow seek cylinder and head */
#define FILE_MASK_SEEK_HEAD       0x10  /* allow seek head only */
#define FILE_MASK_SEEK_INHIBIT    0x18  /* inhibit seek and multitrack */
#define SENSE1_WRITE_INHIBITED 0x02

struct EckdCcwDevice {
    CcwDevice parent_obj;
    BlockBackend *blk;
    CkdImage image;

    /* Optional explicit CU/LSS topology; UINT16_MAX selects legacy mode. */
    uint16_t unit_address;
    uint16_t unit_address_limit;
    uint16_t subsystem_id;
    uint16_t interface_id;
    uint16_t lcu_queue_number;
    uint16_t path_cu_number;
    uint16_t fla;
    uint16_t chpid;

    /*
     * Identity presented in Sense ID, RDC, RCD and RSSD (properties
     * control-unit-type/-model, device-type/-model).  Default: 2107-E8
     * control unit, 3390 device; a model of 0 is derived from the type
     * (and for the device from the cylinder count).  The geometry always
     * stays the image's (3390 tracks).
     */
    uint16_t cu_type;
    uint8_t cu_model;
    uint16_t dev_type;
    uint8_t dev_model;
    /*
     * Expected cylinder count (property cylinders, 0 = not given): must
     * match the image header where it declares one, else it is the volume
     * size of an image whose length may include padding (ckd_image_open).
     */
    uint32_t cylinders;

    bool extent_defined;
    uint16_t extent_begin_cylinder;
    uint16_t extent_begin_head;
    uint16_t extent_end_cylinder;
    uint16_t extent_end_head;
    uint8_t file_mask;

    uint16_t cylinder;
    uint16_t head;
    uint16_t record;
    uint8_t locate_count;
    uint8_t locate_operation;
    uint16_t locate_length;
    bool positioned;
    bool search_match;
    bool search_count_advance;
    unsigned int search_index;
    bool search_index_seen;
    bool reserved;
    bool fenced;
    uint8_t pgid[11];
    uint8_t psf_order;
    uint8_t psf_suborder;
    uint16_t rssd_length;
    uint8_t rssd_data[512];

    uint8_t *format_track;
    uint32_t format_used;
    uint16_t format_cylinder;
    uint16_t format_head;
    bool format_active;

    uint8_t *write_record;
    uint32_t write_expected;
    uint32_t write_used;
    bool write_active;

    uint8_t update_command;
    uint8_t update_record;
    uint32_t update_offset;
    uint32_t update_expected;
    uint32_t update_used;
    bool update_active;
    /*
     * A Write Data/Write Key-Data CCW that has just completed leaves the
     * device oriented to the end of that record's data area.  A Write CKD
     * command-chained directly to it formats the track from that point:
     * this is how a Locate Record "Write Track" domain rewrites R0's data
     * and then the following records.  Valid for the next command only.
     */
    bool update_done;
    /* image.write_count when the current channel program started. */
    uint64_t program_write_count;
    uint8_t update_done_record;

    uint8_t read_command;
    uint8_t read_record;
    uint32_t read_length;
    uint32_t read_used;
    bool read_advance;
    bool read_active;

    /*
     * Asynchronous CCW execution, see eckd_ccw_cb().  The previous CCW of
     * the channel program as it was when the current CCW started: css
     * replaces sch->last_cmd with the current CCW once it is pending.
     */
    CCW1 prev_cmd;
    bool prev_cmd_valid;
    /*
     * The previous CCW transferred a record and moved on: 'record' names
     * the record that follows it, not the one the device is oriented to.
     */
    bool read_advanced;
    /*
     * The previous CCW was a Read CKD: 'record' names the record it
     * transferred completely, and the device has left that record.
     */
    bool record_consumed;
    CCW1 co_ccw;
    int co_ret;
    /* The CCW coroutine has not returned yet. */
    bool co_running;
    /* It yielded, so its result is presented by eckd_complete_bh(). */
    bool co_yielded;
    /* A CCW is executing or its completion has not been presented. */
    bool co_active;
    /* Drop the result of the executing CCW (clear, halt, reset). */
    bool co_discard;
    /* The block backend is drained: no new channel program may start. */
    bool quiesced;
    /* A channel program waits for the drained section (or a CCW) to end. */
    bool start_deferred;
    CCW1 deferred_ccw;
    VMChangeStateEntry *vmstate_change;
};

static bool eckd_has_explicit_topology(EckdCcwDevice *eckd)
{
    return eckd->unit_address != UINT16_MAX;
}

static uint8_t eckd_unit_address(EckdCcwDevice *eckd)
{
    return eckd_has_explicit_topology(eckd) ? eckd->unit_address :
           CCW_DEVICE(eckd)->sch->devno;
}

static uint16_t eckd_subsystem_id(EckdCcwDevice *eckd)
{
    return eckd_has_explicit_topology(eckd) ? eckd->subsystem_id :
           CCW_DEVICE(eckd)->sch->devno & ~0x1f;
}

static uint16_t eckd_interface_id(EckdCcwDevice *eckd)
{
    return eckd_has_explicit_topology(eckd) ? eckd->interface_id :
           (eckd_subsystem_id(eckd) >> 5) & 0x7;
}

static uint8_t eckd_model(uint32_t cylinders)
{
    if (cylinders <= 1113) {
        return 0x02;
    }
    if (cylinders <= 2226) {
        return 0x06;
    }
    if (cylinders <= 3339) {
        return 0x0a;
    }
    return 0x0c;
}

/* Sense ID model of a 3380 with @cylinders cylinders (A/D/J, E, K). */
static uint8_t eckd_model_3380(uint32_t cylinders)
{
    if (cylinders <= 885) {
        return 0x02;
    }
    if (cylinders <= 1770) {
        return 0x0a;
    }
    return 0x1e;
}

/*
 * Resolve the identity properties.  A model may be left out only where it
 * follows from the type: the 2105/2107 control units (E8), the 3990-6
 * (E9), and 3390 or 3380 devices (from the cylinder count).
 */
static bool eckd_resolve_identity(EckdCcwDevice *eckd, Error **errp)
{
    if (!eckd->cu_model) {
        switch (eckd->cu_type) {
        case 0x2105:
        case 0x2107:
            eckd->cu_model = ECKD_CU_MODEL;
            break;
        case 0x3990:
            eckd->cu_model = 0xe9;
            break;
        default:
            error_setg(errp, "eckd-ccw: control-unit-type %04x needs "
                       "control-unit-model", eckd->cu_type);
            return false;
        }
    }
    if (!eckd->dev_model) {
        switch (eckd->dev_type) {
        case 0x3390:
            eckd->dev_model = eckd_model(eckd->image.cylinders);
            break;
        case 0x3380:
            eckd->dev_model = eckd_model_3380(eckd->image.cylinders);
            break;
        default:
            error_setg(errp, "eckd-ccw: device-type %04x needs device-model",
                       eckd->dev_type);
            return false;
        }
    }
    return true;
}

static uint8_t eckd_type_code(uint32_t cylinders)
{
    if (cylinders <= 1113) {
        return 0x26;
    }
    if (cylinders <= 2226) {
        return 0x27;
    }
    if (cylinders <= 3339) {
        return 0x24;
    }
    return 0x32;
}

static int eckd_unit_check(EckdCcwDevice *eckd, uint8_t sense0,
                           uint8_t sense1)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    SCHIB *schib = &sch->curr_status;

    memset(sch->sense_data, 0, sizeof(sch->sense_data));
    sch->sense_data[0] = sense0;
    sch->sense_data[1] = sense1;
    /*
     * 3390 compatibility sense.  Byte 4 is the physical device address,
     * bytes 5-6 contain the packed cylinder/head position, and byte 7 is
     * the format/message byte (format 0, message 0 here).  Extended
     * position fields remain available in bytes 29-31.
     */
    sch->sense_data[4] = 0;
    if (eckd->image.cylinders > 4095) {
        sch->sense_data[5] = 0xff;
        sch->sense_data[6] = 0xff;
    } else {
        sch->sense_data[5] = eckd->cylinder;
        sch->sense_data[6] = ((eckd->cylinder >> 8) << 4) |
                             (eckd->head & 0x0f);
    }
    sch->sense_data[7] = 0;
    sch->sense_data[27] = 0x80;
    stw_be_p(sch->sense_data + 29, eckd->cylinder);
    sch->sense_data[31] = eckd->head;
    trace_eckd_unit_check(sch->devno, sense0, sense1, eckd->cylinder,
                          eckd->head, eckd->record,
                          eckd->image.record_count);
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

static int eckd_unit_exception(EckdCcwDevice *eckd)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    SCHIB *schib = &sch->curr_status;

    schib->scsw.dstat = SCSW_DSTAT_CHANNEL_END |
                        SCSW_DSTAT_DEVICE_END |
                        SCSW_DSTAT_UNIT_EXCEP;
    schib->scsw.ctrl &= ~SCSW_ACTL_START_PEND;
    schib->scsw.ctrl &= ~SCSW_CTRL_MASK_STCTL;
    schib->scsw.ctrl |= SCSW_STCTL_PRIMARY | SCSW_STCTL_SECONDARY |
                        SCSW_STCTL_ALERT | SCSW_STCTL_STATUS_PEND;
    schib->scsw.cpa = sch->channel_prog + 8;
    return -EIO;
}

static void eckd_set_length_status(EckdCcwDevice *eckd, const CCW1 *ccw,
                                   uint32_t transferred, uint32_t available)
{
    bool suppress = (ccw->flags & CCW_FLAG_SLI) &&
                    !(ccw->flags & CCW_FLAG_DC);
    bool continues = (ccw->flags & CCW_FLAG_DC) && transferred < available;

    if (!suppress && !continues && ccw->count != available) {
        CCW_DEVICE(eckd)->sch->curr_status.scsw.cstat |=
            SCSW_CSTAT_INCORR_LEN;
    }
}

static int eckd_read_guest(EckdCcwDevice *eckd, void *buf, size_t length)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    int ret = ccw_dstream_read_buf(&sch->cds, buf, length);

    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    return ret;
}

static int eckd_write_guest(EckdCcwDevice *eckd, const void *buf,
                            size_t length)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    int ret = ccw_dstream_write_buf(&sch->cds, (void *)buf, length);

    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    return ret;
}

static int eckd_copy_response(EckdCcwDevice *eckd, const CCW1 *ccw,
                              const void *buf, size_t available)
{
    size_t length = MIN((size_t)ccw->count, available);
    int ret = eckd_write_guest(eckd, buf, length);

    if (!ret) {
        eckd_set_length_status(eckd, ccw, length, available);
    }
    return ret;
}

static bool eckd_track_in_extent(EckdCcwDevice *eckd, uint16_t cylinder,
                                 uint16_t head)
{
    uint32_t track = (uint32_t)cylinder * eckd->image.heads + head;
    uint32_t first = (uint32_t)eckd->extent_begin_cylinder *
                     eckd->image.heads + eckd->extent_begin_head;
    uint32_t last = (uint32_t)eckd->extent_end_cylinder *
                    eckd->image.heads + eckd->extent_end_head;

    return !eckd->extent_defined || (track >= first && track <= last);
}

static bool eckd_valid_track(EckdCcwDevice *eckd, uint16_t cylinder,
                             uint16_t head)
{
    if (cylinder >= eckd->image.cylinders || head >= eckd->image.heads) {
        return false;
    }
    return eckd_track_in_extent(eckd, cylinder, head);
}

static int eckd_position(EckdCcwDevice *eckd, uint16_t cylinder,
                         uint16_t head, uint8_t record)
{
    int ret;

    if (!eckd_valid_track(eckd, cylinder, head)) {
        return eckd_unit_check(eckd, 0, SENSE1_END_CYLINDER);
    }
    ret = ckd_image_load_track(&eckd->image, cylinder, head);
    if (ret < 0) {
        return eckd_unit_check(eckd, SENSE_DATA_CHECK,
                               SENSE1_INVALID_TRACK);
    }
    eckd->cylinder = cylinder;
    eckd->head = head;
    eckd->record = record;
    eckd->positioned = true;
    eckd->search_count_advance = false;
    eckd->search_index = 0;
    eckd->search_index_seen = false;
    return 0;
}

static int eckd_next_track(EckdCcwDevice *eckd);

/*
 * Advance a multitrack operation to the next track.  Outside a Locate
 * Record domain, multitrack operation terminates at the cylinder boundary
 * with End of Cylinder instead of continuing on the next cylinder.
 */
static int eckd_next_track_mt(EckdCcwDevice *eckd)
{
    if (!eckd->locate_count) {
        /*
         * A file mask that inhibits seek and multitrack operation makes
         * the track switch itself a file-protection violation.
         */
        if ((eckd->file_mask & FILE_MASK_SEEK_CTL) == FILE_MASK_SEEK_INHIBIT) {
            return eckd_unit_check(eckd, 0, SENSE1_FILE_PROTECTED);
        }
        if (eckd->head + 1 >= eckd->image.heads) {
            return eckd_unit_check(eckd, 0, SENSE1_END_CYLINDER);
        }
    }
    return eckd_next_track(eckd);
}

/* Is a seek command permitted by the current file mask? */
static bool eckd_seek_permitted(EckdCcwDevice *eckd, uint8_t command)
{
    uint8_t control = eckd->file_mask & FILE_MASK_SEEK_CTL;

    switch (command) {
    case CMD_SEEK:
        return control == FILE_MASK_SEEK_ALL;
    case CMD_SEEK_CYLINDER:
        return control == FILE_MASK_SEEK_ALL ||
               control == FILE_MASK_SEEK_CYL_HEAD;
    case CMD_SEEK_HEAD:
        return control != FILE_MASK_SEEK_INHIBIT;
    default:
        return true;
    }
}

static int eckd_next_track(EckdCcwDevice *eckd)
{
    uint16_t cylinder = eckd->cylinder;
    uint16_t head = eckd->head + 1;

    if (head == eckd->image.heads) {
        head = 0;
        cylinder++;
    }
    if (cylinder >= eckd->image.cylinders) {
        return eckd_unit_check(eckd, 0, SENSE1_END_CYLINDER);
    }
    if (!eckd_track_in_extent(eckd, cylinder, head)) {
        return eckd_unit_check(eckd, 0, SENSE1_FILE_PROTECTED);
    }
    return eckd_position(eckd, cylinder, head, 0);
}

static const CkdImageRecord *eckd_first_data_record(const CkdImage *image)
{
    unsigned int i;

    for (i = 0; i < image->record_count; i++) {
        if (image->records[i].number != 0) {
            return &image->records[i];
        }
    }
    return NULL;
}

static const CkdImageRecord *eckd_next_record(const CkdImage *image,
                                               const CkdImageRecord *record)
{
    unsigned int i = record - image->records + 1;

    for (; i < image->record_count; i++) {
        if (image->records[i].number != 0) {
            return &image->records[i];
        }
    }
    return NULL;
}

static const CkdImageRecord *eckd_find_cchhr(const CkdImage *image,
                                              const uint8_t *cchhr)
{
    unsigned int i;

    for (i = 0; i < image->record_count; i++) {
        const CkdImageRecord *record = &image->records[i];

        if (!memcmp(image->track + record->header_offset, cchhr, 5)) {
            return record;
        }
    }
    return NULL;
}

static const CkdImageRecord *eckd_current_record(EckdCcwDevice *eckd,
                                                 bool multitrack)
{
    const CkdImageRecord *record;

    record = eckd->record <= UINT8_MAX ?
             ckd_image_find_record(&eckd->image, eckd->record) : NULL;
    while (!record && multitrack) {
        /*
         * Outside a Locate Record domain, multitrack operation terminates
         * at the cylinder boundary.  VSE's VTOC search distinguishes this
         * End-of-Cylinder indication from No-Record-Found.
         *
         * A CKD track need not contain any user records.  Multitrack read
         * commands skip such tracks and continue looking for record 1,
         * just as a real count-field scan (and Hercules ckd_read_count())
         * does.  Trying only the immediately following track incorrectly
         * reported No Record Found when z/OS crossed a run of empty tracks.
         */
        if (eckd_next_track_mt(eckd)) {
            break;
        }
        record = eckd_first_data_record(&eckd->image);
        if (record) {
            eckd->record = record->number;
        }
    }
    return record;
}

/*
 * Reject a command, order or mode that this model does not implement.  Each
 * rejection is traced explicitly: guests probe optional storage-control
 * functions, and an unimplemented one is a candidate for behaviour that
 * differs from real hardware.
 */
static int eckd_reject_unimplemented(EckdCcwDevice *eckd, const char *what,
                                     uint8_t code, uint8_t modifier)
{
    trace_eckd_unimplemented(CCW_DEVICE(eckd)->sch->devno, what, code,
                             modifier);
    return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
}

static int eckd_no_record(EckdCcwDevice *eckd)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;

    if (sch->curr_status.scsw.dstat & SCSW_DSTAT_UNIT_CHECK) {
        return -EIO;
    }
    return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
}

static void eckd_advance_record(EckdCcwDevice *eckd,
                                const CkdImageRecord *record)
{
    const CkdImageRecord *next = eckd_next_record(&eckd->image, record);

    /*
     * Record identifiers are labels, not a physical ordinal.  Advance to the
     * next count field in track order; CKD tracks commonly leave gaps in the
     * record numbers.  A value that cannot name a real record makes the next
     * multitrack operation advance to the following track.
     */
    eckd->record = next ? next->number : UINT8_MAX + 1;
    if (eckd->locate_count) {
        eckd->locate_count--;
    }
}

static void eckd_build_sense_id(EckdCcwDevice *eckd, uint8_t *buf)
{
    static const uint32_t ciws[] = {
        0x40fa0100, 0x41270004, 0x423e01a0, 0x433e0008,
    };
    unsigned int i;

    memset(buf, 0, 24);
    buf[0] = 0xff;
    stw_be_p(buf + 1, eckd->cu_type);
    buf[3] = eckd->cu_model;
    stw_be_p(buf + 4, eckd->dev_type);
    buf[6] = eckd->dev_model;
    for (i = 0; i < ARRAY_SIZE(ciws); i++) {
        stl_be_p(buf + 8 + i * 4, ciws[i]);
    }
}

static void eckd_build_rdc(EckdCcwDevice *eckd, uint8_t *buf)
{
    uint8_t code = eckd_type_code(eckd->image.cylinders);

    memset(buf, 0, 64);
    stw_be_p(buf, eckd->cu_type);
    buf[2] = eckd->cu_model;
    stw_be_p(buf + 3, eckd->dev_type);
    buf[5] = eckd->dev_model;
    stl_be_p(buf + 6, 0x528010b7);
    buf[10] = 0x20;
    buf[11] = code;
    stw_be_p(buf + 12, eckd->image.cylinders);
    stw_be_p(buf + 14, 15);
    buf[16] = 224;
    stw_be_p(buf + 18, 58786);
    stw_be_p(buf + 20, 1428);
    buf[22] = 2;
    buf[23] = 34;
    buf[24] = 19;
    buf[25] = 9;
    buf[26] = 6;
    buf[27] = 116;
    buf[40] = code;
    buf[41] = code;
    buf[42] = 0x1f;
    buf[43] = 2;
    stw_be_p(buf + 44, 57326);
    buf[47] = 1;
    buf[48] = 6;
    stw_be_p(buf + 49, 0x7708);
    buf[54] = 0x48;
    buf[57] = 0xff;
    stl_be_p(buf + 60, eckd->image.cylinders);
}

static void eckd_ned_text(uint8_t *dest, const char *ascii, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++) {
        unsigned char ch = ascii[i];
        dest[i] = ascii2ebcdic[g_ascii_isalnum(ch) ? ch : '0'];
    }
}

static void eckd_build_rcd(EckdCcwDevice *eckd, uint8_t *buf)
{
    CcwDevice *cdev = CCW_DEVICE(eckd);
    bool explicit_topology = eckd_has_explicit_topology(eckd);
    uint16_t lss = eckd_subsystem_id(eckd);
    uint16_t interface_id = eckd_interface_id(eckd);
    uint8_t unit_address = eckd_unit_address(eckd);
    char text[33] = { 0 };

    memset(buf, 0, 256);
    stl_be_p(buf, 0xdc010100);
    if (explicit_topology) {
        snprintf(text, sizeof(text), "002107900IBM75%012X",
                 (unsigned int)lss);
    } else {
        snprintf(text, sizeof(text), "  %04X0%02XHRCZZ",
                 eckd->dev_type, eckd->dev_model);
        memcpy(text + 14, eckd->image.serial, 12);
    }
    eckd_ned_text(buf + 4, text, 26);
    stw_be_p(buf + 30, explicit_topology ? unit_address :
             cdev->sch->devno);

    stl_be_p(buf + 32, 0xd4020000);
    if (explicit_topology) {
        snprintf(text, sizeof(text), "002107986IBM75%012X",
                 (unsigned int)lss);
    } else {
        snprintf(text, sizeof(text), "  %04X0%02XHRCZZ000000000003",
                 eckd->dev_type, eckd->dev_model);
    }
    eckd_ned_text(buf + 36, text, 26);

    stl_be_p(buf + 64, 0xd0000000);
    if (explicit_topology) {
        snprintf(text, sizeof(text), "002107986IBM75%012X",
                 (unsigned int)lss);
    } else {
        snprintf(text, sizeof(text), "  %04X0%02XHRCZZ000000000002",
                 eckd->cu_type, eckd->cu_model);
    }
    eckd_ned_text(buf + 68, text, 26);
    if (!explicit_topology) {
        buf[95] = cdev->sch->devno >> 8;
    }

    stl_be_p(buf + 96, 0xf0000001);
    if (explicit_topology) {
        snprintf(text, sizeof(text), "002107900IBM75%012X",
                 (unsigned int)lss);
    } else {
        snprintf(text, sizeof(text), "  %04X   HRCZZ000000000001",
                 eckd->cu_type);
    }
    eckd_ned_text(buf + 100, text, 26);

    if (explicit_topology) {
        static const uint8_t neq_template[32] = {
            0x80, 0x00, 0x02, 0x13, 0x2d, 0x00, 0x1e, 0x00,
            0x00, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x01, 0x0c, 0xc0, 0x00, 0x4f, 0xe5, 0x64, 0x1b,
            0x02, 0x13, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x00,
        };

        /*
         * Keep the interface identifier distinct from the storage-access
         * identifier.  IBM 2107-compatible RCD data uses a 16-bit interface
         * identifier and leaves the latter qualifier zero.
         */
        memcpy(buf + 224, neq_template, sizeof(neq_template));
        stw_be_p(buf + 226, interface_id);
        stw_be_p(buf + 232, lss);
    } else {
        buf[224] = 0x80;
        stw_be_p(buf + 226, interface_id);
        buf[230] = 0x1e;
        stw_be_p(buf + 232, lss);
        buf[234] = 0x80;
        buf[235] = unit_address;
        buf[236] = unit_address;
        buf[237] = unit_address;
        buf[238] = interface_id;
        buf[241] = 0x80;
        buf[242] = 0x80;
        buf[243] = unit_address;
    }
}

static int eckd_perform_subsystem_function(EckdCcwDevice *eckd,
                                           const uint8_t *buf,
                                           uint32_t length)
{
    uint8_t *data = eckd->rssd_data;
    char node_id[27];

    eckd->rssd_length = 0;
    eckd->psf_order = buf[0];
    eckd->psf_suborder = length > 6 ? buf[6] : 0;

    switch (eckd->psf_order) {
    case 0x18: /* Prepare for Read Subsystem Data */
        if (length < 12 || buf[1]) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        switch (eckd->psf_suborder) {
        case 0x00: /* Storage path status */
            memset(data, 0, 16);
            data[0] = 0xc0; /* storage path valid and attached */
            data[1] = 0x80; /* one logical path configured */
            eckd->rssd_length = 16;
            break;
        case 0x01: /* Subsystem statistics */
            if (buf[7] != 0x00 && buf[7] != 0xff) {
                return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
            }
            eckd->rssd_length = buf[8] ? 192 : 96;
            memset(data, 0, eckd->rssd_length);
            data[1] = eckd_unit_address(eckd);
            break;
        case 0x03: /* Read attention message: no message pending */
            memset(data, 0, 9);
            data[1] = 9;
            memcpy(data + 4, buf + 8, 4);
            eckd->rssd_length = 9;
            break;
        case 0x0e: /* Unit address configuration */
            memset(data, 0, sizeof(eckd->rssd_data));
            eckd->rssd_length = sizeof(eckd->rssd_data);
            break;
        case 0x1c: /* Query host access */
            memset(data, 0, 52);
            data[17] = 0x20;
            data[19] = 0x01;
            eckd->rssd_length = 52;
            break;
        case 0x41: /* Feature codes */
            memset(data, 0, 256);
            eckd->rssd_length = 256;
            break;
        default:
            return eckd_reject_unimplemented(eckd, "psf-prsd-suborder",
                                             eckd->psf_order,
                                             eckd->psf_suborder);
        }
        return 0;
    case 0xb0: /* Set Interface Identifier */
        if (length < 4 || (buf[1] & 0xfe)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        /*
         * Return the subsystem node descriptor and qualifier records.
         * This is the same architected compatibility response used by
         * Hercules for an emulated 2107.
         */
        memset(data, 0, 96);
        stl_be_p(data, 0x00000100);
        snprintf(node_id, sizeof(node_id), "00%04X   HRCZZ000000000001",
                 eckd->cu_type);
        ebcdic_put(data + 4, node_id, 26);
        stl_be_p(data + 40, 0x41010000);
        stl_be_p(data + 44, 0x41010001);
        stl_be_p(data + 48, 0x41010010);
        stl_be_p(data + 52, 0x41010011);
        eckd->rssd_length = (buf[1] & 0x01) ? 32 : 96;
        return 0;
    case 0x1b: /* Set Special Intercept Condition */
    case 0x1d: /* Set Subsystem Characteristics */
        /* Accepted without effect: also a candidate for divergence. */
        trace_eckd_accepted_ignored(CCW_DEVICE(eckd)->sch->devno, "psf-order",
                                    eckd->psf_order);
        return 0;
    default:
        return eckd_reject_unimplemented(eckd, "psf-order", eckd->psf_order,
                                         eckd->psf_suborder);
    }
}

static int eckd_define_extent(EckdCcwDevice *eckd, const CCW1 *ccw,
                              const uint8_t *buf, size_t length)
{
    if (length < 16) {
        return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
    }
    eckd->file_mask = buf[0];
    eckd->extent_begin_cylinder = lduw_be_p(buf + 8);
    eckd->extent_begin_head = lduw_be_p(buf + 10);
    eckd->extent_end_cylinder = lduw_be_p(buf + 12);
    eckd->extent_end_head = lduw_be_p(buf + 14);
    eckd->extent_defined = true;
    CCW_DEVICE(eckd)->sch->curr_status.scsw.count = ccw->count - length;
    return 0;
}

static int eckd_locate(EckdCcwDevice *eckd, const CCW1 *ccw,
                       const uint8_t *buf, size_t length, bool extended)
{
    const CkdImageRecord *record;
    uint16_t cylinder;
    uint16_t head;
    uint8_t orientation;
    uint8_t operation;

    if (length < 16) {
        return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
    }
    operation = buf[0] & 0x3f;
    orientation = buf[0] & 0xc0;
    eckd->locate_operation = operation;
    eckd->locate_count = buf[3];
    cylinder = lduw_be_p(buf + 4);
    head = lduw_be_p(buf + 6);
    eckd->record = buf[12];
    eckd->locate_length = lduw_be_p(buf + 14);
    if (operation == 0x03 || operation == 0x0b) {
        /*
         * A new Format Write or Write Track domain replaces the track tail
         * following the locate argument (for Write Track, following the
         * record updated by the domain's first Write Data).  The first
         * Write CKD will preserve that prefix.
         */
        eckd->format_active = false;
    }
    trace_eckd_locate(CCW_DEVICE(eckd)->sch->devno, operation,
                      eckd->locate_count, cylinder, head, eckd->record,
                      eckd->locate_length);
    CCW_DEVICE(eckd)->sch->curr_status.scsw.count = ccw->count - length;
    if (eckd_position(eckd, cylinder, head, eckd->record)) {
        return -EIO;
    }
    /*
     * Count orientation leaves the device at the selected count field.
     * A following Read Data therefore consumes the selected record, while a
     * following Read Count must advance to the next count field first.
     * Data orientation has already passed the selected record, so the next
     * data/key-data command operates on its successor.  Hyperion models all
     * of this with ckdorient/ckdcurrec; retain the equivalent distinction in
     * search_count_advance instead of treating both cases as merely a record
     * number.
     */
    if (orientation == 0x00 || orientation == 0x80) {
        /*
         * The seek address chooses a track, while the search argument names
         * a physical count field on that track.  Record numbers are labels,
         * so matching only R can silently select data from the wrong CCHH.
         */
        record = eckd_find_cchhr(&eckd->image, buf + 8);
        if (!record) {
            return eckd_no_record(eckd);
        }
        eckd->record = record->number;
    }
    if (orientation == 0x00) {
        eckd->search_count_advance = true;
    } else if (orientation == 0x80) {
        record = eckd_next_record(&eckd->image, record);
        eckd->record = record ? record->number : UINT8_MAX + 1;
    }
    return 0;
}

static int eckd_prefix(EckdCcwDevice *eckd, const CCW1 *ccw,
                       uint8_t *buf, size_t length)
{
    int ret;

    if (length < 44 || buf[0] > 1) {
        return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
    }
    if (buf[1] & 0x80) {
        ret = eckd_define_extent(eckd, ccw, buf + 12, 16);
        if (ret) {
            return ret;
        }
    }
    if (buf[0] == 1) {
        if (length < 64) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        return eckd_locate(eckd, ccw, buf + 44, length - 44, true);
    }
    CCW_DEVICE(eckd)->sch->curr_status.scsw.count = ccw->count - length;
    return 0;
}

static int eckd_read_record(EckdCcwDevice *eckd, const CCW1 *ccw,
                            uint8_t command, bool advance)
{
    bool continuation = eckd->read_active && eckd->prev_cmd_valid &&
                        (eckd->prev_cmd.flags & CCW_FLAG_DC);
    const CkdImageRecord *record;
    const uint8_t *data;
    uint32_t transferred;
    uint32_t length;

    if (continuation) {
        command = eckd->read_command;
        advance = eckd->read_advance;
        record = ckd_image_find_record(&eckd->image, eckd->read_record);
    } else {
        eckd->read_active = false;
        eckd->read_used = 0;
        record = eckd_current_record(eckd, command & 0x80);
    }
    if (!record) {
        return eckd_no_record(eckd);
    }
    switch (command & 0x7f) {
    case CMD_READ:
        data = eckd->image.track + record->data_offset;
        length = record->data_length;
        break;
    case CMD_READ_KD:
        data = eckd->image.track + record->key_offset;
        length = record->key_length + record->data_length;
        break;
    default:
        data = eckd->image.track + record->header_offset;
        length = 8 + record->key_length + record->data_length;
        break;
    }
    if (!continuation) {
        eckd->read_command = command;
        eckd->read_record = record->number;
        eckd->read_length = length;
        eckd->read_advance = advance;
    } else {
        if (eckd->read_used > length) {
            eckd->read_active = false;
            return eckd_unit_check(eckd, SENSE_DATA_CHECK, 0);
        }
        data += eckd->read_used;
        length -= eckd->read_used;
    }
    trace_eckd_read(CCW_DEVICE(eckd)->sch->devno, command, eckd->cylinder,
                    eckd->head, record->number, length, ccw->count);
    if (eckd_copy_response(eckd, ccw, data, length)) {
        eckd->read_active = false;
        return -EFAULT;
    }
    transferred = MIN((uint32_t)ccw->count, length);
    eckd->read_used += transferred;
    if (ccw->flags & CCW_FLAG_DC) {
        /*
         * Data chaining preserves orientation even when this CCW exactly
         * exhausts the record.  A following data-chained CCW must then see
         * no data and retain its full residual; it must not start reading
         * the next physical record.
         */
        eckd->read_active = true;
        return 0;
    }
    eckd->read_active = false;
    if (advance) {
        eckd_advance_record(eckd, record);
        eckd->read_advanced = true;
    } else {
        eckd->record_consumed = true;
        if (eckd->locate_count) {
            eckd->locate_count--;
        }
    }
    eckd->search_count_advance = false;
    /*
     * A data transfer from a CKD record whose data length is zero completes
     * with Channel End, Device End and Unit Exception.  In particular,
     * z/OS uses Read Count followed by Read Data while scanning a VTOC and
     * relies on Unit Exception to distinguish an empty record from a
     * successful short transfer.  Returning only CE+DE leaves the full
     * residual count with normal status and makes IOS retry indefinitely.
     *
     * Hercules applies this rule to Read IPL, Read Data, Read Key and Data,
     * and the count/key/data variants after consuming the record.
     */
    if (!record->data_length) {
        return eckd_unit_exception(eckd);
    }
    return 0;
}

static int eckd_write_existing(EckdCcwDevice *eckd, const CCW1 *ccw,
                               uint8_t command)
{
    bool continuation = eckd->update_active && eckd->prev_cmd_valid &&
                        (eckd->prev_cmd.flags & CCW_FLAG_DC);
    const CkdImageRecord *record;
    uint32_t offset;
    uint32_t expected;
    uint32_t remaining;
    uint32_t length;
    int ret;

    if (eckd->fenced) {
        return eckd_unit_check(eckd, SENSE_EQUIPMENT_CHECK, 0);
    }
    if (!blk_is_writable(eckd->blk)) {
        return eckd_unit_check(eckd, 0, SENSE1_WRITE_INHIBITED);
    }
    if (continuation) {
        command = eckd->update_command;
        record = ckd_image_find_record(&eckd->image,
                                       eckd->update_record);
        offset = eckd->update_offset;
        expected = eckd->update_expected;
    } else {
        eckd->update_active = false;
        eckd->write_active = false;
        record = eckd_current_record(eckd, command & 0x80);
        if (record) {
            if ((command & 0x7f) == CMD_WRITE) {
                offset = record->data_offset;
                expected = record->data_length;
            } else {
                offset = record->key_offset;
                expected = record->key_length + record->data_length;
            }
            memcpy(eckd->write_record, eckd->image.track,
                   eckd->image.used_length);
            eckd->update_command = command;
            eckd->update_record = record->number;
            eckd->update_offset = offset;
            eckd->update_expected = expected;
            eckd->update_used = 0;
        }
    }
    if (!record) {
        eckd->update_active = false;
        return eckd_no_record(eckd);
    }
    if (!record->data_length) {
        eckd->update_active = false;
        return eckd_unit_exception(eckd);
    }
    if (eckd->update_used > expected) {
        eckd->update_active = false;
        return eckd_unit_check(eckd, SENSE_DATA_CHECK, 0);
    }
    remaining = expected - eckd->update_used;
    length = MIN((uint32_t)ccw->count, remaining);
    ret = eckd_read_guest(eckd,
                          eckd->write_record + offset + eckd->update_used,
                          length);
    if (ret) {
        eckd->update_active = false;
        return ret;
    }
    eckd->update_used += length;
    if (ccw->flags & CCW_FLAG_DC) {
        eckd->update_active = true;
        return 0;
    }
    eckd->update_active = false;
    if (ccw->count < remaining) {
        eckd_set_length_status(eckd, ccw, length, remaining);
    }
    if (eckd->update_used < expected) {
        /*
         * CKD update writes pad a short data transfer with zeroes.  This is
         * how ckd_write_data()/ckd_write_kd() behave in Hercules and is used
         * by VSE when rewriting fixed-size library blocks.
         */
        memset(eckd->write_record + offset + eckd->update_used, 0,
               expected - eckd->update_used);
    }
    ret = ckd_image_store_track(&eckd->image, eckd->cylinder,
                                eckd->head, eckd->write_record,
                                eckd->image.used_length);
    if (ret < 0) {
        eckd->fenced = true;
        return eckd_unit_check(eckd, SENSE_EQUIPMENT_CHECK, 0);
    }
    eckd->update_done = true;
    eckd->update_done_record = eckd->update_record;
    eckd_advance_record(eckd, record);
    return 0;
}

static void eckd_format_begin(EckdCcwDevice *eckd)
{
    eckd->format_cylinder = eckd->cylinder;
    eckd->format_head = eckd->head;
    eckd->format_track[0] = 0;
    stw_be_p(eckd->format_track + 1, eckd->cylinder);
    stw_be_p(eckd->format_track + 3, eckd->head);
    stw_be_p(eckd->format_track + 5, eckd->cylinder);
    stw_be_p(eckd->format_track + 7, eckd->head);
    eckd->format_track[9] = 0;
    eckd->format_track[10] = 0;
    stw_be_p(eckd->format_track + 11, 8);
    memset(eckd->format_track + 13, 0, 8);
    eckd->format_used = 21;
    eckd->format_active = true;
}

/*
 * Start a format-write domain at the current orientation.  Writing a new
 * count field replaces the old tail of the track; it does not append after
 * records which physically followed the locate argument.
 */
static int eckd_format_begin_after(EckdCcwDevice *eckd, uint8_t record_number)
{
    const CkdImageRecord *record =
        ckd_image_find_record(&eckd->image, record_number);

    if (!record) {
        return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
    }
    eckd->format_cylinder = eckd->cylinder;
    eckd->format_head = eckd->head;
    eckd->format_used = record->data_offset + record->data_length;
    memcpy(eckd->format_track, eckd->image.track, eckd->format_used);
    eckd->format_active = true;
    return 0;
}

static int eckd_format_commit(EckdCcwDevice *eckd)
{
    int ret;

    memset(eckd->format_track + eckd->format_used, 0xff, 8);
    ret = ckd_image_store_track(&eckd->image, eckd->format_cylinder,
                                eckd->format_head, eckd->format_track,
                                eckd->format_used + 8);
    if (ret < 0) {
        eckd->fenced = true;
        return eckd_unit_check(eckd, SENSE_EQUIPMENT_CHECK, 0);
    }
    return 0;
}

static int eckd_write_r0(EckdCcwDevice *eckd, const CCW1 *ccw)
{
    uint8_t data[8] = { 0 };

    if (!blk_is_writable(eckd->blk)) {
        return eckd_unit_check(eckd, 0, SENSE1_WRITE_INHIBITED);
    }
    if (ccw->count != sizeof(data) ||
        eckd_read_guest(eckd, data, sizeof(data))) {
        return eckd_unit_check(eckd, SENSE_OVERRUN, 0);
    }
    eckd_format_begin(eckd);
    memcpy(eckd->format_track + 13, data, 8);
    eckd->record = 1;
    return eckd_format_commit(eckd);
}

static int eckd_write_ckd(EckdCcwDevice *eckd, const CCW1 *ccw)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    bool continuation = eckd->write_active && eckd->prev_cmd_valid &&
                        (eckd->prev_cmd.flags & CCW_FLAG_DC);
    uint8_t *data = eckd->write_record;
    uint16_t cylinder;
    uint16_t head;
    uint32_t required;
    uint32_t length;
    uint32_t padding;
    bool overlength = false;
    g_autofree uint8_t *discard = NULL;
    int ret;

    if (!blk_is_writable(eckd->blk)) {
        return eckd_unit_check(eckd, 0, SENSE1_WRITE_INHIBITED);
    }
    if (!continuation) {
        eckd->write_active = false;
        eckd->read_active = false;
        eckd->write_used = 0;
        if (ccw->count < 8) {
            return eckd_unit_check(eckd, SENSE_OVERRUN, 0);
        }
        /*
         * The count field describes the physical record length.  As in
         * Hercules, transfer no more than that length and leave any excess
         * CCW count as residual data instead of reporting a device data
         * check.  VSE uses such an overlength Write CKD while initializing
         * fresh volumes.
         */
        if (eckd_read_guest(eckd, data, 8)) {
            return -EFAULT;
        }
        eckd->write_expected = 8 + data[5] + lduw_be_p(data + 6);
        length = MIN((uint32_t)ccw->count, eckd->write_expected);
        if (length > 8 &&
            eckd_read_guest(eckd, data + 8, length - 8)) {
            return -EFAULT;
        }
        eckd->write_used = length;
        overlength = ccw->count > eckd->write_expected;
    } else {
        trace_eckd_write_chain(sch->devno, true, eckd->write_expected,
                               eckd->write_used, ccw->count, ccw->flags);
        length = MIN((uint32_t)ccw->count,
                     eckd->write_expected - eckd->write_used);
        if (length &&
            eckd_read_guest(eckd, data + eckd->write_used, length)) {
            eckd->write_active = false;
            return -EFAULT;
        }
        eckd->write_used += length;
        padding = ccw->count - length;
        if (padding) {
            /*
             * VSE can append IDA data-chain padding after a complete CKD
             * count field (notably for a zero-length record).  Consume the
             * continuation so the channel program advances, but do not make
             * those bytes part of the count-declared physical record.
             * Hercules rejects non-read data chaining altogether, so this
             * is a required extension rather than a behavior to copy.
             */
            discard = g_malloc(padding);
            if (eckd_read_guest(eckd, discard, padding)) {
                eckd->write_active = false;
                return -EFAULT;
            }
        }
    }
    trace_eckd_write_chain(sch->devno, continuation, eckd->write_expected,
                           eckd->write_used, ccw->count, ccw->flags);
    required = eckd->write_expected;
    if (eckd->write_used > required) {
        eckd->write_active = false;
        return eckd_unit_check(eckd, SENSE_DATA_CHECK, 0);
    }
    if (ccw->flags & CCW_FLAG_DC) {
        eckd->write_active = true;
        return 0;
    }
    eckd->write_active = false;
    eckd->read_active = false;
    if (overlength && !(ccw->flags & CCW_FLAG_SLI)) {
        sch->curr_status.scsw.cstat |= SCSW_CSTAT_INCORR_LEN;
    }
    if (eckd->write_used < required) {
        /*
         * A short Write CKD is valid: the unwritten key/data portion is
         * padded with zeroes.  VSE uses this when initializing tracks by
         * supplying only the eight-byte count field.  Hercules implements
         * the same rule in ckd_write_ckd().
         */
        memset(data + eckd->write_used, 0, required - eckd->write_used);
    }

    cylinder = lduw_be_p(data);
    head = lduw_be_p(data + 2);
    if (cylinder != eckd->cylinder || head != eckd->head) {
        uint32_t current = eckd->cylinder * eckd->image.heads + eckd->head;
        uint32_t incoming = cylinder * eckd->image.heads + head;

        if (ccw->cmd_code != CMD_WRITE_CKD_MT || incoming != current + 1) {
            return eckd_unit_check(eckd, SENSE_DATA_CHECK, 0);
        }
        ret = eckd_position(eckd, cylinder, head, 0);
        if (ret) {
            return ret;
        }
    }
    if (!eckd->format_active ||
        eckd->format_cylinder != eckd->cylinder ||
        eckd->format_head != eckd->head) {
        ret = eckd_format_begin_after(eckd, eckd->record);
        if (ret) {
            return ret;
        }
    }
    trace_eckd_write_ckd(sch->devno, cylinder, head, data[4], data[5],
                         lduw_be_p(data + 6), eckd->write_used,
                         eckd->format_used);
    if (data[4] != eckd->record + 1) {
        return eckd_unit_check(eckd, SENSE_DATA_CHECK, 0);
    }
    if (eckd->format_used + required + 8 > ECKD_TRACK_SIZE) {
        return eckd_unit_check(eckd, SENSE_OVERRUN, 0);
    }
    memcpy(eckd->format_track + eckd->format_used, data, required);
    eckd->format_used += required;
    eckd->record = data[4];
    return eckd_format_commit(eckd);
}

static int eckd_read_track(EckdCcwDevice *eckd, const CCW1 *ccw)
{
    int ret;

    ret = ckd_image_load_track(&eckd->image, eckd->cylinder, eckd->head);
    if (ret < 0) {
        return eckd_unit_check(eckd, SENSE_DATA_CHECK,
                               SENSE1_INVALID_TRACK);
    }
    /* Read Track starts with R0's count field; the home address is not
     * part of the transferred track data. */
    return eckd_copy_response(eckd, ccw, eckd->image.track + 5,
                              eckd->image.used_length - 5);
}

static int eckd_sense(EckdCcwDevice *eckd, const CCW1 *ccw)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    uint8_t sense[32];

    memcpy(sense, sch->sense_data, sizeof(sense));
    memset(sch->sense_data, 0, sizeof(sch->sense_data));
    return eckd_copy_response(eckd, ccw, sense, sizeof(sense));
}

static int eckd_ccw_cmd(SubchDev *sch, CCW1 ccw)
{
    EckdCcwDevice *eckd = sch->driver_data;
    uint8_t buf[256] = { 0 };
    uint32_t length;
    bool update_done = eckd->update_done;
    bool read_advanced = eckd->read_advanced;
    bool record_consumed = eckd->record_consumed;
    int ret;

    eckd->update_done = false;
    eckd->read_advanced = false;
    eckd->record_consumed = false;
    if (!eckd->prev_cmd_valid) {
        /*
         * Define Extent, Locate Record and format-write domains belong to
         * one channel program.  Retaining the final formatting extent into
         * a later START SUBCHANNEL incorrectly rejects ordinary seeks.
         */
        eckd->extent_defined = false;
        eckd->file_mask = 0;
        eckd->locate_count = 0;
        eckd->locate_operation = 0;
        eckd->format_active = false;
        eckd->search_match = false;
        eckd->search_count_advance = false;
        eckd->search_index = 0;
        eckd->search_index_seen = false;
        eckd->write_active = false;
        eckd->update_active = false;
        eckd->read_active = false;
        update_done = false;
    }
    trace_eckd_ccw(sch->devno, ccw.cmd_code, ccw.cda, ccw.count, ccw.flags,
                   eckd->cylinder, eckd->head, eckd->record);

    switch (ccw.cmd_code) {
    case 0x02:
        ret = eckd_position(eckd, 0, 0, 1);
        return ret ? ret : eckd_read_record(eckd, &ccw, CMD_READ, true);
    case 0x03:
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    case 0x04:
        return eckd_sense(eckd, &ccw);
    case CMD_SUSPEND_MPR:
        /*
         * Suspend Multipath Reconnection: the rest of the channel program
         * stays on the path it was started on.  The device has a single
         * path, so there is nothing to do.  The command must be the first
         * of the channel program.
         */
        if (eckd->prev_cmd_valid) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    case CMD_SET_FILE_MASK:
        if (ccw.count < 1 || eckd_read_guest(eckd, buf, 1)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        eckd->file_mask = buf[0];
        return 0;
    case CMD_DEFINE_EXTENT:
        length = MIN((uint32_t)ccw.count, 16u);
        if (eckd_read_guest(eckd, buf, length)) {
            return -EFAULT;
        }
        return eckd_define_extent(eckd, &ccw, buf, length);
    case CMD_LOCATE:
    case CMD_LOCATE_EXT:
        length = MIN((uint32_t)ccw.count, 22u);
        if (eckd_read_guest(eckd, buf, length)) {
            return -EFAULT;
        }
        return eckd_locate(eckd, &ccw, buf, length,
                           ccw.cmd_code == CMD_LOCATE_EXT);
    case CMD_PREFIX:
        if (ccw.count > sizeof(buf) ||
            eckd_read_guest(eckd, buf, ccw.count)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        return eckd_prefix(eckd, &ccw, buf, ccw.count);
    case CMD_PREFIX_READ:
        memset(buf, 0, sizeof(buf));
        return eckd_copy_response(eckd, &ccw, buf,
                                  MIN((uint32_t)ccw.count, 64u));
    case CMD_SEEK:
    case CMD_SEEK_CYLINDER:
        if (ccw.count < 6 || eckd_read_guest(eckd, buf, 6)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        if (!eckd_seek_permitted(eckd, ccw.cmd_code)) {
            return eckd_unit_check(eckd, 0, SENSE1_FILE_PROTECTED);
        }
        if (lduw_be_p(buf) != 0) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        return eckd_position(eckd, lduw_be_p(buf + 2),
                             lduw_be_p(buf + 4), 0);
    case CMD_SEEK_HEAD:
        /*
         * Seek Head has the same BBCCHH operand as Seek, but retains the
         * current cylinder.  z/OS uses it while locating paging data sets;
         * rejecting it boxes an otherwise valid paging volume.
         */
        if (ccw.count < 6 || eckd_read_guest(eckd, buf, 6)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        if (!eckd_seek_permitted(eckd, ccw.cmd_code)) {
            return eckd_unit_check(eckd, 0, SENSE1_FILE_PROTECTED);
        }
        if (lduw_be_p(buf) != 0) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        return eckd_position(eckd, eckd->cylinder,
                             lduw_be_p(buf + 4), 0);
    case CMD_SEARCH_HA_EQ:
        if (ccw.count < 4 || eckd_read_guest(eckd, buf, 4)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        eckd->search_match = lduw_be_p(buf) == eckd->cylinder &&
                             lduw_be_p(buf + 2) == eckd->head;
        if (eckd->search_match) {
            sch->curr_status.scsw.dstat |= SCSW_DSTAT_STAT_MOD;
        }
        return 0;
    case CMD_SEARCH_ID_EQ:
    case CMD_SEARCH_ID_HIGH:
    case CMD_SEARCH_ID_EQ_HIGH:
    case CMD_SEARCH_ID_EQ | 0x80:
    case CMD_SEARCH_ID_HIGH | 0x80:
    case CMD_SEARCH_ID_EQ_HIGH | 0x80:
        if (ccw.count < 5 || eckd_read_guest(eckd, buf, 5)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        /*
         * Search ID compares the next physical count field, rather than
         * looking up the requested record directly.  A TIC loop therefore
         * walks the track.  A non-multitrack search crosses the index once
         * and searches the same track again; the second crossing reports
         * no-record-found.  A multitrack search (the high bit in B1/D1/F1)
         * advances to the next track at the index, as Hercules does in
         * ckd_read_count().
         */
        if (eckd->search_index >= eckd->image.record_count) {
            if (ccw.cmd_code & 0x80) {
                ret = eckd_next_track_mt(eckd);
                if (ret) {
                    return ret;
                }
            } else {
                if (eckd->search_index_seen) {
                    return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
                }
                eckd->search_index_seen = true;
                eckd->search_index = 0;
            }
        }
        if (!eckd->image.record_count) {
            return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
        }
        {
            const CkdImageRecord *record =
                &eckd->image.records[eckd->search_index++];

            int comparison =
                memcmp(eckd->image.track + record->header_offset, buf, 5);
            uint8_t operation = ccw.cmd_code & 0x7f;

            eckd->search_match =
                (operation == CMD_SEARCH_ID_EQ && comparison == 0) ||
                (operation == CMD_SEARCH_ID_HIGH && comparison > 0) ||
                (operation == CMD_SEARCH_ID_EQ_HIGH && comparison >= 0);
            eckd->search_count_advance = eckd->search_match;
            eckd->record = record->number;
            trace_eckd_search_id(sch->devno, lduw_be_p(buf),
                                 lduw_be_p(buf + 2), buf[4],
                                 record->cylinder, record->head,
                                 record->number, eckd->search_match);
        }
        if (eckd->search_match) {
            sch->curr_status.scsw.dstat |= SCSW_DSTAT_STAT_MOD;
        }
        return 0;
    case CMD_SEARCH_KEY_EQ:
    case CMD_SEARCH_KEY_HIGH:
    case CMD_SEARCH_KEY_EQ_HIGH:
    case CMD_SEARCH_KEY_EQ | 0x80:
    case CMD_SEARCH_KEY_HIGH | 0x80:
    case CMD_SEARCH_KEY_EQ_HIGH | 0x80: {
        bool multitrack = ccw.cmd_code & 0x80;
        uint8_t operation = ccw.cmd_code & 0x7f;
        const CkdImageRecord *record;
        uint32_t compare_length;
        int comparison;

        /*
         * A non-multitrack SEARCH KEY scans through index and gets one more
         * revolution of the current track.  Only the second index crossing
         * reports no-record-found.  This matters for the canonical
         * READ COUNT / SEARCH KEY / TIC loop: the final READ COUNT leaves
         * orientation at index, and the following search starts the second
         * revolution rather than failing immediately.
         *
         * Hercules models the same rule with ckdxmark in ckd_read_count().
         */
        if (!multitrack && eckd->record > UINT8_MAX) {
            if (eckd->search_index_seen) {
                return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
            }
            eckd->search_index_seen = true;
            record = eckd_first_data_record(&eckd->image);
            if (record) {
                eckd->record = record->number;
            }
        } else {
            record = eckd_current_record(eckd, multitrack);
        }
        if (!record) {
            return eckd_no_record(eckd);
        }
        compare_length = MIN((uint32_t)ccw.count,
                             (uint32_t)record->key_length);
        if (compare_length &&
            eckd_read_guest(eckd, buf, compare_length)) {
            return -EFAULT;
        }
        if (!record->key_length) {
            comparison = 0;
            eckd->search_match = false;
        } else {
            comparison = memcmp(eckd->image.track + record->key_offset,
                                buf, compare_length);
            eckd->search_match =
                (operation == CMD_SEARCH_KEY_EQ && comparison == 0) ||
                (operation == CMD_SEARCH_KEY_HIGH && comparison > 0) ||
                (operation == CMD_SEARCH_KEY_EQ_HIGH && comparison >= 0);
        }
        trace_eckd_search_key(sch->devno, ccw.cmd_code, eckd->cylinder,
                              eckd->head, record->number, comparison,
                              eckd->search_match);
        if (eckd->search_match) {
            eckd->search_count_advance = true;
            sch->curr_status.scsw.dstat |= SCSW_DSTAT_STAT_MOD;
        } else {
            eckd->search_count_advance = false;
            const CkdImageRecord *next =
                eckd_next_record(&eckd->image, record);

            /*
             * SEARCH KEY consumes the key field it compares.  A chained TIC
             * therefore continues with the next count/key field; retaining
             * the current record here makes a valid search loop examine the
             * same key forever.  Keep the matching record selected so a
             * following READ DATA operates on it.
             */
            eckd->record = next ? next->number : UINT8_MAX + 1;
        }
        return 0;
    }
    case CMD_SET_SECTOR:
        length = MIN((uint32_t)ccw.count, 1u);
        return length && eckd_read_guest(eckd, buf, length) ? -EFAULT : 0;
    case CMD_READ_SECTOR:
        buf[0] = 0;
        return eckd_copy_response(eckd, &ccw, buf, 1);
    case CMD_READ:
    case CMD_READ_MT:
    case CMD_READ_KD:
    case CMD_READ_KD_MT:
        return eckd_read_record(eckd, &ccw, ccw.cmd_code, true);
    case 0x1e:
    case CMD_READ_CKD_MT:
        /*
         * Read CKD starts by reading the next count field.  Locate Record
         * with count orientation leaves the device after the matching count
         * field, so the record named by the search argument is not itself
         * transferred.  This matters while formatting track zero: ICKDSF
         * locates R0, writes R1, locates R0 again, then verifies R1 with
         * Read CKD.
         */
        /*
         * After a Read Data or Read Key and Data the device has left the
         * record it transferred, and the next count field is the one this
         * command reads.
         */
        if (!read_advanced) {
            eckd->record++;
        }
        return eckd_read_record(eckd, &ccw, ccw.cmd_code, false);
    case CMD_READ_COUNT:
    case CMD_READ_COUNT_MT: {
        bool multitrack = ccw.cmd_code == CMD_READ_COUNT_MT;
        const CkdImageRecord *record;

        /*
         * A Read Count leaves the device count-oriented to the record whose
         * count field it returned.  Search Key and Read Data must therefore
         * still see that same record.  A subsequent Read Count advances to
         * the next count field first.
         *
         * search_count_advance represents that count/key orientation.
         * Data-transfer commands clear it after consuming the record, and a
         * failed Search Key has already selected the following record.
         */
        record = eckd->record <= UINT8_MAX ?
                 ckd_image_find_record(&eckd->image, eckd->record) : NULL;
        if ((eckd->search_count_advance || record_consumed) && record) {
            eckd_advance_record(eckd, record);
            record = eckd->record <= UINT8_MAX ?
                     ckd_image_find_record(&eckd->image, eckd->record) : NULL;
        }
        eckd->search_count_advance = false;
        /*
         * Read Count requires a user-data record: record zero is skipped
         * (only Read R0, Read Track, Search ID and Locate Record reach it).
         * After Read HA or a Seek the device is oriented at R0, so the next
         * Read Count returns R1's count field.
         */
        if (record && record->number == 0) {
            eckd_advance_record(eckd, record);
            record = eckd->record <= UINT8_MAX ?
                     ckd_image_find_record(&eckd->image, eckd->record) : NULL;
        }
        if (!record && !multitrack && !eckd->search_index_seen) {
            /*
             * Reaching index during a non-multitrack count scan starts one
             * more revolution of the current track.  No-record-found is
             * reported only on the second index crossing without an
             * intervening command that resets the index marker.  Hercules
             * models this with ckdxmark in ckd_read_count().
             */
            eckd->search_index_seen = true;
            record = eckd_first_data_record(&eckd->image);
            if (record) {
                eckd->record = record->number;
            }
        }
        while (!record && multitrack) {
            /*
             * Read Count requires a user-data count field.  As on Hercules,
             * skip R0 and continue over completely empty tracks until the
             * next nonzero record is found.  Search operations use different
             * index/cylinder-boundary rules and stay in their own helper.
             */
            ret = eckd_next_track_mt(eckd);
            if (ret) {
                return ret;
            }
            record = eckd_first_data_record(&eckd->image);
            if (record) {
                eckd->record = record->number;
            }
        }
        if (!record) {
            return eckd_no_record(eckd);
        }
        memcpy(buf, eckd->image.track + record->header_offset, 8);
        eckd->search_count_advance = true;
        return eckd_copy_response(eckd, &ccw, buf, 8);
    }
    case CMD_DIAG_READ_HA:
        /*
         * Diagnostic RHA has a 28-byte 3390 layout.  Surface-management
         * fields are not meaningful for an image-backed device; return
         * them as zero and place Flag/CCHH at the architected offset.
         */
        memset(buf, 0, 28);
        memcpy(buf + 19, eckd->image.track, 5);
        return eckd_copy_response(eckd, &ccw, buf, 28);
    case CMD_READ_HA:
    case CMD_READ_HA_MT:
        if (ccw.cmd_code == CMD_READ_HA_MT && !eckd->locate_count) {
            ret = eckd_next_track_mt(eckd);
            if (ret) {
                return ret;
            }
        }
        eckd->record = 0;
        return eckd_copy_response(eckd, &ccw, eckd->image.track, 5);
    case CMD_READ_R0:
        eckd->record = 0;
        return eckd_read_record(eckd, &ccw, 0x1e, true);
    case CMD_READ_TRACK_DATA:
    case CMD_READ_DATA_SPECIAL_MT:
        return eckd_read_record(eckd, &ccw, CMD_READ_MT, true);
    case CMD_READ_TRACK:
        return eckd_read_track(eckd, &ccw);
    case CMD_WRITE:
    case CMD_WRITE_MT:
    case CMD_WRITE_KD:
    case CMD_WRITE_KD_MT:
        return eckd_write_existing(eckd, &ccw, ccw.cmd_code);
    case CMD_WRITE_TRACK_DATA:
    case CMD_WRITE_DATA_SPECIAL_MT:
        return eckd_write_existing(eckd, &ccw, CMD_WRITE_MT);
    case CMD_WRITE_R0:
        return eckd_write_r0(eckd, &ccw);
    case 0x1d:
    case CMD_WRITE_CKD_MT:
        if (update_done) {
            /*
             * Write Data advanced the orientation to the next count field,
             * but a Write CKD chained to it writes the record that follows
             * the one just updated and erases the rest of the track.  z/OS
             * formats JES2 checkpoint tracks with Locate Record Write Track:
             * Write Data (R0), then data-chained Write CKD R1..Rn.  Starting
             * the format domain after the advanced record instead kept the
             * old R1 and rejected the new R1 with a data check.
             */
            eckd->record = eckd->update_done_record;
            eckd->format_active = false;
        }
        return eckd_write_ckd(eckd, &ccw);
    case CMD_ERASE:
        eckd_format_begin(eckd);
        return eckd_format_commit(eckd);
    case CMD_DIAG_WRITE_HA:
    case CMD_WRITE_HA:
        if (ccw.count < 5 || eckd_read_guest(eckd, buf, 5)) {
            return eckd_unit_check(eckd, SENSE_OVERRUN, 0);
        }
        eckd_format_begin(eckd);
        return eckd_format_commit(eckd);
    case CMD_WRITE_FULL_TRACK:
        if (ccw.count > ECKD_TRACK_SIZE) {
            return eckd_unit_check(eckd, SENSE_OVERRUN, 0);
        }
        if (eckd_read_guest(eckd, eckd->format_track, ccw.count)) {
            return -EFAULT;
        }
        ret = ckd_image_store_track(&eckd->image, eckd->cylinder,
                                    eckd->head, eckd->format_track,
                                    ccw.count);
        return ret < 0 ? eckd_unit_check(eckd, SENSE_EQUIPMENT_CHECK, 0) : 0;
    case CMD_RDC:
        eckd_build_rdc(eckd, buf);
        return eckd_copy_response(eckd, &ccw, buf, 64);
    case 0xe4:
        eckd_build_sense_id(eckd, buf);
        return eckd_copy_response(eckd, &ccw, buf, 24);
    case CMD_RCD:
        eckd_build_rcd(eckd, buf);
        return eckd_copy_response(eckd, &ccw, buf, 256);
    case CMD_RESERVE:
        eckd->reserved = true;
        return eckd_sense(eckd, &ccw);
    case CMD_RELEASE:
        eckd->reserved = false;
        return eckd_sense(eckd, &ccw);
    case CMD_SNID:
        memset(buf, 0, 12);
        buf[0] = memcmp(eckd->pgid, (uint8_t[11]) { 0 }, 11) ?
                 0xc0 : 0x80;
        if (eckd->reserved) {
            buf[0] |= 0x30;
        }
        memcpy(buf + 1, eckd->pgid, sizeof(eckd->pgid));
        return eckd_copy_response(eckd, &ccw, buf, 12);
    case CMD_SET_PGID:
        if (ccw.count < 12 || eckd_read_guest(eckd, buf, 12)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        switch (buf[0] & 0x60) {
        case 0x00:
            if (!memcmp(buf + 1, (uint8_t[11]) { 0 }, 11) ||
                (memcmp(eckd->pgid, (uint8_t[11]) { 0 }, 11) &&
                 memcmp(eckd->pgid, buf + 1, 11))) {
                return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
            }
            memcpy(eckd->pgid, buf + 1, sizeof(eckd->pgid));
            return 0;
        case 0x20:
        case 0x40:
            memset(eckd->pgid, 0, sizeof(eckd->pgid));
            return 0;
        default:
            return eckd_reject_unimplemented(eckd, "spid-function", buf[0],
                                             0);
        }
    case CMD_SNSS:
        /*
         * Report the full unit address.  The device can be one of 32 units
         * in the modeled logical subsystem, but this field is the unit
         * address rather than an ordinal within that subsystem.
         */
        memset(buf, 0, 40);
        buf[1] = eckd_unit_address(eckd);
        buf[3] = 1;
        buf[2] = eckd_has_explicit_topology(eckd) ?
                 eckd->unit_address_limit : 31;
        stw_be_p(buf + 38, eckd_subsystem_id(eckd));
        return eckd_copy_response(eckd, &ccw, buf, 40);
    case CMD_RSCK:
        memset(buf, 0, sizeof(buf));
        return eckd_copy_response(eckd, &ccw, buf,
                                  MIN((uint32_t)ccw.count, 32u));
    case CMD_PSF:
        length = MIN((uint32_t)ccw.count, (uint32_t)sizeof(buf));
        if (length < 2 || eckd_read_guest(eckd, buf, length)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        return eckd_perform_subsystem_function(eckd, buf, length);
    case CMD_RSSD:
        if (!eckd->rssd_length) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        ret = eckd_copy_response(eckd, &ccw, eckd->rssd_data,
                                 eckd->rssd_length);
        eckd->rssd_length = 0;
        return ret;
    default:
        return eckd_reject_unimplemented(eckd, "ccw", ccw.cmd_code, 0);
    }
}

/*
 * A real DASD control unit acknowledges a write once the data is in its
 * battery-backed nonvolatile storage (NVS): when a channel program ends,
 * everything it wrote survives a power loss, and z/OS and z/VSE rely on
 * that (journals, catalogs, VSAM, JES2 checkpoints).  The image writes go
 * through the host page cache or a write-back cache of the block layer, so
 * flush the backend before the final status of every channel program that
 * wrote a track.  This includes programs ending in unit check: error
 * recovery redrives a program from the failing CCW, assuming that the
 * writes before it are complete.  The flush is part of the last CCW: its
 * status is presented when the flush has completed.
 *
 * Channel programs which wrote nothing do not flush.  In particular, after
 * a failed flush the next Sense must not fail as well; the unhardened data
 * is flushed again by the next writing program, a reset, or unplug.
 */
static int coroutine_fn eckd_ccw_exec(EckdCcwDevice *eckd, CCW1 ccw)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    int ret;

    if (!eckd->prev_cmd_valid) {
        eckd->program_write_count = eckd->image.write_count;
    }
    /*
     * The commands work on the cached current track.  If it was
     * invalidated, read it again first; a failure leaves a track without
     * records, and the command reports what it finds (or does not).
     */
    ckd_image_revalidate(&eckd->image);
    ret = eckd_ccw_cmd(sch, ccw);
    if (eckd->image.write_count != eckd->program_write_count &&
        css_ccw_ends_program(sch, &ccw, ret) &&
        ckd_image_flush(&eckd->image) < 0 && ret == 0) {
        /* The data did not reach stable storage: report it as a write. */
        return eckd_unit_check(eckd, SENSE_EQUIPMENT_CHECK, 0);
    }
    return ret;
}

/*
 * ECKD I/O is asynchronous.  Each CCW runs in a coroutine; the image
 * accesses (ckd_image_load_track(), ckd_image_store_track() and the flush)
 * use the co_wrapper_mixed block layer calls, which yield inside a
 * coroutine instead of waiting on the vCPU thread with the BQL held.
 *
 * Most CCWs need no I/O (Define Extent, Locate Record, searches and reads
 * on the cached track, sense commands): the coroutine then returns before
 * qemu_coroutine_enter() does, and the CCW completes synchronously as
 * before.  A CCW that yields is reported to css as CSS_CCW_PENDING; when
 * it finishes, eckd_complete_bh() presents its result and css continues
 * the channel program, which may start the next CCW from there.
 *
 * Drained sections of the block backend (stop/cont, migration, block
 * graph changes, snapshots) let the channel program in progress run to
 * its end: request queuing is disabled for the backend, and
 * eckd_drained_poll() keeps the drain waiting while a CCW is active.  A
 * channel program that would start during the drained section waits for
 * its end (eckd_run_deferred()).  So no CCW is executing when the device
 * state is saved, and migration needs no in-flight state.
 */
static void eckd_complete_bh(void *opaque);

static void coroutine_fn eckd_ccw_co(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    eckd->co_ret = eckd_ccw_exec(eckd, eckd->co_ccw);
    eckd->co_running = false;
    if (eckd->co_yielded) {
        /* Present the result outside of coroutine context. */
        aio_bh_schedule_oneshot(qemu_get_aio_context(), eckd_complete_bh,
                                eckd);
    }
}

static int eckd_start_ccw(EckdCcwDevice *eckd, CCW1 ccw)
{
    Coroutine *co;

    eckd->co_ccw = ccw;
    eckd->co_yielded = false;
    eckd->co_discard = false;
    eckd->co_running = true;
    eckd->co_active = true;
    co = qemu_coroutine_create(eckd_ccw_co, eckd);
    qemu_coroutine_enter(co);
    if (eckd->co_running) {
        eckd->co_yielded = true;
        trace_eckd_async(CCW_DEVICE(eckd)->sch->devno, "pending", 0);
        return CSS_CCW_PENDING;
    }
    eckd->co_active = false;
    return eckd->co_ret;
}

static bool eckd_may_start(EckdCcwDevice *eckd)
{
    return !eckd->co_active && !eckd->quiesced && runstate_is_running();
}

/*
 * Start a channel program that had to wait, see eckd_ccw_cb().  Not while
 * the VM is stopped: the state may be saved before it runs again, and a
 * waiting program is migrated (vmstate_eckd_deferred), a running one not.
 */
static void eckd_run_deferred(void *opaque)
{
    EckdCcwDevice *eckd = opaque;
    SubchDev *sch = CCW_DEVICE(eckd)->sch;
    int ret;

    if (!eckd->start_deferred || !eckd_may_start(eckd)) {
        return;
    }
    eckd->start_deferred = false;
    trace_eckd_async(sch->devno, "start deferred", 0);
    ret = eckd_start_ccw(eckd, eckd->deferred_ccw);
    if (ret != CSS_CCW_PENDING) {
        css_virtual_ccw_complete(sch, ret);
    }
}

static void eckd_complete_bh(void *opaque)
{
    EckdCcwDevice *eckd = opaque;
    SubchDev *sch = CCW_DEVICE(eckd)->sch;

    eckd->co_active = false;
    trace_eckd_async(sch->devno, eckd->co_discard ? "discarded" : "complete",
                     eckd->co_ret);
    if (!eckd->co_discard) {
        /* css ignores this if a clear or halt ended the program. */
        css_virtual_ccw_complete(sch, eckd->co_ret);
    }
    eckd_run_deferred(eckd);
}

static int eckd_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    EckdCcwDevice *eckd = sch->driver_data;

    eckd->prev_cmd = sch->last_cmd;
    eckd->prev_cmd_valid = sch->last_cmd_valid;
    /*
     * A new channel program waits while the backend is drained, or while a
     * CCW of a program that was cleared is still executing.  Inside a
     * program, the next CCW always starts: the drain waits for the end of
     * the program.  (The IPL starts its channel program before the VM
     * runs and waits for it, so do not wait for the VM to run here.)
     */
    if (eckd->co_active || (!sch->last_cmd_valid && eckd->quiesced)) {
        eckd->deferred_ccw = ccw;
        eckd->start_deferred = true;
        trace_eckd_async(sch->devno, "deferred", eckd->co_active);
        return CSS_CCW_PENDING;
    }
    return eckd_start_ccw(eckd, ccw);
}

/* Clear, halt or reset of the subchannel while a CCW is pending. */
static void eckd_cancel(SubchDev *sch)
{
    EckdCcwDevice *eckd = sch->driver_data;

    eckd->start_deferred = false;
    if (eckd->co_active) {
        eckd->co_discard = true;
    }
}

/* Wait until no CCW is executing, dropping its result. */
static void eckd_quiesce(EckdCcwDevice *eckd)
{
    eckd->start_deferred = false;
    if (eckd->co_active) {
        eckd->co_discard = true;
        blk_drain(eckd->blk);
    }
    assert(!eckd->co_active);
}

static void eckd_reset_hold(Object *obj, ResetType type)
{
    EckdCcwDevice *eckd = ECKD_CCW(obj);
    EckdCcwDeviceClass *ec = ECKD_CCW_GET_CLASS(eckd);

    /* A CCW still executing must not change the state reset below. */
    eckd_quiesce(eckd);
    eckd->extent_defined = false;
    eckd->file_mask = 0;
    eckd->positioned = false;
    eckd->search_match = false;
    eckd->search_index = 0;
    eckd->search_index_seen = false;
    eckd->reserved = false;
    eckd->format_active = false;
    eckd->write_active = false;
    eckd->update_active = false;
    eckd->update_done = false;
    eckd->read_active = false;
    eckd->read_advanced = false;
    eckd->record_consumed = false;
    /*
     * The device loses its orientation, and the image may have been
     * replaced or reverted while the guest was reset (loadvm, PVE
     * rollback): do not serve the old track from the cache.
     */
    ckd_image_drop_track(&eckd->image);
    /* Writes of a program cut short by the reset are still hardened. */
    ckd_image_flush(&eckd->image);
    if (ec->parent_phases.hold) {
        ec->parent_phases.hold(obj, type);
    }
}

/*
 * Every change of the block graph below the device (blockdev-reopen, a
 * mirror or commit job completing, a snapshot being loaded) drains it, and
 * so do stop/cont and migration: whatever happens inside the drained
 * section may change the image, so the cached track must be read again.
 */
static void eckd_drained_begin(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    eckd->quiesced = true;
    ckd_image_invalidate(&eckd->image);
}

/* The drain waits for the channel program in progress, see above. */
static bool eckd_drained_poll(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    return eckd->co_active;
}

static void eckd_drained_end(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    eckd->quiesced = false;
    ckd_image_invalidate(&eckd->image);
    if (eckd->start_deferred) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), eckd_run_deferred,
                                eckd);
    }
}

static void eckd_vm_state_change(void *opaque, bool running, RunState state)
{
    EckdCcwDevice *eckd = opaque;

    if (running && eckd->start_deferred) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), eckd_run_deferred,
                                eckd);
    }
}

static void eckd_resize_cb(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    ckd_image_invalidate(&eckd->image);
}

static const BlockDevOps eckd_block_ops = {
    .drained_begin = eckd_drained_begin,
    .drained_poll = eckd_drained_poll,
    .drained_end = eckd_drained_end,
    .resize_cb = eckd_resize_cb,
};

static void eckd_realize(DeviceState *dev, Error **errp)
{
    EckdCcwDevice *eckd = ECKD_CCW(dev);
    CcwDevice *cdev = CCW_DEVICE(dev);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    SubchDev *sch;
    uint16_t chpid;
    uint64_t perm = BLK_PERM_CONSISTENT_READ;
    Error *local_err = NULL;
    bool explicit_topology = eckd->unit_address != UINT16_MAX ||
                             eckd->unit_address_limit != UINT16_MAX ||
                             eckd->subsystem_id != UINT16_MAX ||
                             eckd->lcu_queue_number != UINT16_MAX ||
                             eckd->path_cu_number != UINT16_MAX ||
                             eckd->interface_id != UINT16_MAX ||
                             eckd->fla != UINT16_MAX;

    if (explicit_topology &&
        (eckd->unit_address > UINT8_MAX ||
         eckd->unit_address_limit > UINT8_MAX ||
         eckd->subsystem_id == UINT16_MAX ||
         eckd->lcu_queue_number == UINT16_MAX ||
         eckd->path_cu_number == UINT16_MAX ||
         eckd->interface_id == UINT16_MAX)) {
        error_setg(errp, "Explicit ECKD topology requires unit-address, "
                   "unit-address-limit, subsystem-id, lcu-queue-number, "
                   "and path-cu-number; unit-address and unit-address-limit "
                   "must fit in 8 bits");
        return;
    }
    if (explicit_topology && eckd->interface_id == UINT16_MAX) {
        eckd->interface_id = 0;
    }

    if (!eckd->blk) {
        error_setg(errp, "eckd-ccw requires a block backend via drive=");
        return;
    }
    if (blk_supports_write_perm(eckd->blk)) {
        perm |= BLK_PERM_WRITE;
    }
    if (blk_set_perm(eckd->blk, perm, BLK_PERM_ALL, errp) < 0 ||
        !ckd_image_open(&eckd->image, eckd->blk, eckd->cylinders, errp)) {
        return;
    }
    if (!eckd_resolve_identity(eckd, errp)) {
        ckd_image_close(&eckd->image);
        return;
    }
    eckd->format_track = g_malloc0(ECKD_TRACK_SIZE);
    eckd->write_record = g_malloc(ECKD_MAX_RECORD_SIZE);
    blk_set_dev_ops(eckd->blk, &eckd_block_ops, eckd);
    blk_set_disable_request_queuing(eckd->blk, true);

    sch = css_create_sch_at(cdev->devno, ECKD_AUTO_DEVNO, errp);
    if (!sch) {
        goto fail_image;
    }
    sch->driver_data = eckd;
    cdev->sch = sch;
    chpid = eckd->chpid == UINT16_MAX ? sch->devno >> 8 : eckd->chpid;
    if (chpid > MAX_CHPID) {
        error_setg(&local_err, "No available CHPID for eckd-ccw");
        goto fail_sch;
    }
    sch->id.reserved = 0xff;
    sch->id.cu_type = eckd->cu_type;
    sch->id.cu_model = eckd->cu_model;
    sch->id.dev_type = eckd->dev_type;
    sch->id.dev_model = eckd->dev_model;
    css_sch_build_virtual_schib(sch, chpid, ECKD_CHPID_TYPE);
    if (explicit_topology) {
        sch->topology.valid = true;
        sch->topology.unit_address = eckd->unit_address;
        sch->topology.lcu_queue_number = eckd->lcu_queue_number;
        sch->topology.path_cu_number[0] = eckd->path_cu_number;
        if (eckd->fla != UINT16_MAX) {
            sch->topology.fla_valid_mask = 0x80;
            sch->topology.fla[0] = eckd->fla;
        }
    }
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = eckd_ccw_cb;
    sch->ccw_cb_first = true;
    sch->cancel_cb = eckd_cancel;
    sch->disable_cb = eckd_cancel;
    sch->ccw_async_migratable = true;
    sch->irb_cb = build_irb_virtual;

    if (!cdk->realize(cdev, &local_err)) {
        goto fail_sch;
    }
    eckd->vmstate_change = qemu_add_vm_change_state_handler(
        eckd_vm_state_change, eckd);
    return;

fail_sch:
    error_propagate(errp, local_err);
    css_subch_assign(sch->cssid, sch->ssid, sch->schid, sch->devno, NULL);
    cdev->sch = NULL;
    g_free(sch);
fail_image:
    blk_set_dev_ops(eckd->blk, NULL, NULL);
    g_clear_pointer(&eckd->write_record, g_free);
    g_clear_pointer(&eckd->format_track, g_free);
    ckd_image_close(&eckd->image);
}

static void eckd_unrealize(DeviceState *dev)
{
    EckdCcwDevice *eckd = ECKD_CCW(dev);

    eckd_quiesce(eckd);
    g_clear_pointer(&eckd->vmstate_change,
                    qemu_del_vm_change_state_handler);
    blk_set_dev_ops(eckd->blk, NULL, NULL);
    ckd_image_close(&eckd->image);
    g_clear_pointer(&eckd->write_record, g_free);
    g_clear_pointer(&eckd->format_track, g_free);
}

static const Property eckd_properties[] = {
    DEFINE_PROP_DRIVE("drive", EckdCcwDevice, blk),
    DEFINE_PROP_CCW_LOADPARM("loadparm", CcwDevice, loadparm),
    DEFINE_PROP_UINT16("unit-address", EckdCcwDevice, unit_address,
                       UINT16_MAX),
    DEFINE_PROP_UINT16("unit-address-limit", EckdCcwDevice,
                       unit_address_limit, UINT16_MAX),
    DEFINE_PROP_UINT16("subsystem-id", EckdCcwDevice, subsystem_id,
                       UINT16_MAX),
    DEFINE_PROP_UINT16("interface-id", EckdCcwDevice, interface_id,
                       UINT16_MAX),
    DEFINE_PROP_UINT16("lcu-queue-number", EckdCcwDevice, lcu_queue_number,
                       UINT16_MAX),
    DEFINE_PROP_UINT16("path-cu-number", EckdCcwDevice, path_cu_number,
                       UINT16_MAX),
    DEFINE_PROP_UINT16("fla", EckdCcwDevice, fla, UINT16_MAX),
    DEFINE_PROP_UINT16("chpid", EckdCcwDevice, chpid, UINT16_MAX),
    DEFINE_PROP_UINT16("control-unit-type", EckdCcwDevice, cu_type,
                       ECKD_CU_TYPE),
    DEFINE_PROP_UINT8("control-unit-model", EckdCcwDevice, cu_model, 0),
    DEFINE_PROP_UINT16("device-type", EckdCcwDevice, dev_type, ECKD_DEV_TYPE),
    DEFINE_PROP_UINT8("device-model", EckdCcwDevice, dev_model, 0),
    DEFINE_PROP_UINT32("cylinders", EckdCcwDevice, cylinders, 0),
};

/*
 * Migration state.  The drain before the device state is saved lets the
 * channel program in progress finish (see eckd_ccw_cb()), so no CCW is
 * executing then.  A channel program may still be suspended,
 * so the multi-CCW contexts (Define Extent, Locate Record, format-write,
 * write, update and read domains) are part of the state; the large
 * format-write and record buffers go into subsections that are sent only
 * while their domain is active.
 *
 * The cached track itself is not migrated: the destination reads it again
 * from the (shared or mirrored) image before the next CCW uses it.
 */
static int eckd_pre_save(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    /* The drain before saving finishes every CCW that started. */
    if (eckd->co_active) {
        return -EBUSY;
    }
    return 0;
}

static int eckd_pre_load(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    /* State of subsections the stream may not have (loadvm of a snapshot) */
    eckd->read_advanced = false;
    eckd->record_consumed = false;
    return 0;
}

static int eckd_post_load(void *opaque, int version_id)
{
    EckdCcwDevice *eckd = opaque;
    CkdImage *image = &eckd->image;

    if (eckd->format_used > ECKD_TRACK_SIZE ||
        eckd->write_used > ECKD_MAX_RECORD_SIZE ||
        eckd->write_expected > ECKD_MAX_RECORD_SIZE ||
        eckd->rssd_length > sizeof(eckd->rssd_data)) {
        return -EINVAL;
    }
    image->record_count = 0;
    image->used_length = 0;
    image->track_stale = image->track_loaded || image->track_stale;
    image->track_loaded = false;
    if (image->track_stale &&
        (image->loaded_cylinder >= image->cylinders ||
         image->loaded_head >= image->heads)) {
        return -EINVAL;
    }
    return 0;
}

static bool eckd_format_needed(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    return eckd->format_active;
}

static const VMStateDescription vmstate_eckd_format = {
    .name = TYPE_ECKD_CCW "/format",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = eckd_format_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_POINTER_UNSAFE(format_track, EckdCcwDevice, 0,
                                      ECKD_TRACK_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static bool eckd_write_needed(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    return eckd->write_active;
}

static const VMStateDescription vmstate_eckd_write = {
    .name = TYPE_ECKD_CCW "/write",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = eckd_write_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_POINTER_UNSAFE(write_record, EckdCcwDevice, 0,
                                      ECKD_MAX_RECORD_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static bool eckd_deferred_needed(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    return eckd->start_deferred;
}

/*
 * A channel program that waits for a drained section to end when the VM
 * stops: its first CCW is pending at css level (the subchannel migrates
 * its data stream, see ccw_async_migratable) and starts on the
 * destination when the VM runs (eckd_vm_state_change()).
 */
static const VMStateDescription vmstate_eckd_deferred = {
    .name = TYPE_ECKD_CCW "/deferred",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = eckd_deferred_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(start_deferred, EckdCcwDevice),
        VMSTATE_UINT8(deferred_ccw.cmd_code, EckdCcwDevice),
        VMSTATE_UINT8(deferred_ccw.flags, EckdCcwDevice),
        VMSTATE_UINT16(deferred_ccw.count, EckdCcwDevice),
        VMSTATE_UINT32(deferred_ccw.cda, EckdCcwDevice),
        VMSTATE_UINT8(prev_cmd.cmd_code, EckdCcwDevice),
        VMSTATE_UINT8(prev_cmd.flags, EckdCcwDevice),
        VMSTATE_UINT16(prev_cmd.count, EckdCcwDevice),
        VMSTATE_UINT32(prev_cmd.cda, EckdCcwDevice),
        VMSTATE_BOOL(prev_cmd_valid, EckdCcwDevice),
        VMSTATE_END_OF_LIST()
    },
};

static bool eckd_orientation_needed(void *opaque)
{
    EckdCcwDevice *eckd = opaque;

    return eckd->read_advanced || eckd->record_consumed;
}

/*
 * How the last read CCW left the device relative to record.  This
 * outlives the channel program, like the rest of the orientation: a Read
 * CKD or Read Count that starts the next program continues from there.
 * Only sent when one of the flags is set, so the stream of a device whose
 * last CCW was not such a read is unchanged.
 */
static const VMStateDescription vmstate_eckd_orientation = {
    .name = TYPE_ECKD_CCW "/orientation",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = eckd_orientation_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(read_advanced, EckdCcwDevice),
        VMSTATE_BOOL(record_consumed, EckdCcwDevice),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_eckd_ccw = {
    .name = TYPE_ECKD_CCW,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = eckd_pre_save,
    .pre_load = eckd_pre_load,
    .post_load = eckd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_CCW_DEVICE(parent_obj, EckdCcwDevice),
        VMSTATE_UINT32_EQUAL(image.cylinders, EckdCcwDevice),
        VMSTATE_BOOL(extent_defined, EckdCcwDevice),
        VMSTATE_UINT16(extent_begin_cylinder, EckdCcwDevice),
        VMSTATE_UINT16(extent_begin_head, EckdCcwDevice),
        VMSTATE_UINT16(extent_end_cylinder, EckdCcwDevice),
        VMSTATE_UINT16(extent_end_head, EckdCcwDevice),
        VMSTATE_UINT8(file_mask, EckdCcwDevice),
        VMSTATE_UINT16(cylinder, EckdCcwDevice),
        VMSTATE_UINT16(head, EckdCcwDevice),
        VMSTATE_UINT16(record, EckdCcwDevice),
        VMSTATE_UINT8(locate_count, EckdCcwDevice),
        VMSTATE_UINT8(locate_operation, EckdCcwDevice),
        VMSTATE_UINT16(locate_length, EckdCcwDevice),
        VMSTATE_BOOL(positioned, EckdCcwDevice),
        VMSTATE_BOOL(search_match, EckdCcwDevice),
        VMSTATE_BOOL(search_count_advance, EckdCcwDevice),
        VMSTATE_UINT32(search_index, EckdCcwDevice),
        VMSTATE_BOOL(search_index_seen, EckdCcwDevice),
        VMSTATE_BOOL(reserved, EckdCcwDevice),
        VMSTATE_BOOL(fenced, EckdCcwDevice),
        VMSTATE_UINT8_ARRAY(pgid, EckdCcwDevice, 11),
        VMSTATE_UINT8(psf_order, EckdCcwDevice),
        VMSTATE_UINT8(psf_suborder, EckdCcwDevice),
        VMSTATE_UINT16(rssd_length, EckdCcwDevice),
        VMSTATE_UINT8_ARRAY(rssd_data, EckdCcwDevice, 512),
        VMSTATE_UINT32(format_used, EckdCcwDevice),
        VMSTATE_UINT16(format_cylinder, EckdCcwDevice),
        VMSTATE_UINT16(format_head, EckdCcwDevice),
        VMSTATE_BOOL(format_active, EckdCcwDevice),
        VMSTATE_UINT32(write_expected, EckdCcwDevice),
        VMSTATE_UINT32(write_used, EckdCcwDevice),
        VMSTATE_BOOL(write_active, EckdCcwDevice),
        VMSTATE_UINT8(update_command, EckdCcwDevice),
        VMSTATE_UINT8(update_record, EckdCcwDevice),
        VMSTATE_UINT32(update_offset, EckdCcwDevice),
        VMSTATE_UINT32(update_expected, EckdCcwDevice),
        VMSTATE_UINT32(update_used, EckdCcwDevice),
        VMSTATE_BOOL(update_active, EckdCcwDevice),
        VMSTATE_BOOL(update_done, EckdCcwDevice),
        VMSTATE_UINT64(program_write_count, EckdCcwDevice),
        VMSTATE_UINT8(update_done_record, EckdCcwDevice),
        VMSTATE_UINT8(read_command, EckdCcwDevice),
        VMSTATE_UINT8(read_record, EckdCcwDevice),
        VMSTATE_UINT32(read_length, EckdCcwDevice),
        VMSTATE_UINT32(read_used, EckdCcwDevice),
        VMSTATE_BOOL(read_advance, EckdCcwDevice),
        VMSTATE_BOOL(read_active, EckdCcwDevice),
        VMSTATE_UINT16(image.loaded_cylinder, EckdCcwDevice),
        VMSTATE_UINT16(image.loaded_head, EckdCcwDevice),
        VMSTATE_BOOL(image.track_loaded, EckdCcwDevice),
        VMSTATE_BOOL(image.track_stale, EckdCcwDevice),
        VMSTATE_UINT64(image.write_count, EckdCcwDevice),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_eckd_format,
        &vmstate_eckd_write,
        &vmstate_eckd_deferred,
        &vmstate_eckd_orientation,
        NULL
    },
};

static void eckd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CCWDeviceClass *cdk = CCW_DEVICE_CLASS(klass);
    EckdCcwDeviceClass *ec = ECKD_CCW_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = eckd_realize;
    dc->unrealize = eckd_unrealize;
    dc->hotpluggable = false;
    dc->vmsd = &vmstate_eckd_ccw;
    device_class_set_props(dc, eckd_properties);
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    cdk->build_iplb = s390_ipl_build_ccw_iplb;
    resettable_class_set_parent_phases(rc, NULL, eckd_reset_hold, NULL,
                                       &ec->parent_phases);
}

static const TypeInfo eckd_info = {
    .name = TYPE_ECKD_CCW,
    .parent = TYPE_CCW_DEVICE,
    .instance_size = sizeof(EckdCcwDevice),
    .class_size = sizeof(EckdCcwDeviceClass),
    .class_init = eckd_class_init,
};

static void eckd_register_types(void)
{
    type_register_static(&eckd_info);
}

type_init(eckd_register_types)
