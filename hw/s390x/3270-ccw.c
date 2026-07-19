/*
 * Emulated ccw-attached 3270 implementation
 *
 * Copyright 2017 IBM Corp.
 * Author(s): Yang Chen <bjcyang@linux.vnet.ibm.com>
 *            Jing Liu <liujbjl@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/s390x/css.h"
#include "hw/s390x/css-bridge.h"
#include "hw/core/qdev-properties.h"
#include "hw/s390x/3270-ccw.h"

/* Handle READ ccw commands from guest */
static int handle_payload_3270_read(EmulatedCcw3270Device *dev, CCW1 *ccw)
{
    EmulatedCcw3270Class *ck = EMULATED_CCW_3270_GET_CLASS(dev);
    CcwDevice *ccw_dev = CCW_DEVICE(dev);
    int len;

    if (!ccw->cda) {
        return -EFAULT;
    }

    len = ck->read_payload_3270(dev, ccw);
    if (len == CSS_CCW_PENDING) {
        return len;
    }
    if (len < 0) {
        return len;
    }
    ccw_dev->sch->curr_status.scsw.count = ccw->count - len;

    return 0;
}

/* Handle WRITE ccw commands to write data to client */
static int handle_payload_3270_write(EmulatedCcw3270Device *dev, CCW1 *ccw)
{
    EmulatedCcw3270Class *ck = EMULATED_CCW_3270_GET_CLASS(dev);
    CcwDevice *ccw_dev = CCW_DEVICE(dev);
    int len;

    if (ccw->count && !ccw->cda && ccw->cmd_code != TC_EAU &&
        ccw->cmd_code != TC_SELRM && ccw->cmd_code != TC_SELRB &&
        ccw->cmd_code != TC_SELRMP && ccw->cmd_code != TC_SELRBP &&
        ccw->cmd_code != TC_SELWRITE) {
        return -EFAULT;
    }

    len = ck->write_payload_3270(dev, ccw);

    if (len < 0) {
        return len;
    }

    ccw_dev->sch->curr_status.scsw.count = ccw->count - len;
    return 0;
}

static int emulated_ccw_3270_cb(SubchDev *sch, CCW1 ccw)
{
    int rc = 0;
    EmulatedCcw3270Device *dev = sch->driver_data;
    EmulatedCcw3270Class *ck = EMULATED_CCW_3270_GET_CLASS(dev);

    switch (ccw.cmd_code) {
    case TC_NOP:
    case TC_SENSE:
    case TC_SENSEID:
        rc = ck->control_3270 ? ck->control_3270(dev, &ccw) : -ENOSYS;
        break;
    case TC_EAU:
    case TC_WRITESF:
    case TC_WRITE:
    case TC_EWRITE:
    case TC_EWRITEA:
    case TC_SELRM:
    case TC_SELRB:
    case TC_SELRMP:
    case TC_SELRBP:
    case TC_SELWRITE:
        rc = handle_payload_3270_write(dev, &ccw);
        break;
    case TC_RDBUF:
    case TC_READMOD:
        rc = handle_payload_3270_read(dev, &ccw);
        break;
    default:
        rc = -ENOSYS;
        break;
    }

    if (rc == -EIO) {
        /* I/O error, specific devices generate specific conditions */
        SCHIB *schib = &sch->curr_status;

        if (!sch->sense_data[0]) {
            sch->sense_data[0] = 0x40;    /* intervention-req */
        }
        sch->curr_status.scsw.dstat = SCSW_DSTAT_UNIT_CHECK;
        if (sch->sense_data[0] != 0x40) {
            sch->curr_status.scsw.dstat |= SCSW_DSTAT_CHANNEL_END |
                                           SCSW_DSTAT_DEVICE_END;
        }
        schib->scsw.ctrl &= ~SCSW_ACTL_START_PEND;
        schib->scsw.ctrl &= ~SCSW_CTRL_MASK_STCTL;
        schib->scsw.ctrl |= SCSW_STCTL_PRIMARY | SCSW_STCTL_SECONDARY |
                   SCSW_STCTL_ALERT | SCSW_STCTL_STATUS_PEND;
        schib->scsw.cpa = sch->channel_prog + 8;
    }

    return rc;
}

static void emulated_ccw_3270_cancel(SubchDev *sch)
{
    EmulatedCcw3270Device *dev = sch->driver_data;
    EmulatedCcw3270Class *ck = EMULATED_CCW_3270_GET_CLASS(dev);

    if (ck->cancel) {
        ck->cancel(dev);
    }
}

static void emulated_ccw_3270_realize(DeviceState *ds, Error **errp)
{
    uint16_t chpid;
    EmulatedCcw3270Device *dev = EMULATED_CCW_3270(ds);
    EmulatedCcw3270Class *ck = EMULATED_CCW_3270_GET_CLASS(dev);
    CcwDevice *cdev = CCW_DEVICE(ds);
    CCWDeviceClass *cdk = CCW_DEVICE_GET_CLASS(cdev);
    SubchDev *sch;
    Error *err = NULL;

    sch = css_create_sch(cdev->devno, errp);
    if (!sch) {
        return;
    }

    if (!ck->init) {
        goto out_err;
    }

    sch->driver_data = dev;
    cdev->sch = sch;
    /* All emulated 3270 devices in a CSS share their virtual channel path. */
    chpid = css_find_virtual_chpid(sch->cssid,
                                   EMULATED_CCW_3270_CHPID_TYPE);

    if (chpid > MAX_CHPID) {
        error_setg(&err, "No available chpid to use.");
        goto out_err;
    }

    sch->id.reserved = 0xff;
    sch->id.cu_type = EMULATED_CCW_3270_CU_TYPE;
    sch->id.cu_model = EMULATED_CCW_3270_CU_MODEL;
    sch->id.dev_type = EMULATED_CCW_3270_DEV_TYPE;
    sch->id.dev_model = EMULATED_CCW_3270_DEV_MODEL;
    css_sch_build_virtual_schib(sch, (uint8_t)chpid,
                                EMULATED_CCW_3270_CHPID_TYPE);
    sch->do_subchannel_work = do_subchannel_work_virtual;
    sch->ccw_cb = emulated_ccw_3270_cb;
    sch->ccw_cb_first = true;
    sch->cancel_cb = emulated_ccw_3270_cancel;
    sch->disable_cb = emulated_ccw_3270_cancel;
    sch->irb_cb = build_irb_virtual;

    ck->init(dev, &err);
    if (err) {
        goto out_err;
    }

    cdk->realize(cdev, &err);
    if (err) {
        goto out_err;
    }

    return;

out_err:
    error_propagate(errp, err);
    css_subch_assign(sch->cssid, sch->ssid, sch->schid, sch->devno, NULL);
    cdev->sch = NULL;
    g_free(sch);
}

static void emulated_ccw_3270_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = emulated_ccw_3270_realize;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo emulated_ccw_3270_info = {
    .name = TYPE_EMULATED_CCW_3270,
    .parent = TYPE_CCW_DEVICE,
    .instance_size = sizeof(EmulatedCcw3270Device),
    .class_init = emulated_ccw_3270_class_init,
    .class_size = sizeof(EmulatedCcw3270Class),
    .abstract = true,
};

static void emulated_ccw_register(void)
{
    type_register_static(&emulated_ccw_3270_info);
}

type_init(emulated_ccw_register)
