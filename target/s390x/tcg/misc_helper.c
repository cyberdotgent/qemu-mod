/*
 *  S/390 misc helper routines
 *
 *  Copyright (c) 2009 Ulrich Hecht
 *  Copyright (c) 2009 Alexander Graf
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "cpu.h"
#include "s390x-internal.h"
#include "qemu/host-utils.h"
#include "exec/helper-proto.h"
#include "qemu/timer.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/target_page.h"
#include "qapi/error.h"
#include "tcg_s390x.h"
#include "s390-tod.h"

#if !defined(CONFIG_USER_ONLY)
#include "system/cpus.h"
#include "system/system.h"
#include "hw/s390x/ebcdic.h"
#include "hw/s390x/s390-hypercall.h"
#include "hw/s390x/sclp.h"
#include "hw/s390x/s390_flic.h"
#include "hw/s390x/ioinst.h"
#include "hw/s390x/s390-pci-inst.h"
#include "hw/core/boards.h"
#include "hw/s390x/tod.h"
#include CONFIG_DEVICES
#endif

/* #define DEBUG_HELPER */
#ifdef DEBUG_HELPER
#define HELPER_LOG(x...) qemu_log(x)
#else
#define HELPER_LOG(x...)
#endif

uint64_t HELPER(ecag)(uint64_t address)
{
    unsigned int ai = extract64(address, 4, 4);
    unsigned int li = extract64(address, 1, 3);

    /* Address bits 40-55 are reserved. */
    if (address & 0xffff00) {
        return -1;
    }

    switch (ai) {
    case 0:
        /*
         * Topology summary: cache level 0 is private to this CPU and
         * cache levels 1-7 are not implemented.
         */
        return 0x0400000000000000ULL;
    case 1:
        /* Cache-line size for the implemented level 0 cache. */
        return li == 0 ? 256 : -1;
    case 2:
        /* Fictitious 512 KiB total size for the level 0 cache. */
        return li == 0 ? 256 * 2048 : -1;
    default:
        return -1;
    }
}

uint32_t HELPER(pfpo)(CPUS390XState *env)
{
    uint32_t control = env->regs[0];
    bool test = control & 0x80000000;
    unsigned int operation_type = extract32(control, 24, 7);
    unsigned int rounding_method = extract32(control, 0, 4);

    /*
     * PFPO permits a machine to implement a subset of the conversion
     * operations.  TCG currently implements the test-operation interface
     * and reports every conversion operation as unsupported.
     */
    if (operation_type != 1 ||
        (rounding_method >= 2 && rounding_method <= 7)) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
    }

    env->regs[1] = deposit64(env->regs[1], 0, 32, 0);
    if (test) {
        return 3;
    }

    tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
}

uint32_t HELPER(svs)(CPUS390XState *env, uint32_t r1)
{
    /*
     * SVS manages the coupling-facility list-notification summary state.
     * QEMU does not provide list-notification vectors, so the only global
     * summary state it can expose is the inactive state.  Setting or
     * resetting that state therefore completes immediately.
     */
    switch ((uint32_t)env->regs[1]) {
    case 1:                         /* set global summary */
        return 0;
    case 3:                         /* reset global summary */
        env->regs[r1 + 1] = 0;      /* no active local summaries */
        return 0;
    default:
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
    }
}

/*
 * In a one-vCPU TCG machine a constrained transaction cannot conflict with
 * another CPU.  Keep asynchronous interruptions pending until TEND, making
 * the permitted constrained instruction sequence indivisible.  System TCG
 * also snapshots first-written pages and selected GPR pairs so a synchronous
 * constraint or program interruption can roll the transaction back.
 */
uint32_t HELPER(tbeginc)(CPUS390XState *env, uint32_t b1, uint32_t i2,
                         uint64_t start_addr)
{
    if (!(env->cregs[0] & CR0_TRANSACTIONAL_EXE)) {
        tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
    }
    if (b1 != 0) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
    }
    if (env->tx_depth != 0) {
#ifdef CONFIG_USER_ONLY
        env->tx_depth = 0;
        env->tx_constrained = false;
#else
        s390_tx_abort(env);
#endif
        tcg_s390_program_interrupt(env, PGM_TXF_EVENT |
                                   PGM_TRANSACTION_CONSTRAINT, GETPC());
    }

#ifdef CONFIG_USER_ONLY
    env->tx_depth = 1;
    env->tx_constrained = true;
#else
    s390_tx_begin(env, start_addr, extract32(i2, 8, 8));
#endif
    return 0;
}

uint32_t HELPER(tend)(CPUS390XState *env)
{
    if (!(env->cregs[0] & CR0_TRANSACTIONAL_EXE)) {
        tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
    }
    if (env->tx_depth == 0) {
        return 2;
    }

#ifdef CONFIG_USER_ONLY
    env->tx_depth = 0;
    env->tx_constrained = false;
#else
    s390_tx_commit(env);
#endif
    return 0;
}

/* Raise an exception statically from a TB.  */
void HELPER(exception)(CPUS390XState *env, uint32_t excp)
{
    CPUState *cs = env_cpu(env);

    HELPER_LOG("%s: exception %d\n", __func__, excp);
    cs->exception_index = excp;
    cpu_loop_exit(cs);
}

/* Store CPU Timer (also used for EXTRACT CPU TIME) */
uint64_t HELPER(stpt)(CPUS390XState *env)
{
#if defined(CONFIG_USER_ONLY)
    /*
     * Fake a descending CPU timer. We could get negative values here,
     * but we don't care as it is up to the OS when to process that
     * interrupt and reset to > 0.
     */
    return UINT64_MAX - (uint64_t)cpu_get_host_ticks();
#else
    int64_t deadline = env->cputm;
    int64_t remaining = deadline -
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    return time2tod_signed(remaining);
#endif
}

static uint64_t get_tod_clock(CPUS390XState *env)
{
#ifdef CONFIG_USER_ONLY
    struct timespec ts;
    uint64_t ns;

    clock_gettime(CLOCK_REALTIME, &ts);
    ns = ts.tv_sec * NANOSECONDS_PER_SECOND + ts.tv_nsec;

    return TOD_UNIX_EPOCH + time2tod(ns);
#else
    S390TODState *td = s390_get_todstate();
    S390TODClass *tdc = S390_TOD_GET_CLASS(td);
    S390TOD tod;

    tdc->get(td, &tod, &error_abort);
    return tod.low;
#endif
}

#ifdef CONFIG_USER_ONLY
static __thread uint64_t user_unique_high;
static __thread uint64_t user_unique_low;
static __thread bool user_unique_valid;
#endif

/* Store Clock Fast does not provide the uniqueness guarantee of STCK. */
uint64_t HELPER(stckf)(CPUS390XState *env)
{
    return get_tod_clock(env);
}

/* Store Clock */
uint64_t HELPER(stck)(CPUS390XState *env)
{
#ifdef CONFIG_USER_ONLY
    uint64_t clock = get_tod_clock(env);

    if (user_unique_valid && clock <= user_unique_high) {
        clock = user_unique_high + 1;
    }
    user_unique_high = clock;
    user_unique_low = 0;
    user_unique_valid = true;
    return clock;
#else
    S390TODState *td = s390_get_todstate();
    uint64_t clock = get_tod_clock(env);

    qemu_mutex_lock(&td->unique_lock);
    if (td->unique_valid && clock <= td->unique_high) {
        clock = td->unique_high + 1;
    }
    td->unique_high = clock;
    td->unique_low = 0;
    td->unique_valid = true;
    qemu_mutex_unlock(&td->unique_lock);
    return clock;
#endif
}

#ifdef CONFIG_USER_ONLY
void HELPER(stcke)(CPUS390XState *env, uint64_t addr, uint32_t mmu_idx)
{
    const uint64_t ext_mask = (1ULL << 40) - 1;
    uint64_t clock = get_tod_clock(env);
    uint64_t extension;
    uintptr_t ra = GETPC();

    probe_write_access(env, wrap_address(env, addr), 16, ra);
    if (!user_unique_valid || clock > user_unique_high) {
        extension = 1;
    } else {
        clock = user_unique_high;
        extension = (user_unique_low + 1) & ext_mask;
        if (!extension) {
            clock++;
            extension = 1;
        }
    }
    user_unique_high = clock;
    user_unique_low = extension;
    user_unique_valid = true;

    cpu_stq_be_mmuidx_ra(env, wrap_address(env, addr), clock >> 8,
                         mmu_idx, ra);
    cpu_stq_be_mmuidx_ra(env, wrap_address(env, addr + 8),
                         (clock << 56) | (extension << 16) | env->todpr,
                         mmu_idx, ra);
}
#endif

#ifndef CONFIG_USER_ONLY
void HELPER(stcke)(CPUS390XState *env, uint64_t addr, uint32_t mmu_idx)
{
    const uint64_t ext_mask = (1ULL << 40) - 1;
    S390TODState *td = s390_get_todstate();
    uint64_t clock = get_tod_clock(env);
    uint64_t extension;
    uint64_t word0, word1;
    uintptr_t ra = GETPC();

    /*
     * STCKE checks the complete 16-byte result area before retrieving the
     * clock, so an exception on the second doubleword cannot leave a partial
     * clock value behind.
     */
    probe_write_access(env, wrap_address(env, addr), 16, ra);

    qemu_mutex_lock(&td->unique_lock);
    if (!td->unique_valid || clock > td->unique_high) {
        extension = 1;
    } else {
        clock = td->unique_high;
        extension = (td->unique_low + 1) & ext_mask;
        if (!extension) {
            clock++;
            extension = 1;
        }
    }
    td->unique_high = clock;
    td->unique_low = extension;
    td->unique_valid = true;
    qemu_mutex_unlock(&td->unique_lock);

    word0 = clock >> 8;
    word1 = (clock << 56) | (extension << 16) | env->todpr;
    cpu_stq_be_mmuidx_ra(env, wrap_address(env, addr), word0, mmu_idx, ra);
    cpu_stq_be_mmuidx_ra(env, wrap_address(env, addr + 8), word1, mmu_idx, ra);
}

static void ptff_store(CPUS390XState *env, const uint8_t *buf, size_t len,
                       int mmu_idx, uintptr_t ra)
{
    uint64_t addr = env->regs[1];
    MemOpIdx oi = make_memop_idx(MO_UB, mmu_idx);

    for (size_t i = 0; i < len; i++) {
        cpu_stb_mmu(env, wrap_address(env, addr + i), buf[i], oi, ra);
    }
}

uint32_t HELPER(ptff)(CPUS390XState *env, uint32_t mmu_idx)
{
    uint8_t functions[16] = { 0 };
    uint8_t result[256] = { 0 };
    uint32_t fc = env->regs[0] & 0x7f;
    uintptr_t ra = GETPC();
    uint64_t clock;
    size_t len;

    if (env->regs[0] & 0x80) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }

    /*
     * QAF itself is implicit.  Report the query functions implemented by
     * TCG and installed in the selected CPU model, but do not claim the
     * steering-control or multiple-epoch functions that TCG cannot perform.
     */
    s390_get_feat_block(S390_FEAT_TYPE_PTFF, functions);
    functions[0] = (functions[0] & 0x7c) | 0x80;
    memset(functions + 1, 0, sizeof(functions) - 1);

    if (fc != 0 && !test_be_bit(fc, functions)) {
        if (fc >= 64 && !(env->psw.mask & PSW_MASK_PSTATE)) {
            return 3;
        }
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }

    switch (fc) {
    case 0: /* Query available functions */
        ptff_store(env, functions, sizeof(functions), mmu_idx, ra);
        return 0;
    case 1: /* Query TOD offset */
        len = 32;
        break;
    case 2: /* Query steering information */
        len = 56;
        break;
    case 3: /* Query physical clock */
        len = 8;
        break;
    case 4: /* Query UTC information: all zero when STP is not installed */
        ptff_store(env, result, sizeof(result), mmu_idx, ra);
        return 0;
    case 5: /* Query TOD offset user */
        len = 40;
        break;
    default:
        g_assert_not_reached();
    }

    /*
     * QEMU does not steer its TCG TOD clock separately from the host clock.
     * Consequently the current TOD value is also its physical-clock value,
     * while TOD, logical-TOD, epoch, and user offsets are all zero.
     */
    clock = HELPER(stckf)(env);
    stq_be_p(result, clock);
    ptff_store(env, result, len, mmu_idx, ra);
    return 0;
}

/* SCLP service call */
uint32_t HELPER(servc)(CPUS390XState *env, uint64_t r1, uint64_t r2)
{
    /*
     * SERVC operands are the low 32 bits of the selected general registers.
     * In particular, ESA/390 callers are free to retain unrelated values in
     * the high halves.  Passing the full 64-bit SCCB register to the memory
     * layer turns those values into a spurious addressing exception.
     */
    uint32_t sccb = r1;
    uint32_t code = r2;

    bql_lock();
    int r = sclp_service_call(env_archcpu(env), sccb, code);
    bql_unlock();
    if (r < 0) {
        tcg_s390_program_interrupt(env, -r, GETPC());
    }
    return r;
}

void HELPER(esea)(CPUS390XState *env, uint32_t r1)
{
    uint16_t new_eax = extract64(env->regs[r1], 0, 16);
    uint16_t old_eax = extract64(env->cregs[8], 16, 16);

    env->regs[r1] = deposit64(env->regs[r1], 16, 16, old_eax);
    env->cregs[8] = deposit64(env->cregs[8], 16, 16, new_eax);
}

void HELPER(diag)(CPUS390XState *env, uint32_t r1, uint32_t r3, uint32_t num)
{
    uint64_t r;

    switch (num) {
#ifdef CONFIG_S390_CCW_VIRTIO
    case 0x500:
        /* QEMU/KVM hypercall */
        bql_lock();
        handle_diag_500(env_archcpu(env), GETPC());
        bql_unlock();
        r = 0;
        break;
#endif /* CONFIG_S390_CCW_VIRTIO */
    case 0x44:
        /* yield */
        r = 0;
        break;
    case 0x80:
        /* MSSF service-processor call */
        bql_lock();
        handle_diag_080(env, r1, r3, GETPC());
        bql_unlock();
        r = 0;
        break;
    case 0x204:
        /* LPAR RMF interface */
        handle_diag_204(env, r1, r3, GETPC());
        r = 0;
        break;
    case 0x308:
        /* ipl */
        bql_lock();
        if (handle_diag_308(env, r1, r3, GETPC())) {
            /* As reset is triggered by the CPU, make sure to exit the loop */
            cpu_loop_exit(CPU(env_archcpu(env)));
        }
        bql_unlock();
        r = 0;
        break;
    case 0x31c:
        /*
         * CZAM removable-media notification.  The z/VSE recovery loader
         * issues subfunction zero after its tape image has been placed in
         * storage.  There is no action for QEMU to take: the s390-ccw
         * firmware has already performed that service.
         */
        if (env->regs[r3] != 0) {
            r = -1;
            break;
        }
        r = 0;
        break;
    case 0x288:
        /* time bomb (watchdog) */
        r = handle_diag_288(env, r1, r3);
        break;
    case 0x320:
        /* cert store */
        bql_lock();
        handle_diag_320(env, r1, r3, GETPC());
        bql_unlock();
        r = 0;
        break;
    case 0x508:
        /* secure ipl operations */
        bql_lock();
        handle_diag_508(env, r1, r3, GETPC());
        bql_unlock();
        r = 0;
        break;
    default:
        r = -1;
        break;
    }

    if (r) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
    }
}

/* Set Prefix */
void HELPER(spx)(CPUS390XState *env, uint64_t a1)
{
    const uint32_t prefix = a1 & 0x7fffe000;
    const uint32_t old_prefix = env->psa;
    CPUState *cs = env_cpu(env);

    if (prefix == old_prefix) {
        return;
    }
    /*
     * Since prefix got aligned to 8k and memory increments are a multiple of
     * 8k checking the first page is sufficient
     */
    if (!mmu_absolute_addr_valid(prefix, true)) {
        tcg_s390_program_interrupt(env, PGM_ADDRESSING, GETPC());
    }

    env->psa = prefix;
    HELPER_LOG("prefix: %#x\n", prefix);
    tlb_flush_page(cs, 0);
    tlb_flush_page(cs, TARGET_PAGE_SIZE);
    if (prefix != 0) {
        tlb_flush_page(cs, prefix);
        tlb_flush_page(cs, prefix + TARGET_PAGE_SIZE);
    }
    if (old_prefix != 0) {
        tlb_flush_page(cs, old_prefix);
        tlb_flush_page(cs, old_prefix + TARGET_PAGE_SIZE);
    }
}

static void update_ckc_timer(CPUS390XState *env)
{
    S390TODState *td = s390_get_todstate();
    uint64_t time;

    /* stop the timer and remove pending CKC IRQs */
    timer_del(env->tod_timer);
    g_assert(bql_locked());
    env->pending_int &= ~INTERRUPT_EXT_CLOCK_COMPARATOR;

    /* the tod has to exceed the ckc, this can never happen if ckc is all 1's */
    if (env->ckc == -1ULL) {
        return;
    }

    if (env->ckc < td->base.low) {
        time = 0;
    } else {
        /* difference between origins */
        time = env->ckc - td->base.low;

        /* nanoseconds */
        time = tod2time(time);
        if (time < INT64_MAX) {
            time++;
        }
    }

    timer_mod(env->tod_timer, MIN(time, (uint64_t)INT64_MAX));
}

/* Set Clock Comparator */
void HELPER(sckc)(CPUS390XState *env, uint64_t ckc)
{
    env->ckc = ckc;

    bql_lock();
    update_ckc_timer(env);
    bql_unlock();
}

void tcg_s390_tod_updated(CPUState *cs, run_on_cpu_data opaque)
{
    update_ckc_timer(cpu_env(cs));
}

/* Set Clock */
uint32_t HELPER(sck)(CPUS390XState *env, uint64_t tod_low)
{
    S390TODState *td = s390_get_todstate();
    S390TODClass *tdc = S390_TOD_GET_CLASS(td);
    S390TOD tod = {
        .high = 0,
        .low = tod_low,
    };

    bql_lock();
    tdc->set(td, &tod, &error_abort);
    bql_unlock();
    return 0;
}

/* Set Tod Programmable Field */
void HELPER(sckpf)(CPUS390XState *env, uint64_t r0)
{
    uint32_t val = r0;

    if (val & 0xffff0000) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
    }
    env->todpr = val;
}

/* Store Clock Comparator */
uint64_t HELPER(stckc)(CPUS390XState *env)
{
    return env->ckc;
}

/* Set CPU Timer */
void HELPER(spt)(CPUS390XState *env, uint64_t time)
{
    int64_t delta = tod2time_signed(time);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t deadline = now + delta;
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    timer_del(env->cpu_timer);
    env->pending_int &= ~INTERRUPT_EXT_CPU_TIMER;
    env->cputm = deadline;

    if ((int64_t)time < 0) {
        cpu_inject_cpu_timer(cpu);
    } else {
        /*
         * The interruption condition starts when the CPU timer is
         * negative, not when it is zero.  QEMU's virtual timers use
         * nanosecond granularity, so schedule at the first representable
         * instant after zero.
         */
        timer_mod(env->cpu_timer,
                  deadline == INT64_MAX ? deadline : deadline + 1);
    }
    bql_unlock();
}

/* Store System Information */
uint32_t HELPER(stsi)(CPUS390XState *env, uint64_t a0, uint64_t r0, uint64_t r1)
{
    const uintptr_t ra = GETPC();
    const uint32_t sel1 = r0 & STSI_R0_SEL1_MASK;
    const uint32_t sel2 = r1 & STSI_R1_SEL2_MASK;
    const MachineState *ms = MACHINE(qdev_get_machine());
    uint16_t total_cpus = 0, conf_cpus = 0, reserved_cpus = 0;
    S390CPU *cpu = env_archcpu(env);
    SysIB sysib = { };
    int i, cc = 0;

    if ((r0 & STSI_R0_FC_MASK) > STSI_R0_FC_LEVEL_3) {
        /* invalid function code: no other checks are performed */
        return 3;
    }

    if ((r0 & STSI_R0_RESERVED_MASK) || (r1 & STSI_R1_RESERVED_MASK)) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }

    if ((r0 & STSI_R0_FC_MASK) == STSI_R0_FC_CURRENT) {
        /* query the current level: no further checks are performed */
        env->regs[0] = STSI_R0_FC_LEVEL_3;
        return 0;
    }

    if (a0 & ~TARGET_PAGE_MASK) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }

    /* count the cpus and split them into configured and reserved ones */
    for (i = 0; i < ms->possible_cpus->len; i++) {
        total_cpus++;
        if (ms->possible_cpus->cpus[i].cpu) {
            conf_cpus++;
        } else {
            reserved_cpus++;
        }
    }

    /*
     * In theory, we could report Level 1 / Level 2 as current. However,
     * the Linux kernel will detect this as running under LPAR and assume
     * that we have a sclp linemode console (which is always present on
     * LPAR, but not the default for QEMU), therefore not displaying boot
     * messages and making booting a Linux kernel under TCG harder.
     *
     * For now we fake the same SMP configuration on all levels.
     *
     * TODO: We could later make the level configurable via the machine
     *       and change defaults (linemode console) based on machine type
     *       and accelerator.
     */
    switch (r0 & STSI_R0_FC_MASK) {
    case STSI_R0_FC_LEVEL_1:
        if ((sel1 == 1) && (sel2 == 1)) {
            /* Basic Machine Configuration */
            char type[5] = {};

            ebcdic_put(sysib.sysib_111.manuf, "QEMU            ", 16);
            /* same as machine type number in STORE CPU ID, but in EBCDIC */
            snprintf(type, ARRAY_SIZE(type), "%X", cpu->model->def->type);
            ebcdic_put(sysib.sysib_111.type, type, 4);
            /* model number (not stored in STORE CPU ID for z/Architecture) */
            ebcdic_put(sysib.sysib_111.model, "QEMU            ", 16);
            ebcdic_put(sysib.sysib_111.sequence, "QEMU            ", 16);
            ebcdic_put(sysib.sysib_111.plant, "QEMU", 4);
        } else if ((sel1 == 2) && (sel2 == 1)) {
            /* Basic Machine CPU */
            ebcdic_put(sysib.sysib_121.sequence, "QEMUQEMUQEMUQEMU", 16);
            ebcdic_put(sysib.sysib_121.plant, "QEMU", 4);
            sysib.sysib_121.cpu_addr = cpu_to_be16(env->core_id);
        } else if ((sel1 == 2) && (sel2 == 2)) {
            /* Basic Machine CPUs */
            sysib.sysib_122.capability = cpu_to_be32(0x443afc29);
            sysib.sysib_122.total_cpus = cpu_to_be16(total_cpus);
            sysib.sysib_122.conf_cpus = cpu_to_be16(conf_cpus);
            sysib.sysib_122.reserved_cpus = cpu_to_be16(reserved_cpus);
        } else {
            cc = 3;
        }
        break;
    case STSI_R0_FC_LEVEL_2:
        if ((sel1 == 2) && (sel2 == 1)) {
            /* LPAR CPU */
            ebcdic_put(sysib.sysib_221.sequence, "QEMUQEMUQEMUQEMU", 16);
            ebcdic_put(sysib.sysib_221.plant, "QEMU", 4);
            sysib.sysib_221.cpu_addr = cpu_to_be16(env->core_id);
        } else if ((sel1 == 2) && (sel2 == 2)) {
            /* LPAR CPUs */
            sysib.sysib_222.lcpuc = 0x80; /* dedicated */
            sysib.sysib_222.total_cpus = cpu_to_be16(total_cpus);
            sysib.sysib_222.conf_cpus = cpu_to_be16(conf_cpus);
            sysib.sysib_222.reserved_cpus = cpu_to_be16(reserved_cpus);
            ebcdic_put(sysib.sysib_222.name, "QEMU    ", 8);
            sysib.sysib_222.caf = cpu_to_be32(1000);
            sysib.sysib_222.dedicated_cpus = cpu_to_be16(conf_cpus);
        } else {
            cc = 3;
        }
        break;
    case STSI_R0_FC_LEVEL_3:
        if ((sel1 == 2) && (sel2 == 2)) {
            /* VM CPUs */
            sysib.sysib_322.count = 1;
            sysib.sysib_322.vm[0].total_cpus = cpu_to_be16(total_cpus);
            sysib.sysib_322.vm[0].conf_cpus = cpu_to_be16(conf_cpus);
            sysib.sysib_322.vm[0].reserved_cpus = cpu_to_be16(reserved_cpus);
            sysib.sysib_322.vm[0].caf = cpu_to_be32(1000);
            /* Linux kernel uses this to distinguish us from z/VM */
            ebcdic_put(sysib.sysib_322.vm[0].cpi, "KVM/Linux       ", 16);
            sysib.sysib_322.vm[0].ext_name_encoding = 2; /* UTF-8 */

            /* If our VM has a name, use the real name */
            if (qemu_name) {
                memset(sysib.sysib_322.vm[0].name, 0x40,
                       sizeof(sysib.sysib_322.vm[0].name));
                ebcdic_put(sysib.sysib_322.vm[0].name, qemu_name,
                           MIN(sizeof(sysib.sysib_322.vm[0].name),
                               strlen(qemu_name)));
                strpadcpy((char *)sysib.sysib_322.ext_names[0],
                          sizeof(sysib.sysib_322.ext_names[0]),
                          qemu_name, '\0');

            } else {
                ebcdic_put(sysib.sysib_322.vm[0].name, "TCGguest", 8);
                strcpy((char *)sysib.sysib_322.ext_names[0], "TCGguest");
            }

            /* add the uuid */
            memcpy(sysib.sysib_322.vm[0].uuid, &qemu_uuid,
                   sizeof(sysib.sysib_322.vm[0].uuid));
        } else {
            cc = 3;
        }
        break;
    }

    if (cc == 0) {
        if (s390_cpu_virt_mem_write(cpu, a0, 0, &sysib, sizeof(sysib))) {
            s390_cpu_virt_mem_handle_exc(cpu, ra);
        }
    }

    return cc;
}

uint32_t HELPER(sigp)(CPUS390XState *env, uint64_t order_code, uint32_t r1,
                      uint32_t r3)
{
    int cc;

    /* TODO: needed to inject interrupts  - push further down */
    bql_lock();
    cc = handle_sigp(env, order_code & SIGP_ORDER_MASK, r1, r3);
    bql_unlock();

    return cc;
}
#endif

#ifndef CONFIG_USER_ONLY
void HELPER(xsch)(CPUS390XState *env, uint64_t r1)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_xsch(cpu, r1, GETPC());
    bql_unlock();
}

void HELPER(csch)(CPUS390XState *env, uint64_t r1)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_csch(cpu, r1, GETPC());
    bql_unlock();
}

void HELPER(hsch)(CPUS390XState *env, uint64_t r1)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_hsch(cpu, r1, GETPC());
    bql_unlock();
}

void HELPER(msch)(CPUS390XState *env, uint64_t r1, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_msch(cpu, r1, inst >> 16, GETPC());
    bql_unlock();
}

void HELPER(rchp)(CPUS390XState *env, uint64_t r1)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_rchp(cpu, r1, GETPC());
    bql_unlock();
}

void HELPER(rsch)(CPUS390XState *env, uint64_t r1)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_rsch(cpu, r1, GETPC());
    bql_unlock();
}

void HELPER(sal)(CPUS390XState *env, uint64_t r1)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    ioinst_handle_sal(cpu, r1, GETPC());
    bql_unlock();
}

void HELPER(schm)(CPUS390XState *env, uint64_t r1, uint64_t r2, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    ioinst_handle_schm(cpu, r1, r2, inst >> 16, GETPC());
    bql_unlock();
}

void HELPER(siga)(CPUS390XState *env)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    ioinst_handle_siga(cpu, GETPC());
    bql_unlock();
}

void HELPER(ssch)(CPUS390XState *env, uint64_t r1, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_ssch(cpu, r1, inst >> 16, GETPC());
    bql_unlock();
}

void HELPER(stcps)(CPUS390XState *env, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    ioinst_handle_stcps(cpu, inst >> 16, GETPC());
    bql_unlock();
}

void HELPER(stcrw)(CPUS390XState *env, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    ioinst_handle_stcrw(cpu, inst >> 16, GETPC());
    bql_unlock();
}

void HELPER(stsch)(CPUS390XState *env, uint64_t r1, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_stsch(cpu, r1, inst >> 16, GETPC());
    bql_unlock();
}

uint32_t HELPER(tpi)(CPUS390XState *env, uint64_t addr)
{
    const uintptr_t ra = GETPC();
    S390CPU *cpu = env_archcpu(env);
    QEMUS390FLICState *flic = s390_get_qemu_flic(s390_get_flic());
    QEMUS390FlicIO *io = NULL;
    LowCore *lowcore;

    if (addr & 0x3) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }

    bql_lock();
    io = qemu_s390_flic_dequeue_io(flic, env->cregs[6]);
    if (!io) {
        bql_unlock();
        return 0;
    }

    if (addr) {
        struct {
            uint16_t id;
            uint16_t nr;
            uint32_t parm;
        } intc = {
            .id = cpu_to_be16(io->id),
            .nr = cpu_to_be16(io->nr),
            .parm = cpu_to_be32(io->parm),
        };

        if (s390_cpu_virt_mem_write(cpu, addr, 0, &intc, sizeof(intc))) {
            /* writing failed, reinject and properly clean up */
            s390_io_interrupt(io->id, io->nr, io->parm, io->word);
            bql_unlock();
            g_free(io);
            s390_cpu_virt_mem_handle_exc(cpu, ra);
            return 0;
        }
    } else {
        /* no protection applies */
        lowcore = cpu_map_lowcore(env);
        lowcore->subchannel_id = cpu_to_be16(io->id);
        lowcore->subchannel_nr = cpu_to_be16(io->nr);
        lowcore->io_int_parm = cpu_to_be32(io->parm);
        lowcore->io_int_word = cpu_to_be32(io->word);
        cpu_unmap_lowcore(env, lowcore);
    }

    g_free(io);
    bql_unlock();
    return 1;
}

void HELPER(tsch)(CPUS390XState *env, uint64_t r1, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_tsch(cpu, r1, inst >> 16, GETPC());
    bql_unlock();
}

void HELPER(chsc)(CPUS390XState *env, uint64_t inst)
{
    S390CPU *cpu = env_archcpu(env);
    bql_lock();
    ioinst_handle_chsc(cpu, inst >> 16, GETPC());
    bql_unlock();
}
#endif

#ifndef CONFIG_USER_ONLY
static G_NORETURN void per_raise_exception(CPUS390XState *env)
{
    trigger_pgm_exception(env, PGM_PER);
    cpu_loop_exit(env_cpu(env));
}

static G_NORETURN void per_raise_exception_log(CPUS390XState *env)
{
    qemu_log_mask(CPU_LOG_INT, "PER interrupt after 0x%" PRIx64 "\n",
                  env->per_address);
    per_raise_exception(env);
}

void HELPER(per_check_exception)(CPUS390XState *env)
{
    /* psw_addr, per_address and int_pgm_ilen are already set. */
    if (unlikely(env->per_perc_atmid)) {
        per_raise_exception_log(env);
    }
}

/* Check if an address is within the PER starting address and the PER
   ending address.  The address range might loop.  */
static inline bool get_per_in_range(CPUS390XState *env, uint64_t addr)
{
    if (env->cregs[10] <= env->cregs[11]) {
        return env->cregs[10] <= addr && addr <= env->cregs[11];
    } else {
        return env->cregs[10] <= addr || addr <= env->cregs[11];
    }
}

void HELPER(per_branch)(CPUS390XState *env, uint64_t dest, uint32_t ilen)
{
    if ((env->cregs[9] & PER_CR9_CONTROL_BRANCH_ADDRESS)
        && !get_per_in_range(env, dest)) {
        return;
    }

    env->psw.addr = dest;
    env->int_pgm_ilen = ilen;
    env->per_address = env->gbea;
    env->per_perc_atmid = PER_CODE_EVENT_BRANCH | get_per_atmid(env);
    per_raise_exception_log(env);
}

void HELPER(per_ifetch)(CPUS390XState *env, uint32_t ilen)
{
    if (get_per_in_range(env, env->psw.addr)) {
        env->per_address = env->psw.addr;
        env->int_pgm_ilen = ilen;
        env->per_perc_atmid = PER_CODE_EVENT_IFETCH | get_per_atmid(env);

        /* If the instruction has to be nullified, trigger the
           exception immediately. */
        if (env->cregs[9] & PER_CR9_EVENT_IFETCH_NULLIFICATION) {
            env->per_perc_atmid |= PER_CODE_EVENT_NULLIFICATION;
            qemu_log_mask(CPU_LOG_INT, "PER interrupt before 0x%" PRIx64 "\n",
                          env->per_address);
            per_raise_exception(env);
        }
    }
}

void HELPER(per_store_real)(CPUS390XState *env, uint32_t ilen)
{
    /* PSW is saved just before calling the helper.  */
    env->per_address = env->psw.addr;
    env->int_pgm_ilen = ilen;
    env->per_perc_atmid = PER_CODE_EVENT_STORE_REAL | get_per_atmid(env);
    per_raise_exception_log(env);
}
#endif

static uint8_t stfl_bytes[2048];
static unsigned int used_stfl_bytes;

static void prepare_stfl(void)
{
    static bool initialized;
    int i;

    /* racy, but we don't care, the same values are always written */
    if (initialized) {
        return;
    }

    s390_get_feat_block(S390_FEAT_TYPE_STFL, stfl_bytes);
    for (i = 0; i < sizeof(stfl_bytes); i++) {
        if (stfl_bytes[i]) {
            used_stfl_bytes = i + 1;
        }
    }
    initialized = true;
}

#ifndef CONFIG_USER_ONLY
void HELPER(stfl)(CPUS390XState *env)
{
    LowCore *lowcore;

    lowcore = cpu_map_lowcore(env);
    prepare_stfl();
    memcpy(&lowcore->stfl_fac_list, stfl_bytes, sizeof(lowcore->stfl_fac_list));
    cpu_unmap_lowcore(env, lowcore);
}
#endif

uint32_t HELPER(stfle)(CPUS390XState *env, uint64_t addr)
{
    const int mmu_idx = cpu_mmu_index(env_cpu(env), false);
    const MemOpIdx oi = make_memop_idx(MO_8, mmu_idx);
    const uintptr_t ra = GETPC();
    const int count_bytes = ((env->regs[0] & 0xff) + 1) * 8;
    int max_bytes;
    int i;

    if (addr & 0x7) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, ra);
    }

    prepare_stfl();
    max_bytes = ROUND_UP(used_stfl_bytes, 8);

    /*
     * The PoP says that doublewords beyond the highest-numbered facility
     * bit may or may not be stored.  However, existing hardware appears to
     * not store the words, and existing software depend on that.
     */
    for (i = 0; i < MIN(count_bytes, max_bytes); ++i) {
        cpu_stb_mmu(env, addr + i, stfl_bytes[i], oi, ra);
    }

    env->regs[0] = deposit64(env->regs[0], 0, 8, (max_bytes / 8) - 1);
    return count_bytes >= max_bytes ? 0 : 3;
}

#ifndef CONFIG_USER_ONLY
/*
 * Note: we ignore any return code of the functions called for the pci
 * instructions, as the only time they return !0 is when the stub is
 * called, and in that case we didn't even offer the zpci facility.
 * The only exception is SIC, where program checks need to be handled
 * by the caller.
 */
void HELPER(clp)(CPUS390XState *env, uint32_t r2)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    clp_service_call(cpu, r2, GETPC());
    bql_unlock();
}

void HELPER(pcilg)(CPUS390XState *env, uint32_t r1, uint32_t r2)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    pcilg_service_call(cpu, r1, r2, GETPC());
    bql_unlock();
}

void HELPER(pcistg)(CPUS390XState *env, uint32_t r1, uint32_t r2)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    pcistg_service_call(cpu, r1, r2, GETPC());
    bql_unlock();
}

void HELPER(stpcifc)(CPUS390XState *env, uint32_t r1, uint64_t fiba,
                     uint32_t ar)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    stpcifc_service_call(cpu, r1, fiba, ar, GETPC());
    bql_unlock();
}

void HELPER(sic)(CPUS390XState *env, uint64_t r1, uint64_t r3)
{
    S390CPU *cpu = env_archcpu(env);
    int r;

    bql_lock();
    r = css_do_sic(cpu, (r3 >> 27) & 0x7, r1 & 0xffff);
    bql_unlock();
    /* css_do_sic() may actually return a PGM_xxx value to inject */
    if (r) {
        tcg_s390_program_interrupt(env, -r, GETPC());
    }
}

void HELPER(rpcit)(CPUS390XState *env, uint32_t r1, uint32_t r2)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    rpcit_service_call(cpu, r1, r2, GETPC());
    bql_unlock();
}

void HELPER(pcistb)(CPUS390XState *env, uint32_t r1, uint32_t r3,
                    uint64_t gaddr, uint32_t ar)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    pcistb_service_call(cpu, r1, r3, gaddr, ar, GETPC());
    bql_unlock();
}

void HELPER(mpcifc)(CPUS390XState *env, uint32_t r1, uint64_t fiba,
                    uint32_t ar)
{
    S390CPU *cpu = env_archcpu(env);

    bql_lock();
    mpcifc_service_call(cpu, r1, fiba, ar, GETPC());
    bql_unlock();
}
#endif
