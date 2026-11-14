/*
 * Verify explicit ECKD unit, subsystem, and control-unit topology.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "s390-arch.h"

#define ECKD_CMD_SNSS 0x54
#define ECKD_CMD_RCD  0xfa

typedef struct ChscRequest {
    uint16_t length;
    uint16_t command;
    uint32_t param0;
    uint32_t param1;
    uint32_t param2;
} __attribute__((packed)) ChscRequest;

typedef struct ChscResponse {
    uint16_t length;
    uint16_t code;
    uint32_t param;
    uint8_t data[];
} __attribute__((packed)) ChscResponse;

LowCore *lowcore;
static uint8_t page[4096] __attribute__((aligned(4096)));
static uint8_t data[256] __attribute__((aligned(8)));
static Ccw1 ccw __attribute__((aligned(8)));

static uint32_t addr32(const void *p)
{
    return (uint32_t)(unsigned long)p;
}

static ChscResponse *query_chsc(uint16_t command, uint32_t param0,
                                uint32_t param1)
{
    typedef struct { uint8_t bytes[4096]; } Page;
    ChscRequest *request = (ChscRequest *)page;
    unsigned long cc;

    memset(page, 0xa5, sizeof(page));
    memset(request, 0, sizeof(*request));
    request->length = sizeof(*request);
    request->command = command;
    request->param0 = param0;
    request->param1 = param1;
    asm volatile(
        ".insn rre,0xb25f0000,%2,0\n"
        "ipm %0\n"
        "srl %0,28\n"
        : "=d" (cc), "=m" (*(Page *)page)
        : "d" (page), "m" (*(Page *)page)
        : "cc");
    return cc ? NULL : (ChscResponse *)(page + sizeof(*request));
}

static int run_ccw(SubChannelId schid, CmdOrb *orb, uint8_t command,
                   uint16_t count)
{
    Irb irb = { 0 };

    ccw = (Ccw1) {
        .cmd_code = command,
        .flags = CCW_FLAG_SLI,
        .count = count,
        .cda = addr32(data),
    };
    memset(data, 0xa5, sizeof(data));
    if (ssch(schid, orb)) {
        return 1;
    }
    consume_io_int();
    if (tsch(schid, &irb) || irb.scsw.cstat ||
        irb.scsw.dstat != (SCSW_DSTAT_CHEND | SCSW_DSTAT_DEVEND) ||
        irb.scsw.count) {
        return 1;
    }
    return 0;
}

int main(void)
{
    static const uint8_t expected_neq[32] = {
        0x80, 0x00, 0x02, 0x13, 0x2d, 0x00, 0x1e, 0x00,
        0x0a, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x0c, 0xc0, 0x00, 0x4f, 0xe5, 0x64, 0x1b,
        0x02, 0x13, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x00,
    };
    SubChannelId schid = { .one = 1, .sch_no = 0 };
    CmdOrb orb = { .fmt = 1, .pfch = 1, .lpm = 0xff,
                   .cpa = addr32(&ccw) };
    ChscResponse *response;
    uint8_t *desc;
    Schib schib;

    if (stsch_err(schid, &schib)) {
        return 1;
    }
    schib.pmcw.ena = 1;
    if (msch_err(schid, &schib)) {
        return 2;
    }

    /* SSD format 0: independent unit address and configured FLA. */
    response = query_chsc(0x0004, 0x00400000, 0);
    if (!response || response->code != 1 || response->length != 40) {
        return 3;
    }
    desc = response->data;
    if (desc[0] != 0xc0 || desc[1] != 0x05 ||
        desc[2] != 0x0a || desc[3] != 0x85 || desc[4] != 0x80 ||
        desc[5] != 0x80 || desc[6] || desc[7] || desc[8] != 0x01 ||
        desc[16] != 0x12 || desc[17] != 0x34) {
        return 4;
    }

    /* SSCUD format 0: queue number and per-path CU number. */
    response = query_chsc(0x0006, 0, 0);
    if (!response || response->code != 1 || response->length != 40) {
        return 5;
    }
    desc = response->data;
    if (desc[0] != 0xc0 || desc[1] != 0x80 ||
        desc[2] != 0x0a || desc[3] != 0x85 ||
        desc[4] || desc[5] != 1 || desc[6] || desc[7] ||
        desc[8] != 0x01 || desc[16] || desc[17] != 2) {
        return 6;
    }

    if (run_ccw(schid, &orb, ECKD_CMD_SNSS, 40) ||
        data[0] || data[1] != 0x05 || data[2] != 0x24 || data[3] != 1 ||
        data[38] || data[39] != 3) {
        return 7;
    }

    if (run_ccw(schid, &orb, ECKD_CMD_RCD, sizeof(data))) {
        return 8;
    }
    for (unsigned int i = 0; i < sizeof(expected_neq); i++) {
        if (data[224 + i] != expected_neq[i]) {
            return 8;
        }
    }

    return 0;
}
