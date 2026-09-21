/*
 * Test Store Multiple High.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

int main(void)
{
    uint32_t stored = 0;
    uint64_t value = 0x0123456789abcdefULL;

    asm volatile(
        "lgr %%r7,%1\n"
        "stmh %%r7,%%r7,0(%2)"
        : "=m" (stored)
        : "d" (value), "a" (&stored)
        : "r7", "memory");

    return stored != 0x01234567;
}
