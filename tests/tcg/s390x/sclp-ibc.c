/*
 * Verify the SCLP IBC level for IBM z14 Model ZR1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

typedef struct ReadInfoIbc {
    SCCBHeader h;
    uint16_t rnmax;
    uint8_t rnsize;
    uint8_t reserved0[4];
    uint8_t stsi_parm;
    uint16_t entries_cpu;
    uint16_t offset_cpu;
    uint8_t reserved1[4];
    uint8_t loadparm[8];
    uint8_t reserved2[16];
    uint64_t facilities;
    uint8_t reserved3[20];
    uint32_t ibc;
} __attribute__((packed)) ReadInfoIbc;

static uint8_t sccb_page[SCCB_SIZE] __attribute__((aligned(PAGE_SIZE)));
void ml_printf(const char *format, ...);

static int read_scp_info(ReadInfoIbc **info)
{
    register unsigned long command asm("0") = SCLP_CMDW_READ_SCP_INFO;
    register unsigned long sccb_addr asm("1") =
        (unsigned long)__pa(sccb_page);
    unsigned long cc;

    memset(sccb_page, 0, sizeof(sccb_page));
    *info = (ReadInfoIbc *)sccb_page;
    (*info)->h.length = sizeof(sccb_page);
    asm volatile(
        ".insn rre,0xb2200000,%1,%2\n"
        "ipm %0\n"
        "srl %0,28"
        : "=&d" (cc)
        : "d" (command), "a" (sccb_addr)
        : "cc", "memory");
    consume_sclp_int();
    return cc;
}

static uint64_t store_cpu_id(void)
{
    uint64_t cpuid;

    asm volatile("stidp %0" : "=Q" (cpuid));
    return cpuid;
}

int main(void)
{
    ReadInfoIbc *info;
    uint64_t cpuid = store_cpu_id();
    uint16_t type = cpuid >> 16;
    int cc;

    /* This regression is specific to the type-3907 KVM host model. */
    if (type != 0x3907) {
        return 0;
    }

    cc = read_scp_info(&info);
    if (cc || info->h.response_code != SCLP_RC_NORMAL_READ_COMPLETION) {
        ml_printf("cc=%d response=%04x\n", cc, info->h.response_code);
        return 1;
    }
    if ((info->ibc & 0xfff) != 0x42) {
        ml_printf("cpuid=%lx ibc=%08x expected-unblocked=042\n",
                  cpuid, info->ibc);
        return 2;
    }
    return 0;
}
