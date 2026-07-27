/*
 * Exercise residual status from a disconnected IBM 3270 display.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define TERM3270_CMD_NOP 0x03
#define TERM3270_CMD_SENSE 0x04
#define TERM3270_CMD_SENSE_ID 0xe4

LowCore *lowcore;

static Ccw1 ccw __attribute__((aligned(8)));
static uint8_t sense_id[0x100] __attribute__((aligned(8)));
static uint8_t sense[6] __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
}

static int run_ccw(SubChannelId schid, CmdOrb *orb, Irb *irb)
{
    *irb = (Irb) { 0 };
    if (ssch(schid, orb)) {
        return 1;
    }
    consume_io_int();
    return tsch(schid, irb);
}

int main(void)
{
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    CmdOrb orb = {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(&ccw),
    };
    Schib schib;
    Irb irb;

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }

    ccw = (Ccw1) {
        .cmd_code = TERM3270_CMD_SENSE_ID,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(sense_id),
        .cda = addr32(sense_id),
    };
    if (run_ccw(schid, &orb, &irb) ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.cstat || irb.scsw.count != sizeof(sense_id) - 7) {
        return 3;
    }

    /*
     * The disconnected NOP presents Unit Check as initial status.  Its
     * residual must be its own count, not the Sense-ID residual above.
     */
    ccw = (Ccw1) {
        .cmd_code = TERM3270_CMD_NOP,
        .flags = CCW_FLAG_SLI,
        .count = 1,
        .cda = 0,
    };
    if (run_ccw(schid, &orb, &irb) ||
        irb.scsw.dstat != SCSW_DSTAT_UCHK ||
        irb.scsw.cstat || irb.scsw.count != 1) {
        return 4;
    }

    ccw = (Ccw1) {
        .cmd_code = TERM3270_CMD_SENSE,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(sense),
        .cda = addr32(sense),
    };
    if (run_ccw(schid, &orb, &irb) ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.cstat || irb.scsw.count != sizeof(sense) - 1 ||
        sense[0] != 0x40) {
        return 5;
    }
    return 0;
}
