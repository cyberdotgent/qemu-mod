/*
 * Emulated fixed-block architecture DASD
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_FBA_CCW_H
#define HW_S390X_FBA_CCW_H

#include "hw/s390x/ccw-device.h"
#include "hw/core/resettable.h"
#include "qom/object.h"

#define TYPE_FBA_CCW "fba-ccw"
OBJECT_DECLARE_TYPE(FBACcwDevice, FBACcwDeviceClass, FBA_CCW)

struct FBACcwDeviceClass {
    CCWDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#endif
