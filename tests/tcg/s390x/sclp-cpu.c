/*
 * Verify the offsets in SCLP Read CPU Information.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

#define SCLP_READ_CPU_INFO 0x00010001

typedef struct CPUEntry {
    uint8_t address;
    uint8_t reserved0;
    uint8_t features[6];
    uint8_t reserved2[6];
    uint8_t type;
    uint8_t reserved1;
} __attribute__((packed)) CPUEntry;

typedef struct ReadCpuInfo {
    SCCBHeader h;
    uint16_t nr_configured;
    uint16_t offset_configured;
    uint16_t nr_standby;
    uint16_t offset_standby;
    uint8_t reserved0[8];
    CPUEntry entries[1];
} __attribute__((packed)) ReadCpuInfo;

static ReadCpuInfo cpu_info __attribute__((aligned(PAGE_SIZE)));
void ml_printf(const char *format, ...);

static int read_cpu_info(void)
{
    register unsigned long command asm("0") = SCLP_READ_CPU_INFO;
    register unsigned long sccb_addr asm("1") =
        (unsigned long)__pa(&cpu_info);
    unsigned long cc;

    cpu_info.h.length = sizeof(cpu_info);
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

int main(void)
{
    int cc = read_cpu_info();

    if (cc || cpu_info.h.response_code != SCLP_RC_NORMAL_READ_COMPLETION) {
        ml_printf("cc=%d response=%04x\n", cc, cpu_info.h.response_code);
        return 1;
    }
    if (cpu_info.nr_configured != 1 || cpu_info.offset_configured != 24 ||
        cpu_info.nr_standby != 0 || cpu_info.offset_standby != 40) {
        ml_printf("configured=%u/%u standby=%u/%u\n",
                  cpu_info.nr_configured, cpu_info.offset_configured,
                  cpu_info.nr_standby, cpu_info.offset_standby);
        return 2;
    }
    return 0;
}
