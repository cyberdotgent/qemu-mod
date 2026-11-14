/*
 * Report the machine identity a guest sees on the mtm-3907 personality.
 *
 * The companion mtm-3907.py launches this guest under KVM and compares the
 * printed values with the host's /proc/sysinfo and memory block size.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

static uint8_t sccb_page[SCCB_SIZE] __attribute__((aligned(PAGE_SIZE)));
static uint8_t sysib[4096] __attribute__((aligned(4096)));
void ml_printf(const char *format, ...);

static int read_scp_info(void)
{
    register unsigned long command asm("0") = SCLP_CMDW_READ_SCP_INFO;
    register unsigned long sccb_addr asm("1") =
        (unsigned long)__pa(sccb_page);
    unsigned long cc;

    memset(sccb_page, 0, sizeof(sccb_page));
    ((SCCBHeader *)sccb_page)->length = sizeof(sccb_page);
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

static int stsi_raw(unsigned long fc, unsigned long sel1, unsigned long sel2)
{
    /* No calls between loading GR0/GR1 and the instruction. */
    register unsigned long r0 asm("0") = (fc << 28) | sel1;
    register unsigned long r1 asm("1") = sel2;
    int cc;

    asm volatile(
        "stsi 0(%3)\n"
        "ipm %0\n"
        "srl %0,28"
        : "=d" (cc)
        : "d" (r0), "d" (r1), "a" (sysib)
        : "cc", "memory");
    return cc;
}

static int stsi(unsigned long fc, unsigned long sel1, unsigned long sel2)
{
    memset(sysib, 0, sizeof(sysib));
    return stsi_raw(fc, sel1, sel2);
}

/* The firmware printf has no field widths, so emit hex digits directly. */
static void print_hex(const char *label, const uint8_t *p, int len)
{
    static const char digits[] = "0123456789abcdef";

    ml_printf("%s=", label);
    for (int i = 0; i < len; i++) {
        ml_printf("%c%c", digits[p[i] >> 4], digits[p[i] & 0xf]);
    }
    ml_printf("\n");
}

int main(void)
{
    uint64_t cpuid;
    uint32_t rnsize, rnsize2;
    uint16_t rnmax;
    uint64_t rnmax2;
    int cc;

    asm volatile("stidp %0" : "=Q" (cpuid));
    print_hex("stidp", (const uint8_t *)&cpuid, sizeof(cpuid));

    cc = stsi(1, 1, 1);
    ml_printf("stsi111-cc=%d\n", cc);
    print_hex("stsi111-type", sysib + 48, 4);
    print_hex("stsi111-sequence", sysib + 80, 16);

    cc = stsi(2, 2, 2);
    ml_printf("stsi222-cc=%d\n", cc);
    ml_printf("stsi222-lpar-number=%d\n", (sysib[32] << 8) | sysib[33]);
    print_hex("stsi222-lpar-name", sysib + 44, 8);

    cc = read_scp_info();
    if (cc || ((SCCBHeader *)sccb_page)->response_code !=
        SCLP_RC_NORMAL_READ_COMPLETION) {
        ml_printf("scp-info-cc=%d\n", cc);
        return 1;
    }
    rnmax = (sccb_page[8] << 8) | sccb_page[9];
    rnsize = sccb_page[10];
    rnsize2 = ((uint32_t)sccb_page[100] << 24) | (sccb_page[101] << 16) |
              (sccb_page[102] << 8) | sccb_page[103];
    rnmax2 = 0;
    for (int i = 0; i < 8; i++) {
        rnmax2 = (rnmax2 << 8) | sccb_page[104 + i];
    }
    ml_printf("increment-mb=%u\n", rnsize ? rnsize : rnsize2);
    ml_printf("increments=%lu\n", rnmax ? (uint64_t)rnmax : rnmax2);
    return 0;
}
