/*
 * SCLP integrated-3270 console with no terminal attached.
 *
 * The console's chardev is a listening socket that no client connects to.
 * Writing a 3270 data stream must be rejected with response code 0x05F0
 * (improper state) and the event buffer left unaccepted, rather than being
 * reported as an equipment check; a NOP still completes normally.
 * Built with PERSISTENT_TEST, for a persistent console: the console keeps
 * output for a later client and a read waits for one, so all complete.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

#define SCLP_WRITE_EVENT_DATA 0x00760005
#define SCLP_WRITE_EVENT_MASK 0x00780005
#define SCLP_EVENT_3270       0x1b
#define SCLP_EVENT_ACCEPTED   0x80
#define SCLP_RC_IMPROPER_STATE 0x05f0

typedef struct WriteEventMask8 {
    SCCBHeader h;
    uint16_t reserved;
    uint16_t mask_length;
    uint8_t cp_receive[8];
    uint8_t cp_send[8];
    uint8_t sclp_receive[8];
    uint8_t sclp_send[8];
} __attribute__((packed)) WriteEventMask8;

typedef struct Write3270 {
    SCCBHeader h;
    uint16_t event_length;
    uint8_t event_type;
    uint8_t event_flags;
    uint16_t reserved;
    uint8_t command;
    uint8_t wcc;
} __attribute__((packed)) Write3270;

static union {
    WriteEventMask8 wem;
    Write3270 wed;
    uint8_t page[PAGE_SIZE];
} sccb __attribute__((aligned(PAGE_SIZE)));

void ml_printf(const char *format, ...);

static int service_call(unsigned long command)
{
    register unsigned long r0 asm("0") = command;
    register unsigned long r1 asm("1") = (unsigned long)__pa(&sccb);
    unsigned long cc;

    asm volatile(
        ".insn rre,0xb2200000,%1,%2\n"
        "ipm %0\n"
        "srl %0,28"
        : "=&d" (cc)
        : "d" (r0), "a" (r1)
        : "cc", "memory");
    consume_sclp_int();
    return cc;
}

static int write_3270(uint8_t command, uint16_t *response, uint8_t *flags)
{
    int cc;

    memset(&sccb, 0, sizeof(sccb));
    sccb.wed.h.length = sizeof(sccb.wed);
    sccb.wed.event_length = sizeof(sccb.wed) - sizeof(SCCBHeader);
    sccb.wed.event_type = SCLP_EVENT_3270;
    sccb.wed.command = command;
    sccb.wed.wcc = 0x40;
    cc = service_call(SCLP_WRITE_EVENT_DATA);
    *response = sccb.wed.h.response_code;
    *flags = sccb.wed.event_flags;
    return cc;
}

int main(void)
{
    uint16_t response;
    uint8_t flags;
    int cc;

    memset(&sccb, 0, sizeof(sccb));
    sccb.wem.h.length = sizeof(sccb.wem);
    sccb.wem.mask_length = 8;
    sccb.wem.cp_receive[3] = 0x20;
    sccb.wem.cp_send[3] = 0x20;
    cc = service_call(SCLP_WRITE_EVENT_MASK);
    if (cc || sccb.wem.h.response_code != SCLP_RC_NORMAL_COMPLETION) {
        ml_printf("WEM cc=%d response=%x\n", cc, sccb.wem.h.response_code);
        return 1;
    }

#ifdef PERSISTENT_TEST
    cc = write_3270(0x0d, &response, &flags); /* Erase/Write Alternate */
    if (cc || response != SCLP_RC_NORMAL_COMPLETION ||
        !(flags & SCLP_EVENT_ACCEPTED)) {
        ml_printf("EWA cc=%d response=%x\n", cc, response);
        return 2;
    }
    cc = write_3270(0x06, &response, &flags); /* Read Modified */
    if (cc || response != SCLP_RC_NORMAL_COMPLETION ||
        !(flags & SCLP_EVENT_ACCEPTED)) {
        ml_printf("RM cc=%d response=%x\n", cc, response);
        return 4;
    }
    return 0;
#endif

    cc = write_3270(0x0d, &response, &flags); /* Erase/Write Alternate */
    if (cc || response != SCLP_RC_IMPROPER_STATE) {
        ml_printf("EWA cc=%d response=%x\n", cc, response);
        return 2;
    }
    if (flags & SCLP_EVENT_ACCEPTED) {
        ml_printf("EWA accepted without a terminal\n");
        return 3;
    }

    cc = write_3270(0x06, &response, &flags); /* Read Modified */
    if (cc || response != SCLP_RC_IMPROPER_STATE) {
        ml_printf("RM cc=%d response=%x\n", cc, response);
        return 4;
    }

    cc = write_3270(0x03, &response, &flags); /* NOP */
    if (cc || response != SCLP_RC_NORMAL_COMPLETION ||
        !(flags & SCLP_EVENT_ACCEPTED)) {
        ml_printf("NOP cc=%d response=%x\n", cc, response);
        return 5;
    }
    return 0;
}
