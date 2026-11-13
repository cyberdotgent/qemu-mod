/*
 * Verify that SCLP Read Channel-Path Information describes devices in the
 * guest's default channel-subsystem image.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

#define SCLP_READ_CHP_INFO 0x00030001
#define SCLP_READ_CSI_INFO 0x001c0001

typedef struct ReadChpInfo {
    SCCBHeader h;
    uint8_t installed[32];
    uint8_t standby[32];
    uint8_t online[32];
} __attribute__((packed)) ReadChpInfo;

static ReadChpInfo chp_info __attribute__((aligned(PAGE_SIZE)));
typedef struct ReadCsiInfo {
    SCCBHeader h;
    uint8_t facilities[8];
    uint8_t reserved[48];
} __attribute__((packed)) ReadCsiInfo;
static ReadCsiInfo csi_info __attribute__((aligned(PAGE_SIZE)));
void ml_printf(const char *format, ...);

static int read_chp_info(void)
{
    register unsigned long command asm("0") = SCLP_READ_CHP_INFO;
    register unsigned long sccb_addr asm("1") =
        (unsigned long)__pa(&chp_info);
    unsigned long cc;

    chp_info.h.length = sizeof(chp_info);
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

static int read_csi_info(void)
{
    register unsigned long command asm("0") = SCLP_READ_CSI_INFO;
    register unsigned long sccb_addr asm("1") =
        (unsigned long)__pa(&csi_info);
    unsigned long cc;

    csi_info.h.length = sizeof(csi_info);
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

int main(void)
{
    int cc;

    /*
     * The test command line attaches the first non-virtio device.  CHPID 00
     * is reserved for virtio-ccw, so the FBA channel path is CHPID 01.
     */
    cc = read_chp_info();
    if (cc || chp_info.h.response_code != SCLP_RC_NORMAL_READ_COMPLETION) {
        ml_printf("cc=%d response=%04x\n", cc, chp_info.h.response_code);
        return 1;
    }
    if (!(chp_info.installed[0] & 0x40) ||
        !(chp_info.online[0] & 0x40) ||
        (chp_info.standby[0] & 0x40)) {
        ml_printf("installed=%x standby=%x online=%x\n",
                  chp_info.installed[0], chp_info.standby[0],
                  chp_info.online[0]);
        return 2;
    }
    cc = read_csi_info();
    if (cc || csi_info.h.response_code != SCLP_RC_NORMAL_READ_COMPLETION ||
        csi_info.facilities[0] != 0x03) {
        ml_printf("CSI cc=%d response=%04x facilities=%02x\n", cc,
                  csi_info.h.response_code, csi_info.facilities[0]);
        return 3;
    }
    return 0;
}
