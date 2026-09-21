/*
 * Test Load Address register-width semantics.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

int main(void)
{
    register uint64_t value asm("7") = 0x0123456700001000ULL;

    asm volatile(
        "sam31\n"
        "la %0,8(%0)\n"
        "sam64"
        : "+a" (value));
    if (value != 0x0123456700001008ULL) {
        return 1;
    }

    asm volatile(
        "sam31\n"
        "lay %0,-8(%0)\n"
        "sam64"
        : "+a" (value));
    if (value != 0x0123456700001000ULL) {
        return 2;
    }

    asm volatile("la %0,8(%0)" : "+a" (value));
    if (value != 0x0123456700001008ULL) {
        return 3;
    }

    return 0;
}
