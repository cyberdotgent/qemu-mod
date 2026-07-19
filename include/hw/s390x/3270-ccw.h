/*
 * Emulated ccw-attached 3270 definitions
 *
 * Copyright 2017 IBM Corp.
 * Author(s): Yang Chen <bjcyang@linux.vnet.ibm.com>
 *            Jing Liu <liujbjl@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#ifndef HW_S390X_3270_CCW_H
#define HW_S390X_3270_CCW_H

#include "hw/core/sysbus.h"
#include "hw/s390x/css.h"
#include "hw/s390x/ccw-device.h"
#include "qom/object.h"

#define EMULATED_CCW_3270_CU_TYPE 0x3270
#define EMULATED_CCW_3270_CHPID_TYPE 0x1a
#define EMULATED_CCW_3270_CU_MODEL 0x1d
#define EMULATED_CCW_3270_DEV_TYPE 0x3278
#define EMULATED_CCW_3270_DEV_MODEL 0x02

#define TYPE_EMULATED_CCW_3270 "emulated-ccw-3270"

/* Local Channel Commands */
#define TC_WRITE   0x01         /* Write */
#define TC_RDBUF   0x02         /* Read buffer */
#define TC_NOP     0x03         /* No operation */
#define TC_SENSE   0x04         /* Basic sense */
#define TC_EWRITE  0x05         /* Erase write */
#define TC_READMOD 0x06         /* Read modified */
#define TC_SELRM   0x0b         /* Select read modified */
#define TC_EWRITEA 0x0d         /* Erase write alternate */
#define TC_EAU     0x0f         /* Erase all unprotected */
#define TC_WRITESF 0x11         /* Write structured field */
#define TC_SELRB   0x1b         /* Select read buffer */
#define TC_SELRMP  0x2b         /* Select read modified protected */
#define TC_SELRBP  0x3b         /* Select read buffer protected */
#define TC_SELWRITE 0x4b        /* Select write */
#define TC_SENSEID 0xe4         /* Sense ID */

/* Remote TN3270 commands */
#define TN3270_CMD_WRITE   0xf1
#define TN3270_CMD_RDBUF   0xf2
#define TN3270_CMD_WRITESF 0xf3
#define TN3270_CMD_EWRITE  0xf5
#define TN3270_CMD_READMOD 0xf6
#define TN3270_CMD_EAU     0x6f
#define TN3270_CMD_EWRITEA 0x7e

/* 3270 data-stream orders and inbound structured-field AID. */
#define O3270_PT  0x05
#define O3270_GE  0x08
#define O3270_SBA 0x11
#define O3270_EUA 0x12
#define O3270_IC  0x13
#define O3270_SF  0x1d
#define O3270_SA  0x28
#define O3270_SFE 0x29
#define O3270_MF  0x2c
#define O3270_RA  0x3c

#define SF3270_AID 0x88

OBJECT_DECLARE_TYPE(EmulatedCcw3270Device, EmulatedCcw3270Class, EMULATED_CCW_3270)

struct EmulatedCcw3270Device {
    CcwDevice parent_obj;
};

struct EmulatedCcw3270Class {
    CCWDeviceClass parent_class;

    void (*init)(EmulatedCcw3270Device *, Error **);
    void (*cancel)(EmulatedCcw3270Device *);
    int (*control_3270)(EmulatedCcw3270Device *, CCW1 *);
    int (*read_payload_3270)(EmulatedCcw3270Device *, CCW1 *);
    int (*write_payload_3270)(EmulatedCcw3270Device *, CCW1 *);
};

#endif
