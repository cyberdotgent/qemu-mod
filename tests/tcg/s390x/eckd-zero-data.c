/*
 * Verify Unit Exception for a zero-data-length CKD record.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define ECKD_CMD_READ 0x06
#define ECKD_CMD_WRITE 0x05
#define ECKD_CMD_SEEK 0x07
#define ECKD_CMD_SEEK_CYLINDER 0x0b
#define ECKD_CMD_SEEK_HEAD 0x1b
#define ECKD_CMD_READ_COUNT 0x12
#define ECKD_CMD_READ_COUNT_MT 0x92
#define ECKD_CMD_SEARCH_KEY_HIGH 0x49
#define ECKD_CMD_SET_FILE_MASK 0x1f
#define ECKD_CMD_WRITE_DATA_SPECIAL 0xa5
#define ECKD_CMD_READ_DATA_SPECIAL 0xa6
#define ECKD_CMD_WRITE_DATA_SPECIAL_2 0xb5
#define ECKD_CMD_READ_DATA_SPECIAL_2 0xb6
#define ECKD_CMD_READ_TRACK 0xde
#define ECKD_CMD_LOCATE 0x47
#define ECKD_CMD_DEFINE_EXTENT 0x63
#define ECKD_CMD_SNSS 0x54
#define ECKD_CMD_RCD 0xfa
#define ECKD_CMD_RELEASE 0x94
#define ECKD_CMD_RESERVE 0xb4
#define ECKD_CMD_WRITE_CKD 0x1d
#define ECKD_LR_WRITE_TRACK 0x0b
#define ECKD_LR_READ_TRACKS 0x0c

LowCore *lowcore;

static uint8_t seek[6] __attribute__((aligned(8)));
static uint8_t seek_head[6] __attribute__((aligned(8))) = {
    0, 0, 0, 0, 0, 2,
};
static uint8_t seek_cylinder[6] __attribute__((aligned(8))) = {
    0, 0, 0, 1, 0, 2,
};
static uint8_t seek_chain[6] __attribute__((aligned(8))) = {
    0, 0, 0, 1, 0, 3,
};
static uint8_t define_extent[16] __attribute__((aligned(8)));
static uint8_t locate[16] __attribute__((aligned(8))) = {
    0x06, 0, 0, 2,             /* count-oriented read, two-command domain */
    0, 0, 0, 0,                /* seek cylinder/head */
    0, 0, 0, 0, 0,            /* search CCHHR for record zero */
    0xff, 0, 4,                /* sector, transfer length */
};
static uint8_t count[8] __attribute__((aligned(8)));
static uint8_t data[256] __attribute__((aligned(8)));
static uint8_t sense[32] __attribute__((aligned(8)));
static uint8_t file_mask __attribute__((aligned(8)));
static uint8_t active_chpids[32] __attribute__((aligned(32)));
static Ccw1 ccws[8] __attribute__((aligned(8)));

/*
 * Write Track on track 2/1: Write Data (R0), then Write CKD R1 whose count
 * field is data-chained to an IDA continuation carrying the 4K data area,
 * then Write CKD R2.  The image holds old R1..R3 there.
 */
static uint8_t wt_extent[16] __attribute__((aligned(8))) = {
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 2, 0, 1,                /* begin CCHH 0002/0001 */
    0, 2, 0, 1,                /* end CCHH 0002/0001 */
};
static uint8_t wt_locate[16] __attribute__((aligned(8)));
static uint8_t wt_r0[8] __attribute__((aligned(8)));
static uint8_t wt_count1[8] __attribute__((aligned(8)));
static uint8_t wt_ckd2[12] __attribute__((aligned(8)));
static uint8_t wt_buf[3 * 4096] __attribute__((aligned(4096)));
static uint8_t rt_buf[8192] __attribute__((aligned(4096)));
static uint64_t idaw2[2] __attribute__((aligned(8)));
static uint32_t idaw1[3] __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
}

static bool bytes_differ(const void *a, const void *b, unsigned int n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;

    for (unsigned int i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return true;
        }
    }
    return false;
}

/*
 * Run the channel program in ccws[], expect unit check, then read basic
 * sense and expect sense byte 0 clear and byte 1 equal to sense1.
 */
static bool check_unit_check_sense(SubChannelId schid, CmdOrb *orb,
                                   uint8_t sense1)
{
    Irb irb = { 0 };

    if (ssch(schid, orb)) {
        return false;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UCHK)) {
        return false;
    }
    memset(sense, 0, sizeof(sense));
    ccws[0] = (Ccw1) {
        .cmd_code = CCW_CMD_BASIC_SENSE,
        .count = sizeof(sense),
        .cda = addr32(sense),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, orb)) {
        return false;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND)) {
        return false;
    }
    return !sense[0] && sense[1] == sense1;
}

int main(void)
{
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    CmdOrb orb = {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(ccws),
    };
    Schib schib;
    Irb irb = { 0 };

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }

    /*
     * Sense Subsystem Status returns the full unit address, not its ordinal
     * within a 32-device logical subsystem.  Keep it consistent with the
     * device and logical-subsystem identities returned by RCD.
     */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SNSS,
        .flags = CCW_FLAG_SLI,
        .count = 40,
        .cda = addr32(data),
    };
    memset(data, 0xa5, 40);
    if (ssch(schid, &orb)) {
        return 33;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 34;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count || data[0] || data[1] != 0x85 || data[2] != 31 ||
        data[38] != 0x0a || data[39] != 0x80) {
        return 35;
    }
    if (stsch_err(schid, &schib) || schib.pmcw.lpum != 0x80) {
        return 45;
    }
    irb = (Irb) { 0 };

    /* RCD must describe the same logical subsystem and interface. */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_RCD,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0xa5, sizeof(data));
    if (ssch(schid, &orb)) {
        return 36;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 37;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count || data[226] || data[227] != 4 ||
        data[232] != 0x0a || data[233] != 0x80 || data[238] != 4) {
        return 38;
    }
    irb = (Irb) { 0 };

    /* Reserve and Release complete with the same transfer shape as Sense. */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_RESERVE,
        .flags = CCW_FLAG_SLI,
        .count = 1,
        .cda = addr32(data),
    };
    data[0] = 0xa5;
    if (ssch(schid, &orb)) {
        return 39;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 40;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count || data[0]) {
        return 41;
    }
    irb = (Irb) { 0 };

    ccws[0].cmd_code = ECKD_CMD_RELEASE;
    data[0] = 0xa5;
    if (ssch(schid, &orb)) {
        return 42;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 43;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count || data[0]) {
        return 44;
    }
    irb = (Irb) { 0 };

    /*
     * KVM exits STCPS to userspace.  The intercept handler must execute the
     * common implementation instead of silently leaving the operand and
     * condition code unchanged.  No channel path is active at this point.
     */
    memset(active_chpids, 0xa5, sizeof(active_chpids));
    asm volatile("stcps %0" : "=Q" (active_chpids) : : "cc");
    for (int i = 0; i < sizeof(active_chpids); i++) {
        if (active_chpids[i]) {
            return 32;
        }
    }

    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek),
        .cda = addr32(seek),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI | CCW_FLAG_PCI,
        .count = sizeof(data),
        .cda = addr32(data),
    };

    if (ssch(schid, &orb)) {
        return 3;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 4;
    }
    if (irb.scsw.cstat != SCSW_CSTAT_PCINT ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UEXCP) ||
        (irb.scsw.ctrl & (SCSW_SCTL_INTERMED | SCSW_SCTL_PRIMARY |
                          SCSW_SCTL_SECONDARY | SCSW_SCTL_ALERT |
                          SCSW_SCTL_STATUS_PEND)) !=
        (SCSW_SCTL_INTERMED | SCSW_SCTL_PRIMARY |
         SCSW_SCTL_SECONDARY | SCSW_SCTL_ALERT |
         SCSW_SCTL_STATUS_PEND) ||
        irb.scsw.count != sizeof(data) ||
        irb.scsw.cpa != addr32(&ccws[2])) {
        return 5;
    }

    /*
     * Seek Head changes only the head.  This command is used by z/OS while
     * opening paging volumes and must not be confused with PSF order 0x1b.
     */
    seek[5] = 1;
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek),
        .cda = addr32(seek),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_HEAD,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_head),
        .cda = addr32(seek_head),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 9;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 10;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 4 ||
        irb.scsw.cpa != addr32(&ccws[3]) ||
        data[0] != 'h' || data[1] != 'e' ||
        data[2] != 'a' || data[3] != 'd') {
        return 11;
    }

    seek[5] = 0;

    /*
     * Locate count orientation selects record zero.  Read Count must move
     * to record one, and the following Read Data must consume record one's
     * contents.  Returning record zero again is the failure that truncated
     * z/OS parmlib members during NIP.
     */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_DEFINE_EXTENT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(define_extent),
        .cda = addr32(define_extent),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_LOCATE,
        .flags = CCW_FLAG_CC,
        .count = sizeof(locate),
        .cda = addr32(locate),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_COUNT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(count),
        .cda = addr32(count),
    };
    ccws[3] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(count, 0, sizeof(count));
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 6;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 7;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 4 ||
        irb.scsw.cpa != addr32(&ccws[4]) ||
        count[4] != 1 ||
        data[0] != 'g' || data[1] != 'o' ||
        data[2] != 'o' || data[3] != 'd') {
        return 8;
    }

    /*
     * Locate Record's search argument is the complete CCHHR, not merely R.
     * The seek address names track 0/0, while this search argument names
     * track 0/1.  Record 1 exists on both tracks, so accepting only its R
     * byte would incorrectly complete normally instead of reporting NRF.
     */
    locate[3] = 1;
    locate[11] = 1;
    locate[12] = 1;
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_DEFINE_EXTENT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(define_extent),
        .cda = addr32(define_extent),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_LOCATE,
        .count = sizeof(locate),
        .cda = addr32(locate),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 58;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UCHK)) {
        return 59;
    }
    memset(sense, 0, sizeof(sense));
    ccws[0] = (Ccw1) {
        .cmd_code = CCW_CMD_BASIC_SENSE,
        .count = sizeof(sense),
        .cda = addr32(sense),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 60;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        sense[0] || sense[1] != 0x08) {
        return 61;
    }

    /*
     * Data orientation leaves the device after the located record.  Track
     * 0/1 contains R1 followed physically by R3, with no R2.  The following
     * Read Data must therefore return R3 rather than looking up numeric R2.
     */
    define_extent[15] = 1;
    locate[0] = 0x86;
    locate[7] = 1;
    locate[10] = 0;
    locate[11] = 1;
    locate[12] = 1;
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_DEFINE_EXTENT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(define_extent),
        .cda = addr32(define_extent),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_LOCATE,
        .flags = CCW_FLAG_CC,
        .count = sizeof(locate),
        .cda = addr32(locate),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 62;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 4 ||
        data[0] != 'n' || data[1] != 'e' ||
        data[2] != 'x' || data[3] != 't') {
        return 63;
    }
    define_extent[15] = 0;
    locate[0] = 0x06;
    locate[3] = 2;
    locate[7] = 0;
    locate[11] = 0;
    locate[12] = 0;

    /*
     * A multitrack operation that reaches the end of its defined extent
     * reports file protected, not end of cylinder.  z/OS relies on this
     * distinction while scanning catalog and library extents during NIP.
     */
    define_extent[0] = 0x48;
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_DEFINE_EXTENT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(define_extent),
        .cda = addr32(define_extent),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_LOCATE,
        .flags = CCW_FLAG_CC,
        .count = sizeof(locate),
        .cda = addr32(locate),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_COUNT_MT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(count),
        .cda = addr32(count),
    };
    ccws[3] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_COUNT_MT,
        .count = sizeof(count),
        .cda = addr32(count),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 33;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 34;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UCHK)) {
        return 35;
    }
    memset(sense, 0, sizeof(sense));
    ccws[0] = (Ccw1) {
        .cmd_code = CCW_CMD_BASIC_SENSE,
        .count = sizeof(sense),
        .cda = addr32(sense),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 36;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        sense[0] || sense[1] != 0x04) {
        return 37;
    }
    define_extent[0] = 0;

    /*
     * Seek Cylinder (0x0b) has the same BBCCHH operand as Seek.  XCF uses
     * it to move between couple-data-set control records.
     */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_cylinder),
        .cda = addr32(seek_cylinder),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 12;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 13;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 3 ||
        irb.scsw.cpa != addr32(&ccws[2]) ||
        data[0] != 'c' || data[1] != 'y' || data[2] != 'l') {
        return 14;
    }

    /*
     * A data-chained read continues the same physical record rather than
     * advancing to the next count field.  XCF splits a large couple-data-set
     * control record across two Read Data CCWs this way.
     */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_DC,
        .count = 60,
        .cda = addr32(data),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .count = 40,
        .cda = addr32(data + 60),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 15;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 16;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count ||
        irb.scsw.cpa != addr32(&ccws[3])) {
        return 17;
    }
    for (int i = 0; i < 100; i++) {
        if (data[i] != i) {
            return 18;
        }
    }

    /*
     * XCF updates its large couple-data-set records with a data-chained
     * Write Data.  Both CCWs must update the same physical record; treating
     * the first CCW as a short write advances to a nonexistent next record
     * and reports unit check on the continuation.
     */
    for (int i = 0; i < 100; i++) {
        data[i] = 255 - i;
    }
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_WRITE,
        .flags = CCW_FLAG_DC,
        .count = 60,
        .cda = addr32(data),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_WRITE,
        .count = 40,
        .cda = addr32(data + 60),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 19;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 20;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count ||
        irb.scsw.cpa != addr32(&ccws[3])) {
        return 21;
    }

    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 22;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 23;
    }
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 100 ||
        irb.scsw.cpa != addr32(&ccws[2])) {
        return 24;
    }
    for (int i = 0; i < 100; i++) {
        if (data[i] != 255 - i) {
            return 25;
        }
    }

    /*
     * Exact exhaustion on a data-chained CCW preserves the original record.
     * The continuation sees no data and retains its complete residual rather
     * than advancing into the next record or track.
     */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_DC,
        .count = 100,
        .cda = addr32(data),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI,
        .count = 16,
        .cda = addr32(data + 100),
    };
    memset(data, 0x77, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 54;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != 16 || irb.scsw.cpa != addr32(&ccws[3])) {
        return 55;
    }
    for (int i = 0; i < 100; i++) {
        if (data[i] != 255 - i) {
            return 56;
        }
    }
    for (int i = 100; i < 116; i++) {
        if (data[i] != 0x77) {
            return 57;
        }
    }

    /* A6/B6 read record data; they must not expose the raw track header. */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_DATA_SPECIAL,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 38;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 100) {
        return 39;
    }
    for (int i = 0; i < 100; i++) {
        if (data[i] != 255 - i) {
            return 40;
        }
        data[i] = i ^ 0x5a;
    }

    /* A5/B5 are the corresponding Write Data forms. */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_WRITE_DATA_SPECIAL,
        .count = 100,
        .cda = addr32(data),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 41;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count) {
        return 42;
    }

    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_DATA_SPECIAL_2,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 43;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 100) {
        return 44;
    }
    for (int i = 0; i < 100; i++) {
        if (data[i] != (i ^ 0x5a)) {
            return 45;
        }
        data[i] = 0xf0 - i;
    }

    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_WRITE_DATA_SPECIAL_2,
        .count = 100,
        .cda = addr32(data),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 46;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count) {
        return 47;
    }

    /* Short update data is padded but reports incorrect length. */
    memset(data, 0xa5, 60);
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK_CYLINDER,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek_chain),
        .cda = addr32(seek_chain),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_WRITE,
        .count = 60,
        .cda = addr32(data),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 48;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat != SCSW_CSTAT_BADLEN ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count) {
        return 49;
    }

    /* A zero-data-length update completes with Unit Exception. */
    memset(seek, 0, sizeof(seek));
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek),
        .cda = addr32(seek),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_WRITE,
        .count = 16,
        .cda = addr32(data),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 50;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UEXCP) ||
        irb.scsw.count != 16) {
        return 51;
    }

    /* Read Track begins with R0's count field, not the home address. */
    locate[0] = 0x0c;
    locate[3] = 1;
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_DEFINE_EXTENT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(define_extent),
        .cda = addr32(define_extent),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_LOCATE,
        .flags = CCW_FLAG_CC,
        .count = sizeof(locate),
        .cda = addr32(locate),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_TRACK,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(data),
        .cda = addr32(data),
    };
    memset(data, 0, sizeof(data));
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 52;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(data) - 28 ||
        data[12] != 1 || data[14] != 0 || data[15] != 4 ||
        data[16] != 'g' || data[17] != 'o' ||
        data[18] != 'o' || data[19] != 'd') {
        return 53;
    }

    /*
     * Outside a Locate Record domain, a multitrack Read Count that runs off
     * the last track of a cylinder ends with End of Cylinder; it must not
     * continue on the next cylinder.  Head 14 of cylinder 1 holds one
     * record; the second Read Count reaches the cylinder boundary, where a
     * wrapping implementation would instead return cylinder 2's record.
     */
    memset(seek, 0, sizeof(seek));
    seek[3] = 1;
    seek[5] = 14;
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek),
        .cda = addr32(seek),
    };
    /*
     * As in VSE's VTOC scan: Read Count MT, Search Key High (the records
     * have no key, so the search transfers nothing and does not match),
     * then Read Count MT again past the last record of the cylinder.
     */
    memset(data, 0xff, 44);    /* higher than every key: never matches */
    for (int i = 0; i < 2; i++) {
        /* Like VSE, transfer only the CCHHR (5 bytes) of each count. */
        ccws[1 + 2 * i] = (Ccw1) {
            .cmd_code = ECKD_CMD_READ_COUNT_MT,
            .flags = CCW_FLAG_CC | CCW_FLAG_SLI,
            .count = 5,
            .cda = addr32(count),
        };
        ccws[2 + 2 * i] = (Ccw1) {
            .cmd_code = ECKD_CMD_SEARCH_KEY_HIGH,
            .flags = CCW_FLAG_CC,
            .count = 44,
            .cda = addr32(data),
        };
    }
    ccws[5] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_COUNT_MT,
        .flags = CCW_FLAG_SLI,
        .count = 5,
        .cda = addr32(count),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 70;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UCHK)) {
        return 71;
    }
    memset(sense, 0, sizeof(sense));
    ccws[0] = (Ccw1) {
        .cmd_code = CCW_CMD_BASIC_SENSE,
        .count = sizeof(sense),
        .cda = addr32(sense),
    };
    irb = (Irb) { 0 };
    if (ssch(schid, &orb)) {
        return 72;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        sense[0] || sense[1] != 0x20) {
        return 73;
    }

    /*
     * File mask seek control "inhibit seek and multitrack" (X'18'): a Seek
     * after Set File Mask is a file-protection violation.
     */
    file_mask = 0x18;
    memset(seek, 0, sizeof(seek));
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SET_FILE_MASK,
        .flags = CCW_FLAG_CC,
        .count = 1,
        .cda = addr32(&file_mask),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK,
        .count = sizeof(seek),
        .cda = addr32(seek),
    };
    if (!check_unit_check_sense(schid, &orb, 0x04)) {
        return 74;
    }

    /*
     * The same mask makes a multitrack track switch file protected: R1 is
     * the last record of track 0/0, so the second Read Count MT would have
     * to move to head 1.
     */
    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek),
        .cda = addr32(seek),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_SET_FILE_MASK,
        .flags = CCW_FLAG_CC,
        .count = 1,
        .cda = addr32(&file_mask),
    };
    ccws[2] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_COUNT_MT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(count),
        .cda = addr32(count),
    };
    ccws[3] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ_COUNT_MT,
        .count = sizeof(count),
        .cda = addr32(count),
    };
    if (!check_unit_check_sense(schid, &orb, 0x04)) {
        return 75;
    }

    /*
     * Locate Record Write Track: z/OS formats JES2 checkpoint tracks with
     * Write Data for R0 followed by Write CKD R1..Rn, each Write CKD's
     * count field data-chained to an IDA continuation holding the data
     * area.  The Write CKD must follow R0, replacing the old R1 and erasing
     * the old track tail.  Run it with format-2 (4K) and format-1 (2K)
     * IDAWs; the first IDAW starts mid-block so the transfer crosses blocks.
     */
    for (int variant = 0; variant < 2; variant++) {
        CmdOrb ida_orb = orb;
        uint8_t *r1 = wt_buf + 2048;
        uint32_t expected = (8 + 8) + (8 + 4096) + (8 + 4) + 8;

        ida_orb.c64 = variant == 0;
        for (int i = 0; i < 4096; i++) {
            r1[i] = (uint8_t)(i * 7 + variant);
        }
        if (variant == 0) {
            idaw2[0] = (unsigned long)r1;
            idaw2[1] = (unsigned long)(wt_buf + 4096);
        } else {
            idaw1[0] = addr32(r1);
            idaw1[1] = addr32(wt_buf + 4096);
            idaw1[2] = addr32(wt_buf + 6144);
        }
        memcpy(wt_r0, variant ? "R0DATA-1" : "R0DATA-0", 8);
        memset(wt_locate, 0, sizeof(wt_locate));
        wt_locate[0] = ECKD_LR_WRITE_TRACK;
        wt_locate[3] = 3;
        wt_locate[5] = 2;
        wt_locate[7] = 1;
        wt_locate[9] = 2;
        wt_locate[11] = 1;
        wt_locate[13] = 0xff;
        wt_locate[14] = 0x10;
        memcpy(wt_count1, "\x00\x02\x00\x01\x01\x00\x10\x00", 8);
        memcpy(wt_ckd2, "\x00\x02\x00\x01\x02\x00\x00\x04", 8);
        memcpy(wt_ckd2 + 8, variant ? "new1" : "new0", 4);
        ccws[0] = (Ccw1) {
            .cmd_code = ECKD_CMD_DEFINE_EXTENT,
            .flags = CCW_FLAG_CC,
            .count = sizeof(wt_extent),
            .cda = addr32(wt_extent),
        };
        ccws[1] = (Ccw1) {
            .cmd_code = ECKD_CMD_LOCATE,
            .flags = CCW_FLAG_CC,
            .count = sizeof(wt_locate),
            .cda = addr32(wt_locate),
        };
        ccws[2] = (Ccw1) {
            .cmd_code = ECKD_CMD_WRITE,
            .flags = CCW_FLAG_CC,
            .count = sizeof(wt_r0),
            .cda = addr32(wt_r0),
        };
        ccws[3] = (Ccw1) {
            .cmd_code = ECKD_CMD_WRITE_CKD,
            .flags = CCW_FLAG_DC,
            .count = sizeof(wt_count1),
            .cda = addr32(wt_count1),
        };
        ccws[4] = (Ccw1) {
            .cmd_code = ECKD_CMD_WRITE,   /* ignored: data chained */
            .flags = CCW_FLAG_CC | CCW_FLAG_IDA,
            .count = 4096,
            .cda = variant ? addr32(idaw1) : addr32(idaw2),
        };
        ccws[5] = (Ccw1) {
            .cmd_code = ECKD_CMD_WRITE_CKD,
            .count = sizeof(wt_ckd2),
            .cda = addr32(wt_ckd2),
        };
        irb = (Irb) { 0 };
        if (ssch(schid, &ida_orb)) {
            return 76;
        }
        consume_io_int();
        if (tsch(schid, &irb) || irb.scsw.cstat ||
            irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
            irb.scsw.count || irb.scsw.cpa != addr32(&ccws[6])) {
            return 77;
        }

        /* Read the whole track back: R0, R1, R2, end of track only. */
        memset(wt_locate, 0, sizeof(wt_locate));
        wt_locate[0] = ECKD_LR_READ_TRACKS;
        wt_locate[3] = 1;
        wt_locate[5] = 2;
        wt_locate[7] = 1;
        wt_locate[9] = 2;
        wt_locate[11] = 1;
        wt_locate[13] = 0xff;
        ccws[2] = (Ccw1) {
            .cmd_code = ECKD_CMD_READ_TRACK,
            .flags = CCW_FLAG_SLI,
            .count = sizeof(rt_buf),
            .cda = addr32(rt_buf),
        };
        memset(rt_buf, 0xa5, sizeof(rt_buf));
        irb = (Irb) { 0 };
        if (ssch(schid, &orb)) {
            return 78;
        }
        consume_io_int();
        if (tsch(schid, &irb) || irb.scsw.cstat ||
            irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
            irb.scsw.count != sizeof(rt_buf) - expected) {
            return 79;
        }
        if (bytes_differ(rt_buf, "\x00\x02\x00\x01\x00\x00\x00\x08", 8) ||
            bytes_differ(rt_buf + 8, wt_r0, 8)) {
            return 80;
        }
        if (bytes_differ(rt_buf + 16, wt_count1, 8) ||
            bytes_differ(rt_buf + 24, r1, 4096)) {
            return 81;
        }
        if (bytes_differ(rt_buf + 4120, wt_ckd2, sizeof(wt_ckd2))) {
            return 82;
        }
        for (int i = 4132; i < 4140; i++) {
            if (rt_buf[i] != 0xff) {
                return 83;
            }
        }
    }

    return 0;
}
