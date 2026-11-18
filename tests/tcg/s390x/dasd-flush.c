/*
 * DASD write hardening: a channel program that wrote anything flushes the
 * backend before its final status; a read-only program does not.
 *
 * Devices (see dasd-flush.py, which also checks the flush counters):
 *   0200 ECKD, read-only programs        0201 ECKD, write programs
 *   0202 FBA,  read-only programs        0203 FBA,  write programs
 *   0204 ECKD, flush fails (blkdebug)    0205 FBA,  flush fails (blkdebug)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define ECKD_CMD_WRITE_CKD 0x1d
#define ECKD_CMD_SEEK 0x07
#define ECKD_CMD_READ_COUNT 0x12
#define ECKD_CMD_READ_R0 0x16
#define ECKD_CMD_READ_HA 0x1a
#define ECKD_CMD_SEARCH_ID_EQ 0x31

#define FBA_CMD_NOP 0x03
#define FBA_CMD_WRITE 0x41
#define FBA_CMD_READ 0x42
#define FBA_CMD_LOCATE 0x43
#define FBA_CMD_DEFINE_EXTENT 0x63

#define SENSE_EQUIPMENT_CHECK 0x10

LowCore *lowcore;

static uint8_t seek[6] __attribute__((aligned(8)));
static uint8_t search_r0[5] __attribute__((aligned(8)));
static uint8_t ckd_r1[16] __attribute__((aligned(8)));
static uint8_t fba_extent[16] __attribute__((aligned(8))) = {
    0, 0, 0, 0,
    0, 0, 0, 0,               /* extent starts at block 0 */
    0, 0, 0, 0,               /* first block */
    0, 0, 0, 63,              /* last block */
};
static uint8_t fba_locate[8] __attribute__((aligned(8)));
static uint8_t data[512] __attribute__((aligned(8)));
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

static int run(SubChannelId schid)
{
    CmdOrb orb = {
        .fmt = 1,
        .pfch = 1,
        .lpm = 0xff,
        .cpa = addr32(ccws),
    };
    Irb irb = { 0 };

    if (ssch(schid, &orb)) {
        return -1;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat) {
        return -1;
    }
    return irb.scsw.dstat;
}

static bool run_ok(SubChannelId schid)
{
    return run(schid) == (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND);
}

static bool run_equipment_check(SubChannelId schid)
{
    if (run(schid) != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND |
                       SCSW_DSTAT_UCHK)) {
        return false;
    }
    memset(sense, 0, sizeof(sense));
    ccws[0] = ccw(CCW_CMD_BASIC_SENSE, CCW_FLAG_SLI, sizeof(sense), sense);
    return run_ok(schid) && sense[0] == SENSE_EQUIPMENT_CHECK;
}

static bool enable(uint16_t sch_no, uint16_t devno)
{
    SubChannelId schid = { .one = 1, .sch_no = sch_no };
    Schib schib;

    if (stsch_err(schid, &schib) || schib.pmcw.dev != devno) {
        return false;
    }
    schib.pmcw.ena = 1;
    return !msch_err(schid, &schib);
}

static void eckd_position(uint16_t head)
{
    memset(seek, 0, sizeof(seek));
    seek[5] = head;
    memset(search_r0, 0, sizeof(search_r0));
    search_r0[3] = head;
}

/* Seek, Read HA, Read R0: reads only. */
static void eckd_read_program(uint16_t head)
{
    eckd_position(head);
    ccws[0] = ccw(ECKD_CMD_SEEK, CCW_FLAG_CC, 6, seek);
    ccws[1] = ccw(ECKD_CMD_READ_HA, CCW_FLAG_CC, 5, data);
    ccws[2] = ccw(ECKD_CMD_READ_R0, 0, 16, data + 8);
}

/*
 * Seek, Search ID R0, Write CKD R1; optionally followed by a chained Read
 * Count of R1 so that the last CCW is not the write.
 */
static void eckd_write_program(uint16_t head, bool read_after)
{
    eckd_position(head);
    memset(ckd_r1, 0, sizeof(ckd_r1));
    ckd_r1[3] = head;
    ckd_r1[4] = 1;
    ckd_r1[7] = 8;
    memcpy(ckd_r1 + 8, "HARDENED", 8);
    ccws[0] = ccw(ECKD_CMD_SEEK, CCW_FLAG_CC, 6, seek);
    ccws[1] = ccw(ECKD_CMD_SEARCH_ID_EQ, CCW_FLAG_CC, 5, search_r0);
    ccws[2] = ccw(CCW_CMD_TIC, 0, 0, &ccws[1]);
    ccws[3] = ccw(ECKD_CMD_WRITE_CKD, read_after ? CCW_FLAG_CC : 0,
                  sizeof(ckd_r1), ckd_r1);
    ccws[4] = ccw(ECKD_CMD_READ_COUNT, 0, 8, data);
}

/* Define Extent, Locate, then Read or Write of one block, maybe + NOP. */
static void fba_program(bool write, uint32_t block, bool nop_after)
{
    memset(fba_locate, 0, sizeof(fba_locate));
    fba_locate[0] = write ? 0x01 : 0x06;
    fba_locate[3] = 1;
    fba_locate[7] = block;
    if (write) {
        memset(data, 0x5a, sizeof(data));
    }
    ccws[0] = ccw(FBA_CMD_DEFINE_EXTENT, CCW_FLAG_CC, 16, fba_extent);
    ccws[1] = ccw(FBA_CMD_LOCATE, CCW_FLAG_CC, 8, fba_locate);
    ccws[2] = ccw(write ? FBA_CMD_WRITE : FBA_CMD_READ,
                  nop_after ? CCW_FLAG_CC : 0, 512, data);
    ccws[3] = ccw(FBA_CMD_NOP, CCW_FLAG_SLI, 1, data);
}

int main(void)
{
    SubChannelId eckd_ro = { .one = 1, .sch_no = 0 };
    SubChannelId eckd_rw = { .one = 1, .sch_no = 1 };
    SubChannelId fba_ro = { .one = 1, .sch_no = 2 };
    SubChannelId fba_rw = { .one = 1, .sch_no = 3 };
    SubChannelId eckd_err = { .one = 1, .sch_no = 4 };
    SubChannelId fba_err = { .one = 1, .sch_no = 5 };

    for (uint16_t i = 0; i < 6; i++) {
        if (!enable(i, 0x0200 + i)) {
            return 1;
        }
    }

    /* Read-only programs never flush. */
    eckd_read_program(0);
    if (!run_ok(eckd_ro)) {
        return 10;
    }
    eckd_read_program(1);
    if (!run_ok(eckd_ro)) {
        return 11;
    }
    fba_program(false, 0, false);
    if (!run_ok(fba_ro)) {
        return 12;
    }
    fba_program(false, 1, true);
    if (!run_ok(fba_ro)) {
        return 13;
    }

    /* Three ECKD write programs flush three times; reads add nothing. */
    eckd_write_program(0, false);
    if (!run_ok(eckd_rw)) {
        return 20;
    }
    eckd_write_program(1, true);
    if (!run_ok(eckd_rw) || !bytes_equal(data, ckd_r1, 8)) {
        return 21;
    }
    eckd_read_program(1);
    if (!run_ok(eckd_rw)) {
        return 22;
    }
    eckd_write_program(2, false);
    if (!run_ok(eckd_rw)) {
        return 23;
    }

    /* Two FBA write programs flush twice; reads add nothing. */
    fba_program(true, 0, false);
    if (!run_ok(fba_rw)) {
        return 30;
    }
    fba_program(false, 0, false);
    if (!run_ok(fba_rw)) {
        return 31;
    }
    fba_program(true, 1, true);
    if (!run_ok(fba_rw)) {
        return 32;
    }

    /* A failed flush is reported as an equipment check. */
    eckd_read_program(0);
    if (!run_ok(eckd_err)) {
        return 40;
    }
    eckd_write_program(0, false);
    if (!run_equipment_check(eckd_err)) {
        return 41;
    }
    fba_program(false, 0, false);
    if (!run_ok(fba_err)) {
        return 50;
    }
    fba_program(true, 0, false);
    if (!run_equipment_check(fba_err)) {
        return 51;
    }
    return 0;
}
