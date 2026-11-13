/*
 * Verify storage-key side effects of an SCLP service call.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

#define SCLP_READ_CHP_INFO 0x00030001

typedef struct ReadChpInfo {
    SCCBHeader h;
    uint8_t installed[32];
    uint8_t standby[32];
    uint8_t online[32];
} __attribute__((packed)) ReadChpInfo;

static ReadChpInfo chp_info __attribute__((aligned(PAGE_SIZE)));
void ml_printf(const char *format, ...);

static void sske(unsigned long key, unsigned long address)
{
    register unsigned long r1 asm("1") = key;
    register unsigned long r2 asm("2") = address;

    asm volatile("sske %0,%1" : "+d" (r1), "+a" (r2) : : "cc", "memory");
}

static uint8_t iske(unsigned long address)
{
    register unsigned long r1 asm("1");
    register unsigned long r2 asm("2") = address;

    asm volatile("iske %0,%1" : "=d" (r1) : "a" (r2) : "cc");
    return r1;
}

int main(void)
{
    unsigned long sccb_addr = (unsigned long)__pa(&chp_info);
    unsigned long cc;
    uint8_t key;

    memset(&chp_info, 0, sizeof(chp_info));
    chp_info.h.length = sizeof(chp_info);

    /* Clear R/C last, so only SERVC can account for the following access. */
    sske(0xa0, sccb_addr);
    {
        register unsigned long command asm("0") = SCLP_READ_CHP_INFO;
        register unsigned long sccb_reg asm("1") = sccb_addr;

        asm volatile(
            ".insn rre,0xb2200000,%1,%2\n"
            "ipm %0\n"
            "srl %0,28"
            : "=&d" (cc)
            : "d" (command), "a" (sccb_reg)
            : "cc", "memory");
    }
    key = iske(sccb_addr);
    consume_sclp_int();

    if (cc || key != 0xa6) {
        ml_printf("cc=%lu key=%x expected=a6\n", cc, key);
        return 1;
    }
    if (chp_info.h.response_code != SCLP_RC_NORMAL_READ_COMPLETION) {
        ml_printf("response=%04x\n", chp_info.h.response_code);
        return 2;
    }
    return 0;
}
