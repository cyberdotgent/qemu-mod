/*
 * KVM single-step instruction tracer for s390x guests
 *
 * Copyright 2026 Yvan Janssens
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Logs one fixed-size record per guest instruction while armed, using
 * KVM_GUESTDBG_SINGLESTEP.  Each single-step debug exit is consumed on the
 * vCPU thread before the BQL is taken or the register state is synchronized:
 * the PER address, PSW, GPRs, CR1, CR4 and breaking-event address are read
 * directly from the kvm_run sync-register area, and the vCPU re-enters KVM
 * immediately.  No guest state is modified.
 *
 * Unlike KVM hardware breakpoints, single-step does not depend on guest
 * CR9-CR11 surviving between SIE entries: KVM re-patches the PER registers
 * on every KVM_RUN, and every step returns to userspace.
 *
 * Configuration (environment, read once at arm time):
 *   QEMU_S390_ITRACE_FILE      output path (required)
 *   QEMU_S390_ITRACE_MAX       record limit (default 5,000,000)
 *   QEMU_S390_ITRACE_STOP_PSW  stop tracing and pause the VM once an
 *                              instruction at this address has executed
 *   QEMU_S390_ITRACE_AT_START  "1": arm before the first KVM_RUN
 * Arming at runtime: call s390_kvm_itrace_arm() (e.g. from a host gdb).
 */

#include "qemu/osdep.h"
#include <linux/kvm.h>
#include "qemu/atomic.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "system/kvm.h"
#include "system/runstate.h"
#include "hw/core/cpu.h"
#include "kvm_s390x.h"

#define ITRACE_MAGIC "QS390IT1"

typedef struct S390ITraceRecord {
    uint64_t seq;
    uint64_t per_addr;          /* address of the executed instruction */
    uint64_t psw_mask;          /* PSW after the instruction */
    uint64_t psw_addr;
    uint64_t gprs[16];          /* GPRs after the instruction */
    uint64_t cr1;
    uint64_t cr4;               /* PASN in bits 48-63 */
    uint64_t gbea;              /* breaking-event address */
    uint64_t flags;             /* bits 0-7: kvm debug exit type */
} S390ITraceRecord;

QEMU_BUILD_BUG_ON(sizeof(S390ITraceRecord) != 192);

static int itrace_requested;     /* set by s390_kvm_itrace_arm() */
static bool itrace_active;       /* vCPU thread only */
static bool itrace_done;
static FILE *itrace_file;
static uint64_t itrace_count;
static uint64_t itrace_max;
static uint64_t itrace_stop_psw;
static bool itrace_have_stop;

/* Callable from a debugger attached to QEMU; only sets a flag. */
void s390_kvm_itrace_arm(void)
{
    qatomic_set(&itrace_requested, 1);
}

static int itrace_set_singlestep(CPUState *cs, bool on)
{
    struct kvm_guest_debug dbg = { 0 };

    if (on) {
        dbg.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
    }
    /* Keep any gdbstub hardware breakpoints. */
    kvm_arch_update_guest_debug(cs, &dbg);
    return kvm_vcpu_ioctl(cs, KVM_SET_GUEST_DEBUG, &dbg);
}

static void itrace_finish(CPUState *cs, const char *why)
{
    itrace_active = false;
    itrace_done = true;
    if (itrace_set_singlestep(cs, false)) {
        error_report("s390 itrace: could not disable single-step");
    }
    if (itrace_file) {
        fclose(itrace_file);
        itrace_file = NULL;
    }
    info_report("s390 itrace: stopped after %" PRIu64 " records (%s)",
                itrace_count, why);
}

static void itrace_start(CPUState *cs)
{
    const char *path = getenv("QEMU_S390_ITRACE_FILE");
    const char *max = getenv("QEMU_S390_ITRACE_MAX");
    const char *stop = getenv("QEMU_S390_ITRACE_STOP_PSW");
    uint64_t header[2];

    itrace_done = true;     /* a failed start is not retried */
    if (!path) {
        error_report("s390 itrace: QEMU_S390_ITRACE_FILE is not set");
        return;
    }
    itrace_max = max ? strtoull(max, NULL, 0) : 5000000;
    itrace_have_stop = stop != NULL;
    itrace_stop_psw = stop ? strtoull(stop, NULL, 16) : 0;
    itrace_file = fopen(path, "wb");
    if (!itrace_file) {
        error_report("s390 itrace: cannot open %s: %s", path,
                     strerror(errno));
        return;
    }
    setvbuf(itrace_file, NULL, _IOFBF, 8 * MiB);
    header[0] = sizeof(S390ITraceRecord);
    header[1] = itrace_max;
    if (fwrite(ITRACE_MAGIC, 8, 1, itrace_file) != 1 ||
        fwrite(header, sizeof(header), 1, itrace_file) != 1 ||
        itrace_set_singlestep(cs, true)) {
        error_report("s390 itrace: cannot start");
        fclose(itrace_file);
        itrace_file = NULL;
        return;
    }
    itrace_done = false;
    itrace_active = true;
    info_report("s390 itrace: armed, writing %s (limit %" PRIu64 ")",
                path, itrace_max);
}

/* vCPU thread, before every KVM_RUN. */
void s390_kvm_itrace_pre_run(CPUState *cs)
{
    static bool checked_env;

    if (itrace_active || itrace_done) {
        return;
    }
    if (!checked_env) {
        const char *at_start = getenv("QEMU_S390_ITRACE_AT_START");

        checked_env = true;
        if (at_start && !strcmp(at_start, "1")) {
            s390_kvm_itrace_arm();
        }
    }
    if (qatomic_read(&itrace_requested)) {
        itrace_start(cs);
    }
}

/*
 * vCPU thread, at the start of exit handling and without the BQL.
 * Returns true when the exit was a traced single-step and has been consumed.
 */
bool s390_kvm_itrace_exit(CPUState *cs, struct kvm_run *run)
{
    S390ITraceRecord rec;

    if (!itrace_active || run->exit_reason != KVM_EXIT_DEBUG ||
        run->debug.arch.type != KVM_SINGLESTEP || cpu_single_stepping(cs)) {
        return false;
    }
    rec.seq = itrace_count;
    rec.per_addr = run->debug.arch.addr;
    rec.psw_mask = run->psw_mask;
    rec.psw_addr = run->psw_addr;
    memcpy(rec.gprs, run->s.regs.gprs, sizeof(rec.gprs));
    rec.cr1 = run->s.regs.crs[1];
    rec.cr4 = run->s.regs.crs[4];
    rec.gbea = run->s.regs.gbea;
    rec.flags = run->debug.arch.type;
    if (fwrite(&rec, sizeof(rec), 1, itrace_file) != 1) {
        itrace_finish(cs, "write error");
        return true;
    }
    itrace_count++;
    if (itrace_have_stop && rec.per_addr == itrace_stop_psw) {
        itrace_finish(cs, "stop PSW reached");
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(RUN_STATE_PAUSED);
    } else if (itrace_count >= itrace_max) {
        itrace_finish(cs, "record limit");
    }
    return true;
}
