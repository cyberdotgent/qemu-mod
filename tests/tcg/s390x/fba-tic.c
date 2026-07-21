/*
 * Test FBA data chaining across Transfer in Channel.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define FBA_CMD_WRITE          0x41
#define FBA_CMD_LOCATE         0x43
#define FBA_CMD_DEFINE_EXTENT  0x63

LowCore *lowcore;

static uint8_t define_extent[16] __attribute__((aligned(8))) = {
    0xc0, 0, 0, 0,             /* permit all writes */
    0, 0, 0, 0,                /* extent begins at device block zero */
    0, 0, 0, 0,                /* dataset block zero */
    0, 0, 0, 1,                /* two-block extent */
};
static uint8_t locate[8] __attribute__((aligned(8))) = {
    0x01, 0, 0, 2,             /* write two blocks */
    0, 0, 0, 0,                /* starting at dataset block zero */
};
static uint8_t data[1024] __attribute__((aligned(8)));
static Ccw1 write_chain[6] __attribute__((aligned(8)));
static Ccw1 consecutive_tics[4] __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
}

static int run_io(SubChannelId schid, Ccw1 *ccws, Irb *irb)
{
    CmdOrb orb = {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(ccws),
    };

    if (ssch(schid, &orb)) {
        return 1;
    }
    consume_io_int();
    if (lowcore->subchannel_id != schid.sch_id ||
        lowcore->subchannel_nr != schid.sch_no) {
        return 2;
    }
    return tsch(schid, irb) ? 3 : 0;
}

int main(void)
{
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    Schib schib;
    Irb irb = { 0 };
    int ret;

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }

    write_chain[0] = (Ccw1) {
        .cmd_code = FBA_CMD_DEFINE_EXTENT,
        .flags = CCW_FLAG_CC,
        .count = sizeof(define_extent),
        .cda = addr32(define_extent),
    };
    write_chain[1] = (Ccw1) {
        .cmd_code = FBA_CMD_LOCATE,
        .flags = CCW_FLAG_CC,
        .count = sizeof(locate),
        .cda = addr32(locate),
    };
    write_chain[2] = (Ccw1) {
        .cmd_code = FBA_CMD_WRITE,
        .flags = CCW_FLAG_DC | CCW_FLAG_CC,
        .count = 512,
        .cda = addr32(data),
    };
    write_chain[3] = (Ccw1) {
        .cmd_code = CCW_CMD_TIC,
        .cda = addr32(&write_chain[5]),
    };
    write_chain[5] = (Ccw1) {
        .cmd_code = FBA_CMD_WRITE,
        .flags = CCW_FLAG_SLI,
        .count = 512,
        .cda = addr32(data + 512),
    };

    ret = run_io(schid, write_chain, &irb);
    if (ret || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND)) {
        return 3;
    }

    consecutive_tics[0] = (Ccw1) {
        .cmd_code = CCW_CMD_NOOP,
        .flags = CCW_FLAG_CC,
        .count = 1,
        .cda = addr32(&consecutive_tics[0]),
    };
    consecutive_tics[1] = (Ccw1) {
        .cmd_code = CCW_CMD_TIC,
        .cda = addr32(&consecutive_tics[2]),
    };
    consecutive_tics[2] = (Ccw1) {
        .cmd_code = CCW_CMD_TIC,
        .cda = addr32(&consecutive_tics[3]),
    };
    consecutive_tics[3] = (Ccw1) {
        .cmd_code = CCW_CMD_NOOP,
        .count = 1,
        .cda = addr32(&consecutive_tics[3]),
    };

    irb = (Irb) { 0 };
    ret = run_io(schid, consecutive_tics, &irb);
    if (ret || irb.scsw.cstat != SCSW_CSTAT_PROGCHK || irb.scsw.dstat) {
        return 4;
    }

    return 0;
}
