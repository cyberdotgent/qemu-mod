/*
 * Exercise AWS-backed tape write, overwrite, tape-mark and readback.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define TAPE_WRITE          0x01
#define TAPE_SENSE          0x04
#define TAPE_READ_FORWARD   0x06
#define TAPE_REWIND         0x07
#define TAPE_WRITE_MARK     0x1f
#define TAPE_FORWARD_BLOCK  0x37
#define TAPE_SYNCHRONIZE    0x43

LowCore *lowcore;

static Ccw1 ccw __attribute__((aligned(8)));
static uint8_t read_buffer[64] __attribute__((aligned(8)));
static const uint8_t first[] = "first-record";
static const uint8_t obsolete[] = "obsolete-record";
static const uint8_t old_tail[] = "obsolete-tail";
static const uint8_t replacement[] = "replacement";
static const uint8_t final[] = "final-record";

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

    *irb = (Irb) { 0 };
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

static int command(SubChannelId schid, uint8_t code, const void *data,
                   uint16_t count, Irb *irb)
{
    ccw = (Ccw1) {
        .cmd_code = code,
        .flags = CCW_FLAG_SLI,
        .count = count,
        .cda = addr32(data ?: &ccw),
    };
    return run_io(schid, irb);
}

static int normal(SubChannelId schid, uint8_t code, const void *data,
                  uint16_t count)
{
    Irb irb;
    int ret = command(schid, code, data, count, &irb);

    return ret || irb.scsw.cstat ||
           irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND);
}

static int read_record(SubChannelId schid, const uint8_t *expected,
                       uint16_t length)
{
    Irb irb;
    unsigned int i;

    for (i = 0; i < sizeof(read_buffer); i++) {
        read_buffer[i] = 0;
    }
    if (command(schid, TAPE_READ_FORWARD, read_buffer,
                sizeof(read_buffer), &irb) ||
        irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND)) {
        return 1;
    }
    for (i = 0; i < length; i++) {
        if (read_buffer[i] != expected[i]) {
            return 1;
        }
    }
    return 0;
}

static int setup_subchannel(SubChannelId schid)
{
    Schib schib;

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }
    return 0;
}

#ifdef TAPE_WRITE_PROTECT_TEST
int main(void)
{
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    uint8_t sense[32] = { 0 };
    Irb irb;

    if (setup_subchannel(schid)) {
        return 1;
    }
    if (command(schid, TAPE_WRITE, first, sizeof(first) - 1, &irb) ||
        irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UCHK)) {
        return 2;
    }
    if (command(schid, TAPE_SENSE, sense, sizeof(sense), &irb) ||
        irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        sense[0] != 0x80 || sense[3] != 0x30) {
        return 3;
    }
    return 0;
}
#else
int main(void)
{
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    Irb irb;

    if (setup_subchannel(schid)) {
        return 1;
    }

    if (normal(schid, TAPE_WRITE, first, sizeof(first) - 1) ||
        normal(schid, TAPE_WRITE, obsolete, sizeof(obsolete) - 1) ||
        normal(schid, TAPE_WRITE_MARK, NULL, 0) ||
        normal(schid, TAPE_WRITE, old_tail, sizeof(old_tail) - 1) ||
        normal(schid, TAPE_SYNCHRONIZE, NULL, 0) ||
        normal(schid, TAPE_REWIND, NULL, 0) ||
        normal(schid, TAPE_FORWARD_BLOCK, NULL, 0) ||
        normal(schid, TAPE_WRITE, replacement, sizeof(replacement) - 1) ||
        normal(schid, TAPE_WRITE_MARK, NULL, 0) ||
        normal(schid, TAPE_WRITE, final, sizeof(final) - 1) ||
        normal(schid, TAPE_SYNCHRONIZE, NULL, 0) ||
        normal(schid, TAPE_REWIND, NULL, 0)) {
        return 3;
    }

    if (read_record(schid, first, sizeof(first) - 1) ||
        read_record(schid, replacement, sizeof(replacement) - 1)) {
        return 4;
    }
    if (command(schid, TAPE_READ_FORWARD, read_buffer,
                sizeof(read_buffer), &irb) ||
        irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                           SCSW_DSTAT_UEXCP)) {
        return 5;
    }
    if (read_record(schid, final, sizeof(final) - 1)) {
        return 6;
    }
    return 0;
}
#endif
