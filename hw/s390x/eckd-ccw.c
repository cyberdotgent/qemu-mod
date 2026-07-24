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
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/module.h"
#include "system/block-backend.h"
#include "trace.h"

#define ECKD_AUTO_DEVNO 0x0300
#define ECKD_CU_TYPE 0x2107
#define ECKD_CU_MODEL 0xe8
#define ECKD_DEV_TYPE 0x3390
#define ECKD_CHPID_TYPE 0x1b
#define ECKD_TRACK_SIZE 56832
#define ECKD_MAX_RECORD_SIZE (8 + UINT8_MAX + UINT16_MAX)

#define CMD_WRITE 0x05
#define CMD_READ 0x06
#define CMD_SEEK 0x07
#define CMD_DIAG_WRITE_HA 0x09
#define CMD_DIAG_READ_HA 0x0a
#define CMD_WRITE_KD 0x0d
#define CMD_READ_KD 0x0e
#define CMD_ERASE 0x11
#define CMD_READ_COUNT 0x12
#define CMD_WRITE_R0 0x15
#define CMD_READ_R0 0x16
#define CMD_WRITE_HA 0x19
#define CMD_READ_HA 0x1a
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
#define CMD_SNSS 0x54
#define CMD_SEARCH_KEY_EQ_HIGH 0x69
#define CMD_DEFINE_EXTENT 0x63
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
#define SENSE1_WRITE_INHIBITED 0x02

struct EckdCcwDevice {
    CcwDevice parent_obj;
    BlockBackend *blk;
    CkdImage image;

    bool extent_defined;
    uint16_t extent_begin_cylinder;
    uint16_t extent_begin_head;
    uint16_t extent_end_cylinder;
    uint16_t extent_end_head;
    uint8_t file_mask;

    uint16_t cylinder;
    uint16_t head;
    uint8_t record;
    uint8_t locate_count;
    uint8_t locate_operation;
    uint16_t locate_length;
    bool positioned;
    bool search_match;
    unsigned int search_index;
    bool search_index_seen;
    bool reserved;
    bool fenced;
    uint8_t pgid[11];
    uint8_t psf_order;
    uint8_t psf_suborder;
    bool rssd_prepared;

    uint8_t *format_track;
    uint32_t format_used;
    uint16_t format_cylinder;
    uint16_t format_head;
    bool format_active;

    uint8_t *write_record;
    uint32_t write_expected;
    uint32_t write_used;
    bool write_active;
};

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

static bool eckd_valid_track(EckdCcwDevice *eckd, uint16_t cylinder,
                             uint16_t head)
{
    uint32_t track = (uint32_t)cylinder * eckd->image.heads + head;
    uint32_t first = (uint32_t)eckd->extent_begin_cylinder *
                     eckd->image.heads + eckd->extent_begin_head;
    uint32_t last = (uint32_t)eckd->extent_end_cylinder *
                    eckd->image.heads + eckd->extent_end_head;

    if (cylinder >= eckd->image.cylinders || head >= eckd->image.heads) {
        return false;
    }
    return !eckd->extent_defined || (track >= first && track <= last);
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
    eckd->search_index = 0;
    eckd->search_index_seen = false;
    return 0;
}

static int eckd_next_track(EckdCcwDevice *eckd)
{
    uint16_t cylinder = eckd->cylinder;
    uint16_t head = eckd->head + 1;

    if (head == eckd->image.heads) {
        head = 0;
        cylinder++;
    }
    return eckd_position(eckd, cylinder, head, 0);
}

static const CkdImageRecord *eckd_current_record(EckdCcwDevice *eckd,
                                                 bool multitrack)
{
    const CkdImageRecord *record;

    record = ckd_image_find_record(&eckd->image, eckd->record);
    if (!record && multitrack) {
        /*
         * Outside a Locate Record domain, multitrack operation terminates
         * at the cylinder boundary.  VSE's VTOC search distinguishes this
         * End-of-Cylinder indication from No-Record-Found.
         */
        if (!eckd->locate_count && eckd->head + 1 >= eckd->image.heads) {
            eckd_unit_check(eckd, 0, SENSE1_END_CYLINDER);
        } else if (eckd_next_track(eckd) == 0) {
            eckd->record = 1;
            record = ckd_image_find_record(&eckd->image, eckd->record);
        }
    }
    return record;
}

static int eckd_no_record(EckdCcwDevice *eckd)
{
    SubchDev *sch = CCW_DEVICE(eckd)->sch;

    if (sch->curr_status.scsw.dstat & SCSW_DSTAT_UNIT_CHECK) {
        return -EIO;
    }
    return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
}

static void eckd_advance_record(EckdCcwDevice *eckd)
{
    eckd->record++;
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
    stw_be_p(buf + 1, ECKD_CU_TYPE);
    buf[3] = ECKD_CU_MODEL;
    stw_be_p(buf + 4, ECKD_DEV_TYPE);
    buf[6] = eckd_model(eckd->image.cylinders);
    for (i = 0; i < ARRAY_SIZE(ciws); i++) {
        stl_be_p(buf + 8 + i * 4, ciws[i]);
    }
}

static void eckd_build_rdc(EckdCcwDevice *eckd, uint8_t *buf)
{
    uint8_t code = eckd_type_code(eckd->image.cylinders);

    memset(buf, 0, 64);
    stw_be_p(buf, ECKD_CU_TYPE);
    buf[2] = ECKD_CU_MODEL;
    stw_be_p(buf + 3, ECKD_DEV_TYPE);
    buf[5] = eckd_model(eckd->image.cylinders);
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
    char text[33] = { 0 };

    memset(buf, 0, 256);
    stl_be_p(buf, 0xdc010100);
    snprintf(text, sizeof(text), "  33900%02XHRCZZ",
             eckd_model(eckd->image.cylinders));
    memcpy(text + 14, eckd->image.serial, 12);
    eckd_ned_text(buf + 4, text, 26);
    stw_be_p(buf + 30, cdev->sch->devno);

    stl_be_p(buf + 32, 0xd4020000);
    snprintf(text, sizeof(text), "  33900%02XHRCZZ000000000003",
             eckd_model(eckd->image.cylinders));
    eckd_ned_text(buf + 36, text, 26);

    stl_be_p(buf + 64, 0xd0000000);
    snprintf(text, sizeof(text), "  21070E8HRCZZ000000000002");
    eckd_ned_text(buf + 68, text, 26);
    buf[95] = cdev->sch->devno >> 8;

    stl_be_p(buf + 96, 0xf0000001);
    snprintf(text, sizeof(text), "  2107   HRCZZ000000000001");
    eckd_ned_text(buf + 100, text, 26);

    buf[224] = 0x80;
    buf[230] = 0x1e;
    buf[234] = 0x80;
    buf[235] = cdev->sch->devno;
    buf[236] = cdev->sch->devno;
    buf[237] = cdev->sch->devno;
    buf[241] = 0x80;
    buf[242] = 0x80;
    buf[243] = cdev->sch->devno;
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
    uint16_t cylinder;
    uint16_t head;
    uint8_t operation;

    if (length < 16) {
        return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
    }
    operation = buf[0] & 0x3f;
    eckd->locate_operation = operation;
    eckd->locate_count = buf[3];
    cylinder = lduw_be_p(buf + 4);
    head = lduw_be_p(buf + 6);
    eckd->record = buf[12];
    eckd->locate_length = lduw_be_p(buf + 14);
    if (operation == 0x03) {
        /*
         * A new format-write domain replaces the track tail following the
         * locate argument.  The first Write CKD will preserve that prefix.
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
     * Data orientation has already passed that record, so the next
     * data/key-data command operates on its successor.  Hyperion models
     * this with ckdorient/ckdcurrec; keeping the adjustment here preserves
     * the simpler "next record to transfer" representation used below.
     */
    if ((buf[0] & 0xc0) == 0x80) {
        eckd->record++;
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
    bool multitrack = command & 0x80;
    const CkdImageRecord *record = eckd_current_record(eckd, multitrack);
    const uint8_t *data;
    uint32_t length;

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
    trace_eckd_read(CCW_DEVICE(eckd)->sch->devno, command, eckd->cylinder,
                    eckd->head, record->number, length, ccw->count);
    if (eckd_copy_response(eckd, ccw, data, length)) {
        return -EFAULT;
    }
    if (advance) {
        eckd_advance_record(eckd);
    } else if (eckd->locate_count) {
        eckd->locate_count--;
    }
    return 0;
}

static int eckd_write_existing(EckdCcwDevice *eckd, const CCW1 *ccw,
                               uint8_t command)
{
    const CkdImageRecord *record;
    uint8_t *copy;
    uint32_t offset;
    uint32_t expected;
    uint32_t length;
    int ret;

    if (eckd->fenced) {
        return eckd_unit_check(eckd, SENSE_EQUIPMENT_CHECK, 0);
    }
    if (!blk_is_writable(eckd->blk)) {
        return eckd_unit_check(eckd, 0, SENSE1_WRITE_INHIBITED);
    }
    record = eckd_current_record(eckd, command & 0x80);
    if (!record) {
        return eckd_no_record(eckd);
    }
    if ((command & 0x7f) == CMD_WRITE) {
        offset = record->data_offset;
        expected = record->data_length;
    } else {
        offset = record->key_offset;
        expected = record->key_length + record->data_length;
    }
    length = MIN((uint32_t)ccw->count, expected);

    copy = g_memdup2(eckd->image.track, eckd->image.used_length);
    ret = eckd_read_guest(eckd, copy + offset, length);
    if (!ret && length < expected) {
        /*
         * CKD update writes pad a short data transfer with zeroes.  This is
         * how ckd_write_data()/ckd_write_kd() behave in Hercules and is used
         * by VSE when rewriting fixed-size library blocks.
         */
        memset(copy + offset + length, 0, expected - length);
    }
    if (!ret) {
        ret = ckd_image_store_track(&eckd->image, eckd->cylinder,
                                    eckd->head, copy,
                                    eckd->image.used_length);
    }
    g_free(copy);
    if (ret < 0) {
        eckd->fenced = true;
        return eckd_unit_check(eckd, SENSE_EQUIPMENT_CHECK, 0);
    }
    eckd_advance_record(eckd);
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
    bool continuation = eckd->write_active && sch->last_cmd_valid &&
                        (sch->last_cmd.flags & CCW_FLAG_DC);
    uint8_t *data = eckd->write_record;
    uint16_t cylinder;
    uint16_t head;
    uint32_t required;
    uint32_t length;
    uint32_t padding;
    g_autofree uint8_t *discard = NULL;
    int ret;

    if (!blk_is_writable(eckd->blk)) {
        return eckd_unit_check(eckd, 0, SENSE1_WRITE_INHIBITED);
    }
    if (!continuation) {
        eckd->write_active = false;
        eckd->write_used = 0;
        if (ccw->count < 8) {
            return eckd_unit_check(eckd, SENSE_OVERRUN, 0);
        }
        if (eckd_read_guest(eckd, data, ccw->count)) {
            return -EFAULT;
        }
        eckd->write_expected = 8 + data[5] + lduw_be_p(data + 6);
        eckd->write_used = ccw->count;
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
    return eckd_copy_response(eckd, ccw, eckd->image.track,
                              eckd->image.used_length);
}

static int eckd_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    EckdCcwDevice *eckd = sch->driver_data;
    g_autofree uint8_t *response = NULL;
    uint8_t buf[256] = { 0 };
    uint32_t length;
    int ret;

    if (!sch->last_cmd_valid) {
        /*
         * Define Extent, Locate Record and format-write domains belong to
         * one channel program.  Retaining the final formatting extent into
         * a later START SUBCHANNEL incorrectly rejects ordinary seeks.
         */
        eckd->extent_defined = false;
        eckd->locate_count = 0;
        eckd->locate_operation = 0;
        eckd->format_active = false;
        eckd->search_match = false;
        eckd->search_index = 0;
        eckd->search_index_seen = false;
        eckd->write_active = false;
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
        memcpy(buf, sch->sense_data, 32);
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
        return eckd_copy_response(eckd, &ccw, buf, 32);
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
        if (ccw.count < 6 || eckd_read_guest(eckd, buf, 6)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        return eckd_position(eckd, lduw_be_p(buf + 2),
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
        if (ccw.count < 5 || eckd_read_guest(eckd, buf, 5)) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        /*
         * Search ID compares the next physical count field, rather than
         * looking up the requested record directly.  A TIC loop therefore
         * walks the track.  After the first index crossing it searches the
         * track once more; a second crossing reports no-record-found.
         */
        if (eckd->search_index >= eckd->image.record_count) {
            if (eckd->search_index_seen) {
                return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
            }
            eckd->search_index_seen = true;
            eckd->search_index = 0;
        }
        if (!eckd->image.record_count) {
            return eckd_unit_check(eckd, 0, SENSE1_NO_RECORD);
        }
        {
            const CkdImageRecord *record =
                &eckd->image.records[eckd->search_index++];

            eckd->search_match =
                lduw_be_p(buf) == record->cylinder &&
                lduw_be_p(buf + 2) == record->head &&
                buf[4] == record->number;
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
        const CkdImageRecord *record =
            eckd_current_record(eckd, multitrack);
        uint32_t compare_length;
        int comparison;

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
            sch->curr_status.scsw.dstat |= SCSW_DSTAT_STAT_MOD;
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
        eckd->record++;
        return eckd_read_record(eckd, &ccw, ccw.cmd_code, false);
    case CMD_READ_COUNT:
    case CMD_READ_COUNT_MT: {
        bool multitrack = ccw.cmd_code == CMD_READ_COUNT_MT;
        const CkdImageRecord *record;

        /*
         * Locate Record leaves the device oriented to the search argument.
         * Read Count transfers the following count field.
         */
        eckd->record++;
        record = ckd_image_find_record(&eckd->image, eckd->record);
        while (!record && multitrack) {
            /*
             * Read Count requires a user-data count field.  As on Hercules,
             * skip R0 and continue over completely empty tracks until the
             * next nonzero record is found.  Search operations use different
             * index/cylinder-boundary rules and stay in their own helper.
             */
            ret = eckd_next_track(eckd);
            if (ret) {
                return ret;
            }
            eckd->record = 1;
            record = ckd_image_find_record(&eckd->image, eckd->record);
        }
        if (!record) {
            return eckd_no_record(eckd);
        }
        memcpy(buf, eckd->image.track + record->header_offset, 8);
        if (eckd->locate_count) {
            eckd->locate_count--;
        }
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
            if (eckd->head + 1 >= eckd->image.heads) {
                return eckd_unit_check(eckd, 0, SENSE1_END_CYLINDER);
            }
            ret = eckd_next_track(eckd);
            if (ret) {
                return ret;
            }
        }
        eckd->record = 0;
        return eckd_copy_response(eckd, &ccw, eckd->image.track, 5);
    case CMD_READ_R0:
        eckd->record = 0;
        return eckd_read_record(eckd, &ccw, 0x1e, true);
    case CMD_READ_TRACK:
    case CMD_READ_TRACK_DATA:
        return eckd_read_track(eckd, &ccw);
    case CMD_WRITE:
    case CMD_WRITE_MT:
    case CMD_WRITE_KD:
    case CMD_WRITE_KD_MT:
        return eckd_write_existing(eckd, &ccw, ccw.cmd_code);
    case CMD_WRITE_R0:
        return eckd_write_r0(eckd, &ccw);
    case 0x1d:
    case CMD_WRITE_CKD_MT:
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
    case CMD_WRITE_TRACK_DATA:
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
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    case CMD_RELEASE:
        eckd->reserved = false;
        sch->curr_status.scsw.count = ccw.count;
        return 0;
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
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
    case CMD_SNSS:
        /*
         * Report the device's ordinal in the modeled 32-device logical
         * subsystem.  Other subsystem-status fields remain zero.
         */
        memset(buf, 0, 40);
        buf[1] = sch->devno & 0x1f;
        buf[2] = 31;
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
        eckd->psf_order = buf[0];
        eckd->psf_suborder = length > 6 ? buf[6] : 0;
        eckd->rssd_prepared = eckd->psf_order == 0x18;
        return 0;
    case CMD_RSSD:
        if (!eckd->rssd_prepared) {
            return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
        }
        eckd->rssd_prepared = false;
        response = g_malloc0(ccw.count);
        return eckd_copy_response(eckd, &ccw, response, ccw.count);
    default:
        return eckd_unit_check(eckd, SENSE_COMMAND_REJECT, 0);
    }
}

static void eckd_reset_hold(Object *obj, ResetType type)
{
    EckdCcwDevice *eckd = ECKD_CCW(obj);
    EckdCcwDeviceClass *ec = ECKD_CCW_GET_CLASS(eckd);

    eckd->extent_defined = false;
    eckd->positioned = false;
    eckd->search_match = false;
    eckd->search_index = 0;
    eckd->search_index_seen = false;
    eckd->reserved = false;
    eckd->format_active = false;
    eckd->write_active = false;
    if (ec->parent_phases.hold) {
        ec->parent_phases.hold(obj, type);
    }
}

static void eckd_realize(DeviceState *dev, Error **errp)
{
    EckdCcwDevice *eckd = ECKD_CCW(dev);
    CcwDevice *cdev = CCW_DEVICE(dev);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    SubchDev *sch;
    uint16_t chpid;
    uint64_t perm = BLK_PERM_CONSISTENT_READ;
    Error *local_err = NULL;

    if (!eckd->blk) {
        error_setg(errp, "eckd-ccw requires a block backend via drive=");
        return;
    }
    if (blk_supports_write_perm(eckd->blk)) {
        perm |= BLK_PERM_WRITE;
    }
    if (blk_set_perm(eckd->blk, perm, BLK_PERM_ALL, errp) < 0 ||
        !ckd_image_open(&eckd->image, eckd->blk, errp)) {
        return;
    }
    eckd->format_track = g_malloc0(ECKD_TRACK_SIZE);
    eckd->write_record = g_malloc(ECKD_MAX_RECORD_SIZE);

    sch = css_create_sch_at(cdev->devno, ECKD_AUTO_DEVNO, errp);
    if (!sch) {
        goto fail_image;
    }
    sch->driver_data = eckd;
    cdev->sch = sch;
    chpid = css_find_virtual_chpid(sch->cssid, ECKD_CHPID_TYPE);
    if (chpid > MAX_CHPID) {
        error_setg(&local_err, "No available CHPID for eckd-ccw");
        goto fail_sch;
    }
    sch->id.reserved = 0xff;
    sch->id.cu_type = ECKD_CU_TYPE;
    sch->id.cu_model = ECKD_CU_MODEL;
    sch->id.dev_type = ECKD_DEV_TYPE;
    sch->id.dev_model = eckd_model(eckd->image.cylinders);
    css_sch_build_virtual_schib(sch, chpid, ECKD_CHPID_TYPE);
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = eckd_ccw_cb;
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
fail_image:
    g_clear_pointer(&eckd->write_record, g_free);
    g_clear_pointer(&eckd->format_track, g_free);
    ckd_image_close(&eckd->image);
}

static void eckd_unrealize(DeviceState *dev)
{
    EckdCcwDevice *eckd = ECKD_CCW(dev);

    ckd_image_close(&eckd->image);
    g_clear_pointer(&eckd->write_record, g_free);
    g_clear_pointer(&eckd->format_track, g_free);
}

static const Property eckd_properties[] = {
    DEFINE_PROP_DRIVE("drive", EckdCcwDevice, blk),
    DEFINE_PROP_CCW_LOADPARM("loadparm", CcwDevice, loadparm),
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
