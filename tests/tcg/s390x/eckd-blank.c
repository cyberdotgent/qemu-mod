/*
 * Blank CKD volumes: header-defined size and unformatted track slots.
 *
 * Device 0200 is a two-cylinder CKD64 volume of which only the header was
 * written; its backend has trailing slack.  Every track slot is zero and
 * must read as an empty track (home address, R0, end of track).  A record
 * written to the last track must be readable again; eckd-blank.py then
 * checks the slot on disk and that the slack is untouched.
 *
 * Device 0201 has a track slot holding nonzero garbage, which must still
 * be reported as an invalid track.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define ECKD_CMD_WRITE_CKD 0x1d
#define ECKD_CMD_READ 0x06
#define ECKD_CMD_SEEK 0x07
#define ECKD_CMD_READ_COUNT 0x12
#define ECKD_CMD_READ_R0 0x16
#define ECKD_CMD_READ_HA 0x1a
#define ECKD_CMD_SEARCH_ID_EQ 0x31
#define ECKD_CMD_RDC 0x64

#define SENSE_DATA_CHECK 0x08
#define SENSE1_INVALID_TRACK 0x40
#define SENSE1_END_CYLINDER 0x20
#define SENSE1_NO_RECORD 0x08

/* The last track of the two-cylinder volume: cylinder 1, head 14. */
#define TEST_CYL 1
#define TEST_HEAD 14

LowCore *lowcore;

static uint8_t seek_last[6] __attribute__((aligned(8))) = {
    0, 0, 0, TEST_CYL, 0, TEST_HEAD,
};
static uint8_t seek_beyond[6] __attribute__((aligned(8))) = {
    0, 0, 0, 2, 0, 0,
};
static uint8_t seek_first[6] __attribute__((aligned(8)));
static uint8_t seek_garbage[6] __attribute__((aligned(8))) = {
    0, 0, 0, 0, 0, 1,
};
static uint8_t id_r0[5] __attribute__((aligned(8))) = {
    0, TEST_CYL, 0, TEST_HEAD, 0,
};
static uint8_t id_r1[5] __attribute__((aligned(8))) = {
    0, TEST_CYL, 0, TEST_HEAD, 1,
};
static uint8_t ckd_r1[24] __attribute__((aligned(8))) = {
    0, TEST_CYL, 0, TEST_HEAD, 1, 0, 0, 16,
    'Q', 'E', 'M', 'U', '-', 'E', 'C', 'K',
    'D', '-', 'B', 'L', 'A', 'N', 'K', '!',
};
static uint8_t data[64] __attribute__((aligned(8)));
static uint8_t sense[32] __attribute__((aligned(8)));
static Ccw1 ccws[8] __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
}

static bool bytes_equal(const void *a, const void *b, unsigned int n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;

    for (unsigned int i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return false;
        }
    }
    return true;
}

static Ccw1 ccw(uint8_t cmd, uint8_t flags, uint16_t count, const void *p)
{
    return (Ccw1) {
        .cmd_code = cmd,
        .flags = flags,
        .count = count,
        .cda = addr32(p),
    };
}

/* Run ccws[] and return the device status, or -1 on a channel error. */
static int run(SubChannelId schid, CmdOrb *orb)
{
    Irb irb = { 0 };

    if (ssch(schid, orb)) {
        return -1;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat) {
        return -1;
    }
    return irb.scsw.dstat;
}

static bool run_ok(SubChannelId schid, CmdOrb *orb)
{
    return run(schid, orb) == (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND);
}

/* Run ccws[], expect unit check and the given sense bytes 0 and 1. */
static bool run_unit_check(SubChannelId schid, CmdOrb *orb, uint8_t sense0,
                           uint8_t sense1)
{
    if (run(schid, orb) != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                            SCSW_DSTAT_UCHK)) {
        return false;
    }
    memset(sense, 0, sizeof(sense));
    ccws[0] = ccw(CCW_CMD_BASIC_SENSE, 0, sizeof(sense), sense);
    if (!run_ok(schid, orb)) {
        return false;
    }
    return sense[0] == sense0 && sense[1] == sense1;
}

static bool enable(SubChannelId schid, uint16_t devno)
{
    Schib schib;

    if (stsch_err(schid, &schib) || schib.pmcw.dev != devno) {
        return false;
    }
    schib.pmcw.ena = 1;
    return !msch_err(schid, &schib);
}

static int test_blank(SubChannelId schid, CmdOrb *orb)
{
    static const uint8_t ha[5] = { 0, 0, TEST_CYL, 0, TEST_HEAD };
    static const uint8_t r0[16] = {
        0, TEST_CYL, 0, TEST_HEAD, 0, 0, 0, 8,
    };

    /* The header defines two cylinders, whatever the slack. */
    ccws[0] = ccw(ECKD_CMD_RDC, CCW_FLAG_SLI, 64, data);
    if (!run_ok(schid, orb) || data[12] != 0 || data[13] != 2) {
        return 10;
    }
    ccws[0] = ccw(ECKD_CMD_SEEK, 0, 6, seek_beyond);
    if (!run_unit_check(schid, orb, 0, SENSE1_END_CYLINDER)) {
        return 11;
    }

    /* An unwritten slot reads as home address + R0 + end of track. */
    memset(data, 0xa5, sizeof(data));
    ccws[0] = ccw(ECKD_CMD_SEEK, CCW_FLAG_CC, 6, seek_last);
    ccws[1] = ccw(ECKD_CMD_READ_HA, CCW_FLAG_CC, 5, data);
    ccws[2] = ccw(ECKD_CMD_READ_R0, 0, 16, data + 8);
    if (!run_ok(schid, orb) || !bytes_equal(data, ha, sizeof(ha)) ||
        !bytes_equal(data + 8, r0, sizeof(r0))) {
        return 12;
    }

    /* There is no user record on it. */
    ccws[0] = ccw(ECKD_CMD_SEEK, CCW_FLAG_CC, 6, seek_last);
    ccws[1] = ccw(ECKD_CMD_READ_COUNT, 0, 8, data);
    if (!run_unit_check(schid, orb, 0, SENSE1_NO_RECORD)) {
        return 13;
    }

    /* Format R1 after R0, then read it back. */
    ccws[0] = ccw(ECKD_CMD_SEEK, CCW_FLAG_CC, 6, seek_last);
    ccws[1] = ccw(ECKD_CMD_SEARCH_ID_EQ, CCW_FLAG_CC, 5, id_r0);
    ccws[2] = ccw(CCW_CMD_TIC, 0, 0, &ccws[1]);
    ccws[3] = ccw(ECKD_CMD_WRITE_CKD, 0, sizeof(ckd_r1), ckd_r1);
    if (!run_ok(schid, orb)) {
        return 14;
    }
    memset(data, 0xa5, sizeof(data));
    ccws[0] = ccw(ECKD_CMD_SEEK, CCW_FLAG_CC, 6, seek_last);
    ccws[1] = ccw(ECKD_CMD_SEARCH_ID_EQ, CCW_FLAG_CC, 5, id_r1);
    ccws[2] = ccw(CCW_CMD_TIC, 0, 0, &ccws[1]);
    ccws[3] = ccw(ECKD_CMD_READ, 0, 16, data);
    if (!run_ok(schid, orb) || !bytes_equal(data, ckd_r1 + 8, 16)) {
        return 15;
    }
    ccws[0] = ccw(ECKD_CMD_SEEK, CCW_FLAG_CC, 6, seek_last);
    ccws[1] = ccw(ECKD_CMD_READ_COUNT, 0, 8, data);
    if (!run_ok(schid, orb) || !bytes_equal(data, ckd_r1, 8)) {
        return 16;
    }
    return 0;
}

static int test_garbage(SubChannelId schid, CmdOrb *orb)
{
    ccws[0] = ccw(ECKD_CMD_SEEK, 0, 6, seek_garbage);
    if (!run_unit_check(schid, orb, SENSE_DATA_CHECK, SENSE1_INVALID_TRACK)) {
        return 20;
    }
    /* Its all-zero neighbour is an empty track. */
    ccws[0] = ccw(ECKD_CMD_SEEK, 0, 6, seek_first);
    if (!run_ok(schid, orb)) {
        return 21;
    }
    return 0;
}

int main(void)
{
    SubChannelId blank = { .one = 1, .sch_no = 0 };
    SubChannelId garbage = { .one = 1, .sch_no = 1 };
    CmdOrb orb = {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(ccws),
    };
    int ret;

    if (!enable(blank, 0x0200)) {
        return 1;
    }
    if (!enable(garbage, 0x0201)) {
        return 2;
    }
    ret = test_blank(blank, &orb);
    if (ret) {
        return ret;
    }
    return test_garbage(garbage, &orb);
}
