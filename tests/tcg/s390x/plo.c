/*
 * Test base Perform Locked Operation functions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>
#include "minilib.h"

#define FAIL(n) do { ml_printf("PLO failure %d\n", n); return n; } while (0)

static unsigned int plo(unsigned int fc, unsigned long regs[4],
                        void *op2, void *op4)
{
    register unsigned long gr0 asm("0") = fc;
    register unsigned long gr2 asm("2") = regs[0];
    register unsigned long gr3 asm("3") = regs[1];
    register unsigned long gr4 asm("4") = regs[2];
    register unsigned long gr5 asm("5") = regs[3];
    unsigned long psw;

    asm volatile("plo 2,0(%6),4,0(%7)\n\tipm %5"
                 : "+d"(gr0), "+d"(gr2), "+d"(gr3), "+d"(gr4), "+d"(gr5),
                   "=d"(psw)
                 : "a"(op2), "a"(op4)
                 : "cc", "memory");
    regs[0] = gr2;
    regs[1] = gr3;
    regs[2] = gr4;
    regs[3] = gr5;
    return (psw >> 28) & 3;
}

static void clear_pl(uint64_t *pl, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++) {
        pl[i] = 0;
    }
}

int main(void)
{
    uint32_t word2 = 0x11223344;
    uint32_t word4 = 0;
    uint32_t word6 = 0;
    uint32_t word8 = 0;
    uint64_t dword2 = 0x1122334455667788ULL;
    static struct {
        uint64_t high;
        uint64_t low;
    } quad = { 0x0123456789abcdefULL, 0xfedcba9876543210ULL };
    static uint64_t pl[20] __attribute__((aligned(8)));
    static unsigned long regs[4];
    unsigned int i;

    for (i = 0; i < 24; i++) {
        unsigned int cc = plo(0x100 | i, regs, &word2, &word4);

        if (cc != 0) {
            ml_printf("PLO query %d returned CC %d\n", i, cc);
            FAIL(1);
        }
    }
    if (plo(0x100 | 24, regs, &word2, &word4) != 3) {
        FAIL(1);
    }

    /* PLO.CS: compare R2, replace from R3. */
    regs[0] = ((uint64_t)word2 << 32) | 0x11223344;
    regs[1] = 0xaabbccdd;
    if (plo(4, regs, &word2, &word4) != 0 ||
        word2 != 0xaabbccdd) {
        FAIL(2);
    }
    regs[0] = 0;
    if (plo(4, regs, &word2, &word4) != 1 ||
        (uint32_t)regs[0] != word2) {
        FAIL(3);
    }

    /* PLO.CL: compare R2 and, when equal, load operand 4 into R4. */
    word4 = 0x55667788;
    regs[0] = word2;
    regs[2] = 0;
    if (plo(0, regs, &word2, &word4) != 0 ||
        (uint32_t)regs[2] != word4) {
        FAIL(4);
    }

    /* PLO.CSG uses its compare and replacement values in the list. */
    clear_pl(pl, 20);
    pl[1] = dword2;
    pl[3] = 0x8877665544332211ULL;
    if (plo(5, regs, &dword2, pl) != 0 ||
        dword2 != 0x8877665544332211ULL) {
        FAIL(5);
    }

    /* PLO.DCS distinguishes a mismatch of the second comparison with CC 2. */
    word2 = 0x11111111;
    word4 = 0x22222222;
    regs[0] = word2;
    regs[1] = 0xaaaaaaaa;
    regs[2] = 0;
    regs[3] = 0xbbbbbbbb;
    if (plo(8, regs, &word2, &word4) != 2 ||
        (uint32_t)regs[2] != word4 || word2 != 0x11111111) {
        FAIL(6);
    }
    regs[2] = word4;
    if (plo(8, regs, &word2, &word4) != 0 ||
        word2 != 0xaaaaaaaa || word4 != 0xbbbbbbbb) {
        FAIL(7);
    }

    /* PLO.CSST swaps operand 2 and stores R4 at operand 4. */
    word2 = 0x33333333;
    regs[0] = word2;
    regs[1] = 0x44444444;
    regs[2] = 0x55555555;
    if (plo(12, regs, &word2, &word4) != 0 ||
        word2 != 0x44444444 || word4 != 0x55555555) {
        FAIL(8);
    }

    /* PLO.CSDST and PLO.CSTST take store values and addresses from the PL. */
    clear_pl(pl, 20);
    word2 = 0x66666666;
    word4 = 0;
    word6 = 0;
    regs[0] = word2;
    regs[1] = 0x77777777;
    *(uint32_t *)((char *)pl + 60) = 0x88888888;
    pl[9] = (unsigned long)&word4;
    *(uint32_t *)((char *)pl + 92) = 0x99999999;
    pl[13] = (unsigned long)&word6;
    i = plo(16, regs, &word2, pl);
    if (i != 0 ||
        word2 != 0x77777777 || word4 != 0x88888888 ||
        word6 != 0x99999999) {
        ml_printf("CSDST cc=%d op2=%x op4=%x op6=%x\n", i, word2,
                  word4, word6);
        FAIL(9);
    }
    word2 = 0xaaaaaaaa;
    regs[0] = word2;
    regs[1] = 0xbbbbbbbb;
    *(uint32_t *)((char *)pl + 124) = 0xcccccccc;
    pl[17] = (unsigned long)&word8;
    if (plo(20, regs, &word2, pl) != 0 ||
        word2 != 0xbbbbbbbb || word8 != 0xcccccccc) {
        FAIL(10);
    }

    /* PLO.CSX exercises the installed 128-bit parameter-list form. */
    clear_pl(pl, 20);
    pl[0] = quad.high;
    pl[1] = quad.low;
    pl[2] = 0x1111222233334444ULL;
    pl[3] = 0x5555666677778888ULL;
    if (plo(7, regs, &quad, pl) != 0 ||
        quad.high != pl[2] || quad.low != pl[3]) {
        FAIL(11);
    }

    return 0;
}
