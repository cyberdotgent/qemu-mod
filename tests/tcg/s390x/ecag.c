/*
 * Test Extract CPU Attribute.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

static uint64_t ecag_topology(void)
{
    register uint64_t r1 asm("1");

    asm volatile(".byte 0xeb,0x10,0x00,0x00,0x00,0x4c" : "=d" (r1));
    return r1;
}

static uint64_t ecag_line_size(void)
{
    register uint64_t r1 asm("1");

    asm volatile(".byte 0xeb,0x10,0x00,0x10,0x00,0x4c" : "=d" (r1));
    return r1;
}

static uint64_t ecag_total_size(void)
{
    register uint64_t r1 asm("1");

    asm volatile(".byte 0xeb,0x10,0x00,0x20,0x00,0x4c" : "=d" (r1));
    return r1;
}

static uint64_t ecag_unsupported(void)
{
    register uint64_t r1 asm("1");

    asm volatile(".byte 0xeb,0x10,0x00,0x30,0x00,0x4c" : "=d" (r1));
    return r1;
}

static uint64_t ecag_reserved(void)
{
    register uint64_t r1 asm("1");

    asm volatile(".byte 0xeb,0x10,0x01,0x00,0x00,0x4c" : "=d" (r1));
    return r1;
}

int main(void)
{
    return ecag_topology() != 0x0400000000000000ULL ||
           ecag_line_size() != 256 ||
           ecag_total_size() != 512 * 1024 ||
           ecag_unsupported() != UINT64_MAX ||
           ecag_reserved() != UINT64_MAX;
}
