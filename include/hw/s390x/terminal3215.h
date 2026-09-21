/*
 * IBM 3215 console printer-keyboard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_S390X_TERMINAL3215_H
#define HW_S390X_TERMINAL3215_H

#include "hw/s390x/ccw-device.h"
#include "qom/object.h"

#define TYPE_TERMINAL_3215 "3215-ccw"
OBJECT_DECLARE_TYPE(Terminal3215, Terminal3215Class, TERMINAL_3215)

struct Terminal3215Class {
    CCWDeviceClass parent_class;
    ResettablePhases parent_phases;
};

#endif
