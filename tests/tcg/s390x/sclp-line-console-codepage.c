/*
 * SCLP line-mode console code page and character set.
 *
 * With codepage=500, the meson wrapper types "|!" and the not sign (UTF-8)
 * and the guest checks that they arrive as code page 500 EBCDIC; the guest
 * then writes "[]" and the cent sign in code page 500 and the wrapper checks
 * that they reach the character device as UTF-8.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define main firmware_main
#include "s390-ccw.h"
#undef main
#include "sclp.h"

#define SCLP_EVENT_MESSAGE        0x02
#define MDB_TYPE                  0x0001
#define MDB_TAG                   0xd4c4c240 /* "MDB " in EBCDIC */
#define MESSAGE_TEXT              0x0004

typedef struct WriteEventMask8 {
    SCCBHeader h;
    uint16_t reserved;
    uint16_t mask_length;
    uint8_t cp_receive[8];
    uint8_t cp_send[8];
    uint8_t sclp_receive[8];
    uint8_t sclp_send[8];
} __attribute__((packed)) WriteEventMask8;

typedef struct MessageText {
    uint16_t length;
    uint16_t type;
    uint16_t line_type_flags;
    uint8_t alarm_control;
    uint8_t reserved[3];
    uint8_t text[3];
} __attribute__((packed)) MessageText;

typedef struct LineMessage {
    SCCBHeader h;
    EventBufferHeader event;
    uint16_t mdb_length;
    uint16_t mdb_type;
    uint32_t mdb_tag;
    uint32_t mdb_revision;
    MessageText mto;
} __attribute__((packed)) LineMessage;

typedef struct OperatorCommand {
    SCCBHeader h;
    EventBufferHeader event;
    uint8_t gds[4 * 3 + 2 * 2];
    uint8_t text[3];
} __attribute__((packed)) OperatorCommand;

static union {
    WriteEventMask8 wem;
    LineMessage message;
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

static int write_message(const uint8_t text[3])
{
    LineMessage *msg = &sccb.message;

    memset(&sccb, 0, sizeof(sccb));
    msg->h.length = sizeof(*msg);
    msg->h.function_code = SCLP_FC_NORMAL_WRITE;
    msg->event.length = sizeof(*msg) - sizeof(msg->h);
    msg->event.type = SCLP_EVENT_MESSAGE;
    msg->mdb_length = sizeof(*msg) - sizeof(msg->h) - sizeof(msg->event);
    msg->mdb_type = MDB_TYPE;
    msg->mdb_tag = MDB_TAG;
    msg->mdb_revision = 1;
    msg->mto.length = sizeof(msg->mto);
    msg->mto.type = MESSAGE_TEXT;
    memcpy(msg->mto.text, text, sizeof(msg->mto.text));
    return service_call(SCLP_CMD_WRITE_EVENT_DATA) ||
           msg->h.response_code != SCLP_RC_NORMAL_COMPLETION;
}

static int read_command(const uint8_t *expected, unsigned int len)
{
    OperatorCommand *command = &sccb.command;
    unsigned int i;

    consume_sclp_int();
    memset(&sccb, 0, sizeof(sccb));
    command->h.length = PAGE_SIZE;
    if (service_call(SCLP_CMD_READ_EVENT_DATA) ||
        command->h.response_code != SCLP_RC_NORMAL_COMPLETION ||
        command->event.length != sizeof(command->event) +
                                 sizeof(command->gds) + len) {
        return 1;
    }
    for (i = 0; i < len; i++) {
        if (command->text[i] != expected[i]) {
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    /* "[]" and the cent sign in code page 500 */
    static const uint8_t message[] = { 0x4a, 0x5a, 0xb0 };
    /* "|", "!" and the not sign in code page 500 */
    static const uint8_t command[] = { 0xbb, 0x4f, 0xba };

    memset(&sccb, 0, sizeof(sccb));
    sccb.wem.h.length = sizeof(sccb.wem);
    sccb.wem.mask_length = 8;
    sccb.wem.cp_receive[0] = 0x80;
    sccb.wem.cp_send[0] = 0x40;
    if (service_call(SCLP_CMD_WRITE_EVENT_MASK) ||
        sccb.wem.h.response_code != SCLP_RC_NORMAL_COMPLETION) {
        return 1;
    }
    if (read_command(command, sizeof(command))) {
        return 2;
    }
    if (write_message(message)) {
        return 3;
    }
    return 0;
}
