/*
 * Test Enhanced-DAT Facility 1 frame-management operations.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

#define PAGE_SIZE       4096UL
#define SEGMENT_SIZE    (1024UL * 1024)

#define PFMF_SK         0x00020000UL
#define PFMF_CF         0x00010000UL
#define PFMF_FSC_1M     0x00001000UL

static uint8_t area[SEGMENT_SIZE] __attribute__((aligned(SEGMENT_SIZE)));

static void fill(uint8_t *p, unsigned long size, uint8_t value)
{
    while (size--) {
        *p++ = value;
    }
}

static int is_zero(const uint8_t *p, unsigned long size)
{
    while (size--) {
        if (*p++) {
            return 0;
        }
    }
    return 1;
}

static unsigned long pfmf(unsigned long control, unsigned long address)
{
    register unsigned long r1 asm("1") = control;
    register unsigned long r2 asm("2") = address;

    asm volatile("pfmf %0,%1"
                 : "+d"(r1), "+a"(r2)
                 :
                 : "cc", "memory");
    return r2;
}

static unsigned long sske_mb(unsigned long key, unsigned long address)
{
    register unsigned long r1 asm("1") = key;
    register unsigned long r2 asm("2") = address;

    asm volatile("sske %0,%1,1"
                 : "+d"(r1), "+a"(r2)
                 :
                 : "cc", "memory");
    return r2;
}

static uint8_t iske(unsigned long address)
{
    register unsigned long r1 asm("1");
    register unsigned long r2 asm("2") = address;

    asm volatile("iske %0,%1" : "=d"(r1) : "a"(r2) : "cc");
    return r1;
}

int main(void)
{
    unsigned long base = (unsigned long)area;
    unsigned long start;
    unsigned long result;

    /* A 4K PFMF uses a real address and leaves R2 unchanged. */
    fill(area, PAGE_SIZE, 0x5a);
    result = pfmf(PFMF_CF, base + 7);
    if (result != base + 7 || !is_zero(area, PAGE_SIZE)) {
        return 1;
    }

    /* A 1M PFMF starts at a page and advances R2 to the boundary. */
    start = base + SEGMENT_SIZE - 2 * PAGE_SIZE;
    fill((uint8_t *)start, 2 * PAGE_SIZE, 0xa5);
    result = pfmf(PFMF_CF | PFMF_FSC_1M, start + 9);
    if (result != base + SEGMENT_SIZE + 9 ||
        !is_zero((uint8_t *)start, 2 * PAGE_SIZE)) {
        return 2;
    }

    /* PFMF sets every key selected by a 1M operation. */
    start = base + SEGMENT_SIZE - 2 * PAGE_SIZE;
    result = pfmf(PFMF_SK | PFMF_FSC_1M | 0xa6, start + 11);
    if (result != base + SEGMENT_SIZE + 11 ||
        iske(start) != 0xa6 || iske(start + PAGE_SIZE) != 0xa6) {
        return 3;
    }

    /* EDAT-1 multiple-block SSKE uses absolute addresses and updates R2. */
    result = sske_mb(0x54, start + 13);
    if (result != base + SEGMENT_SIZE + 13 ||
        iske(start) != 0x54 || iske(start + PAGE_SIZE) != 0x54) {
        return 4;
    }

    return 0;
}
