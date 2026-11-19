/*
 * SCLP event types
 *    Operations Command - Line Mode input
 *    Message            - Line Mode output
 *
 * Copyright IBM, Corp. 2013
 *
 * Authors:
 *  Heinz Graalfs <graalfs@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at your
 * option) any later version.  See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "trace.h"
#include "qemu/thread.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/readline.h"
#include "chardev/char-fe.h"

#include "hw/s390x/sclp.h"
#include "migration/vmstate.h"
#include "hw/s390x/event-facility.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/s390x/ebcdic.h"
#include "qom/object.h"

#define SIZE_BUFFER 4096
#define NEWLINE     "\n"

/* operator commands entered while the guest has not read the previous one */
#define MAX_QUEUED_COMMANDS 16

typedef struct OprtnsCommand {
    EventBufferHeader header;
    MDMSU message_unit;
    char data[];
} QEMU_PACKED OprtnsCommand;

/* max size for line-mode data in 4K SCCB page */
#define SIZE_CONSOLE_BUFFER (SCCB_DATA_LEN - sizeof(OprtnsCommand))

/*
 * The command is nested in two one-byte-length GDS subvectors.  The outer
 * self-defining-text subvector contains both subvector headers, so it is the
 * tighter limit.
 */
#define MAX_COMMAND_LEN (UINT8_MAX - (2 * sizeof(GdsSubvector)))

struct SCLPConsoleLM {
    SCLPEvent event;
    CharFrontend chr;
    CharFrontend hardcopy;      /* log of the messages and IPLs (optional) */
    Notifier ipl_notifier;
    GQueue commands;            /* queued operator commands (strings)     */
    bool echo;                  /* immediate echo of input if true        */
    bool last_was_cr;           /* swallow the LF of a CR LF pair         */
    uint32_t codepage;          /* EBCDIC code page of the guest           */
    Utf8Decoder utf8;           /* input from the character device         */
    ReadLineState *rs;          /* line editor for echoed operator input  */
    uint32_t write_errors;      /* errors writing to char layer           */
    uint32_t length;            /* length of byte stream in buffer        */
    uint8_t buf[SIZE_CONSOLE_BUFFER];
};
typedef struct SCLPConsoleLM SCLPConsoleLM;

#define TYPE_SCLPLM_CONSOLE "sclplmconsole"
DECLARE_INSTANCE_CHECKER(SCLPConsoleLM, SCLPLM_CONSOLE,
                         TYPE_SCLPLM_CONSOLE)

/*
 * The hardcopy: like the printer log of a hardware console, every message
 * of the guest as one line, and a marker line for every IPL.  It keeps
 * everything since the VM was started, whether or not a client is connected
 * to the console.  UTF-8, LF line ends.
 */
static void hardcopy_write(SCLPConsoleLM *scon, const uint8_t *buf, int len)
{
    if (qemu_chr_fe_backend_connected(&scon->hardcopy)) {
        qemu_chr_fe_write_all(&scon->hardcopy, buf, len);
    }
}

static void hardcopy_ipl(Notifier *notifier, void *data)
{
    SCLPConsoleLM *scon = container_of(notifier, SCLPConsoleLM, ipl_notifier);
    S390IPLNotice *notice = data;
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *time = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
    g_autofree char *line = NULL;

    /* U+2500 BOX DRAWINGS LIGHT HORIZONTAL, like a perforation line */
    line = g_strdup_printf("\u2500\u2500\u2500\u2500 IPL%s%s%s%s at %s "
                           "\u2500\u2500\u2500\u2500\n",
                           *notice->device ? " " : "", notice->device,
                           *notice->loadparm ? " LOADPARM " : "",
                           notice->loadparm, time);
    hardcopy_write(scon, (uint8_t *)line, strlen(line));
}

/* Hand the next queued operator command to the guest, if there is one. */
static bool start_queued_command(SCLPConsoleLM *scon)
{
    g_autofree char *cmd = g_queue_pop_head(&scon->commands);

    if (!cmd) {
        return false;
    }
    scon->length = MIN(strlen(cmd), MAX_COMMAND_LEN);
    memcpy(scon->buf, cmd, scon->length);
    scon->event.event_pending = true;
    return true;
}

static void clear_queued_commands(SCLPConsoleLM *scon)
{
    g_queue_clear_full(&scon->commands, g_free);
}

/*
*  Character layer call-back functions
 *
 * Allow 1 character at a time
 *
 * Accumulate bytes from character layer in console buffer,
 * event_pending is set when a newline character is encountered
 *
 * The maximum command line length is limited by the maximum
 * space available in an SCCB. Line mode console input is sent
 * truncated to the guest in case it doesn't fit into the SCCB.
 */

static int chr_can_read(void *opaque)
{
    SCLPConsoleLM *scon = opaque;

    /*
     * The line editor keeps accepting input while the guest has not read
     * the previous command yet: commands entered meanwhile are queued, so
     * the operator still sees what they type.
     */
    if (scon->rs) {
        return 1;
    }
    if (scon->event.event_pending) {
        return 0;
    }
    return 1;
}

/*
 * The console works in ISO 8859-1, the character set the EBCDIC code page
 * maps onto; the character device carries UTF-8.  Write ISO 8859-1 text
 * to the character device, as UTF-8.
 */
static int write_latin1(SCLPConsoleLM *scon, const uint8_t *buf, int len)
{
    g_autofree uint8_t *utf8 = g_malloc(len * 2);
    int n = latin1_to_utf8(utf8, buf, len);

    /*
     * XXX this blocks entire thread. Rewrite to use
     * qemu_chr_fe_write and background I/O callbacks
     */
    return qemu_chr_fe_write_all(&scon->chr, utf8, n);
}

static void echo_output(SCLPConsoleLM *scon, const void *buf, int len)
{
    if (scon->echo) {
        write_latin1(scon, buf, len);
    }
}

static void G_GNUC_PRINTF(2, 3) lm_readline_printf(void *opaque,
                                                   const char *fmt, ...)
{
    SCLPConsoleLM *scon = opaque;
    g_autofree char *str = NULL;
    va_list ap;

    va_start(ap, fmt);
    str = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    echo_output(scon, str, strlen(str));
}

static void lm_readline_flush(void *opaque)
{
}

static void lm_readline_completion(void *opaque, const char *cmdline)
{
    /* No completion: Tab offers nothing. */
}

/*
 * A complete operator command from the line editor.  The editor has
 * already echoed the line end; hand the command to the guest.
 */
static void lm_readline_command(void *opaque, const char *str,
                                void *readline_opaque)
{
    SCLPConsoleLM *scon = opaque;

    if (scon->event.event_pending) {
        if (g_queue_get_length(&scon->commands) >= MAX_QUEUED_COMMANDS) {
            echo_output(scon, "(command not sent: the system has not read "
                        "the previous commands)" NEWLINE,
                        strlen("(command not sent: the system has not read "
                               "the previous commands)" NEWLINE));
            return;
        }
        g_queue_push_tail(&scon->commands, g_strdup(str));
        return;
    }
    scon->length = MIN(strlen(str), MAX_COMMAND_LEN);
    memcpy(scon->buf, str, scon->length);
    scon->event.event_pending = true;
    sclp_service_interrupt(0);
}

static void chr_read(void *opaque, const uint8_t *buf, int size)
{
    SCLPConsoleLM *scon = opaque;
    bool after_cr = scon->last_was_cr;
    int c;

    assert(size == 1);

    c = utf8_decode_latin1(&scon->utf8, *buf);
    if (c < 0) {
        return;
    }
    buf = &(uint8_t){ c };

    scon->last_was_cr = *buf == '\r';
    if (*buf == '\n' && after_cr) {
        /* CR LF from a terminal or socket client is a single line end. */
        return;
    }
    if (scon->rs) {
        /*
         * Echoed (interactive) input is edited locally, like on a real
         * line-mode console, and only delivered to the guest on Enter:
         * cursor keys, Home/End, Insert-style editing, Backspace/Delete,
         * ^U/^W and command history all act on the local line.
         */
        readline_handle_byte(scon->rs, *buf);
        return;
    }
    if (*buf == '\r' || *buf == '\n') {
        /* Leave the cursor at the start of the line after the command. */
        echo_output(scon, NEWLINE, 1);
        scon->event.event_pending = true;
        sclp_service_interrupt(0);
        return;
    }
    if (*buf == '\b' || *buf == 0x7f) {
        /* Raw terminals (e.g. -nographic) deliver erase to the device. */
        if (scon->length) {
            scon->length--;
            echo_output(scon, "\b \b", 3);
        }
        return;
    }
    if (scon->length == MAX_COMMAND_LEN) {
        /* Eat the character, but still process CR and LF.  */
        return;
    }
    scon->buf[scon->length] = *buf;
    scon->length += 1;
    echo_output(scon, buf, size);
}

/* functions to be called by event facility */

static bool can_handle_event(uint8_t type)
{
    return type == SCLP_EVENT_MESSAGE || type == SCLP_EVENT_PMSGCMD;
}

static sccb_mask_t send_mask(void)
{
    return SCLP_EVENT_MASK_OP_CMD | SCLP_EVENT_MASK_PMSGCMD;
}

static sccb_mask_t receive_mask(void)
{
    return SCLP_EVENT_MASK_MSG | SCLP_EVENT_MASK_PMSGCMD;
}

/*
 * Triggered by SCLP's read_event_data
 * - convert ASCII byte stream to EBCDIC and
 * - copy converted data into provided (SCLP) buffer
 */
static int get_console_data(SCLPEvent *event, uint8_t *buf, size_t *size,
                            int avail)
{
    int len;

    SCLPConsoleLM *cons = SCLPLM_CONSOLE(event);

    len = cons->length;
    /* data need to fit into provided SCLP buffer */
    if (len > avail) {
        return 1;
    }

    ebcdic_put_cp(cons->codepage, buf, (char *)&cons->buf, len);
    *size = len;
    cons->length = 0;
    /*
     * Data provided; a queued command is pending next (the completion of
     * this read tells the guest), otherwise no more data is pending.
     */
    event->event_pending = false;
    start_queued_command(cons);
    qemu_notify_event();
    return 0;
}

static int read_event_data(SCLPEvent *event, EventBufferHeader *evt_buf_hdr,
                           int *slen)
{
    int avail, rc;
    size_t src_len;
    uint8_t *to;
    OprtnsCommand *oc = (OprtnsCommand *) evt_buf_hdr;
    size_t mdmsu_len;
    size_t cpmsu_len;
    size_t text_command_len;
    size_t self_def_text_len;
    size_t text_message_len;

    if (!event->event_pending) {
        /* no data pending */
        return 0;
    }
    if (*slen < (int)sizeof(*oc)) {
        return 1;
    }

    /* READ EVENT DATA does not require the guest to clear its SCCB. */
    memset(oc, 0, sizeof(*oc));

    to = (uint8_t *)&oc->data;
    avail = *slen - sizeof(OprtnsCommand);
    rc = get_console_data(event, to, &src_len, avail);
    if (rc) {
        /* data didn't fit, try next SCCB */
        return 1;
    }

    g_assert(src_len <= MAX_COMMAND_LEN);

    text_message_len = sizeof(GdsSubvector) + src_len;
    self_def_text_len = sizeof(GdsSubvector) + text_message_len;
    text_command_len = sizeof(GdsVector) + self_def_text_len;
    cpmsu_len = sizeof(GdsVector) + text_command_len;
    mdmsu_len = sizeof(GdsVector) + cpmsu_len;

    /* The GDS IDs are big-endian, like the lengths. */
    oc->message_unit.mdmsu.gds_id = cpu_to_be16(GDS_ID_MDSMU);
    oc->message_unit.mdmsu.length = cpu_to_be16(mdmsu_len);

    oc->message_unit.cpmsu.gds_id = cpu_to_be16(GDS_ID_CPMSU);
    oc->message_unit.cpmsu.length = cpu_to_be16(cpmsu_len);

    oc->message_unit.text_command.gds_id = cpu_to_be16(GDS_ID_TEXTCMD);
    oc->message_unit.text_command.length = cpu_to_be16(text_command_len);

    oc->message_unit.self_def_text_message.key = GDS_KEY_SELFDEFTEXTMSG;
    oc->message_unit.self_def_text_message.length = self_def_text_len;

    oc->message_unit.text_message.key = GDS_KEY_TEXTMSG;
    oc->message_unit.text_message.length = text_message_len;

    oc->header.length = cpu_to_be16(sizeof(OprtnsCommand) + src_len);
    oc->header.type = SCLP_EVENT_OPRTNS_COMMAND;
    *slen = avail - src_len;

    return 1;
}

/*
 * Triggered by SCLP's write_event_data
 *  - write console data to character layer
 *  returns < 0 if an error occurred
 */
static int write_console_data(SCLPEvent *event, const uint8_t *buf, int len)
{
    SCLPConsoleLM *scon = SCLPLM_CONSOLE(event);

    if (!qemu_chr_fe_backend_connected(&scon->chr)) {
        /* If there's no backend, we can just say we consumed all data. */
        return len;
    }

    return write_latin1(scon, buf, len);
}

static int process_mdb(SCLPEvent *event, MDBO *mdbo)
{
    SCLPConsoleLM *scon = SCLPLM_CONSOLE(event);
    bool partial_input = scon->rs ? scon->rs->cmd_buf_size > 0 :
                         scon->echo && scon->length && !event->event_pending;
    int rc;
    int len;
    int i;
    int start = partial_input ? 1 : 0;
    /* the message, a line end, and one before it when input is pending */
    QEMU_UNINITIALIZED uint8_t buffer[SIZE_BUFFER + 2];

    len = be16_to_cpu(mdbo->length);
    len -= sizeof(mdbo->length) + sizeof(mdbo->type)
            + sizeof(mdbo->mto.line_type_flags)
            + sizeof(mdbo->mto.alarm_control)
            + sizeof(mdbo->mto._reserved);

    assert(len <= SIZE_BUFFER);

    /* convert EBCDIC SCLP contents to an ISO 8859-1 console message */
    ascii_put_cp(scon->codepage, buffer + start, mdbo->mto.message, len);
    for (i = start; i < start + len; i++) {
        /* A message is one line: show control characters as blanks. */
        if (buffer[i] < 0x20 || (buffer[i] >= 0x7f && buffer[i] < 0xa0)) {
            buffer[i] = ' ';
        }
    }
    trace_sclp_lm_message(len);

    /*
     * Each message is a complete line: terminate it, so that the cursor
     * (and operator input) starts on a fresh line.  If the operator is in
     * the middle of typing a command, finish that line first and repeat
     * the partial input after the message.  The line is written in one
     * piece: a backend without a client (a socket nobody is connected to)
     * fails the write, but still logs it, and the log must keep the line
     * ends.
     */
    if (partial_input) {
        buffer[0] = '\n';
    }
    buffer[start + len] = '\n';
    if (qemu_chr_fe_backend_connected(&scon->hardcopy)) {
        g_autofree uint8_t *utf8 = g_malloc((len + 1) * 2);
        int n = latin1_to_utf8(utf8, buffer + start, len + 1);

        hardcopy_write(scon, utf8, n);
    }
    rc = write_console_data(event, buffer, start + len + 1);
    if (rc < 0) {
        return rc;
    }
    if (partial_input) {
        if (scon->rs) {
            readline_redisplay(scon->rs);
        } else {
            rc = write_console_data(event, scon->buf, scon->length);
        }
    }
    return rc;
}

static int write_event_data(SCLPEvent *event, EventBufferHeader *ebh)
{
    int len;
    int written;
    int errors = 0;
    MDBO *mdbo;
    SclpMsg *data = (SclpMsg *) ebh;
    SCLPConsoleLM *scon = SCLPLM_CONSOLE(event);

    len = be16_to_cpu(data->mdb.header.length);
    if (len < sizeof(data->mdb.header) ||
        len > be16_to_cpu(data->header.length) - sizeof(EventBufferHeader)) {
        return SCLP_RC_INCONSISTENT_LENGTHS;
    }
    len -= sizeof(data->mdb.header);

    /* first check message buffers */
    mdbo = data->mdb.mdbo;
    while (len > 0) {
        if (be16_to_cpu(mdbo->length) > len
                || be16_to_cpu(mdbo->length) == 0) {
            return SCLP_RC_INCONSISTENT_LENGTHS;
        }
        len -= be16_to_cpu(mdbo->length);
        mdbo = (void *) mdbo + be16_to_cpu(mdbo->length);
    }

    /* then execute */
    len = be16_to_cpu(data->mdb.header.length) - sizeof(data->mdb.header);
    mdbo = data->mdb.mdbo;
    while (len > 0) {
        trace_sclp_lm_mdb_object(be16_to_cpu(mdbo->type),
                                 be16_to_cpu(mdbo->length));
        switch (be16_to_cpu(mdbo->type)) {
        case MESSAGE_TEXT:
            /* message text object */
            written = process_mdb(event, mdbo);
            if (written < 0) {
                /* character layer error */
                errors++;
            }
            break;
        default: /* ignore */
            break;
        }
        len -= be16_to_cpu(mdbo->length);
        mdbo = (void *) mdbo + be16_to_cpu(mdbo->length);
    }
    if (errors) {
        scon->write_errors += errors;
    }
    data->header.flags = SCLP_EVENT_BUFFER_ACCEPTED;

    return SCLP_RC_NORMAL_COMPLETION;
}

/* functions for live migration */

static int console_post_load(void *opaque, int version_id)
{
    SCLPConsoleLM *scon = opaque;

    /* tell the guest again, the signal may not have been presented yet */
    if (scon->event.event_pending) {
        sclp_service_interrupt(0);
    }
    return 0;
}

static const VMStateDescription vmstate_sclplmconsole = {
    .name = "sclplmconsole",
    .version_id = 0,
    .minimum_version_id = 0,
    .post_load = console_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(event.event_pending, SCLPConsoleLM),
        VMSTATE_UINT32(write_errors, SCLPConsoleLM),
        VMSTATE_UINT32(length, SCLPConsoleLM),
        VMSTATE_UINT8_ARRAY(buf, SCLPConsoleLM, SIZE_CONSOLE_BUFFER),
        VMSTATE_END_OF_LIST()
     }
};

/* qemu object creation and initialization functions */

/* tell character layer our call-back functions */

static int console_init(SCLPEvent *event)
{
    static bool console_available;

    SCLPConsoleLM *scon = SCLPLM_CONSOLE(event);

    if (console_available) {
        error_report("Multiple line-mode operator consoles are not supported");
        return -1;
    }
    if (!ebcdic_codepage_valid(scon->codepage)) {
        error_report("sclplmconsole: unsupported codepage %u "
                     "(supported: 37, 500)", scon->codepage);
        return -1;
    }
    console_available = true;

    g_queue_init(&scon->commands);
    scon->ipl_notifier.notify = hardcopy_ipl;
    s390_ipl_add_notifier(&scon->ipl_notifier);

    if (scon->echo) {
        scon->rs = readline_init(lm_readline_printf, lm_readline_flush, scon,
                                 lm_readline_completion);
        readline_start(scon->rs, "", 0, lm_readline_command, scon);
    }
    qemu_chr_fe_set_handlers(&scon->chr, chr_can_read,
                             chr_read, NULL, NULL, scon, NULL, true);

    return 0;
}

static void console_reset(DeviceState *dev)
{
    SCLPEvent *event = SCLP_EVENT(dev);
    SCLPConsoleLM *scon = SCLPLM_CONSOLE(event);

    event->event_pending = false;
    scon->length = 0;
    scon->last_was_cr = false;
    scon->utf8.pending = 0;
    scon->write_errors = 0;
    clear_queued_commands(scon);
    if (scon->rs) {
        readline_restart(scon->rs);
    }
}

static const Property console_properties[] = {
    DEFINE_PROP_CHR("chardev", SCLPConsoleLM, chr),
    /*
     * Optional: a character device (normally a file) that gets the
     * hardcopy, every message as a line and a marker line for every IPL.
     */
    DEFINE_PROP_CHR("hardcopy", SCLPConsoleLM, hardcopy),
    DEFINE_PROP_UINT32("write_errors", SCLPConsoleLM, write_errors, 0),
    DEFINE_PROP_BOOL("echo", SCLPConsoleLM, echo, true),
    /*
     * EBCDIC code page of the guest's console: 37 (z/OS, z/VSE, z/VM) or
     * 500 (Linux on LPAR or KVM decodes the line-mode console as 500).
     */
    DEFINE_PROP_UINT32("codepage", SCLPConsoleLM, codepage, EBCDIC_CP037),
};

static void console_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCLPEventClass *ec = SCLP_EVENT_CLASS(klass);

    device_class_set_props(dc, console_properties);
    device_class_set_legacy_reset(dc, console_reset);
    dc->vmsd = &vmstate_sclplmconsole;
    ec->init = console_init;
    ec->get_send_mask = send_mask;
    ec->get_receive_mask = receive_mask;
    ec->can_handle_event = can_handle_event;
    ec->read_event_data = read_event_data;
    ec->write_event_data = write_event_data;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo sclp_console_info = {
    .name          = TYPE_SCLPLM_CONSOLE,
    .parent        = TYPE_SCLP_EVENT,
    .instance_size = sizeof(SCLPConsoleLM),
    .class_init    = console_class_init,
    .class_size    = sizeof(SCLPEventClass),
};

static void register_types(void)
{
    type_register_static(&sclp_console_info);
}

type_init(register_types)
