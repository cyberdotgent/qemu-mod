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

static Ccw1 ccw[2] __attribute__((aligned(8)));
static uint8_t input[0x103];

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
        .cpa = addr32(ccw),
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
     * An already connected display reports unsolicited Device End when the
     * subchannel becomes enabled.  Consume that readiness status before
     * starting the channel program.  Older machine versions may have no
     * status pending here, which is also valid for this data-transfer test.
     */
    {
        int cc = tsch(schid, &irb);

        if (cc != 1 &&
            (cc || irb.scsw.cstat ||
             irb.scsw.dstat != SCSW_DSTAT_DEVEND)) {
            return 3;
        }
        irb = (Irb) { 0 };
    }

    /*
     * SKIP makes the data address immaterial.  VSE uses this exact form to
     * drain a 3270 input record after attention.
     */
    ccw[0] = (Ccw1) {
        .cmd_code = TERM3270_CMD_READ_MODIFIED,
        .flags = CCW_FLAG_SKIP,
        .count = 0x7fff,
        .cda = 0,
    };
    if (ssch(schid, &orb)) {
        return 4;
    }
    consume_io_int();
    if (tsch(schid, &irb)) {
        return 5;
    }
    if (irb.scsw.cstat != SCSW_CSTAT_BADLEN ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != 0x7fff - INPUT_RECORD_SIZE) {
        return 6;
    }

    /*
     * Incorrect length terminates data chaining.  VSE depends on the CPA
     * and residual identifying this first, short Read Modified CCW; running
     * the following drain CCW instead makes the input appear malformed.
     */
    ccw[0] = (Ccw1) {
        .cmd_code = TERM3270_CMD_READ_MODIFIED,
        .flags = CCW_FLAG_DC,
        .count = sizeof(input),
        .cda = addr32(input),
    };
    ccw[1] = (Ccw1) {
        .cmd_code = TERM3270_CMD_READ_MODIFIED,
        .flags = CCW_FLAG_SKIP,
        .count = 0x7fff,
        .cda = 0,
    };
    memset(input, 0, sizeof(input));
    if (ssch(schid, &orb)) {
        return 7;
    }
    consume_io_int();
    memset(&irb, 0, sizeof(irb));
    if (tsch(schid, &irb)) {
        return 8;
    }
    if (irb.scsw.cstat != SCSW_CSTAT_BADLEN ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count != sizeof(input) - INPUT_RECORD_SIZE ||
        irb.scsw.cpa != addr32(&ccw[1]) ||
        input[0] != 0x7d || input[1] != 0x40 || input[2] != 0x40) {
        return 9;
    }
    return 0;
}
