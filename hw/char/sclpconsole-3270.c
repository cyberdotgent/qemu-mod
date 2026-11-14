/*
 * SCLP integrated 3270 system console (event type 0x1b)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/s390x/3270-ccw.h"
#include "hw/s390x/event-facility.h"
#include "hw/s390x/sclp.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_SCLP_3270_CONSOLE "sclp3270console"
OBJECT_DECLARE_SIMPLE_TYPE(SCLPConsole3270, SCLP_3270_CONSOLE)

/* One SCCB page, less the SCCB and event-buffer headers and discriminator. */
#define SCLP_3270_MAX_RECORD (SCCB_SIZE - sizeof(SCCBHeader) - \
                              sizeof(EventBufferHeader) - 1)

#define TN_IAC  0xff
#define TN_SE   0xf0
#define TN_SB   0xfa
#define TN_WILL 0xfb
#define TN_WONT 0xfc
#define TN_DO   0xfd
#define TN_DONT 0xfe
#define TN_EOR  0xef

typedef enum SCLP3270ParseState {
    SCLP_3270_PARSE_DATA,
    SCLP_3270_PARSE_IAC,
    SCLP_3270_PARSE_OPTION,
    SCLP_3270_PARSE_SUBNEG,
    SCLP_3270_PARSE_SUBNEG_IAC,
} SCLP3270ParseState;

struct SCLPConsole3270 {
    SCLPEvent event;
    CharFrontend chr;

    uint8_t record[SCLP_3270_MAX_RECORD];
    uint32_t record_len;
    uint8_t incoming[SCLP_3270_MAX_RECORD];
    uint32_t incoming_len;
    uint8_t read_command;
    uint8_t parse_state;
};

static bool sclp_3270_can_handle_event(uint8_t type)
{
    return type == SCLP_EVENT_INTEGRATED_3270_CONSOLE;
}

static sccb_mask_t sclp_3270_mask(void)
{
    return SCLP_EVENT_MASK_3270;
}

static uint8_t sclp_3270_remote_command(uint8_t command)
{
    switch (command) {
    case TC_WRITE:
    case TC_SELWRITE:
        return TN3270_CMD_WRITE;
    case TC_RDBUF:
    case TC_SELRB:
    case TC_SELRBP:
        return TN3270_CMD_RDBUF;
    case TC_EWRITE:
        return TN3270_CMD_EWRITE;
    case TC_READMOD:
    case TC_SELRM:
    case TC_SELRMP:
        return TN3270_CMD_READMOD;
    case TC_EWRITEA:
        return TN3270_CMD_EWRITEA;
    case TC_EAU:
        return TN3270_CMD_EAU;
    case TC_WRITESF:
        return TN3270_CMD_WRITESF;
    default:
        return 0;
    }
}

static bool sclp_3270_is_read(uint8_t command)
{
    return command == TC_RDBUF || command == TC_READMOD ||
           command == TC_SELRM || command == TC_SELRMP ||
           command == TC_SELRB || command == TC_SELRBP;
}

/* Write one complete TN3270 record, including TELNET quoting and EOR. */
static int sclp_3270_send_record(SCLPConsole3270 *console,
                                 uint8_t command,
                                 const uint8_t *data, size_t len)
{
    g_autofree uint8_t *wire = NULL;
    size_t i, out = 0;
    int written;

    if (!qemu_chr_fe_backend_connected(&console->chr)) {
        return 0;
    }

    wire = g_malloc(2 * (len + 1) + 2);
    wire[out++] = command;
    if (command == TN_IAC) {
        wire[out++] = TN_IAC;
    }
    for (i = 0; i < len; i++) {
        wire[out++] = data[i];
        if (data[i] == TN_IAC) {
            wire[out++] = TN_IAC;
        }
    }
    wire[out++] = TN_IAC;
    wire[out++] = TN_EOR;

    written = qemu_chr_fe_write_all(&console->chr, wire, out);
    return written == out ? 0 : -EIO;
}

static int sclp_3270_write_event_data(SCLPEvent *event,
                                      EventBufferHeader *event_header)
{
    SCLPConsole3270 *console = SCLP_3270_CONSOLE(event);
    uint16_t event_len = be16_to_cpu(event_header->length);
    uint8_t *payload = (uint8_t *)(event_header + 1);
    uint8_t command, remote;
    size_t payload_len;

    if (event_len < sizeof(*event_header) + 1) {
        return SCLP_RC_EVENT_BUFFER_SYNTAX_ERROR;
    }

    payload_len = event_len - sizeof(*event_header);
    command = payload[0];
    if (command == TC_NOP) {
        event_header->flags |= SCLP_EVENT_BUFFER_ACCEPTED;
        return SCLP_RC_NORMAL_COMPLETION;
    }
    remote = sclp_3270_remote_command(command);
    if (!remote) {
        return SCLP_RC_INVALID_FUNCTION;
    }

    if (sclp_3270_send_record(console, remote, payload + 1,
                              payload_len - 1) < 0) {
        return SCLP_RC_CONTAINED_EQUIPMENT_CHECK;
    }

    if (sclp_3270_is_read(command)) {
        console->read_command = command;
        event->event_pending = true;
        sclp_service_interrupt(0);
    }

    event_header->flags |= SCLP_EVENT_BUFFER_ACCEPTED;
    return SCLP_RC_NORMAL_COMPLETION;
}

static int sclp_3270_read_event_data(SCLPEvent *event,
                                     EventBufferHeader *event_header,
                                     int *slen)
{
    SCLPConsole3270 *console = SCLP_3270_CONSOLE(event);
    uint8_t *payload = (uint8_t *)(event_header + 1);
    bool have_data = console->read_command && console->record_len;
    size_t payload_len;

    if (!event->event_pending) {
        return 0;
    }

    payload_len = have_data ? console->record_len + 1 : 1;
    if (*slen < sizeof(*event_header) + payload_len) {
        return 0;
    }

    event_header->length = cpu_to_be16(sizeof(*event_header) + payload_len);
    event_header->type = SCLP_EVENT_INTEGRATED_3270_CONSOLE;
    event_header->flags = 0;
    event_header->_reserved = 0;

    if (have_data) {
        payload[0] = 0x00;
        memcpy(payload + 1, console->record, console->record_len);
        console->record_len = 0;
        console->read_command = 0;
    } else {
        payload[0] = 0x80;
    }

    event->event_pending = false;
    *slen -= sizeof(*event_header) + payload_len;
    qemu_notify_event();
    return 1;
}

static void sclp_3270_finish_record(SCLPConsole3270 *console)
{
    if (!console->incoming_len) {
        return;
    }

    memcpy(console->record, console->incoming, console->incoming_len);
    console->record_len = console->incoming_len;
    console->incoming_len = 0;
    console->event.event_pending = true;
    sclp_service_interrupt(0);
}

static int sclp_3270_chr_can_read(void *opaque)
{
    SCLPConsole3270 *console = opaque;

    return sizeof(console->incoming) - console->incoming_len;
}

/*
 * The socket chardev performs the initial TN3270 negotiation but deliberately
 * leaves the TELNET stream intact for its frontend.  Retain data bytes and
 * record boundaries while ignoring negotiation commands and subnegotiation.
 */
static void sclp_3270_chr_read(void *opaque, const uint8_t *buf, int size)
{
    SCLPConsole3270 *console = opaque;
    int i;

    for (i = 0; i < size; i++) {
        uint8_t byte = buf[i];

        switch ((SCLP3270ParseState)console->parse_state) {
        case SCLP_3270_PARSE_DATA:
            if (byte == TN_IAC) {
                console->parse_state = SCLP_3270_PARSE_IAC;
            } else if (console->incoming_len < sizeof(console->incoming)) {
                console->incoming[console->incoming_len++] = byte;
            }
            break;
        case SCLP_3270_PARSE_IAC:
            if (byte == TN_IAC) {
                if (console->incoming_len < sizeof(console->incoming)) {
                    console->incoming[console->incoming_len++] = byte;
                }
                console->parse_state = SCLP_3270_PARSE_DATA;
            } else if (byte == TN_EOR) {
                sclp_3270_finish_record(console);
                console->parse_state = SCLP_3270_PARSE_DATA;
            } else if (byte == TN_SB) {
                console->parse_state = SCLP_3270_PARSE_SUBNEG;
            } else if (byte == TN_WILL || byte == TN_WONT ||
                       byte == TN_DO || byte == TN_DONT) {
                console->parse_state = SCLP_3270_PARSE_OPTION;
            } else {
                console->parse_state = SCLP_3270_PARSE_DATA;
            }
            break;
        case SCLP_3270_PARSE_OPTION:
            console->parse_state = SCLP_3270_PARSE_DATA;
            break;
        case SCLP_3270_PARSE_SUBNEG:
            if (byte == TN_IAC) {
                console->parse_state = SCLP_3270_PARSE_SUBNEG_IAC;
            }
            break;
        case SCLP_3270_PARSE_SUBNEG_IAC:
            console->parse_state = byte == TN_SE ? SCLP_3270_PARSE_DATA :
                                                   SCLP_3270_PARSE_SUBNEG;
            break;
        }
    }
}

static int sclp_3270_init(SCLPEvent *event)
{
    static bool console_available;
    SCLPConsole3270 *console = SCLP_3270_CONSOLE(event);

    if (console_available) {
        error_report("Multiple integrated 3270 consoles are not supported");
        return -1;
    }
    console_available = true;

    qemu_chr_fe_set_handlers(&console->chr, sclp_3270_chr_can_read,
                             sclp_3270_chr_read, NULL, NULL, console,
                             NULL, true);
    return 0;
}

static void sclp_3270_reset(DeviceState *dev)
{
    SCLPConsole3270 *console = SCLP_3270_CONSOLE(dev);

    console->event.event_pending = false;
    console->record_len = 0;
    console->incoming_len = 0;
    console->read_command = 0;
    console->parse_state = SCLP_3270_PARSE_DATA;
}

static const VMStateDescription vmstate_sclp_3270 = {
    .name = TYPE_SCLP_3270_CONSOLE,
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(event.event_pending, SCLPConsole3270),
        VMSTATE_UINT8_ARRAY(record, SCLPConsole3270, SCLP_3270_MAX_RECORD),
        VMSTATE_UINT32(record_len, SCLPConsole3270),
        VMSTATE_UINT8_ARRAY(incoming, SCLPConsole3270, SCLP_3270_MAX_RECORD),
        VMSTATE_UINT32(incoming_len, SCLPConsole3270),
        VMSTATE_UINT8(read_command, SCLPConsole3270),
        VMSTATE_UINT8(parse_state, SCLPConsole3270),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sclp_3270_properties[] = {
    DEFINE_PROP_CHR("chardev", SCLPConsole3270, chr),
};

static void sclp_3270_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCLPEventClass *ec = SCLP_EVENT_CLASS(klass);

    device_class_set_props(dc, sclp_3270_properties);
    device_class_set_legacy_reset(dc, sclp_3270_reset);
    dc->vmsd = &vmstate_sclp_3270;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);

    ec->init = sclp_3270_init;
    ec->get_send_mask = sclp_3270_mask;
    ec->get_receive_mask = sclp_3270_mask;
    ec->can_handle_event = sclp_3270_can_handle_event;
    ec->read_event_data = sclp_3270_read_event_data;
    ec->write_event_data = sclp_3270_write_event_data;
}

static const TypeInfo sclp_3270_info = {
    .name = TYPE_SCLP_3270_CONSOLE,
    .parent = TYPE_SCLP_EVENT,
    .instance_size = sizeof(SCLPConsole3270),
    .class_size = sizeof(SCLPEventClass),
    .class_init = sclp_3270_class_init,
};

static void sclp_3270_register_types(void)
{
    type_register_static(&sclp_3270_info);
}

type_init(sclp_3270_register_types)
