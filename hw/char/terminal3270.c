/*
 * Terminal 3270 implementation
 *
 * Copyright 2017 IBM Corp.
 *
 * Authors: Yang Chen <bjcyang@linux.vnet.ibm.com>
 *          Jing Liu <liujbjl@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/s390x/3270-ccw.h"
#include "hw/s390x/ebcdic.h"
#include "migration/misc.h"
#include "migration/vmstate.h"
#include "qom/object.h"
#include "trace.h"

#define TN3270_MAX_RECORD_SIZE 65535
#define TN3270_MAX_QUEUED_RECORDS 16
#define TN3270_MAX_QUEUED_BYTES (4 * TN3270_MAX_RECORD_SIZE)
#define TN3270_MAX_TTYPE_SIZE 64
#define TN3270_MAX_SUBNEG_SIZE 256
#define TN3270_MAX_REPLAY_SIZE (4 * 1024 * 1024)

#define TN_TELOPT_TTYPE 0x18
#define TN_TELOPT_BINARY 0x00
#define TN_TELOPT_EOR 0x19
#define TN_TTYPE_IS 0x00
#define TN_TTYPE_SEND 0x01

#define TN_WILL 0xfb
#define TN_WONT 0xfc
#define TN_DO 0xfd
#define TN_DONT 0xfe

#define SENSE_COMMAND_REJECT 0x80
#define SENSE_INTERVENTION_REQUIRED 0x40
#define SENSE_DATA_CHECK 0x08
#define SENSE_OPERATION_CHECK 0x01

typedef enum TN3270ParseState {
    TN3270_PARSE_DATA,
    TN3270_PARSE_IAC,
    TN3270_PARSE_OPTION,
    TN3270_PARSE_SUBNEG,
    TN3270_PARSE_SUBNEG_IAC,
} TN3270ParseState;

typedef struct TN3270Record {
    QTAILQ_ENTRY(TN3270Record) next;
    uint32_t len;
    uint8_t data[];
} TN3270Record;

typedef struct TN3270Type {
    const char *name;
    uint8_t model;
    uint16_t rows;
    uint16_t cols;
    bool eab;
} TN3270Type;

static const TN3270Type tn3270_types[] = {
    { "IBM-DYNAMIC", 0xff, 24, 80, true },
    { "IBM-3179-2", 2, 24, 80, false },
    { "IBM-3180-2", 5, 27, 132, false },
    { "IBM-3277-2", 2, 24, 80, false },
    { "IBM-3278-2", 2, 24, 80, false },
    { "IBM-3278-3", 3, 32, 80, false },
    { "IBM-3278-4", 4, 43, 80, false },
    { "IBM-3278-5", 5, 27, 132, false },
    { "IBM-3278-2-E", 2, 24, 80, true },
    { "IBM-3278-3-E", 3, 32, 80, true },
    { "IBM-3278-4-E", 4, 43, 80, true },
    { "IBM-3278-5-E", 5, 27, 132, true },
    { "IBM-3279-2", 2, 24, 80, false },
    { "IBM-3279-3", 3, 32, 80, false },
    { "IBM-3279-4", 4, 43, 80, false },
    { "IBM-3279-5", 5, 27, 132, false },
    { "IBM-3279-2-E", 2, 24, 80, true },
    { "IBM-3279-3-E", 3, 32, 80, true },
    { "IBM-3279-4-E", 4, 43, 80, true },
    { "IBM-3279-5-E", 5, 27, 132, true },
};

struct Terminal3270 {
    EmulatedCcw3270Device cdev;
    CharFrontend chr;

    QTAILQ_HEAD(, TN3270Record) records;
    TN3270Record *current_record;
    uint32_t current_offset;
    uint16_t current_start_pos;
    uint32_t queued_bytes;
    uint16_t queued_records;
    bool device_end_pending;
    bool attention_pending;

    uint8_t record_buf[TN3270_MAX_RECORD_SIZE];
    uint32_t record_len;
    uint8_t subneg_buf[TN3270_MAX_SUBNEG_SIZE];
    uint16_t subneg_len;
    TN3270ParseState parse_state;

    uint8_t terminal_type[TN3270_MAX_TTYPE_SIZE];
    uint8_t model;
    uint16_t rows;
    uint16_t cols;
    bool eab;
    bool connected;
    bool ready;
    bool local_binary;
    bool peer_binary;
    bool local_eor;
    bool peer_eor;
    bool peer_ttype;
    bool ttype_valid;
    uint8_t telnet_command;
    uint8_t ttype_attempts;

    uint16_t pos;
    bool ewa;
    uint8_t aid;
    GByteArray *write_record;
    uint8_t write_command;
    GByteArray *replay;
    bool migration_disconnect;
    bool reconnect_restore;

    uint8_t *migration_records;
    uint32_t migration_records_size;
    uint32_t migration_record_len[TN3270_MAX_QUEUED_RECORDS + 1];
    uint16_t migration_record_count;
    bool migration_has_current;
    uint8_t *migration_write;
    uint32_t migration_write_len;
    uint8_t *migration_replay;
    uint32_t migration_replay_len;
    NotifierWithReturn migration_notifier;

    bool read_pending;
    CCW1 pending_ccw;
    guint timer_tag;
};
typedef struct Terminal3270 Terminal3270;

#define TYPE_TERMINAL_3270 "x-terminal3270"
DECLARE_INSTANCE_CHECKER(Terminal3270, TERMINAL_3270,
                         TYPE_TERMINAL_3270)

static const uint8_t sba_code[64] = {
    0x40, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
    0x60, 0x61, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
};

static inline SubchDev *terminal_sch(Terminal3270 *t)
{
    return CCW_DEVICE(&t->cdev)->sch;
}

static inline CcwDataStream *terminal_cds(Terminal3270 *t)
{
    return &terminal_sch(t)->cds;
}

static bool terminal_send_record(Terminal3270 *t,
                                 const uint8_t *buf, size_t len);
static void terminal_try_unsolicited_status(Terminal3270 *t);

static void terminal_banner_line(GByteArray *record, Terminal3270 *t,
                                 unsigned int row, const char *text)
{
    int col = MAX(0, ((int)t->cols - (int)strlen(text)) / 2);
    uint16_t pos = row * t->cols + col;
    uint8_t address[] = { O3270_SBA, 0, 0, O3270_SF, 0x60 };
    g_autofree uint8_t *ebcdic = g_malloc(strlen(text));

    if (pos < 4096) {
        address[1] = sba_code[pos >> 6];
        address[2] = sba_code[pos & 0x3f];
    } else {
        address[1] = pos >> 8;
        address[2] = pos;
    }
    ebcdic_put(ebcdic, text, strlen(text));
    g_byte_array_append(record, address, sizeof(address));
    g_byte_array_append(record, ebcdic, strlen(text));
}

static bool terminal_send_banner(Terminal3270 *t)
{
    SubchDev *sch = terminal_sch(t);
    g_autoptr(GByteArray) record = g_byte_array_new();
    g_autofree char *address = NULL;
    g_autofree char *terminal = NULL;
    const uint8_t header[] = { TN3270_CMD_EWRITE, 0x42 };

    address = g_strdup_printf("CCW device %02x.%x.%04x  subchannel %04x",
                              sch->cssid, sch->ssid, sch->devno, sch->schid);
    terminal = g_strdup_printf("%s  model %u  %ux%u%s",
                               t->terminal_type, t->model, t->cols, t->rows,
                               t->eab ? "  extended attributes" : "");
    g_byte_array_append(record, header, sizeof(header));
    terminal_banner_line(record, t, 4, "QEMU s390x 3270 terminal");
    terminal_banner_line(record, t, 7, address);
    terminal_banner_line(record, t, 9, terminal);
    terminal_banner_line(record, t, 13,
                         "Connected - press Enter to signal attention");
    return terminal_send_record(t, record->data, record->len);
}
static int terminal_migration_notify(NotifierWithReturn *notifier,
                                     MigrationEvent *event, Error **errp);

static void terminal_timer_cancel(Terminal3270 *t)
{
    g_clear_handle_id(&t->timer_tag, g_source_remove);
}

static void terminal_record_free(TN3270Record *record)
{
    g_free(record);
}

static void terminal_clear_queued_records(Terminal3270 *t)
{
    TN3270Record *record;

    while ((record = QTAILQ_FIRST(&t->records))) {
        QTAILQ_REMOVE(&t->records, record, next);
        terminal_record_free(record);
    }
    t->queued_bytes = 0;
    t->queued_records = 0;
    t->attention_pending = false;
}

static void terminal_clear_records(Terminal3270 *t)
{
    terminal_clear_queued_records(t);
    terminal_record_free(t->current_record);
    t->current_record = NULL;
    t->current_offset = 0;
    t->current_start_pos = 0;
}

static void terminal_set_unit_check(Terminal3270 *t, uint8_t sense)
{
    SubchDev *sch = terminal_sch(t);

    sch->sense_data[0] = sense;
    sch->curr_status.scsw.dstat = SCSW_DSTAT_UNIT_CHECK;
    if (sense != SENSE_INTERVENTION_REQUIRED) {
        sch->curr_status.scsw.dstat |= SCSW_DSTAT_CHANNEL_END |
                                       SCSW_DSTAT_DEVICE_END;
    }
    sch->curr_status.scsw.ctrl &= ~SCSW_CTRL_MASK_STCTL;
    sch->curr_status.scsw.ctrl |= SCSW_STCTL_PRIMARY |
                                  SCSW_STCTL_SECONDARY |
                                  SCSW_STCTL_ALERT |
                                  SCSW_STCTL_STATUS_PEND;
    sch->curr_status.scsw.cpa = sch->channel_prog + 8;
}

static bool terminal_lookup_type(Terminal3270 *t, const char *name)
{
    g_autofree char *base = g_strdup(name);
    char *suffix = strchr(base, '@');
    unsigned int i;

    if (suffix) {
        *suffix = 0;
    }

    for (i = 0; i < ARRAY_SIZE(tn3270_types); i++) {
        const TN3270Type *type = &tn3270_types[i];

        if (!g_ascii_strcasecmp(base, type->name)) {
            if (t->reconnect_restore && t->terminal_type[0] &&
                g_ascii_strcasecmp(base, (char *)t->terminal_type)) {
                terminal_clear_records(t);
                g_byte_array_set_size(t->replay, 0);
                t->pos = 0;
                t->ewa = false;
            }
            pstrcpy((char *)t->terminal_type, sizeof(t->terminal_type), base);
            t->model = type->model;
            t->rows = type->rows;
            t->cols = type->cols;
            t->eab = type->eab;
            return true;
        }
    }
    return false;
}

static void terminal_signal_ready(Terminal3270 *t)
{
    SubchDev *sch = terminal_sch(t);
    size_t offset = 0;

    if (t->ready) {
        return;
    }
    t->ready = true;
    trace_terminal3270_ready(sch->devno, (char *)t->terminal_type);

    /*
     * A newly attached display is inhibited until it receives its first
     * output.  Real console controllers and Hercules present an initial
     * connection screen before reporting device end.
     */
    if (!t->reconnect_restore && !terminal_send_banner(t)) {
        return;
    }

    while (offset + sizeof(uint32_t) <= t->replay->len) {
        uint32_t len = ldl_be_p(t->replay->data + offset);

        offset += sizeof(uint32_t);
        if (len > t->replay->len - offset ||
            !terminal_send_record(t, t->replay->data + offset, len)) {
            g_byte_array_set_size(t->replay, 0);
            break;
        }
        offset += len;
    }
    t->reconnect_restore = false;
    if (t->queued_records) {
        t->attention_pending = true;
    }
    t->device_end_pending = true;
    terminal_try_unsolicited_status(t);
}

static void terminal_maybe_ready(Terminal3270 *t)
{
    if (t->local_binary && t->peer_binary && t->local_eor && t->peer_eor &&
        t->peer_ttype && t->ttype_valid) {
        terminal_signal_ready(t);
    }
}

static void terminal_send_telnet_option(Terminal3270 *t,
                                        uint8_t command, uint8_t option)
{
    const uint8_t response[] = { IAC, command, option };

    qemu_chr_fe_write_all(&t->chr, response, sizeof(response));
}

static void terminal_request_type(Terminal3270 *t)
{
    const uint8_t request[] = {
        IAC, IAC_SB, TN_TELOPT_TTYPE, TN_TTYPE_SEND, IAC, IAC_SE,
    };

    qemu_chr_fe_write_all(&t->chr, request, sizeof(request));
}

static void terminal_handle_option(Terminal3270 *t, uint8_t option)
{
    bool required = option == TN_TELOPT_BINARY || option == TN_TELOPT_EOR;

    switch (t->telnet_command) {
    case TN_DO:
        if (option == TN_TELOPT_BINARY) {
            t->local_binary = true;
        } else if (option == TN_TELOPT_EOR) {
            t->local_eor = true;
        } else {
            terminal_send_telnet_option(t, TN_WONT, option);
        }
        break;
    case TN_WILL:
        if (option == TN_TELOPT_BINARY) {
            t->peer_binary = true;
        } else if (option == TN_TELOPT_EOR) {
            t->peer_eor = true;
        } else if (option == TN_TELOPT_TTYPE) {
            t->peer_ttype = true;
        } else {
            terminal_send_telnet_option(t, TN_DONT, option);
        }
        break;
    case TN_DONT:
        if (required) {
            qemu_chr_fe_disconnect(&t->chr);
            return;
        }
        break;
    case TN_WONT:
        if (required || option == TN_TELOPT_TTYPE) {
            qemu_chr_fe_disconnect(&t->chr);
            return;
        }
        break;
    default:
        break;
    }
    terminal_maybe_ready(t);
}

static void terminal_handle_subneg(Terminal3270 *t)
{
    size_t len;
    char name[TN3270_MAX_TTYPE_SIZE];

    if (t->subneg_len < 3 || t->subneg_buf[0] != TN_TELOPT_TTYPE ||
        t->subneg_buf[1] != TN_TTYPE_IS) {
        return;
    }

    len = MIN((size_t)t->subneg_len - 2, sizeof(name) - 1);
    memcpy(name, &t->subneg_buf[2], len);
    name[len] = 0;
    if (!terminal_lookup_type(t, name)) {
        if (++t->ttype_attempts < 16) {
            terminal_request_type(t);
        } else {
            qemu_chr_fe_disconnect(&t->chr);
        }
        return;
    }
    t->ttype_valid = true;
    terminal_maybe_ready(t);
}

static bool terminal_append_record_byte(Terminal3270 *t, uint8_t byte)
{
    if (t->record_len == sizeof(t->record_buf)) {
        qemu_chr_fe_disconnect(&t->chr);
        return false;
    }
    t->record_buf[t->record_len++] = byte;
    return true;
}

static void terminal_try_unsolicited_status(Terminal3270 *t)
{
    SubchDev *sch = terminal_sch(t);

    /*
     * A TN3270 client can finish negotiation before the guest enables the
     * subchannel.  css_generate_unsolicited_io_interrupt() then refuses the
     * status, so retain it and retry from the enable/status-clear callbacks.
     * Device End reports that the display became available and must precede
     * any Attention raised by an input record from that display.
     */
    if (t->device_end_pending) {
        if (css_generate_unsolicited_io_interrupt(sch,
                                                  SCSW_DSTAT_DEVICE_END)) {
            t->device_end_pending = false;
        }
        return;
    }
    if (!t->attention_pending) {
        return;
    }
    if (css_generate_unsolicited_io_interrupt(sch, SCSW_DSTAT_ATTENTION)) {
        t->attention_pending = false;
    }
}

static void terminal_status_cleared(SubchDev *sch)
{
    terminal_try_unsolicited_status(TERMINAL_3270(sch->driver_data));
}

static void terminal_enabled(SubchDev *sch)
{
    terminal_try_unsolicited_status(TERMINAL_3270(sch->driver_data));
}

static int terminal_transfer_record(Terminal3270 *t, CCW1 *ccw);
static void terminal_start_timer(Terminal3270 *t);

static void terminal_complete_pending_read(Terminal3270 *t)
{
    int ret;

    if (!t->read_pending || QTAILQ_EMPTY(&t->records)) {
        return;
    }

    t->read_pending = false;
    ret = terminal_transfer_record(t, &t->pending_ccw);
    if (ret < 0) {
        uint8_t sense = terminal_sch(t)->sense_data[0];

        terminal_set_unit_check(t, sense ? sense : SENSE_DATA_CHECK);
    }
    css_virtual_ccw_complete(terminal_sch(t), ret < 0 ? ret : 0);
    trace_terminal3270_complete_read(terminal_sch(t)->devno, ret,
                                     terminal_sch(t)->curr_status.scsw.count);
}

static void terminal_finish_record(Terminal3270 *t)
{
    TN3270Record *record;

    if (!t->ready) {
        t->record_len = 0;
        return;
    }

    /*
     * A non-SNA 3270 has one pending input buffer, not a FIFO of AIDs.
     * If the host restores the keyboard without reading an earlier AID, the
     * next complete input replaces it and raises a fresh attention.  This
     * also matches Hyperion's readpending handling.
     */
    if (!t->read_pending) {
        terminal_clear_queued_records(t);
    }

    if (t->queued_records == TN3270_MAX_QUEUED_RECORDS ||
        t->queued_bytes + t->record_len > TN3270_MAX_QUEUED_BYTES) {
        t->record_len = 0;
        qemu_chr_fe_disconnect(&t->chr);
        return;
    }

    record = g_malloc(sizeof(*record) + t->record_len);
    record->len = t->record_len;
    memcpy(record->data, t->record_buf, t->record_len);
    t->record_len = 0;

    QTAILQ_INSERT_TAIL(&t->records, record, next);
    t->queued_records++;
    t->queued_bytes += record->len;
    trace_terminal3270_record_queue(terminal_sch(t)->devno,
                                    t->queued_records, t->queued_bytes);

    if (t->read_pending) {
        terminal_complete_pending_read(t);
    } else {
        t->attention_pending = true;
        terminal_try_unsolicited_status(t);
    }
}

static int terminal_can_read(void *opaque)
{
    return CHR_READ_BUF_LEN;
}

static void terminal_read(void *opaque, const uint8_t *buf, int size)
{
    Terminal3270 *t = opaque;
    int i;

    terminal_start_timer(t);

    for (i = 0; i < size; i++) {
        uint8_t byte = buf[i];

        switch (t->parse_state) {
        case TN3270_PARSE_DATA:
            if (byte == IAC) {
                t->parse_state = TN3270_PARSE_IAC;
            } else if (t->ready && !terminal_append_record_byte(t, byte)) {
                return;
            }
            break;
        case TN3270_PARSE_IAC:
            if (byte == IAC) {
                if (t->ready && !terminal_append_record_byte(t, byte)) {
                    return;
                }
                t->parse_state = TN3270_PARSE_DATA;
            } else if (byte == IAC_EOR) {
                terminal_finish_record(t);
                t->parse_state = TN3270_PARSE_DATA;
            } else if (byte == IAC_SB) {
                t->subneg_len = 0;
                t->parse_state = TN3270_PARSE_SUBNEG;
            } else if (byte == TN_DO || byte == TN_DONT ||
                       byte == TN_WILL || byte == TN_WONT) {
                t->telnet_command = byte;
                t->parse_state = TN3270_PARSE_OPTION;
            } else {
                t->parse_state = TN3270_PARSE_DATA;
            }
            break;
        case TN3270_PARSE_OPTION:
            terminal_handle_option(t, byte);
            t->parse_state = TN3270_PARSE_DATA;
            break;
        case TN3270_PARSE_SUBNEG:
            if (byte == IAC) {
                t->parse_state = TN3270_PARSE_SUBNEG_IAC;
            } else if (t->subneg_len < sizeof(t->subneg_buf)) {
                t->subneg_buf[t->subneg_len++] = byte;
            } else {
                qemu_chr_fe_disconnect(&t->chr);
                return;
            }
            break;
        case TN3270_PARSE_SUBNEG_IAC:
            if (byte == IAC) {
                if (t->subneg_len < sizeof(t->subneg_buf)) {
                    t->subneg_buf[t->subneg_len++] = byte;
                } else {
                    qemu_chr_fe_disconnect(&t->chr);
                    return;
                }
                t->parse_state = TN3270_PARSE_SUBNEG;
            } else if (byte == IAC_SE) {
                terminal_handle_subneg(t);
                t->parse_state = TN3270_PARSE_DATA;
            } else {
                t->parse_state = TN3270_PARSE_SUBNEG;
            }
            break;
        }
    }
}

static gboolean send_timing_mark_cb(gpointer opaque)
{
    Terminal3270 *t = opaque;
    const uint8_t timing[] = { IAC, 0xfd, 0x06 };

    qemu_chr_fe_write_all(&t->chr, timing, sizeof(timing));
    return G_SOURCE_CONTINUE;
}

static void terminal_start_timer(Terminal3270 *t)
{
    terminal_timer_cancel(t);
    t->timer_tag = g_timeout_add_seconds(600, send_timing_mark_cb, t);
}

static void terminal_cancel_pending(Terminal3270 *t)
{
    t->read_pending = false;
    terminal_record_free(t->current_record);
    t->current_record = NULL;
    t->current_offset = 0;
    t->current_start_pos = 0;
}

static void chr_event(void *opaque, QEMUChrEvent event)
{
    Terminal3270 *t = opaque;
    SubchDev *sch = terminal_sch(t);
    bool pending = t->read_pending;
    bool preserving = t->migration_disconnect;

    switch (event) {
    case CHR_EVENT_OPENED:
        trace_terminal3270_connection(sch->devno, true);
        t->migration_disconnect = false;
        t->connected = true;
        t->ready = false;
        t->parse_state = TN3270_PARSE_DATA;
        t->record_len = 0;
        t->subneg_len = 0;
        t->local_binary = false;
        t->peer_binary = false;
        t->local_eor = false;
        t->peer_eor = false;
        t->peer_ttype = false;
        t->ttype_valid = false;
        t->ttype_attempts = 0;
        if (!t->reconnect_restore) {
            t->pos = 0;
            t->ewa = false;
            g_byte_array_set_size(t->write_record, 0);
            g_byte_array_set_size(t->replay, 0);
            terminal_clear_records(t);
        }
        terminal_start_timer(t);
        break;
    case CHR_EVENT_CLOSED:
        trace_terminal3270_connection(sch->devno, false);
        t->connected = false;
        t->ready = false;
        terminal_timer_cancel(t);
        if (pending) {
            terminal_cancel_pending(t);
        }
        if (!preserving) {
            g_byte_array_set_size(t->write_record, 0);
            g_byte_array_set_size(t->replay, 0);
            terminal_clear_records(t);
            t->pos = 0;
            t->ewa = false;
        }
        t->migration_disconnect = false;
        if (pending) {
            terminal_set_unit_check(t, SENSE_DATA_CHECK);
            css_virtual_ccw_complete(sch, -EIO);
        } else if (!preserving) {
            t->device_end_pending = true;
            terminal_try_unsolicited_status(t);
        }
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        break;
    }
}

static void terminal_init(EmulatedCcw3270Device *dev, Error **errp)
{
    Terminal3270 *t = TERMINAL_3270(dev);

    terminal_sch(t)->enable_cb = terminal_enabled;
    terminal_sch(t)->status_clear_cb = terminal_status_cleared;
    qemu_chr_fe_set_handlers(&t->chr, terminal_can_read,
                             terminal_read, chr_event, NULL, t, NULL, true);
    migration_add_notifier(&t->migration_notifier,
                           terminal_migration_notify);
}

static uint16_t decode_buffer_address(const uint8_t *address)
{
    if (!(address[0] & 0xc0)) {
        return ((uint16_t)address[0] << 8) | address[1];
    }
    return ((uint16_t)(address[0] & 0x3f) << 6) | (address[1] & 0x3f);
}

static bool advance_3270_position(const uint8_t *buf, size_t len,
                                  size_t *offset, uint16_t *pos,
                                  uint16_t cells)
{
    size_t i = *offset;
    size_t need = 1;
    uint8_t pairs;

    if (i >= len) {
        return false;
    }

    switch (buf[i]) {
    case O3270_RA:
        need = 4;
        if (len - i < need) {
            return false;
        }
        if (buf[i + 3] == O3270_GE) {
            need++;
        }
        if (len - i < need) {
            return false;
        }
        *pos = decode_buffer_address(&buf[i + 1]);
        break;
    case O3270_SFE:
    case O3270_MF:
        if (len - i < 2) {
            return false;
        }
        pairs = buf[i + 1];
        need = 2 + 2 * pairs;
        if (len - i < need) {
            return false;
        }
        (*pos)++;
        break;
    case O3270_SBA:
    case O3270_EUA:
        need = 3;
        if (len - i < need) {
            return false;
        }
        *pos = decode_buffer_address(&buf[i + 1]);
        break;
    case O3270_SA:
        need = 3;
        if (len - i < need) {
            return false;
        }
        break;
    case O3270_IC:
    case O3270_PT:
        break;
    case O3270_SF:
    case O3270_GE:
        need = 2;
        if (len - i < need) {
            return false;
        }
        (*pos)++;
        break;
    default:
        (*pos)++;
        break;
    }

    *pos %= cells;
    *offset += need;
    return true;
}

static bool terminal_walk_orders(Terminal3270 *t, const uint8_t *buf,
                                 size_t len, bool strict)
{
    size_t offset = 0;
    uint16_t cells = t->ewa ? t->rows * t->cols : 24 * 80;

    while (offset < len) {
        if (!advance_3270_position(buf, len, &offset, &t->pos, cells)) {
            return !strict;
        }
    }
    return true;
}

static size_t terminal_position_offset(Terminal3270 *t, const uint8_t *buf,
                                       size_t len, uint16_t wanted)
{
    size_t offset = 3;
    uint16_t pos = 0;
    uint16_t cells = t->ewa ? t->rows * t->cols : 24 * 80;

    while (offset < len && pos < wanted) {
        if (!advance_3270_position(buf, len, &offset, &pos, cells)) {
            return 0;
        }
    }
    return pos >= wanted ? offset : 0;
}

static bool terminal_send_record(Terminal3270 *t,
                                 const uint8_t *buf, size_t len)
{
    g_autofree uint8_t *wire = NULL;
    size_t i;
    size_t out = 0;
    int ret;

    if (!t->connected || !t->ready) {
        terminal_sch(t)->sense_data[0] = SENSE_INTERVENTION_REQUIRED;
        return false;
    }

    trace_terminal3270_remote_command(terminal_sch(t)->devno, buf[0], len);

    wire = g_malloc(2 * len + 2);
    for (i = 0; i < len; i++) {
        wire[out++] = buf[i];
        if (buf[i] == IAC) {
            wire[out++] = IAC;
        }
    }
    wire[out++] = IAC;
    wire[out++] = IAC_EOR;
    ret = qemu_chr_fe_write_all(&t->chr, wire, out);
    if (ret != out) {
        terminal_sch(t)->sense_data[0] = SENSE_DATA_CHECK;
        return false;
    }
    return true;
}

static bool terminal_replay_has_room(Terminal3270 *t,
                                     const uint8_t *buf, size_t len)
{
    if (buf[0] == TN3270_CMD_EWRITE || buf[0] == TN3270_CMD_EWRITEA) {
        return len <= TN3270_MAX_REPLAY_SIZE - sizeof(uint32_t);
    }
    return len <= TN3270_MAX_REPLAY_SIZE - sizeof(uint32_t) &&
           t->replay->len <= TN3270_MAX_REPLAY_SIZE - sizeof(uint32_t) - len;
}

static void terminal_remember_record(Terminal3270 *t,
                                     const uint8_t *buf, size_t len)
{
    uint8_t encoded_len[sizeof(uint32_t)];

    if (buf[0] == TN3270_CMD_EWRITE || buf[0] == TN3270_CMD_EWRITEA) {
        g_byte_array_set_size(t->replay, 0);
    }
    assert(terminal_replay_has_room(t, buf, len));
    stl_be_p(encoded_len, len);
    g_byte_array_append(t->replay, encoded_len, sizeof(encoded_len));
    g_byte_array_append(t->replay, buf, len);
}

static uint8_t terminal_remote_command(uint8_t local)
{
    switch (local) {
    case TC_WRITE:
        return TN3270_CMD_WRITE;
    case TC_EWRITE:
        return TN3270_CMD_EWRITE;
    case TC_EWRITEA:
        return TN3270_CMD_EWRITEA;
    case TC_WRITESF:
        return TN3270_CMD_WRITESF;
    case TC_EAU:
        return TN3270_CMD_EAU;
    default:
        return 0;
    }
}

static bool terminal_previous_data_chain(Terminal3270 *t)
{
    SubchDev *sch = terminal_sch(t);

    return sch->last_cmd_valid && (sch->last_cmd.flags & CCW_FLAG_DC);
}

static bool terminal_previous_command_chain(Terminal3270 *t)
{
    SubchDev *sch = terminal_sch(t);

    return sch->last_cmd_valid && (sch->last_cmd.flags & CCW_FLAG_CC);
}

static void terminal_append_sba(GByteArray *record, uint16_t pos)
{
    uint8_t address[3] = { O3270_SBA };

    if (pos < 4096) {
        address[1] = sba_code[pos >> 6];
        address[2] = sba_code[pos & 0x3f];
    } else {
        address[1] = pos >> 8;
        address[2] = pos;
    }
    g_byte_array_append(record, address, sizeof(address));
}

static int write_payload_3270(EmulatedCcw3270Device *dev, CCW1 *ccw)
{
    Terminal3270 *t = TERMINAL_3270(dev);
    g_autoptr(GByteArray) payload = g_byte_array_sized_new(ccw->count);
    bool data_chained = terminal_previous_data_chain(t);
    bool command_chained = terminal_previous_command_chain(t);
    uint8_t remote = terminal_remote_command(ccw->cmd_code);
    SubchDev *sch = terminal_sch(t);
    int ret;

    if (!t->connected || !t->ready) {
        terminal_sch(t)->sense_data[0] = SENSE_INTERVENTION_REQUIRED;
        return -EIO;
    }

    if (!sch->last_cmd_valid || sch->last_cmd.cmd_code == CCW_CMD_NOOP ||
        sch->last_cmd.cmd_code == TC_SELRM ||
        sch->last_cmd.cmd_code == TC_SELRB ||
        sch->last_cmd.cmd_code == TC_SELRMP ||
        sch->last_cmd.cmd_code == TC_SELRBP ||
        sch->last_cmd.cmd_code == TC_SELWRITE) {
        t->pos = 0;
    }

    switch (ccw->cmd_code) {
    case TC_SELRM:
    case TC_SELRB:
    case TC_SELRMP:
    case TC_SELRBP:
    case TC_SELWRITE:
        t->pos = 0;
        return 0;
    case TC_EAU:
        t->pos = 0;
        g_byte_array_set_size(t->write_record, 0);
        if (!terminal_replay_has_room(t, &remote, 1)) {
            sch->sense_data[0] = SENSE_DATA_CHECK;
            return -EIO;
        }
        if (!terminal_send_record(t, &remote, 1)) {
            return -EIO;
        }
        terminal_remember_record(t, &remote, 1);
        return 0;
    case TC_WRITESF:
        if (!t->eab) {
            terminal_sch(t)->sense_data[0] = SENSE_OPERATION_CHECK;
            return -EIO;
        }
        t->pos = 0;
        break;
    case TC_EWRITE:
        t->pos = 0;
        t->ewa = false;
        break;
    case TC_EWRITEA:
        t->pos = 0;
        t->ewa = true;
        break;
    case TC_WRITE:
        break;
    default:
        return -ENOSYS;
    }

    g_byte_array_set_size(payload, ccw->count);
    ret = ccw_dstream_read_buf(terminal_cds(t), payload->data, ccw->count);
    if (ret < 0) {
        return ret;
    }

    if (!data_chained) {
        g_byte_array_set_size(t->write_record, 0);
        t->write_command = ccw->cmd_code;
        g_byte_array_append(t->write_record, &remote, 1);
        if (command_chained && ccw->cmd_code == TC_WRITE && t->pos &&
            payload->len > 1 && payload->data[1] != O3270_SBA &&
            payload->data[1] != O3270_RA && payload->data[1] != O3270_EUA) {
            g_byte_array_append(t->write_record, payload->data, 1);
            terminal_append_sba(t->write_record, t->pos);
            g_byte_array_append(t->write_record, payload->data + 1,
                                payload->len - 1);
        } else {
            g_byte_array_append(t->write_record, payload->data, payload->len);
        }
    } else {
        if (!t->write_record->len) {
            sch->sense_data[0] = SENSE_DATA_CHECK;
            return -EIO;
        }
        g_byte_array_append(t->write_record, payload->data, payload->len);
    }

    if (t->write_record->len > TN3270_MAX_RECORD_SIZE) {
        g_byte_array_set_size(t->write_record, 0);
        sch->sense_data[0] = SENSE_DATA_CHECK;
        return -EIO;
    }

    /* A data chain is one TN3270 record, even if an order crosses CCWs. */
    if (ccw->flags & CCW_FLAG_DC) {
        return ccw->count;
    }

    if (t->write_command != TC_WRITESF && t->write_record->len > 2) {
        if (!terminal_walk_orders(t, t->write_record->data + 2,
                                  t->write_record->len - 2, true)) {
            g_byte_array_set_size(t->write_record, 0);
            sch->sense_data[0] = SENSE_DATA_CHECK;
            return -EIO;
        }
    }

    if (!terminal_replay_has_room(t, t->write_record->data,
                                  t->write_record->len)) {
        g_byte_array_set_size(t->write_record, 0);
        sch->sense_data[0] = SENSE_DATA_CHECK;
        return -EIO;
    }
    if (!terminal_send_record(t, t->write_record->data,
                              t->write_record->len)) {
        g_byte_array_set_size(t->write_record, 0);
        return -EIO;
    }
    terminal_remember_record(t, t->write_record->data,
                             t->write_record->len);
    g_byte_array_set_size(t->write_record, 0);
    return ccw->count;
}

static TN3270Record *terminal_pop_record(Terminal3270 *t)
{
    TN3270Record *record = QTAILQ_FIRST(&t->records);

    if (!record) {
        return NULL;
    }
    QTAILQ_REMOVE(&t->records, record, next);
    t->queued_records--;
    t->queued_bytes -= record->len;
    if (!t->queued_records) {
        t->attention_pending = false;
    }
    return record;
}

static void terminal_discard_current_record(Terminal3270 *t)
{
    terminal_record_free(t->current_record);
    t->current_record = NULL;
    t->current_offset = 0;
    t->current_start_pos = 0;
}

static void terminal_position_record(Terminal3270 *t)
{
    TN3270Record *record = t->current_record;
    size_t offset;
    size_t tail;

    if (!record || !t->pos || record->len < 3 ||
        record->data[0] == SF3270_AID) {
        return;
    }

    offset = terminal_position_offset(t, record->data, record->len, t->pos);
    if (!offset) {
        return;
    }
    tail = record->len - offset;
    memmove(record->data + 3, record->data + offset, tail);
    record->len = 3 + tail;
}

static void terminal_set_length_status(Terminal3270 *t, CCW1 *ccw,
                                       uint32_t transferred,
                                       uint32_t available)
{
    bool suppress = (ccw->flags & CCW_FLAG_SLI) &&
                    !(ccw->flags & CCW_FLAG_DC);
    bool continues = (ccw->flags & CCW_FLAG_DC) && transferred < available;

    if (!suppress && !continues && ccw->count != available) {
        terminal_sch(t)->curr_status.scsw.cstat |= SCSW_CSTAT_INCORR_LEN;
    }
}

static int terminal_transfer_record(Terminal3270 *t, CCW1 *ccw)
{
    TN3270Record *record;
    uint32_t available;
    uint32_t len;
    uint32_t old_offset;
    int ret;

    if (!t->current_record) {
        t->current_record = terminal_pop_record(t);
        t->current_offset = 0;
        terminal_position_record(t);
        t->current_start_pos = t->pos;
    }
    record = t->current_record;
    if (!record) {
        return -EAGAIN;
    }

    old_offset = t->current_offset;
    available = record->len - old_offset;
    len = MIN((uint32_t)ccw_dstream_avail(terminal_cds(t)), available);
    ret = ccw_dstream_write_buf(terminal_cds(t),
                                record->data + old_offset, len);
    if (ret < 0) {
        terminal_discard_current_record(t);
        return ret;
    }

    terminal_sch(t)->curr_status.scsw.count =
        ccw_dstream_residual_count(terminal_cds(t));
    terminal_set_length_status(t, ccw, len, available);

    if (!old_offset) {
        t->aid = record->len ? record->data[0] : 0;
    }

    t->current_offset += len;
    if (t->current_offset == record->len && record->len < 3) {
        terminal_sch(t)->sense_data[0] = SENSE_DATA_CHECK;
        terminal_discard_current_record(t);
        return -EIO;
    }
    if (t->aid != SF3270_AID && t->current_offset > 3) {
        t->pos = t->current_start_pos;
        if (!terminal_walk_orders(t, record->data + 3,
                                  t->current_offset - 3,
                                  t->current_offset == record->len)) {
            terminal_sch(t)->sense_data[0] = SENSE_DATA_CHECK;
            terminal_discard_current_record(t);
            return -EIO;
        }
    }
    if (!(ccw->flags & CCW_FLAG_DC) || t->current_offset == record->len) {
        terminal_discard_current_record(t);
    }
    return len;
}

static int read_payload_3270(EmulatedCcw3270Device *dev, CCW1 *ccw)
{
    Terminal3270 *t = TERMINAL_3270(dev);
    bool data_chained = terminal_previous_data_chain(t);
    uint8_t command;
    int ret;

    if (!t->connected || !t->ready) {
        terminal_sch(t)->sense_data[0] = SENSE_INTERVENTION_REQUIRED;
        return -EIO;
    }

    /* An independent read starts at the beginning of the device buffer. */
    if (!terminal_sch(t)->last_cmd_valid) {
        t->pos = 0;
    }

    if (data_chained) {
        if (t->current_record) {
            return terminal_transfer_record(t, ccw);
        }
        terminal_set_length_status(t, ccw, 0, 0);
        return 0;
    }

    if (ccw->cmd_code == TC_READMOD && !QTAILQ_EMPTY(&t->records)) {
        return terminal_transfer_record(t, ccw);
    }

    terminal_discard_current_record(t);
    if (ccw->cmd_code == TC_RDBUF) {
        terminal_clear_records(t);
        command = TN3270_CMD_RDBUF;
    } else {
        command = TN3270_CMD_READMOD;
    }
    if (!terminal_send_record(t, &command, 1)) {
        return -EIO;
    }

    t->read_pending = true;
    t->pending_ccw = *ccw;
    trace_terminal3270_pending_read(terminal_sch(t)->devno,
                                    ccw->cmd_code, ccw->count);
    ret = CSS_CCW_PENDING;
    return ret;
}

static int terminal_control_3270(EmulatedCcw3270Device *dev, CCW1 *ccw)
{
    Terminal3270 *t = TERMINAL_3270(dev);
    SubchDev *sch = terminal_sch(t);
    uint8_t sense_id[] = {
        0xff,
        dev->cu_type >> 8,
        dev->cu_type,
        dev->cu_model,
        dev->dev_type >> 8,
        dev->dev_type,
        dev->dev_model,
    };
    uint8_t *data;
    uint32_t available;
    uint32_t len;
    int ret;

    /*
     * A disconnected terminal is not ready for normal I/O, but its
     * configured identity remains available.  Hercules likewise treats both
     * Basic Sense (0x04) and Sense ID (0xe4) as sense commands here.  This is
     * needed so that guests can discover and define terminal addresses before
     * a client connects.
     */
    if (ccw->cmd_code != TC_SENSE && ccw->cmd_code != TC_SENSEID &&
        (!t->connected || !t->ready)) {
        sch->sense_data[0] = SENSE_INTERVENTION_REQUIRED;
        return -EIO;
    }

    switch (ccw->cmd_code) {
    case TC_NOP:
        t->pos = 0;
        sch->curr_status.scsw.count = ccw->count;
        return 0;
    case TC_SENSE:
        data = sch->sense_data;
        available = 1;
        break;
    case TC_SENSEID:
        data = sense_id;
        available = sizeof(sense_id);
        break;
    default:
        return -ENOSYS;
    }

    len = MIN((uint32_t)ccw_dstream_avail(terminal_cds(t)), available);
    ret = ccw_dstream_write_buf(terminal_cds(t), data, len);
    if (ret < 0) {
        return ret;
    }
    sch->curr_status.scsw.count = ccw_dstream_residual_count(terminal_cds(t));
    terminal_set_length_status(t, ccw, len, available);
    if (ccw->cmd_code == TC_SENSE) {
        memset(sch->sense_data, 0, sizeof(sch->sense_data));
    }
    return 0;
}

static void terminal_cancel(EmulatedCcw3270Device *dev)
{
    Terminal3270 *t = TERMINAL_3270(dev);

    trace_terminal3270_cancel(terminal_sch(t)->devno);
    terminal_cancel_pending(t);
    g_byte_array_set_size(t->write_record, 0);
    t->pos = 0;
}

static void terminal_reset(DeviceState *dev)
{
    Terminal3270 *t = TERMINAL_3270(dev);

    terminal_cancel_pending(t);
    terminal_clear_records(t);
    g_byte_array_set_size(t->write_record, 0);
    g_byte_array_set_size(t->replay, 0);
    t->record_len = 0;
    t->subneg_len = 0;
    t->parse_state = TN3270_PARSE_DATA;
    t->pos = 0;
    t->ewa = false;
    t->aid = 0;
}

static int terminal_migration_notify(NotifierWithReturn *notifier,
                                     MigrationEvent *event, Error **errp)
{
    Terminal3270 *t = container_of(notifier, Terminal3270,
                                   migration_notifier);

    if (event->type == MIG_EVENT_SETUP && t->connected) {
        /* TCP is external state: preserve the controller, then reconnect. */
        if (t->read_pending) {
            terminal_cancel_pending(t);
            terminal_set_unit_check(t, SENSE_DATA_CHECK);
            css_virtual_ccw_complete(terminal_sch(t), -EIO);
        }
        t->migration_disconnect = true;
        t->reconnect_restore = true;
        qemu_chr_fe_disconnect(&t->chr);
        t->connected = false;
        t->ready = false;
        terminal_timer_cancel(t);
    }
    return 0;
}

static int terminal_pre_save(void *opaque)
{
    Terminal3270 *t = opaque;
    TN3270Record *record;
    uint32_t offset = 0;

    g_free(t->migration_records);
    g_free(t->migration_write);
    g_free(t->migration_replay);
    t->migration_records = NULL;
    t->migration_write = NULL;
    t->migration_replay = NULL;
    t->migration_records_size = 0;
    t->migration_record_count = 0;
    t->migration_has_current = t->current_record != NULL;

    if (t->current_record) {
        t->migration_record_len[t->migration_record_count++] =
            t->current_record->len;
        t->migration_records_size += t->current_record->len;
    }
    QTAILQ_FOREACH(record, &t->records, next) {
        assert(t->migration_record_count <
               ARRAY_SIZE(t->migration_record_len));
        t->migration_record_len[t->migration_record_count++] = record->len;
        t->migration_records_size += record->len;
    }

    if (t->migration_records_size) {
        t->migration_records = g_malloc(t->migration_records_size);
        if (t->current_record) {
            memcpy(t->migration_records, t->current_record->data,
                   t->current_record->len);
            offset += t->current_record->len;
        }
        QTAILQ_FOREACH(record, &t->records, next) {
            memcpy(t->migration_records + offset, record->data, record->len);
            offset += record->len;
        }
    }

    t->migration_write_len = t->write_record->len;
    t->migration_write = g_memdup2(t->write_record->data,
                                   t->migration_write_len);
    t->migration_replay_len = t->replay->len;
    t->migration_replay = g_memdup2(t->replay->data,
                                    t->migration_replay_len);
    return 0;
}

static int terminal_post_load(void *opaque, int version_id)
{
    Terminal3270 *t = opaque;
    uint32_t saved_offset = t->current_offset;
    uint16_t saved_start_pos = t->current_start_pos;
    uint32_t total = 0;
    uint32_t offset = 0;
    unsigned int i;
    uint32_t replay_offset = 0;

    if (t->migration_record_count > ARRAY_SIZE(t->migration_record_len) ||
        (t->migration_has_current && !t->migration_record_count) ||
        t->migration_write_len > TN3270_MAX_RECORD_SIZE ||
        t->migration_replay_len > TN3270_MAX_REPLAY_SIZE) {
        return -EINVAL;
    }
    for (i = 0; i < t->migration_record_count; i++) {
        if (t->migration_record_len[i] > TN3270_MAX_RECORD_SIZE ||
            total > UINT32_MAX - t->migration_record_len[i]) {
            return -EINVAL;
        }
        total += t->migration_record_len[i];
    }
    if (total != t->migration_records_size ||
        total > TN3270_MAX_QUEUED_BYTES + TN3270_MAX_RECORD_SIZE ||
        (t->migration_has_current &&
         saved_offset > t->migration_record_len[0])) {
        return -EINVAL;
    }
    while (replay_offset < t->migration_replay_len) {
        uint32_t len;

        if (t->migration_replay_len - replay_offset < sizeof(uint32_t)) {
            return -EINVAL;
        }
        len = ldl_be_p(t->migration_replay + replay_offset);
        replay_offset += sizeof(uint32_t);
        if (!len || len > t->migration_replay_len - replay_offset) {
            return -EINVAL;
        }
        replay_offset += len;
    }

    if (t->connected) {
        t->migration_disconnect = true;
        qemu_chr_fe_disconnect(&t->chr);
    }
    terminal_clear_records(t);
    for (i = 0; i < t->migration_record_count; i++) {
        uint32_t len = t->migration_record_len[i];
        TN3270Record *record = g_malloc(sizeof(*record) + len);

        record->len = len;
        memcpy(record->data, t->migration_records + offset, len);
        offset += len;
        if (i == 0 && t->migration_has_current) {
            t->current_record = record;
            t->current_offset = saved_offset;
            t->current_start_pos = saved_start_pos;
        } else {
            QTAILQ_INSERT_TAIL(&t->records, record, next);
            t->queued_records++;
            t->queued_bytes += len;
        }
    }
    g_byte_array_set_size(t->write_record, 0);
    g_byte_array_append(t->write_record, t->migration_write,
                        t->migration_write_len);
    g_byte_array_set_size(t->replay, 0);
    g_byte_array_append(t->replay, t->migration_replay,
                        t->migration_replay_len);

    t->connected = false;
    t->ready = false;
    t->read_pending = false;
    t->record_len = 0;
    t->subneg_len = 0;
    t->parse_state = TN3270_PARSE_DATA;
    t->local_binary = false;
    t->peer_binary = false;
    t->local_eor = false;
    t->peer_eor = false;
    t->peer_ttype = false;
    t->ttype_valid = false;
    t->reconnect_restore = true;
    return 0;
}

static char *terminal_get_type(Object *obj, Error **errp)
{
    Terminal3270 *t = TERMINAL_3270(obj);

    return g_strdup((char *)t->terminal_type);
}

static bool terminal_get_connected(Object *obj, Error **errp)
{
    return TERMINAL_3270(obj)->connected;
}

static bool terminal_get_ready(Object *obj, Error **errp)
{
    return TERMINAL_3270(obj)->ready;
}

static bool terminal_get_eab(Object *obj, Error **errp)
{
    return TERMINAL_3270(obj)->eab;
}

static void terminal_instance_init(Object *obj)
{
    Terminal3270 *t = TERMINAL_3270(obj);

    QTAILQ_INIT(&t->records);
    t->write_record = g_byte_array_new();
    t->replay = g_byte_array_new();
    t->model = 2;
    t->rows = 24;
    t->cols = 80;
    object_property_add_uint8_ptr(obj, "model", &t->model,
                                  OBJ_PROP_FLAG_READ);
    object_property_add_uint16_ptr(obj, "rows", &t->rows,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint16_ptr(obj, "columns", &t->cols,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint16_ptr(obj, "queued-records", &t->queued_records,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_bool(obj, "connected", terminal_get_connected, NULL);
    object_property_add_bool(obj, "ready", terminal_get_ready, NULL);
    object_property_add_bool(obj, "extended-attributes", terminal_get_eab,
                             NULL);
}

static void terminal_instance_finalize(Object *obj)
{
    Terminal3270 *t = TERMINAL_3270(obj);

    terminal_timer_cancel(t);
    terminal_clear_records(t);
    g_byte_array_unref(t->write_record);
    g_byte_array_unref(t->replay);
    g_free(t->migration_records);
    g_free(t->migration_write);
    g_free(t->migration_replay);
}

static void terminal_unrealize(DeviceState *dev)
{
    Terminal3270 *t = TERMINAL_3270(dev);

    terminal_sch(t)->enable_cb = NULL;
    terminal_sch(t)->status_clear_cb = NULL;
    migration_remove_notifier(&t->migration_notifier);
    qemu_chr_fe_deinit(&t->chr, false);
}

static const Property terminal_properties[] = {
    DEFINE_PROP_CHR("chardev", Terminal3270, chr),
};

static const VMStateDescription terminal3270_vmstate = {
    .name = TYPE_TERMINAL_3270,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = terminal_pre_save,
    .post_load = terminal_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(terminal_type, Terminal3270,
                            TN3270_MAX_TTYPE_SIZE),
        VMSTATE_UINT8(model, Terminal3270),
        VMSTATE_UINT16(rows, Terminal3270),
        VMSTATE_UINT16(cols, Terminal3270),
        VMSTATE_BOOL(eab, Terminal3270),
        VMSTATE_UINT16(pos, Terminal3270),
        VMSTATE_BOOL(ewa, Terminal3270),
        VMSTATE_UINT8(aid, Terminal3270),
        VMSTATE_UINT32(current_offset, Terminal3270),
        VMSTATE_UINT16(current_start_pos, Terminal3270),
        VMSTATE_UINT32(migration_records_size, Terminal3270),
        VMSTATE_UINT16(migration_record_count, Terminal3270),
        VMSTATE_BOOL(migration_has_current, Terminal3270),
        VMSTATE_UINT32_ARRAY(migration_record_len, Terminal3270,
                             TN3270_MAX_QUEUED_RECORDS + 1),
        VMSTATE_VBUFFER_ALLOC_UINT32(migration_records, Terminal3270, 1, NULL,
                                     migration_records_size),
        VMSTATE_UINT8(write_command, Terminal3270),
        VMSTATE_UINT32(migration_write_len, Terminal3270),
        VMSTATE_VBUFFER_ALLOC_UINT32(migration_write, Terminal3270, 1, NULL,
                                     migration_write_len),
        VMSTATE_UINT32(migration_replay_len, Terminal3270),
        VMSTATE_VBUFFER_ALLOC_UINT32(migration_replay, Terminal3270, 1, NULL,
                                     migration_replay_len),
        VMSTATE_END_OF_LIST()
    },
};

static void terminal_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    EmulatedCcw3270Class *ck = EMULATED_CCW_3270_CLASS(klass);

    device_class_set_props(dc, terminal_properties);
    device_class_set_legacy_reset(dc, terminal_reset);
    dc->unrealize = terminal_unrealize;
    object_class_property_add_str(klass, "terminal-type", terminal_get_type,
                                  NULL);
    dc->vmsd = &terminal3270_vmstate;
    ck->init = terminal_init;
    ck->cancel = terminal_cancel;
    ck->control_3270 = terminal_control_3270;
    ck->read_payload_3270 = read_payload_3270;
    ck->write_payload_3270 = write_payload_3270;
}

static const TypeInfo ccw_terminal_info = {
    .name = TYPE_TERMINAL_3270,
    .parent = TYPE_EMULATED_CCW_3270,
    .instance_size = sizeof(Terminal3270),
    .instance_init = terminal_instance_init,
    .instance_finalize = terminal_instance_finalize,
    .class_init = terminal_class_init,
    .class_size = sizeof(EmulatedCcw3270Class),
};

static void register_types(void)
{
    type_register_static(&ccw_terminal_info);
}

type_init(register_types)
