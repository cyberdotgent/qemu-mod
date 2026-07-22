/*
 * Exercise the IBM 3215 CCW command interface.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"
#include "minilib.h"

#define TERM3215_CMD_WRITE         0x01
#define TERM3215_CMD_SENSE         0x04
#define TERM3215_CMD_WRITE_CR      0x09
#define TERM3215_CMD_READ_INQUIRY  0x0a
#define TERM3215_CMD_SENSE_ID      0xe4

LowCore *lowcore;

static uint8_t sense_id[7] __attribute__((aligned(8)));
static uint8_t sense[1] __attribute__((aligned(8)));
static uint8_t input[150] __attribute__((aligned(8)));
static uint8_t text[] __attribute__((aligned(8))) = {
    0xd8, 0xc5, 0xd4, 0xe4, 0x40, 0xf3, 0xf2, 0xf1, 0xf5,
    0x40, 0xd6, 0xd2,
};
static Ccw1 ccws[2] __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
}

static int run_io(SubChannelId schid, Ccw1 *chain, Irb *irb)
{
    CmdOrb orb = {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(chain),
    };

    if (ssch(schid, &orb)) {
        return 1;
    }
    consume_io_int();
    return tsch(schid, irb) ? 2 : 0;
}

int main(void)
{
    static const uint8_t expected_id[] = {
        0xff, 0x32, 0x15, 0x00, 0x32, 0x15, 0x00,
    };
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    Schib schib;
    Irb irb = { 0 };
    CmdOrb orb;
    unsigned int i;
    int ret;

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }

    ccws[0] = (Ccw1) {
        .cmd_code = TERM3215_CMD_SENSE_ID,
        .count = sizeof(sense_id),
        .cda = addr32(sense_id),
    };
    ret = run_io(schid, ccws, &irb);
    if (ret || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND)) {
        return 3;
    }
    for (i = 0; i < sizeof(sense_id); i++) {
        if (sense_id[i] != expected_id[i]) {
            return 3;
        }
    }

    irb = (Irb) { 0 };
    ccws[0] = (Ccw1) {
        .cmd_code = TERM3215_CMD_WRITE_CR,
        .count = sizeof(text),
        .cda = addr32(text),
    };
    ret = run_io(schid, ccws, &irb);
    if (ret || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND)) {
        ml_printf("3215 write: ret=%d cstat=%x dstat=%x count=%x\n", ret,
                  irb.scsw.cstat, irb.scsw.dstat, irb.scsw.count);
        return 4;
    }

    irb = (Irb) { 0 };
    ccws[0] = (Ccw1) {
        .cmd_code = 0xff,
        .count = 1,
        .cda = addr32(sense),
    };
    ret = run_io(schid, ccws, &irb);
    if (ret || !(irb.scsw.dstat & SCSW_DSTAT_UCHK)) {
        return 5;
    }

    irb = (Irb) { 0 };
    ccws[0] = (Ccw1) {
        .cmd_code = TERM3215_CMD_SENSE,
        .count = sizeof(sense),
        .cda = addr32(sense),
    };
    ret = run_io(schid, ccws, &irb);
    if (ret || sense[0] != 0x80) {
        return 6;
    }

    /* A waiting Read Inquiry must be cleanly cancellable by Clear. */
    ccws[0] = (Ccw1) {
        .cmd_code = TERM3215_CMD_READ_INQUIRY,
        .count = sizeof(input),
        .cda = addr32(input),
    };
    orb = (CmdOrb) {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(ccws),
    };
    if (ssch(schid, &orb) || csch(schid)) {
        return 7;
    }
    consume_io_int();
    irb = (Irb) { 0 };
    if (tsch(schid, &irb)) {
        return 8;
    }

    return 0;
}
