/*
 * ESA/390-compatibility mode under KVM
 *
 * A configuration with the ESA/390-compatibility-mode facility starts in
 * ESA/390 mode and is switched to z/Architecture by the guest with SIGP
 * set-architecture.  KVM runs the vCPUs in the mode; QEMU selects it at
 * machine start and at the resets that belong to a load.
 *
 * The kernel interface for this is that of a kernel series for Linux 7.3
 * and not part of upstream Linux (see the PROVISIONAL blocks in
 * linux-headers/).  The kernel offers the capability only when its kvm
 * module was loaded with esa390_cm=1, wants KVM_CAP_S390_USER_SIGP enabled
 * first, and leaves everything SIGP set-architecture defines apart from
 * the format of the state descriptions to user space.  Everything
 * QEMU knows about it is in this file, behind the functions declared in
 * kvm_s390x.h under "ESA/390-compatibility mode":
 *
 *   kvm_s390_esa390_init()        machine properties -> kernel, once
 *   kvm_s390_esa390_cm_enabled()  the VM was set up for the mode
 *   kvm_s390_esa390_dat_reserve() storage at the top that the guest is
 *                                 not told about
 *   kvm_s390_set_arch_mode()      put the configuration into a mode
 *   kvm_s390_get_arch_mode()      the mode the configuration is in
 *
 * A change of the kernel interface, or moving work from the kernel into
 * QEMU, lands here; the callers (machine reset, register synchronisation,
 * incoming migration) only use these functions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <linux/kvm.h>

#include "cpu.h"
#include "kvm_s390x.h"
#include "system/kvm.h"
#include "system/kvm_int.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/core/boards.h"

/*
 * ESA/390 address translation in the mode: the kernel keeps shadow tables
 * in an area of guest absolute storage that QEMU sets aside for it, the
 * topmost megabyte.
 */
#define ESA390_DAT_AREA_SIZE (1 * MiB)
#define ESA390_DAT_MIN_RAM (16 * MiB)

static bool cap_esa390_cm;
static uint64_t esa390_dat_area_size;

/*
 * The capability number is provisional: do not trust it alone.  After the
 * capability has been enabled, a kernel that implements the mode also has
 * the attribute group and reports a valid mode.
 */
static bool esa390_cm_enable(KVMState *s)
{
    uint32_t mode = ~0U;
    struct kvm_device_attr attr = {
        .group = KVM_S390_VM_ARCH_MODE,
        .attr = KVM_S390_VM_ARCH_MODE_CURRENT,
        .addr = (uint64_t)&mode,
    };

    if (kvm_check_extension(s, KVM_CAP_S390_ESA390_CM) <= 0 ||
        kvm_vm_enable_cap(s, KVM_CAP_S390_ESA390_CM, 0)) {
        return false;
    }
    if (kvm_vm_ioctl(s, KVM_HAS_DEVICE_ATTR, &attr) ||
        kvm_vm_ioctl(s, KVM_GET_DEVICE_ATTR, &attr)) {
        return false;
    }
    return mode == KVM_S390_ARCH_MODE_ZARCH ||
           mode == KVM_S390_ARCH_MODE_ESA390_CM;
}

/*
 * Set the VM up as the machine properties ask.  Called from kvm_arch_init():
 * the capability must be enabled before the CPU model is applied and the
 * first vCPU is created.  Without the properties nothing is asked of the
 * kernel.  Returns a negative value after reporting the error if the host
 * kernel cannot do what was requested.
 */
int kvm_s390_esa390_init(MachineState *ms, KVMState *s)
{
    if (!object_property_get_bool(OBJECT(ms), "esa390-cm", NULL)) {
        return 0;
    }
    if (!esa390_cm_enable(s)) {
        error_report("The KVM of this host kernel does not offer the "
                     "ESA/390-compatibility mode, which machine property "
                     "esa390-cm=on (a default of machine czam-compat) "
                     "needs: it takes a kernel with that support and the "
                     "kvm module parameter esa390_cm=1");
        return -ENOTSUP;
    }
    cap_esa390_cm = true;

    if (object_property_get_bool(OBJECT(ms), "esa390-dat", NULL)) {
        struct kvm_s390_esa390_dat_area area = {
            .base = ms->ram_size - ESA390_DAT_AREA_SIZE,
            .size = ESA390_DAT_AREA_SIZE,
        };
        struct kvm_device_attr attr = {
            .group = KVM_S390_VM_ARCH_MODE,
            .attr = KVM_S390_VM_ARCH_MODE_DAT_AREA,
            .addr = (uint64_t)&area,
        };

        if (ms->ram_size < ESA390_DAT_MIN_RAM) {
            error_report("machine property esa390-dat=on needs at least "
                         "16 MiB of memory");
            return -EINVAL;
        }
        if (kvm_vm_ioctl(s, KVM_HAS_DEVICE_ATTR, &attr) ||
            kvm_vm_ioctl(s, KVM_SET_DEVICE_ATTR, &attr)) {
            error_report("The KVM of this host kernel does not support "
                         "ESA/390 address translation in the "
                         "ESA/390-compatibility mode, which machine "
                         "property esa390-dat=on (a default of machine "
                         "czam-compat) needs: it takes a kernel with that "
                         "support and the kvm module parameter "
                         "esa390_lab_dat=1");
            return -ENOTSUP;
        }
        esa390_dat_area_size = ESA390_DAT_AREA_SIZE;
    }
    return 0;
}

/*
 * Size of the storage at the top of the configuration that is set aside
 * (esa390-dat) and must not be reported to the guest as its own; 0 if none.
 */
uint64_t kvm_s390_esa390_dat_reserve(void)
{
    return esa390_dat_area_size;
}

bool kvm_s390_esa390_cm_enabled(void)
{
    return cap_esa390_cm;
}

/* Put all CPUs of the configuration into the given architectural mode. */
int kvm_s390_set_arch_mode(bool esa)
{
    uint32_t mode = esa ? KVM_S390_ARCH_MODE_ESA390_CM :
                          KVM_S390_ARCH_MODE_ZARCH;
    struct kvm_device_attr attr = {
        .group = KVM_S390_VM_ARCH_MODE,
        .attr = KVM_S390_VM_ARCH_MODE_CURRENT,
        .addr = (uint64_t)&mode,
    };

    if (!cap_esa390_cm) {
        return esa ? -ENOTSUP : 0;
    }
    return kvm_vm_ioctl(kvm_state, KVM_SET_DEVICE_ATTR, &attr);
}

/* 1: ESA/390-compatibility mode, 0: z/Architecture, < 0: not switchable */
int kvm_s390_get_arch_mode(void)
{
    uint32_t mode = 0;
    struct kvm_device_attr attr = {
        .group = KVM_S390_VM_ARCH_MODE,
        .attr = KVM_S390_VM_ARCH_MODE_CURRENT,
        .addr = (uint64_t)&mode,
    };
    int ret;

    if (!cap_esa390_cm) {
        return -ENOTSUP;
    }
    ret = kvm_vm_ioctl(kvm_state, KVM_GET_DEVICE_ATTR, &attr);
    if (ret) {
        return ret;
    }
    return mode == KVM_S390_ARCH_MODE_ESA390_CM;
}
