/*
 * Test classic packed-decimal arithmetic instructions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

static inline __attribute__((always_inline)) unsigned int get_cc(void)
{
    unsigned long pm;

    asm volatile("ipm %0" : "=d"(pm));
    return (pm >> 28) & 3;
}

static int equal(const uint8_t *a, const uint8_t *b, unsigned int n)
{
    while (n--) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    unsigned int cc;
    uint8_t a[16] = { 0 };
    uint8_t b[16] = { 0 };
    static const uint8_t ap_result[] = { 0x00, 0x12, 0x11, 0x1c };
    static const uint8_t mp_result[] = { 0x00, 0x00, 0x03, 0x6c };
    static const uint8_t dp_result[] = { 0x33, 0x3c, 0x00, 0x1c };
    static const uint8_t srp_left[] = { 0x00, 0x12, 0x30, 0x0c };
    static const uint8_t srp_overflow[] = { 0x34, 0x56, 0x70, 0x0c };
    static const uint8_t srp_right[] = { 0x00, 0x00, 0x01, 0x3c };

    a[0] = 0x00; a[1] = 0x12; a[2] = 0x34; a[3] = 0x5c;
    b[0] = 0x23; b[1] = 0x4d;
    asm volatile("ap 0(4,%0),0(2,%1)" : : "a"(a), "a"(b) : "cc", "memory");
    cc = get_cc();
    if (!equal(a, ap_result, 4) || cc != 2) {
        return 1;
    }

    asm volatile("cp 0(4,%0),0(2,%1)" : : "a"(a), "a"(b) : "cc", "memory");
    cc = get_cc();
    if (cc != 2) {
        return 2;
    }
    asm volatile("sp 0(4,%0),0(2,%1)" : : "a"(a), "a"(b) : "cc", "memory");
    cc = get_cc();
    if (cc != 2) {
        return 3;
    }

    a[0] = 0x9c;
    b[0] = 0x1c;
    asm volatile("ap 0(1,%0),0(1,%1)" : : "a"(a), "a"(b) : "cc", "memory");
    cc = get_cc();
    if (a[0] != 0x0c || cc != 3) {
        return 4;
    }

    a[0] = 0xff; a[1] = 0xff;
    b[0] = 0x12; b[1] = 0x34; b[2] = 0x5d;
    asm volatile("zap 0(2,%0),0(3,%1)" : : "a"(a), "a"(b) : "cc", "memory");
    cc = get_cc();
    if (a[0] != 0x34 || a[1] != 0x5d || cc != 3) {
        return 5;
    }

    a[0] = 0x00; a[1] = 0x00; a[2] = 0x01; a[3] = 0x2c;
    b[0] = 0x00; b[1] = 0x3c;
    asm volatile("mp 0(4,%0),0(2,%1)" : : "a"(a), "a"(b) : "cc", "memory");
    if (!equal(a, mp_result, 4)) {
        return 6;
    }

    a[0] = 0x00; a[1] = 0x01; a[2] = 0x00; a[3] = 0x0c;
    asm volatile("dp 0(4,%0),0(2,%1)" : : "a"(a), "a"(b) : "cc", "memory");
    if (!equal(a, dp_result, 4)) {
        return 7;
    }

    a[0] = 0x00; a[1] = 0x00; a[2] = 0x12; a[3] = 0x3c;
    asm volatile("srp 0(4,%0),2,0" : : "a"(a) : "cc", "memory");
    cc = get_cc();
    if (!equal(a, srp_left, 4) || cc != 2) {
        return 8;
    }
    a[0] = 0x00; a[1] = 0x00; a[2] = 0x12; a[3] = 0x8c;
    asm volatile("srp 0(4,%0),63,5" : : "a"(a) : "cc", "memory");
    cc = get_cc();
    if (!equal(a, srp_right, 4) || cc != 2) {
        return 9;
    }

    a[0] = 0x12; a[1] = 0x34; a[2] = 0x56; a[3] = 0x7c;
    asm volatile("srp 0(4,%0),2,0" : : "a"(a) : "cc", "memory");
    cc = get_cc();
    if (!equal(a, srp_overflow, 4) || cc != 3) {
        return 10;
    }

    return 0;
}
