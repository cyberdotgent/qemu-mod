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
#define ECKD_CMD_WRITE_DATA_SPECIAL 0xa5
#define ECKD_CMD_READ_DATA_SPECIAL 0xa6
#define ECKD_CMD_WRITE_DATA_SPECIAL_2 0xb5
#define ECKD_CMD_READ_DATA_SPECIAL_2 0xb6
#define ECKD_CMD_READ_TRACK 0xde
#define ECKD_CMD_LOCATE 0x47
#define ECKD_CMD_DEFINE_EXTENT 0x63

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
static uint8_t data[128] __attribute__((aligned(8)));
static uint8_t sense[32] __attribute__((aligned(8)));
static uint8_t active_chpids[32] __attribute__((aligned(32)));
static Ccw1 ccws[4] __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
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
                          SCSW_SCTL_SECONDARY | SCSW_SCTL_STATUS_PEND)) !=
        (SCSW_SCTL_INTERMED | SCSW_SCTL_PRIMARY |
         SCSW_SCTL_SECONDARY | SCSW_SCTL_STATUS_PEND) ||
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

    return 0;
}
