/*
 * TOD (Time Of Day) clock
 *
 * Copyright 2018 Red Hat, Inc.
 * Author(s): David Hildenbrand <david@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_S390_TOD_H
#define HW_S390_TOD_H

#include "hw/core/qdev.h"
#include "qemu/thread.h"
#include "tcg/s390-tod.h"
#include "qom/object.h"

typedef struct S390TOD {
    uint8_t high;
    uint64_t low;
} S390TOD;

#define TYPE_S390_TOD "s390-tod"
OBJECT_DECLARE_TYPE(S390TODState, S390TODClass, S390_TOD)
#define TYPE_KVM_S390_TOD TYPE_S390_TOD "-kvm"
#define TYPE_QEMU_S390_TOD TYPE_S390_TOD "-qemu"

struct S390TODState {
    /* private */
    DeviceState parent_obj;

    /*
     * Used by TCG to remember the time base. Used by KVM to backup the TOD
     * while the TOD is stopped.
     */
    S390TOD base;
    /* Used by KVM to remember if the TOD is stopped and base is valid. */
    bool stopped;
    /*
     * Used by KVM: set the TOD from the host's CLOCK_REALTIME when the VM
     * starts running for the first time (machine property tod-source=host).
     * Cleared once that happened, or when the TOD was set otherwise before
     * (incoming migration, loadvm).
     */
    bool set_from_host;

    /*
     * STORE CLOCK and STORE CLOCK EXTENDED must return globally unique,
     * correctly ordered values.  TCG host time has coarser resolution than
     * the architectural 104-bit clock, so retain the most recent value and
     * advance its extension when necessary.
     */
    QemuMutex unique_lock;
    uint64_t unique_high;
    uint64_t unique_low;
    bool unique_valid;
};

struct S390TODClass {
    /* private */
    DeviceClass parent_class;
    void (*parent_realize)(DeviceState *dev, Error **errp);

    /* public */
    void (*get)(const S390TODState *td, S390TOD *tod, Error **errp);
    void (*set)(S390TODState *td, const S390TOD *tod, Error **errp);
};

void s390_init_tod(void);
S390TODState *s390_get_todstate(void);
void s390_tod_from_host_clock(S390TOD *tod);

#endif
