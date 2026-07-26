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

LowCore *lowcore;

static uint8_t seek[6] __attribute__((aligned(8)));
static uint8_t data[16] __attribute__((aligned(8)));
static Ccw1 ccws[2] __attribute__((aligned(8)));

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
    return 0;
}
