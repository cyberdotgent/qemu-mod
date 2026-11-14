/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bare-metal KVM regression: synthetic compatibility control blocks. */
#include <stdint.h>

#ifndef EXPECT_COMPAT
#define EXPECT_COMPAT 1
#endif

static uint8_t cvt[256] __attribute__((aligned(4096)));
static uint8_t ecvt[512] __attribute__((aligned(4096)));
static volatile uint8_t svt[1536] __attribute__((aligned(4096)));
static uint8_t sysib[4096] __attribute__((aligned(4096)));

static void put32(unsigned long address, uint32_t value)
{
    asm volatile("st %1,0(%0)" : : "a" (address), "d" (value) : "memory");
}

static void handshake(void)
{
    register unsigned long r0 asm("0") = 0x30000002;
    register unsigned long r1 asm("1") = 2;

    asm volatile("stsi 0(%2)" : "+d" (r0), "+d" (r1)
                 : "a" (sysib) : "cc", "memory");
}

int main(void)
{
    uint32_t saved_cvt, saved_svt;
    int result = 0;

    asm volatile("l %0,16\n l %1,2892"
                 : "=d" (saved_cvt), "=d" (saved_svt));
    put32(16, (uintptr_t)cvt);
    put32((uintptr_t)cvt + 140, (uintptr_t)ecvt);
    put32(2892, (uintptr_t)svt);

    /* Below the level threshold must leave the target unchanged. */
    *(uint16_t *)(ecvt + 476) = 257;
    svt[1376] = 1;
    handshake();
    if (svt[1376] != 1) {
        result = 1;
        goto out;
    }
    *(uint16_t *)(ecvt + 476) = 258;
    handshake();
    if (svt[1376] != !EXPECT_COMPAT) {
        result = 2;
        goto out;
    }
    /* An earlier zero must not suppress a later valid handshake. */
    svt[1376] = 0;
    handshake();
    svt[1376] = 1;
    handshake();
    if (svt[1376] != !EXPECT_COMPAT) {
        result = 3;
        goto out;
    }
    /* Null and inaccessible pointers must neither write nor inject faults. */
    svt[1376] = 1;
    put32(16, 0);
    handshake();
    put32(16, 0x70000000); /* Outside this test's 128 MiB RAM. */
    handshake();
    put32(16, (uintptr_t)cvt);
    put32((uintptr_t)cvt + 140, 0);
    handshake();
    put32((uintptr_t)cvt + 140, (uintptr_t)ecvt);
    put32(2892, 0);
    handshake();
    if (svt[1376] != 1) {
        result = 4;
    }
out:
    put32(16, saved_cvt);
    put32(2892, saved_svt);
    return result;
}
