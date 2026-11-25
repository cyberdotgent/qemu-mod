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
 * linux-headers/).  The kernel offers the capability on every host, wants
 * KVM_CAP_S390_USER_SIGP enabled first, and leaves everything SIGP
 * set-architecture defines apart from the format of the state descriptions
 * to user space.  Of the facility list it only keeps bit 2 (z/Architecture
 * mode active) in line with the mode; the bits that say whether the
 * configuration offers the mode at all (138, 168) are QEMU's.  Whether the
 * machine, or the hypervisor below the host, runs state descriptions in the
 * ESA/390 format is only known when a vCPU is first run in the mode: KVM_RUN
 * fails with EINVAL if not.  Everything QEMU knows about it is in this file,
 * behind the functions declared in kvm_s390x.h under "ESA/390-compatibility
 * mode":
 *
 *   kvm_s390_esa390_init()        machine properties -> kernel, once
 *   kvm_s390_esa390_cm_enabled()  the VM was set up for the mode
 *   kvm_s390_esa390_fac_list()    the facility bits of the mode in the
 *                                 CPU model given to the kernel
 *   kvm_s390_esa390_dat_reserve() storage at the top that the guest is
 *                                 not told about
 *   kvm_s390_set_arch_mode()      put the configuration into a mode
 *   kvm_s390_get_arch_mode()      the mode the configuration is in
 *   kvm_s390_esa390_put_arch_regs() hand a CPU's registers that a mode
 *                                 change alters to the kernel
 *   kvm_s390_esa390_note_exit()   a vCPU ran: the host runs the mode
 *   kvm_s390_esa390_run_failed()  explain a failed KVM_RUN in the mode
 *   kvm_s390_esa390_guest_debug() guest debugging is refused in the mode
 *
 * A change of the kernel interface, or moving work from the kernel into
 * QEMU, lands here; the callers (machine reset, register synchronisation,
 * incoming migration, SIGP set-architecture in target/s390x/sigp.c) only
 * use these functions.
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
/* the mode the configuration was last put into */
static bool arch_mode_esa;
/* a vCPU came back from KVM_RUN with an exit while in the mode */
static bool esa390_cm_ran;
/* the refusal of guest debugging was reported since the mode was entered */
static bool esa390_debug_warned;

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
                     "needs: it takes a kernel with that support");
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
 * The facility bits of a configuration that offers the mode, in the list
 * that QEMU gives the kernel as the CPU model (KVM_S390_VM_CPU_PROCESSOR):
 * the ESA/390-compatibility-mode facility (168) is installed and the
 * configuration-z/Architecture-architectural-mode facility (138), which says
 * that the configuration always runs in z/Architecture, is not.  Bit 2
 * (z/Architecture mode active) is the kernel's: it changes with the mode.
 */
void kvm_s390_esa390_fac_list(uint8_t *fac_list)
{
    if (!cap_esa390_cm) {
        return;
    }
    fac_list[138 / 8] &= ~(0x80 >> (138 % 8));
    fac_list[168 / 8] |= 0x80 >> (168 % 8);
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

    int ret;

    if (!cap_esa390_cm) {
        return esa ? -ENOTSUP : 0;
    }
    ret = kvm_vm_ioctl(kvm_state, KVM_SET_DEVICE_ATTR, &attr);
    if (!ret) {
        if (esa && !arch_mode_esa) {
            esa390_debug_warned = false;
        }
        arch_mode_esa = esa;
    } else if (ret == -EINVAL && esa) {
        /* the mode is valid: the kernel refuses because of guest debugging */
        error_report("KVM does not put the configuration into the "
                     "ESA/390-compatibility mode while guest debugging is "
                     "enabled (single-stepping, hardware breakpoints or "
                     "watchpoints of the gdbstub): remove them first");
    }
    return ret;
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

/*
 * SIGP set-architecture changes control registers and the prefix of all
 * CPUs.  The usual synchronisation writes those only with the state of a
 * reset; mark them for the next entry of the CPU, which is not running.
 * The PSW follows with the runtime state.
 */
void kvm_s390_esa390_put_arch_regs(CPUState *cs)
{
    CPUS390XState *env = cpu_env(cs);

    memcpy(cs->kvm_run->s.regs.crs, env->cregs,
           sizeof(cs->kvm_run->s.regs.crs));
    cs->kvm_run->s.regs.prefix = env->psa;
    cs->kvm_run->kvm_dirty_regs |= KVM_SYNC_CRS | KVM_SYNC_PREFIX;
}

/* Called for every exit of a vCPU from KVM_RUN that was not an error. */
void kvm_s390_esa390_note_exit(void)
{
    if (arch_mode_esa && !esa390_cm_ran) {
        esa390_cm_ran = true;
    }
}

/*
 * KVM_RUN failed: if the configuration is in the mode, say what a failure
 * there most likely means.  The kernel fails KVM_RUN with EINVAL when the
 * machine, or a hypervisor below the host, answers an ESA/390-format state
 * description with a validity interception; nothing tells QEMU in advance.
 */
void kvm_s390_esa390_run_failed(CPUState *cs, int run_ret)
{
    if (!cap_esa390_cm || !arch_mode_esa || run_ret != -EINVAL) {
        return;
    }
    if (!esa390_cm_ran) {
        error_report("CPU %d could not be run in the ESA/390-compatibility "
                     "mode: the machine, or the hypervisor this host runs "
                     "under, does not run guests in that mode (if that is "
                     "a KVM host, its QEMU needs nested-esa390=on or auto "
                     "and a kernel with KVM_CAP_S390_VSIE_ESAMODE).  "
                     "Machine property esa390-cm=on (a default of machine "
                     "czam-compat) cannot be used on this host",
                     cs->cpu_index);
    } else {
        error_report("CPU %d failed in the ESA/390-compatibility mode; "
                     "the host kernel log may give the reason",
                     cs->cpu_index);
    }
}

/*
 * KVM refuses guest debugging in the mode (KVM_SET_GUEST_DEBUG with
 * KVM_GUESTDBG_ENABLE fails with EINVAL).  Say so instead of letting a
 * single step silently become a continue.  Software breakpoints do not need
 * it and keep working.
 */
void kvm_s390_esa390_guest_debug(struct kvm_guest_debug *dbg)
{
    if (!cap_esa390_cm || !arch_mode_esa ||
        !(dbg->control & KVM_GUESTDBG_ENABLE) || esa390_debug_warned) {
        return;
    }
    esa390_debug_warned = true;
    warn_report("guest debugging (single-stepping, hardware breakpoints "
                "and watchpoints) is not available while the guest is in "
                "the ESA/390-compatibility mode: KVM refuses the request");
}
