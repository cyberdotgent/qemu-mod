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
#define ECKD_CMD_SEEK 0x07
#define ECKD_CMD_SEEK_CYLINDER 0x0b
#define ECKD_CMD_SEEK_HEAD 0x1b
#define ECKD_CMD_READ_COUNT 0x12
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

    ccws[0] = (Ccw1) {
        .cmd_code = ECKD_CMD_SEEK,
        .flags = CCW_FLAG_CC,
        .count = sizeof(seek),
        .cda = addr32(seek),
    };
    ccws[1] = (Ccw1) {
        .cmd_code = ECKD_CMD_READ,
        .flags = CCW_FLAG_SLI,
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
    if (irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UEXCP) ||
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
    return 0;
}
