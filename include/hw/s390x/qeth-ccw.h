/*
 * Emulated OSA Express QETH adapter
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_S390X_QETH_CCW_H
#define HW_S390X_QETH_CCW_H

#include "net/net.h"
#include "hw/s390x/ccw-device.h"
#include "hw/s390x/qdio.h"
#include "qom/object.h"

#define TYPE_QETH_CCW "qeth-ccw"
OBJECT_DECLARE_SIMPLE_TYPE(QethCcwDevice, QETH_CCW)

#endif
