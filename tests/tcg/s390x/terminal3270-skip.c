/*
 * Exercise a skipped IBM 3270 read with a zero data address.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define TERM3270_CMD_READ_MODIFIED 0x06
#define INPUT_RECORD_SIZE 3

LowCore *lowcore;

static Ccw1 ccw __attribute__((aligned(8)));

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
        .cpa = addr32(&ccw),
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
     * SKIP makes the data address immaterial.  VSE uses this exact form to
     * drain a 3270 input record after attention.
     */
    ccw = (Ccw1) {
        .cmd_code = TERM3270_CMD_READ_MODIFIED,
        .flags = CCW_FLAG_SKIP,
        .count = 0x7fff,
        .cda = 0,
    };
    if (ssch(schid, &orb)) {
        return 3;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 4;
    }
    if (irb.scsw.cstat != SCSW_CSTAT_BADLEN ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != 0x7fff - INPUT_RECORD_SIZE) {
        return 5;
    }
    return 0;
}
