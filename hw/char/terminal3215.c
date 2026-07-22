/*
 * IBM 3215 console printer-keyboard
 *
 * This models the channel command interface of the 3215-C console used by
 * Hercules.  The operator side is an ordinary QEMU character backend.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/s390x/css-bridge.h"
#include "hw/s390x/ebcdic.h"
#include "hw/s390x/terminal3215.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "trace.h"

#define TERM3215_MAX_RECORD_SIZE     150
#define TERM3215_MAX_QUEUED_RECORDS  16
#define TERM3215_CHPID_TYPE          0x1a

#define TERM3215_CMD_WRITE           0x01
#define TERM3215_CMD_NOP             0x03
#define TERM3215_CMD_SENSE           0x04
#define TERM3215_CMD_WRITE_CR        0x09
#define TERM3215_CMD_READ_INQUIRY    0x0a
#define TERM3215_CMD_ALARM           0x0b
#define TERM3215_CMD_SENSE_ID        0xe4

#define TERM3215_SENSE_COMMAND_REJECT         0x80
#define TERM3215_SENSE_INTERVENTION_REQUIRED  0x40
#define TERM3215_SENSE_EQUIPMENT_CHECK        0x10

struct Terminal3215 {
    CcwDevice parent_obj;

    CharFrontend chr;
    bool echo;
    bool connected;
    bool read_pending;
    bool attention_pending;
    bool saw_cr;
    bool input_overflow;
    CCW1 pending_ccw;

    uint8_t input[TERM3215_MAX_RECORD_SIZE];
    uint16_t input_len;
    uint8_t records[TERM3215_MAX_QUEUED_RECORDS]
                   [TERM3215_MAX_RECORD_SIZE];
    uint16_t record_len[TERM3215_MAX_QUEUED_RECORDS];
    uint8_t record_head;
    uint8_t record_count;
    uint16_t record_offset;
};

static SubchDev *terminal3215_sch(Terminal3215 *t)
{
    return CCW_DEVICE(t)->sch;
}

static void terminal3215_set_length_status(Terminal3215 *t, const CCW1 *ccw,
                                           uint32_t transferred,
                                           uint32_t available)
{
    bool suppress = (ccw->flags & CCW_FLAG_SLI) &&
                    !(ccw->flags & CCW_FLAG_DC);
    bool continues = (ccw->flags & CCW_FLAG_DC) && transferred < available;

    if (!suppress && !continues && ccw->count != available) {
        terminal3215_sch(t)->curr_status.scsw.cstat |=
            SCSW_CSTAT_INCORR_LEN;
    }
}

static int terminal3215_unit_check(Terminal3215 *t, uint8_t sense)
{
    SubchDev *sch = terminal3215_sch(t);
    SCHIB *schib = &sch->curr_status;

    sch->sense_data[0] = sense;
    schib->scsw.dstat = SCSW_DSTAT_CHANNEL_END |
                        SCSW_DSTAT_DEVICE_END |
                        SCSW_DSTAT_UNIT_CHECK;
    schib->scsw.ctrl &= ~SCSW_ACTL_START_PEND;
    schib->scsw.ctrl &= ~SCSW_CTRL_MASK_STCTL;
    schib->scsw.ctrl |= SCSW_STCTL_PRIMARY | SCSW_STCTL_SECONDARY |
                        SCSW_STCTL_ALERT | SCSW_STCTL_STATUS_PEND;
    schib->scsw.cpa = sch->channel_prog + 8;
    return -EIO;
}

static void terminal3215_try_attention(Terminal3215 *t)
{
    if (t->attention_pending &&
        css_generate_unsolicited_io_interrupt(terminal3215_sch(t),
                                               SCSW_DSTAT_ATTENTION)) {
        t->attention_pending = false;
    }
}

static void terminal3215_status_cleared(SubchDev *sch)
{
    terminal3215_try_attention(TERMINAL_3215(sch->driver_data));
}

static void terminal3215_pop_record(Terminal3215 *t)
{
    g_assert(t->record_count);
    t->record_head = (t->record_head + 1) % TERM3215_MAX_QUEUED_RECORDS;
    t->record_count--;
    t->record_offset = 0;
    if (t->record_count) {
        t->attention_pending = true;
    }
    qemu_chr_fe_accept_input(&t->chr);
}

static int terminal3215_transfer_record(Terminal3215 *t, const CCW1 *ccw)
{
    SubchDev *sch = terminal3215_sch(t);
    uint16_t available;
    uint16_t len;
    int ret;

    g_assert(t->record_count);
    available = t->record_len[t->record_head] - t->record_offset;
    len = MIN((uint16_t)ccw_dstream_avail(&sch->cds), available);
    ret = ccw_dstream_write_buf(&sch->cds,
                                &t->records[t->record_head][t->record_offset],
                                len);
    if (ret < 0) {
        return ret;
    }
    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    terminal3215_set_length_status(t, ccw, len, available);

    if ((ccw->flags & CCW_FLAG_DC) && len < available) {
        t->record_offset += len;
    } else {
        terminal3215_pop_record(t);
    }
    return 0;
}

static void terminal3215_complete_read(Terminal3215 *t)
{
    int ret;

    if (!t->read_pending || !t->record_count) {
        return;
    }
    t->read_pending = false;
    ret = terminal3215_transfer_record(t, &t->pending_ccw);
    if (ret < 0) {
        terminal3215_unit_check(t, TERM3215_SENSE_EQUIPMENT_CHECK);
    }
    trace_terminal3215_complete_read(
        terminal3215_sch(t)->devno, ret,
        terminal3215_sch(t)->curr_status.scsw.count);
    css_virtual_ccw_complete(terminal3215_sch(t), ret < 0 ? ret : 0);
}

static void terminal3215_finish_line(Terminal3215 *t)
{
    uint8_t tail;
    bool was_empty;

    if (t->input_overflow || t->record_count == TERM3215_MAX_QUEUED_RECORDS) {
        static const uint8_t bell = '\a';

        qemu_chr_fe_write_all(&t->chr, &bell, 1);
        t->input_len = 0;
        t->input_overflow = false;
        return;
    }

    was_empty = t->record_count == 0;
    tail = (t->record_head + t->record_count) %
           TERM3215_MAX_QUEUED_RECORDS;
    ebcdic_put(t->records[tail], (char *)t->input, t->input_len);
    t->record_len[tail] = t->input_len;
    t->record_count++;
    trace_terminal3215_input(terminal3215_sch(t)->devno, t->input_len,
                             t->record_count);
    t->input_len = 0;

    if (t->read_pending) {
        terminal3215_complete_read(t);
    } else if (was_empty) {
        t->attention_pending = true;
        terminal3215_try_attention(t);
    }
}

static int terminal3215_can_read(void *opaque)
{
    Terminal3215 *t = opaque;

    return t->record_count == TERM3215_MAX_QUEUED_RECORDS ? 0 : 4096;
}

static void terminal3215_read(void *opaque, const uint8_t *buf, int size)
{
    Terminal3215 *t = opaque;
    int i;

    for (i = 0; i < size; i++) {
        uint8_t ch = buf[i];

        if (ch == '\n' && t->saw_cr) {
            t->saw_cr = false;
            continue;
        }
        t->saw_cr = ch == '\r';
        if (ch == '\r' || ch == '\n') {
            if (t->echo) {
                static const uint8_t newline[] = "\r\n";
                qemu_chr_fe_write_all(&t->chr, newline, 2);
            }
            terminal3215_finish_line(t);
        } else if (ch == 0x08 || ch == 0x7f) {
            if (t->input_len) {
                t->input_len--;
                if (t->echo) {
                    static const uint8_t erase[] = "\b \b";
                    qemu_chr_fe_write_all(&t->chr, erase, 3);
                }
            }
        } else if (ch >= 0x20) {
            if (t->input_len < sizeof(t->input)) {
                t->input[t->input_len++] = ch;
                if (t->echo) {
                    qemu_chr_fe_write_all(&t->chr, &ch, 1);
                }
            } else {
                t->input_overflow = true;
            }
        }
    }
}

static void terminal3215_event(void *opaque, QEMUChrEvent event)
{
    Terminal3215 *t = opaque;
    bool pending;

    switch (event) {
    case CHR_EVENT_OPENED:
        t->connected = true;
        trace_terminal3215_connection(terminal3215_sch(t)->devno, true);
        break;
    case CHR_EVENT_CLOSED:
        pending = t->read_pending;
        t->connected = false;
        t->read_pending = false;
        t->attention_pending = false;
        t->input_len = 0;
        t->input_overflow = false;
        t->record_head = 0;
        t->record_count = 0;
        t->record_offset = 0;
        trace_terminal3215_connection(terminal3215_sch(t)->devno, false);
        if (pending) {
            terminal3215_unit_check(t,
                TERM3215_SENSE_INTERVENTION_REQUIRED);
            css_virtual_ccw_complete(terminal3215_sch(t), -EIO);
        }
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        break;
    }
}

static int terminal3215_copy_response(Terminal3215 *t, const CCW1 *ccw,
                                      const uint8_t *data, uint16_t available)
{
    SubchDev *sch = terminal3215_sch(t);
    uint16_t len = MIN((uint16_t)ccw_dstream_avail(&sch->cds), available);
    int ret = ccw_dstream_write_buf(&sch->cds, (void *)data, len);

    if (ret < 0) {
        return ret;
    }
    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    terminal3215_set_length_status(t, ccw, len, available);
    return 0;
}

static int terminal3215_write(Terminal3215 *t, const CCW1 *ccw)
{
    SubchDev *sch = terminal3215_sch(t);
    uint16_t len = MIN((uint16_t)ccw_dstream_avail(&sch->cds),
                       (uint16_t)TERM3215_MAX_RECORD_SIZE);
    g_autofree uint8_t *ebcdic = g_malloc(len);
    g_autofree uint8_t *ascii = g_malloc(len);
    int ret;
    int i;

    ret = ccw_dstream_read_buf(&sch->cds, ebcdic, len);
    if (ret < 0) {
        return ret;
    }
    ascii_put(ascii, (char *)ebcdic, len);
    for (i = 0; i < len; i++) {
        if (ascii[i] != '\r' && ascii[i] != '\n' &&
            (ascii[i] < 0x20 || ascii[i] == 0x7f)) {
            ascii[i] = ' ';
        }
    }
    if (len && qemu_chr_fe_write_all(&t->chr, ascii, len) < 0) {
        return terminal3215_unit_check(t,
                                       TERM3215_SENSE_EQUIPMENT_CHECK);
    }
    if (ccw->cmd_code == TERM3215_CMD_WRITE_CR &&
        !(ccw->flags & CCW_FLAG_DC)) {
        static const uint8_t newline[] = "\r\n";

        if (qemu_chr_fe_write_all(&t->chr, newline, 2) < 0) {
            return terminal3215_unit_check(t,
                                           TERM3215_SENSE_EQUIPMENT_CHECK);
        }
    }
    sch->curr_status.scsw.count = ccw_dstream_residual_count(&sch->cds);
    terminal3215_set_length_status(t, ccw, len,
                                   MIN(ccw->count,
                                       TERM3215_MAX_RECORD_SIZE));
    return 0;
}

static int terminal3215_ccw_cb(SubchDev *sch, CCW1 ccw)
{
    Terminal3215 *t = TERMINAL_3215(sch->driver_data);
    static const uint8_t sense_id[] = {
        0xff, 0x32, 0x15, 0x00, 0x32, 0x15, 0x00,
    };
    int ret;

    trace_terminal3215_command(sch->devno, ccw.cmd_code, ccw.count,
                               ccw.flags);
    if (ccw.cmd_code != TERM3215_CMD_SENSE &&
        ccw.cmd_code != TERM3215_CMD_SENSE_ID && !t->connected) {
        return terminal3215_unit_check(t,
            TERM3215_SENSE_INTERVENTION_REQUIRED);
    }

    switch (ccw.cmd_code) {
    case TERM3215_CMD_WRITE:
    case TERM3215_CMD_WRITE_CR:
        return terminal3215_write(t, &ccw);
    case TERM3215_CMD_NOP:
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    case TERM3215_CMD_SENSE:
        ret = terminal3215_copy_response(t, &ccw, sch->sense_data, 1);
        if (ret == 0) {
            memset(sch->sense_data, 0, sizeof(sch->sense_data));
        }
        return ret;
    case TERM3215_CMD_READ_INQUIRY:
        if (t->record_count) {
            return terminal3215_transfer_record(t, &ccw);
        }
        t->read_pending = true;
        t->pending_ccw = ccw;
        trace_terminal3215_pending_read(sch->devno, ccw.count);
        return CSS_CCW_PENDING;
    case TERM3215_CMD_ALARM: {
        static const uint8_t bell = '\a';

        if (qemu_chr_fe_write_all(&t->chr, &bell, 1) < 0) {
            return terminal3215_unit_check(t,
                                           TERM3215_SENSE_EQUIPMENT_CHECK);
        }
        sch->curr_status.scsw.count = ccw.count;
        return 0;
    }
    case TERM3215_CMD_SENSE_ID:
        return terminal3215_copy_response(t, &ccw, sense_id,
                                          sizeof(sense_id));
    default:
        return terminal3215_unit_check(t,
                                       TERM3215_SENSE_COMMAND_REJECT);
    }
}

static void terminal3215_cancel(SubchDev *sch)
{
    Terminal3215 *t = TERMINAL_3215(sch->driver_data);

    t->read_pending = false;
}

static void terminal3215_reset(DeviceState *dev)
{
    Terminal3215 *t = TERMINAL_3215(dev);

    t->read_pending = false;
    t->attention_pending = false;
    t->saw_cr = false;
    t->input_overflow = false;
    t->input_len = 0;
    t->record_head = 0;
    t->record_count = 0;
    t->record_offset = 0;
}

static void terminal3215_realize(DeviceState *dev, Error **errp)
{
    Terminal3215 *t = TERMINAL_3215(dev);
    CcwDevice *cdev = CCW_DEVICE(dev);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    SubchDev *sch;
    Error *local_err = NULL;
    int chpid;

    if (!qemu_chr_fe_backend_connected(&t->chr)) {
        error_setg(errp, "3215-ccw requires a 'chardev' property");
        return;
    }
    sch = css_create_sch(cdev->devno, errp);
    if (!sch) {
        return;
    }
    sch->driver_data = t;
    cdev->sch = sch;
    chpid = css_find_virtual_chpid(sch->cssid, TERM3215_CHPID_TYPE);
    if (chpid > MAX_CHPID) {
        error_setg(&local_err, "No available CHPID for 3215-ccw");
        goto fail;
    }

    sch->id.reserved = 0xff;
    sch->id.cu_type = 0x3215;
    sch->id.cu_model = 0;
    sch->id.dev_type = 0x3215;
    sch->id.dev_model = 0;
    css_sch_build_virtual_schib(sch, chpid, TERM3215_CHPID_TYPE);
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = terminal3215_ccw_cb;
    sch->ccw_cb_first = true;
    sch->cancel_cb = terminal3215_cancel;
    sch->disable_cb = terminal3215_cancel;
    sch->irb_cb = build_irb_virtual;
    sch->status_clear_cb = terminal3215_status_cleared;

    qemu_chr_fe_set_handlers(&t->chr, terminal3215_can_read,
                             terminal3215_read, terminal3215_event,
                             NULL, t, NULL, true);
    if (!cdk->realize(cdev, &local_err)) {
        goto fail_handlers;
    }
    return;

fail_handlers:
    qemu_chr_fe_set_handlers(&t->chr, NULL, NULL, NULL, NULL, NULL, NULL,
                             false);
fail:
    error_propagate(errp, local_err);
    css_subch_assign(sch->cssid, sch->ssid, sch->schid, sch->devno, NULL);
    cdev->sch = NULL;
    g_free(sch);
}

static void terminal3215_unrealize(DeviceState *dev)
{
    Terminal3215 *t = TERMINAL_3215(dev);

    if (CCW_DEVICE(t)->sch) {
        CCW_DEVICE(t)->sch->status_clear_cb = NULL;
    }
    qemu_chr_fe_deinit(&t->chr, false);
}

static int terminal3215_pre_save(void *opaque)
{
    Terminal3215 *t = opaque;

    /* The active data stream refers to source guest memory. */
    return t->read_pending ? -EBUSY : 0;
}

static int terminal3215_post_load(void *opaque, int version_id)
{
    Terminal3215 *t = opaque;
    int i;

    if (t->record_head >= TERM3215_MAX_QUEUED_RECORDS ||
        t->record_count > TERM3215_MAX_QUEUED_RECORDS ||
        t->record_offset > TERM3215_MAX_RECORD_SIZE ||
        t->input_len > TERM3215_MAX_RECORD_SIZE) {
        return -EINVAL;
    }
    for (i = 0; i < TERM3215_MAX_QUEUED_RECORDS; i++) {
        if (t->record_len[i] > TERM3215_MAX_RECORD_SIZE) {
            return -EINVAL;
        }
    }
    if (t->record_count &&
        t->record_offset > t->record_len[t->record_head]) {
        return -EINVAL;
    }
    t->read_pending = false;
    return 0;
}

static const VMStateDescription terminal3215_vmstate = {
    .name = TYPE_TERMINAL_3215,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = terminal3215_pre_save,
    .post_load = terminal3215_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_CCW_DEVICE(parent_obj, Terminal3215),
        VMSTATE_BOOL(attention_pending, Terminal3215),
        VMSTATE_BOOL(saw_cr, Terminal3215),
        VMSTATE_BOOL(input_overflow, Terminal3215),
        VMSTATE_UINT16(input_len, Terminal3215),
        VMSTATE_UINT8_ARRAY(input, Terminal3215,
                            TERM3215_MAX_RECORD_SIZE),
        VMSTATE_UINT8_2DARRAY(records, Terminal3215,
                              TERM3215_MAX_QUEUED_RECORDS,
                              TERM3215_MAX_RECORD_SIZE),
        VMSTATE_UINT16_ARRAY(record_len, Terminal3215,
                             TERM3215_MAX_QUEUED_RECORDS),
        VMSTATE_UINT8(record_head, Terminal3215),
        VMSTATE_UINT8(record_count, Terminal3215),
        VMSTATE_UINT16(record_offset, Terminal3215),
        VMSTATE_END_OF_LIST()
    },
};

static const Property terminal3215_properties[] = {
    DEFINE_PROP_CHR("chardev", Terminal3215, chr),
    DEFINE_PROP_BOOL("echo", Terminal3215, echo, false),
};

static void terminal3215_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = terminal3215_realize;
    dc->unrealize = terminal3215_unrealize;
    dc->hotpluggable = false;
    dc->vmsd = &terminal3215_vmstate;
    device_class_set_legacy_reset(dc, terminal3215_reset);
    device_class_set_props(dc, terminal3215_properties);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo terminal3215_info = {
    .name = TYPE_TERMINAL_3215,
    .parent = TYPE_CCW_DEVICE,
    .instance_size = sizeof(Terminal3215),
    .class_init = terminal3215_class_init,
};

static void terminal3215_register_types(void)
{
    type_register_static(&terminal3215_info);
}

type_init(terminal3215_register_types)
