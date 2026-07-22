/*
 * Emulated IBM 3590 tape drive
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_3590_CCW_H
#define HW_S390X_3590_CCW_H

#include "hw/core/resettable.h"
#include "hw/s390x/ccw-device.h"
#include "qom/object.h"

#define TYPE_TAPE_3590_CCW "3590-ccw"
OBJECT_DECLARE_TYPE(Tape3590CcwDevice, Tape3590CcwDeviceClass, TAPE_3590_CCW)

struct Tape3590CcwDeviceClass {
    CCWDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#endif
