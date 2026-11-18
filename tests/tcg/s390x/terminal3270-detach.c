/*
 * A persistent 3270 terminal outlives its TN3270 client.
 *
 * The guest writes a screen and starts Read Modified; the wrapper's client
 * receives both and disconnects without answering.  A new client gets the
 * screen replayed (not the connection banner) and the read, and answers
 * Enter.  The guest then writes more, and reads again while the client
 * disconnects and a third one attaches.  No I/O ends with Unit Check and
 * no reconnect presents status to the guest.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define TERM3270_CMD_WRITE 0x01
#define TERM3270_CMD_ERASE_WRITE 0x05
#define TERM3270_CMD_READ_MODIFIED 0x06
#define AID_ENTER 0x7d

LowCore *lowcore;

static Ccw1 ccw __attribute__((aligned(8)));
static uint8_t input[0x103] __attribute__((aligned(8)));
static uint8_t output[4] __attribute__((aligned(8)));

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

static bool ended_normally(Irb *irb, uint8_t cstat, uint16_t residual)
{
    return irb->scsw.dstat == (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) &&
           irb->scsw.cstat == cstat && irb->scsw.count == residual;
}

/* Write "XXX" (EBCDIC character @c) with a WCC that restores the keyboard */
static bool write_screen(SubChannelId schid, CmdOrb *orb, uint8_t command,
                         uint8_t c)
{
    Irb irb;

    output[0] = 0xc2;
    output[1] = output[2] = output[3] = c;
    ccw = (Ccw1) {
        .cmd_code = command,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(output),
        .cda = addr32(output),
    };
    return !run_ccw(schid, orb, &irb) && ended_normally(&irb, 0, 0);
}

static bool read_enter(SubChannelId schid, CmdOrb *orb)
{
    Irb irb;

    memset(input, 0, sizeof(input));
    ccw = (Ccw1) {
        .cmd_code = TERM3270_CMD_READ_MODIFIED,
        .flags = CCW_FLAG_SLI,
        .count = sizeof(input),
        .cda = addr32(input),
    };
    return !run_ccw(schid, orb, &irb) &&
           ended_normally(&irb, 0, sizeof(input) - 3) &&
           input[0] == AID_ENTER;
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
    /* Consume the Device End that reports the first client. */
    cc = tsch(schid, &irb);
    if (cc != 1 &&
        (cc || irb.scsw.cstat || irb.scsw.dstat != SCSW_DSTAT_DEVEND)) {
        return 3;
    }

    if (!write_screen(schid, &orb, TERM3270_CMD_ERASE_WRITE, 0xc1)) {
        return 4;
    }
    /* The first client leaves; the second one answers. */
    if (!read_enter(schid, &orb)) {
        return 5;
    }
    /* Written while the second client leaves, read by the third. */
    if (!write_screen(schid, &orb, TERM3270_CMD_WRITE, 0xc2)) {
        return 6;
    }
    if (!read_enter(schid, &orb)) {
        return 7;
    }
    /* The reconnects presented no status. */
    if (tsch(schid, &irb) != 1) {
        return 8;
    }
    return 0;
}
