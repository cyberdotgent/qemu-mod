/*
 * Verify SCLP integrated-3270 event negotiation and initial screen write.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

#define SCLP_WRITE_EVENT_DATA 0x00760005
#define SCLP_WRITE_EVENT_MASK 0x00780005
#define SCLP_READ_EVENT_DATA  0x00770005
#define SCLP_EVENT_3270       0x1b
#define SCLP_EVENT_ACCEPTED   0x80

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

typedef struct Read3270 {
    SCCBHeader h;
    uint16_t event_length;
    uint8_t event_type;
    uint8_t event_flags;
    uint16_t reserved;
    uint8_t data[SCCB_DATA_LEN - sizeof(EventBufferHeader)];
} __attribute__((packed)) Read3270;

static union {
    WriteEventMask8 wem;
    Write3270 wed;
    Read3270 red;
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

int main(void)
{
    int cc;

    memset(&sccb, 0, sizeof(sccb));
    sccb.wem.h.length = sizeof(sccb.wem);
    sccb.wem.mask_length = 8;
    sccb.wem.cp_receive[3] = 0x20;
    cc = service_call(SCLP_WRITE_EVENT_MASK);
    if (cc || sccb.wem.h.response_code != SCLP_RC_NORMAL_COMPLETION) {
        ml_printf("WEM cc=%d response=%04x\n", cc,
                  sccb.wem.h.response_code);
        return 1;
    }
    /* Event 0x1b is bit 0x20 in byte three of an eight-byte mask. */
    if (!(sccb.wem.sclp_receive[3] & 0x20) ||
        !(sccb.wem.sclp_send[3] & 0x20)) {
        ml_printf("SYSG masks receive=%02x send=%02x\n",
                  sccb.wem.sclp_receive[3], sccb.wem.sclp_send[3]);
        return 2;
    }

    memset(&sccb, 0, sizeof(sccb));
    sccb.wed.h.length = sizeof(sccb.wed);
    sccb.wed.event_length = sizeof(sccb.wed) - sizeof(SCCBHeader);
    sccb.wed.event_type = SCLP_EVENT_3270;
    sccb.wed.command = 0x0d; /* Erase/Write Alternate */
    sccb.wed.wcc = 0x40;
    cc = service_call(SCLP_WRITE_EVENT_DATA);
    if (cc || sccb.wed.h.response_code != SCLP_RC_NORMAL_COMPLETION) {
        ml_printf("WED cc=%d response=%04x\n", cc,
                  sccb.wed.h.response_code);
        return 3;
    }
    if (!(sccb.wed.event_flags & SCLP_EVENT_ACCEPTED)) {
        ml_printf("WED flags=%02x\n", sccb.wed.event_flags);
        return 4;
    }

    /* A read command causes an attention-only event until input arrives. */
    memset(&sccb, 0, sizeof(sccb));
    sccb.wed.h.length = sizeof(sccb.wed) - 1;
    sccb.wed.event_length = sizeof(EventBufferHeader) + 1;
    sccb.wed.event_type = SCLP_EVENT_3270;
    sccb.wed.command = 0x06; /* Read Modified */
    cc = service_call(SCLP_WRITE_EVENT_DATA);
    if (cc || sccb.wed.h.response_code != SCLP_RC_NORMAL_COMPLETION) {
        return 5;
    }

    memset(&sccb, 0, sizeof(sccb));
    sccb.red.h.length = PAGE_SIZE;
    cc = service_call(SCLP_READ_EVENT_DATA);
    if (cc || sccb.red.h.response_code != SCLP_RC_NORMAL_COMPLETION) {
        return 6;
    }
    if (sccb.red.event_type != SCLP_EVENT_3270 ||
        sccb.red.event_length != sizeof(EventBufferHeader) + 1 ||
        sccb.red.data[0] != 0x80) {
        ml_printf("RED type=%02x length=%u flag=%02x\n",
                  sccb.red.event_type, sccb.red.event_length,
                  sccb.red.data[0]);
        return 7;
    }
    return 0;
}
