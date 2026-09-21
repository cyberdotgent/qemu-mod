/*
 * Exercise line input and asynchronous read on the IBM 3215 CCW console.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define TERM3215_CMD_SENSE_ID      0xe4
#define TERM3215_CMD_READ_INQUIRY  0x0a

LowCore *lowcore;

static uint8_t sense_id[7] __attribute__((aligned(8)));
static uint8_t input[150] __attribute__((aligned(8)));
static Ccw1 ccw __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
}

static int run_io(SubChannelId schid, Irb *irb)
{
    CmdOrb orb = {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(&ccw),
    };

    if (ssch(schid, &orb)) {
        return 1;
    }
    consume_io_int();
    return tsch(schid, irb) ? 2 : 0;
}

int main(void)
{
    static const uint8_t expected[] = { 0xf0, 0x40, 0xa8, 0x85, 0xa2 };
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    Schib schib;
    Irb irb = { 0 };
    unsigned int i;
    int ret;

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }

    ccw = (Ccw1) {
        .cmd_code = TERM3215_CMD_SENSE_ID,
        .count = sizeof(sense_id),
        .cda = addr32(sense_id),
    };
    ret = run_io(schid, &irb);
    if (ret || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND)) {
        return 3;
    }

    /* The host deliberately supplies this line after Read Inquiry waits. */
    irb = (Irb) { 0 };
    ccw = (Ccw1) {
        .cmd_code = TERM3215_CMD_READ_INQUIRY,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(input),
        .cda = addr32(input),
    };
    ret = run_io(schid, &irb);
    if (ret || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(input) - sizeof(expected)) {
        return 4;
    }
    for (i = 0; i < sizeof(expected); i++) {
        if (input[i] != expected[i]) {
            return 5;
        }
    }
    return 0;
}
