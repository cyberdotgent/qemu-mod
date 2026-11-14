/*
 * Exercise an IBM 3270 short read: the Clear key sends only its AID.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define TERM3270_CMD_SENSE 0x04
#define TERM3270_CMD_READ_MODIFIED 0x06
#define AID_CLEAR 0x6d
#define AID_ENTER 0x7d

LowCore *lowcore;

static Ccw1 ccw __attribute__((aligned(8)));
static uint8_t input[0x103] __attribute__((aligned(8)));
static uint8_t sense[1] __attribute__((aligned(8)));

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

static int read_modified(SubChannelId schid, CmdOrb *orb, Irb *irb,
                         uint8_t flags)
{
    memset(input, 0, sizeof(input));
    ccw = (Ccw1) {
        .cmd_code = TERM3270_CMD_READ_MODIFIED,
        .flags = flags,
        .count = sizeof(input),
        .cda = addr32(input),
    };
    return run_ccw(schid, orb, irb);
}

static bool ended_normally(Irb *irb, uint8_t cstat, uint16_t residual)
{
    return irb->scsw.dstat == (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) &&
           irb->scsw.cstat == cstat && irb->scsw.count == residual;
}

static bool sense_is_clean(SubChannelId schid, CmdOrb *orb)
{
    Irb irb;

    sense[0] = 0xff;
    ccw = (Ccw1) {
        .cmd_code = TERM3270_CMD_SENSE,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(sense),
        .cda = addr32(sense),
    };
    return !run_ccw(schid, orb, &irb) &&
           ended_normally(&irb, 0, 0) && sense[0] == 0;
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
    int cc;

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }
    /* Consume the Device End that reports the already connected display. */
    cc = tsch(schid, &irb);
    if (cc != 1 &&
        (cc || irb.scsw.cstat || irb.scsw.dstat != SCSW_DSTAT_DEVEND)) {
        return 3;
    }

    /*
     * Solicited Read Modified answered by Clear: one byte, a normal ending
     * with incorrect length (no SLI) and the residual, never a unit check.
     */
    if (read_modified(schid, &orb, &irb, 0)) {
        return 4;
    }
    if (!ended_normally(&irb, SCSW_CSTAT_BADLEN, sizeof(input) - 1) ||
        input[0] != AID_CLEAR || input[1] != 0) {
        return 5;
    }
    if (!sense_is_clean(schid, &orb)) {
        return 6;
    }

    /*
     * The next read works.  The client first sends an empty record, which
     * must not complete the read, then Enter with its cursor address.
     */
    if (read_modified(schid, &orb, &irb, CCW_FLAG_SLI)) {
        return 7;
    }
    if (!ended_normally(&irb, 0, sizeof(input) - 3) ||
        input[0] != AID_ENTER || input[1] != 0x40 || input[2] != 0x40) {
        return 8;
    }

    /* An unsolicited Clear raises Attention and reads back as its AID. */
    consume_io_int();
    irb = (Irb) { 0 };
    if (tsch(schid, &irb) || irb.scsw.dstat != SCSW_DSTAT_ATTN) {
        return 9;
    }
    if (read_modified(schid, &orb, &irb, CCW_FLAG_SLI)) {
        return 10;
    }
    if (!ended_normally(&irb, 0, sizeof(input) - 1) ||
        input[0] != AID_CLEAR) {
        return 11;
    }
    if (!sense_is_clean(schid, &orb)) {
        return 12;
    }

    /* The terminal is still usable after the attention-driven Clear. */
    if (read_modified(schid, &orb, &irb, CCW_FLAG_SLI)) {
        return 13;
    }
    if (!ended_normally(&irb, 0, sizeof(input) - 3) ||
        input[0] != AID_ENTER) {
        return 14;
    }
    return 0;
}
