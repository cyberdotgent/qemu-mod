/*
 * Test classic hexadecimal-floating-point instructions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include "minilib.h"

static inline __attribute__((always_inline)) unsigned int get_cc(void)
{
    unsigned long pm;

    asm volatile("ipm %0" : "=d"(pm));
    return (pm >> 28) & 3;
}

int main(void)
{
    static const struct ext {
        uint64_t high;
        uint64_t low;
    } ext_one = { 0x4110000000000000ULL, 0x3300000000000000ULL },
      ext_two = { 0x4120000000000000ULL, 0x3300000000000000ULL },
      ext_three = { 0x4130000000000000ULL, 0x3300000000000000ULL },
      ext_six = { 0x4160000000000000ULL, 0x3300000000000000ULL };
    struct ext ext_out;
    uint64_t one = 0x4110000000000000ULL;
    uint64_t two = 0x4120000000000000ULL;
    uint64_t three = 0x4130000000000000ULL;
    uint64_t six = 0x4160000000000000ULL;
    uint64_t half = 0x4080000000000000ULL;
    uint64_t negative_one = 0xc110000000000000ULL;
    uint64_t rounded_long = 0x4110000000000001ULL;
    uint64_t long_round_input = 0x4110000080000000ULL;
    uint64_t out;
    uint32_t sone = 0x41100000;
    uint32_t stwo = 0x41200000;
    uint32_t sthree = 0x41300000;
    uint32_t ssix = 0x41600000;
    uint32_t snegative_one = 0xc1100000;
    uint32_t sout;
    unsigned int cc;

    asm volatile("ld 0,0(%1)\n\tld 2,0(%2)\n\tadr 0,2\n\tstd 0,0(%0)"
                 : : "a"(&out), "a"(&one), "a"(&two) : "cc", "memory", "f0", "f2");
    if (out != three) {
        ml_printf("ADR: %x != %x\n", out, three);
        return 1;
    }
    asm volatile("sdr 0,2\n\tstd 0,0(%0)" : : "a"(&out) : "cc", "memory", "f0");
    if (out != one) {
        return 2;
    }
    asm volatile("awr 0,2\n\tstd 0,0(%0)" : : "a"(&out) : "cc", "memory", "f0");
    if (out != three) {
        return 3;
    }
    asm volatile("cdr 0,2" : : : "cc");
    cc = get_cc();
    if (cc != 2) {
        return 4;
    }
    asm volatile("ld 0,0(%1)\n\tmdr 0,2\n\tstd 0,0(%0)"
                 : : "a"(&out), "a"(&three) : "cc", "memory", "f0");
    if (out != six) {
        return 5;
    }
    asm volatile("ddr 0,2\n\tstd 0,0(%0)" : : "a"(&out) : "cc", "memory", "f0");
    cc = get_cc();
    if (out != three || cc != 2) {
        return 6;
    }
    asm volatile("ld 2,0(%1)\n\thdr 0,2\n\tstd 0,0(%0)"
                 : : "a"(&out), "a"(&one) : "cc", "memory", "f0", "f2");
    if (out != half) {
        return 7;
    }

    asm volatile("le 0,0(%1)\n\tle 2,0(%2)\n\taer 0,2\n\tste 0,0(%0)"
                 : : "a"(&sout), "a"(&sone), "a"(&stwo) : "cc", "memory", "f0", "f2");
    if (sout != sthree) {
        return 8;
    }
    asm volatile("ser 0,2\n\taur 0,2\n\tmer 0,2\n\tste 0,0(%0)"
                 : : "a"(&sout) : "cc", "memory", "f0");
    if (sout != ssix) {
        return 9;
    }
    asm volatile("der 0,2\n\ther 0,0\n\tste 0,0(%0)"
                 : : "a"(&sout) : "cc", "memory", "f0");
    if (sout != 0x41180000) {
        return 10;
    }

    asm volatile("ld 0,0(%1)\n\tad 0,0(%2)\n\tsd 0,0(%2)\n\t"
                 "md 0,0(%2)\n\tdd 0,0(%2)\n\tstd 0,0(%0)"
                 : : "a"(&out), "a"(&one), "a"(&two) : "cc", "memory", "f0");
    if (out != one) {
        return 11;
    }

    asm volatile("ld 0,0(%1)\n\tld 2,8(%1)\n\t"
                 "ld 4,0(%2)\n\tld 6,8(%2)\n\t"
                 "axr 0,4\n\tstd 0,0(%0)\n\tstd 2,8(%0)"
                 : : "a"(&ext_out), "a"(&ext_one), "a"(&ext_two)
                 : "cc", "memory", "f0", "f2", "f4", "f6");
    if (ext_out.high != ext_three.high || ext_out.low != ext_three.low) {
        return 12;
    }
    asm volatile("sxr 0,4\n\tstd 0,0(%0)\n\tstd 2,8(%0)"
                 : : "a"(&ext_out) : "cc", "memory", "f0", "f2");
    if (ext_out.high != ext_one.high || ext_out.low != ext_one.low) {
        return 13;
    }
    asm volatile("ld 0,0(%1)\n\tld 4,0(%2)\n\tmxdr 0,4\n\t"
                 "std 0,0(%0)\n\tstd 2,8(%0)"
                 : : "a"(&ext_out), "a"(&two), "a"(&three)
                 : "cc", "memory", "f0", "f2", "f4");
    if (ext_out.high != ext_six.high || ext_out.low != ext_six.low) {
        return 14;
    }
    asm volatile("ld 0,0(%1)\n\tld 2,8(%1)\n\t"
                 "ld 4,0(%2)\n\tld 6,8(%2)\n\tmxr 0,4\n\t"
                 "std 0,0(%0)\n\tstd 2,8(%0)"
                 : : "a"(&ext_out), "a"(&ext_two), "a"(&ext_three)
                 : "cc", "memory", "f0", "f2", "f4", "f6");
    if (ext_out.high != ext_six.high || ext_out.low != ext_six.low) {
        return 15;
    }
    asm volatile("ldxr 0,4\n\tstd 0,0(%0)"
                 : : "a"(&out) : "cc", "memory", "f0");
    if (out != three) {
        return 16;
    }

    /* Exercise all long register sign/test operations with distinct regs. */
    asm volatile("ld 2,0(%1)\n\tlpdr 0,2\n\tstd 0,0(%0)"
                 : : "a"(&out), "a"(&negative_one)
                 : "cc", "memory", "f0", "f2");
    if (out != one) {
        return 17;
    }
    asm volatile("lndr 0,0\n\tltdr 0,0\n\tstd 0,0(%0)"
                 : : "a"(&out) : "cc", "memory", "f0");
    if (out != negative_one) {
        return 18;
    }
    asm volatile("lcdr 0,0\n\tstd 0,0(%0)"
                 : : "a"(&out) : "cc", "memory", "f0");
    if (out != one) {
        return 19;
    }

    /* And the corresponding short register operations. */
    asm volatile("le 2,0(%1)\n\tlper 0,2\n\tste 0,0(%0)"
                 : : "a"(&sout), "a"(&snegative_one)
                 : "cc", "memory", "f0", "f2");
    if (sout != sone) {
        return 20;
    }
    asm volatile("lner 0,0\n\tlter 0,0\n\tste 0,0(%0)"
                 : : "a"(&sout) : "cc", "memory", "f0");
    if (sout != snegative_one) {
        return 21;
    }
    asm volatile("lcer 0,0\n\tste 0,0(%0)"
                 : : "a"(&sout) : "cc", "memory", "f0");
    if (sout != sone) {
        return 22;
    }

    asm volatile("ld 2,0(%1)\n\tledr 0,2\n\tste 0,0(%0)"
                 : : "a"(&sout), "a"(&long_round_input)
                 : "cc", "memory", "f0", "f2");
    if (sout != 0x41100001) {
        return 23;
    }
    ext_out.high = ext_one.high;
    ext_out.low = ext_one.low;
    ext_out.low = 0x3380000000000000ULL;
    asm volatile("ld 4,0(%1)\n\tld 6,8(%1)\n\tldxr 0,4\n\tstd 0,0(%0)"
                 : : "a"(&out), "a"(&ext_out)
                 : "cc", "memory", "f0", "f4", "f6");
    if (out != rounded_long) {
        return 24;
    }

    /* Memory forms, including normalized and unnormalized arithmetic. */
    asm volatile("ld 0,0(%1)\n\taw 0,0(%2)\n\tsw 0,0(%2)\n\t"
                 "cd 0,0(%2)\n\tstd 0,0(%0)"
                 : : "a"(&out), "a"(&one), "a"(&two)
                 : "cc", "memory", "f0");
    if (out != one) {
        return 25;
    }
    asm volatile("ld 0,0(%1)\n\tmxd 0,0(%2)\n\t"
                 "std 0,0(%0)\n\tstd 2,8(%0)"
                 : : "a"(&ext_out), "a"(&one), "a"(&two)
                 : "cc", "memory", "f0", "f2");
    if (ext_out.high != ext_two.high || ext_out.low != ext_two.low) {
        return 26;
    }

    asm volatile("le 0,0(%1)\n\tae 0,0(%2)\n\tse 0,0(%2)\n\t"
                 "au 0,0(%2)\n\tsu 0,0(%2)\n\tce 0,0(%2)\n\t"
                 "de 0,0(%1)\n\tmde 0,0(%2)\n\tste 0,0(%0)"
                 : : "a"(&sout), "a"(&sone), "a"(&stwo)
                 : "cc", "memory", "f0");
    if (sout != stwo) {
        return 27;
    }

    return 0;
}
