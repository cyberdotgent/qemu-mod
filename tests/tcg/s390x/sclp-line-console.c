/*
 * Verify the SCLP line-mode operator-command event layout.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

#define SCLP_EVENT_OPRTNS_COMMAND 0x01
#define GDS_ID_MDSMU              0x1310
#define GDS_ID_CPMSU              0x1212
#define GDS_ID_TEXTCMD            0x1320
#define GDS_KEY_SELFDEFTEXTMSG    0x31
#define GDS_KEY_TEXTMSG           0x30

typedef struct WriteEventMask8 {
    SCCBHeader h;
    uint16_t reserved;
    uint16_t mask_length;
    uint8_t cp_receive[8];
    uint8_t cp_send[8];
    uint8_t sclp_receive[8];
    uint8_t sclp_send[8];
} __attribute__((packed)) WriteEventMask8;

typedef struct GdsVector {
    uint16_t length;
    uint16_t id;
} __attribute__((packed)) GdsVector;

typedef struct GdsSubvector {
    uint8_t length;
    uint8_t key;
} __attribute__((packed)) GdsSubvector;

typedef struct OperatorCommand {
    SCCBHeader h;
    EventBufferHeader event;
    GdsVector mdmsu;
    GdsVector cpmsu;
    GdsVector text_command;
    GdsSubvector self_def_text;
    GdsSubvector text_message;
    uint8_t text[9];
} __attribute__((packed)) OperatorCommand;

static union {
    WriteEventMask8 wem;
    OperatorCommand command;
    uint8_t page[PAGE_SIZE];
} sccb __attribute__((aligned(PAGE_SIZE)));

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
    static const uint8_t expected[] = {
        0xc4, 0x40, 0xc9, 0xd7, 0xd3, 0xc9, 0xd5, 0xc6, 0xd6,
    };
    OperatorCommand *command = &sccb.command;
    unsigned int i;

    memset(&sccb, 0, sizeof(sccb));
    sccb.wem.h.length = sizeof(sccb.wem);
    sccb.wem.mask_length = 8;
    sccb.wem.cp_receive[0] = 0x80;
    if (service_call(SCLP_CMD_WRITE_EVENT_MASK) ||
        sccb.wem.h.response_code != SCLP_RC_NORMAL_COMPLETION ||
        !(sccb.wem.sclp_send[0] & 0x80)) {
        return 1;
    }

    /* The test wrapper supplies "D IPLINFO" after mask negotiation. */
    consume_sclp_int();

    memset(&sccb, 0xa5, sizeof(sccb));
    memset(&command->h, 0, sizeof(command->h));
    command->h.length = PAGE_SIZE;
    if (service_call(SCLP_CMD_READ_EVENT_DATA) ||
        command->h.response_code != SCLP_RC_NORMAL_COMPLETION) {
        return 2;
    }

    if (command->event.length != sizeof(EventBufferHeader) +
                                 sizeof(GdsVector) * 3 +
                                 sizeof(GdsSubvector) * 2 +
                                 sizeof(expected) ||
        command->event.type != SCLP_EVENT_OPRTNS_COMMAND ||
        command->event.flags || command->event._reserved) {
        return 3;
    }
    if (command->mdmsu.length != 25 || command->mdmsu.id != GDS_ID_MDSMU ||
        command->cpmsu.length != 21 || command->cpmsu.id != GDS_ID_CPMSU ||
        command->text_command.length != 17 ||
        command->text_command.id != GDS_ID_TEXTCMD) {
        return 4;
    }
    if (command->self_def_text.length != 13 ||
        command->self_def_text.key != GDS_KEY_SELFDEFTEXTMSG ||
        command->text_message.length != 11 ||
        command->text_message.key != GDS_KEY_TEXTMSG) {
        return 5;
    }
    for (i = 0; i < sizeof(expected); i++) {
        if (command->text[i] != expected[i]) {
            return 6;
        }
    }
    return 0;
}
