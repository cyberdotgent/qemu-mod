/*
 * Common device infrastructure for devices in the virtual css
 *
 * Copyright 2016 IBM Corp.
 * Author(s): Jing Liu <liujbjl@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "ccw-device.h"
#include "virtio-ccw.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "ipl.h"
#include "qapi/visitor.h"
#include "qemu/ctype.h"
#include "qapi/error.h"
#include "qapi/qapi-builtin-type-infos.h"
#include "qobject/qbool.h"
#include "qobject/qstring.h"

static void ccw_device_refill_ids(CcwDevice *dev)
{
    SubchDev *sch = dev->sch;

    assert(sch);

    dev->dev_id.cssid = sch->cssid;
    dev->dev_id.ssid = sch->ssid;
    dev->dev_id.devid = sch->devno;
    dev->dev_id.valid = true;

    dev->subch_id.cssid = sch->cssid;
    dev->subch_id.ssid = sch->ssid;
    dev->subch_id.devid = sch->schid;
    dev->subch_id.valid = true;
}

static bool ccw_device_realize(CcwDevice *dev, Error **errp)
{
    ccw_device_refill_ids(dev);
    return true;
}

static void ccw_device_get_loadparm(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    CcwDevice *dev = CCW_DEVICE(obj);
    char *str = g_strndup((char *) dev->loadparm, sizeof(dev->loadparm));

    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void ccw_device_set_loadparm(Object *obj, Visitor *v,
                                 const char *name, void *opaque,
                                 Error **errp)
{
    CcwDevice *dev = CCW_DEVICE(obj);
    g_autofree char *val = NULL;
    int index;

    index = object_property_get_int(obj, "bootindex", NULL);

    if (index < 0) {
        error_setg(errp, "LOADPARM is only valid for boot devices!");
    }

    if (!visit_type_str(v, name, &val, errp)) {
        return;
    }

    s390_ipl_fmt_loadparm(dev->loadparm, val, errp);
}

const PropertyInfo ccw_loadparm = {
    .type  = "str",
    .qapi_type = &str_type_info,
    .description = "Up to 8 chars in set of [A-Za-z0-9. ] to select"
            " a guest kernel",
    .get = ccw_device_get_loadparm,
    .set = ccw_device_set_loadparm,
};

static const Property ccw_device_properties[] = {
    DEFINE_PROP_CSS_DEV_ID("devno", CcwDevice, devno),
    DEFINE_PROP_CSS_DEV_ID_RO("dev_id", CcwDevice, dev_id),
    DEFINE_PROP_CSS_DEV_ID_RO("subch_id", CcwDevice, subch_id),
};

CcwIplMethod ccw_device_class_ipl_method(CCWDeviceClass *k)
{
    if (!k->build_iplb) {
        return CCW_IPL_METHOD_NONE;
    }
    /* virtio devices are booted by the s390-ccw firmware, never by -ipl */
    if (object_class_dynamic_cast(OBJECT_CLASS(k), TYPE_VIRTIO_CCW_DEVICE)) {
        return CCW_IPL_METHOD_FIRMWARE;
    }
    return CCW_IPL_METHOD_CCW;
}

static const char *const ccw_ipl_method_names[] = {
    [CCW_IPL_METHOD_NONE] = "none",
    [CCW_IPL_METHOD_CCW] = "ccw",
    [CCW_IPL_METHOD_FIRMWARE] = "firmware",
};

static bool ccw_device_get_iplable(Object *obj, Error **errp)
{
    return ccw_device_class_ipl_method(CCW_DEVICE_GET_CLASS(obj)) !=
           CCW_IPL_METHOD_NONE;
}

static char *ccw_device_get_ipl_method(Object *obj, Error **errp)
{
    return g_strdup(ccw_ipl_method_names[
        ccw_device_class_ipl_method(CCW_DEVICE_GET_CLASS(obj))]);
}

/*
 * "iplable" and "ipl-method" are constant per device type, but the type is
 * only known once the subclass has set build_iplb, so they are instance
 * properties.  Their value is also recorded as the default value, which is
 * what "-device <type>,help" and device-list-properties report: a management
 * layer can find the IPL-capable device types without creating a VM.
 * object_property_set_default_*() would add an init hook that needs a
 * setter, so defval is set directly.
 */
static void ccw_device_instance_init(Object *obj)
{
    CcwIplMethod method =
        ccw_device_class_ipl_method(CCW_DEVICE_GET_CLASS(obj));
    ObjectProperty *op;

    op = object_property_add_bool(obj, "iplable", ccw_device_get_iplable,
                                  NULL);
    op->defval = QOBJECT(qbool_from_bool(method != CCW_IPL_METHOD_NONE));
    object_property_set_description(obj, "iplable",
            "Whether the device can be an IPL (boot) device (read-only)");

    op = object_property_add_str(obj, "ipl-method", ccw_device_get_ipl_method,
                                 NULL);
    op->defval = QOBJECT(qstring_from_str(ccw_ipl_method_names[method]));
    object_property_set_description(obj, "ipl-method",
            "How the device is IPLed: ccw (channel-program IPL, -ipl), "
            "firmware (s390-ccw firmware, bootindex) or none (read-only)");
}

static void ccw_device_reset_hold(Object *obj, ResetType type)
{
    CcwDevice *ccw_dev = CCW_DEVICE(obj);

    css_reset_sch(ccw_dev->sch);
}

static void ccw_device_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    CCWDeviceClass *k = CCW_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->realize = ccw_device_realize;
    k->refill_ids = ccw_device_refill_ids;
    device_class_set_props(dc, ccw_device_properties);
    rc->phases.hold = ccw_device_reset_hold;
    dc->bus_type = TYPE_VIRTUAL_CSS_BUS;
}

const VMStateDescription vmstate_ccw_dev = {
    .name = "s390_ccw_dev",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(sch, CcwDevice, vmstate_subch_dev, SubchDev),
        VMSTATE_END_OF_LIST()
    }
};

static const TypeInfo ccw_device_info = {
    .name = TYPE_CCW_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(CcwDevice),
    .instance_init = ccw_device_instance_init,
    .class_size = sizeof(CCWDeviceClass),
    .class_init = ccw_device_class_init,
    .abstract = true,
};

static void ccw_device_register(void)
{
    type_register_static(&ccw_device_info);
}

type_init(ccw_device_register)
