/*
 * Emulated 2107/3390 ECKD DASD
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_ECKD_CCW_H
#define HW_S390X_ECKD_CCW_H

#include "hw/core/resettable.h"
#include "hw/s390x/ccw-device.h"
#include "qom/object.h"

#define TYPE_ECKD_CCW "eckd-ccw"
OBJECT_DECLARE_TYPE(EckdCcwDevice, EckdCcwDeviceClass, ECKD_CCW)

struct EckdCcwDeviceClass {
    CCWDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#endif
