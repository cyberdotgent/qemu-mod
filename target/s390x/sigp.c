/*
 * s390x SIGP instruction handling
 *
 * Copyright (c) 2009 Alexander Graf <agraf@suse.de>
 * Copyright IBM Corp. 2012
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/error-report.h"
#include "s390x-internal.h"
#include "hw/core/boards.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "system/address-spaces.h"
#include "exec/cputlb.h"
#include "system/tcg.h"
#include "system/kvm.h"
#include "kvm/kvm_s390x.h"
#include "trace.h"
#include "qapi/qapi-types-machine.h"
#include "target/s390x/kvm/pv.h"

QemuMutex qemu_sigp_mutex;

typedef struct SigpInfo {
    uint64_t param;
    int cc;
    uint64_t *status_reg;
} SigpInfo;

static void set_sigp_status(SigpInfo *si, uint64_t status)
{
    *si->status_reg &= 0xffffffff00000000ULL;
    *si->status_reg |= status;
    si->cc = SIGP_CC_STATUS_STORED;
}

static void sigp_sense(S390CPU *dst_cpu, SigpInfo *si)
{
    S390CpuState state = s390_cpu_get_state(dst_cpu);
    bool ext_call = dst_cpu->env.pending_int & INTERRUPT_EXTERNAL_CALL;
    uint64_t status = 0;

    if (!tcg_enabled()) {
        /* handled in KVM */
        set_sigp_status(si, SIGP_STAT_INVALID_ORDER);
        return;
    }

    /* sensing without locks is racy, but it's the same for real hw */
    if (state != S390_CPU_STATE_STOPPED && !ext_call) {
        si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
    } else {
        if (ext_call) {
            status |= SIGP_STAT_EXT_CALL_PENDING;
        }
        if (state == S390_CPU_STATE_STOPPED) {
            status |= SIGP_STAT_STOPPED;
        }
        set_sigp_status(si, status);
    }
}

static void sigp_external_call(S390CPU *src_cpu, S390CPU *dst_cpu, SigpInfo *si)
{
    int ret;

    if (!tcg_enabled()) {
        /* handled in KVM */
        set_sigp_status(si, SIGP_STAT_INVALID_ORDER);
        return;
    }

    ret = cpu_inject_external_call(dst_cpu, src_cpu->env.core_id);
    if (!ret) {
        si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
    } else {
        set_sigp_status(si, SIGP_STAT_EXT_CALL_PENDING);
    }
}

static void sigp_emergency(S390CPU *src_cpu, S390CPU *dst_cpu, SigpInfo *si)
{
    if (!tcg_enabled()) {
        /* handled in KVM */
        set_sigp_status(si, SIGP_STAT_INVALID_ORDER);
        return;
    }

    cpu_inject_emergency_signal(dst_cpu, src_cpu->env.core_id);
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_start(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;

    if (s390_cpu_get_state(cpu) != S390_CPU_STATE_STOPPED) {
        si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
        return;
    }

    s390_cpu_set_state(S390_CPU_STATE_OPERATING, cpu);
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_stop(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;

    if (s390_cpu_get_state(cpu) != S390_CPU_STATE_OPERATING) {
        si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
        return;
    }

    /* disabled wait - sleeping in user space */
    if (cs->halted) {
        s390_cpu_set_state(S390_CPU_STATE_STOPPED, cpu);
    } else {
        /* execute the stop function */
        cpu->env.sigp_order = SIGP_STOP;
        cpu_inject_stop(cpu);
    }
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

typedef struct SigpSaveArea {
    uint64_t    fprs[16];                       /* 0x0000 */
    uint64_t    grs[16];                        /* 0x0080 */
    PSW         psw;                            /* 0x0100 */
    uint8_t     pad_0x0110[0x0118 - 0x0110];    /* 0x0110 */
    uint32_t    prefix;                         /* 0x0118 */
    uint32_t    fpc;                            /* 0x011c */
    uint8_t     pad_0x0120[0x0124 - 0x0120];    /* 0x0120 */
    uint32_t    todpr;                          /* 0x0124 */
    uint64_t    cputm;                          /* 0x0128 */
    uint64_t    ckc;                            /* 0x0130 */
    uint8_t     pad_0x0138[0x0140 - 0x0138];    /* 0x0138 */
    uint32_t    ars[16];                        /* 0x0140 */
    uint64_t    crs[16];                        /* 0x0384 */
} SigpSaveArea;
QEMU_BUILD_BUG_ON(sizeof(SigpSaveArea) != 512);

/*
 * The save area of a CPU in the ESA/390 architectural mode.  The offsets
 * are those of the assigned storage locations: store status uses the first
 * 512 bytes of absolute storage, the orders "at address" a block with the
 * same layout.  The parts of the block not listed stay unchanged.
 */
#define ESA_SAVE_AREA_SIZE      512
#define ESA_SA_EXT_SAVE_ADDR    212     /* extended save area, 4 bytes */
#define ESA_SA_CPU_TIMER        216     /* 8 bytes */
#define ESA_SA_CLOCK_COMP       224     /* 8 bytes */
#define ESA_SA_PSW              256     /* 8 bytes */
#define ESA_SA_PREFIX           264     /* 4 bytes */
#define ESA_SA_ARS              288     /* 16 * 4 bytes */
#define ESA_SA_FPRS             352     /* registers 0, 2, 4, 6 */
#define ESA_SA_GRS              384     /* 16 * 4 bytes */
#define ESA_SA_CRS              448     /* 16 * 4 bytes */
/* extended save area: a 4K block */
#define ESA_XSA_FPRS            0       /* registers 0-15 */
#define ESA_XSA_FPC             128     /* 4 bytes, then 12 bytes of zeros */
#define ESA_XSA_STORED          144

static int s390_store_status_esa(S390CPU *cpu, hwaddr addr, bool store_arch)
{
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = CPU(cpu)->as;
    CPUS390XState *env = &cpu->env;
    hwaddr len = ESA_SAVE_AREA_SIZE;
    uint64_t psw;
    uint8_t *sa;
    int i;

    sa = address_space_map(as, addr, &len, true, attrs);
    if (!sa) {
        return -EFAULT;
    }
    if (len != ESA_SAVE_AREA_SIZE) {
        address_space_unmap(as, sa, len, true, 0);
        return -EFAULT;
    }

    psw = ((s390_cpu_get_psw_mask(env) ^ PSW_MASK_SHORTPSW) &
           PSW_MASK_SHORT_CTRL) | (env->psw.addr & PSW_MASK_SHORT_ADDR);
    stq_be_p(sa + ESA_SA_CPU_TIMER, env->cputm);
    stq_be_p(sa + ESA_SA_CLOCK_COMP, env->ckc);
    stq_be_p(sa + ESA_SA_PSW, psw);
    stl_be_p(sa + ESA_SA_PREFIX, env->psa);
    for (i = 0; i < 16; i++) {
        stl_be_p(sa + ESA_SA_ARS + 4 * i, env->aregs[i]);
        stl_be_p(sa + ESA_SA_GRS + 4 * i, env->regs[i]);
        stl_be_p(sa + ESA_SA_CRS + 4 * i, env->cregs[i]);
    }
    for (i = 0; i < 4; i++) {
        stq_be_p(sa + ESA_SA_FPRS + 8 * i, *get_freg(env, 2 * i));
    }
    address_space_unmap(as, sa, len, true, len);

    if (store_arch) {
        /* the architectural-mode identification: ESA/390 */
        address_space_stb(as, offsetof(LowCore, ar_access_id), 0, attrs, NULL);
    }
    return 0;
}

/* floating-point registers 0-15 and control register, by an order only */
static int s390_store_ext_status_esa(S390CPU *cpu, hwaddr addr)
{
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = CPU(cpu)->as;
    CPUS390XState *env = &cpu->env;
    hwaddr len = ESA_XSA_STORED;
    uint8_t *xsa;
    int i;

    xsa = address_space_map(as, addr, &len, true, attrs);
    if (!xsa) {
        return -EFAULT;
    }
    if (len != ESA_XSA_STORED) {
        address_space_unmap(as, xsa, len, true, 0);
        return -EFAULT;
    }
    for (i = 0; i < 16; i++) {
        stq_be_p(xsa + ESA_XSA_FPRS + 8 * i, *get_freg(env, i));
    }
    stl_be_p(xsa + ESA_XSA_FPC, env->fpc);
    memset(xsa + ESA_XSA_FPC + 4, 0, 12);
    address_space_unmap(as, xsa, len, true, len);
    return 0;
}

#define S390_STORE_STATUS_DEF_ADDR offsetof(LowCore, floating_pt_save_area)
static int s390_store_status(S390CPU *cpu, hwaddr addr, bool store_arch)
{
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = CPU(cpu)->as;
    SigpSaveArea *sa;
    hwaddr len = sizeof(*sa);
    int i;

    /* For PVMs storing will occur when this cpu enters SIE again */
    if (s390_is_pv()) {
        return 0;
    }

    if (cpu->env.esa_mode) {
        /* the default address of the other format means: at location 0 */
        return s390_store_status_esa(cpu, store_arch ? 0 : addr, store_arch);
    }

    sa = address_space_map(as, addr, &len, true, attrs);
    if (!sa) {
        return -EFAULT;
    }
    if (len != sizeof(*sa)) {
        address_space_unmap(as, sa, len, true, 0);
        return -EFAULT;
    }

    if (store_arch) {
        static const uint8_t ar_id = 1;

        address_space_stb(as, offsetof(LowCore, ar_access_id),
                          ar_id, attrs, NULL);

    }
    for (i = 0; i < 16; ++i) {
        sa->fprs[i] = cpu_to_be64(*get_freg(&cpu->env, i));
    }
    for (i = 0; i < 16; ++i) {
        sa->grs[i] = cpu_to_be64(cpu->env.regs[i]);
    }
    sa->psw.addr = cpu_to_be64(cpu->env.psw.addr);
    sa->psw.mask = cpu_to_be64(s390_cpu_get_psw_mask(&cpu->env));
    sa->prefix = cpu_to_be32(cpu->env.psa);
    sa->fpc = cpu_to_be32(cpu->env.fpc);
    sa->todpr = cpu_to_be32(cpu->env.todpr);
    sa->cputm = cpu_to_be64(cpu->env.cputm);
    sa->ckc = cpu_to_be64(cpu->env.ckc >> 8);
    for (i = 0; i < 16; ++i) {
        sa->ars[i] = cpu_to_be32(cpu->env.aregs[i]);
    }
    for (i = 0; i < 16; ++i) {
        sa->crs[i] = cpu_to_be64(cpu->env.cregs[i]);
    }

    address_space_unmap(as, sa, len, true, len);

    return 0;
}

static void sigp_stop_and_store_status(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;

    /* disabled wait - sleeping in user space */
    if (s390_cpu_get_state(cpu) == S390_CPU_STATE_OPERATING && cs->halted) {
        s390_cpu_set_state(S390_CPU_STATE_STOPPED, cpu);
    }

    switch (s390_cpu_get_state(cpu)) {
    case S390_CPU_STATE_OPERATING:
        cpu->env.sigp_order = SIGP_STOP_STORE_STATUS;
        cpu_inject_stop(cpu);
        /* store will be performed in do_stop_interrupt() */
        break;
    case S390_CPU_STATE_STOPPED:
        /* already stopped, just store the status */
        cpu_synchronize_state(cs);
        s390_store_status(cpu, S390_STORE_STATUS_DEF_ADDR, true);
        break;
    default:
        break;
    }
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_store_status_at_address(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;
    uint32_t address = si->param & 0x7ffffe00u;

    /* cpu has to be stopped */
    if (s390_cpu_get_state(cpu) != S390_CPU_STATE_STOPPED) {
        set_sigp_status(si, SIGP_STAT_INCORRECT_STATE);
        return;
    }

    cpu_synchronize_state(cs);

    if (s390_store_status(cpu, address, false)) {
        set_sigp_status(si, SIGP_STAT_INVALID_PARAMETER);
        return;
    }
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

/* SIGP store extended status at address: an order of the ESA/390 mode */
static void sigp_store_ext_status_at_address(CPUState *cs,
                                             run_on_cpu_data arg)
{
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;
    uint32_t address = si->param & 0x7ffffe00u;
    MemTxResult res;
    uint32_t xsa;

    /* cpu has to be stopped */
    if (s390_cpu_get_state(cpu) != S390_CPU_STATE_STOPPED) {
        set_sigp_status(si, SIGP_STAT_INCORRECT_STATE);
        return;
    }

    cpu_synchronize_state(cs);

    /* the program supplies the address of the extended save area */
    xsa = address_space_ldl_be(cs->as, address + ESA_SA_EXT_SAVE_ADDR, attrs,
                               &res) & 0x7ffff000u;
    if (res != MEMTX_OK || !xsa ||
        !address_space_access_valid(cs->as, xsa, 4096, true,
                                    attrs) ||
        s390_store_status(cpu, address, false) ||
        s390_store_ext_status_esa(cpu, xsa)) {
        set_sigp_status(si, SIGP_STAT_INVALID_PARAMETER);
        return;
    }
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

typedef struct SigpAdtlSaveArea {
    uint64_t    vregs[32][2];                     /* 0x0000 */
    uint8_t     pad_0x0200[0x0400 - 0x0200];      /* 0x0200 */
    uint64_t    gscb[4];                          /* 0x0400 */
    uint8_t     pad_0x0420[0x1000 - 0x0420];      /* 0x0420 */
} SigpAdtlSaveArea;
QEMU_BUILD_BUG_ON(sizeof(SigpAdtlSaveArea) != 4096);

#define ADTL_GS_MIN_SIZE 2048 /* minimal size of adtl save area for GS */
static int s390_store_adtl_status(S390CPU *cpu, hwaddr addr, hwaddr len)
{
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    AddressSpace *as = CPU(cpu)->as;
    SigpAdtlSaveArea *sa;
    hwaddr save = len;
    int i;

    sa = address_space_map(as, addr, &save, true, attrs);
    if (!sa) {
        return -EFAULT;
    }
    if (save != len) {
        address_space_unmap(as, sa, len, true, 0);
        return -EFAULT;
    }

    if (s390_has_feat(S390_FEAT_VECTOR)) {
        for (i = 0; i < 32; i++) {
            sa->vregs[i][0] = cpu_to_be64(cpu->env.vregs[i][0]);
            sa->vregs[i][1] = cpu_to_be64(cpu->env.vregs[i][1]);
        }
    }
    if (s390_has_feat(S390_FEAT_GUARDED_STORAGE) && len >= ADTL_GS_MIN_SIZE) {
        for (i = 0; i < 4; i++) {
            sa->gscb[i] = cpu_to_be64(cpu->env.gscb[i]);
        }
    }

    address_space_unmap(as, sa, len, true, len);

    return 0;
}

#define ADTL_SAVE_LC_MASK  0xfUL
static void sigp_store_adtl_status(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;
    uint8_t lc = si->param & ADTL_SAVE_LC_MASK;
    hwaddr addr = si->param & ~ADTL_SAVE_LC_MASK;
    hwaddr len = 1UL << (lc ? lc : 10);

    if (!s390_has_feat(S390_FEAT_VECTOR) &&
        !s390_has_feat(S390_FEAT_GUARDED_STORAGE)) {
        set_sigp_status(si, SIGP_STAT_INVALID_ORDER);
        return;
    }

    /* cpu has to be stopped */
    if (s390_cpu_get_state(cpu) != S390_CPU_STATE_STOPPED) {
        set_sigp_status(si, SIGP_STAT_INCORRECT_STATE);
        return;
    }

    /* address must be aligned to length */
    if (addr & (len - 1)) {
        set_sigp_status(si, SIGP_STAT_INVALID_PARAMETER);
        return;
    }

    /* no GS: only lc == 0 is valid */
    if (!s390_has_feat(S390_FEAT_GUARDED_STORAGE) &&
        lc != 0) {
        set_sigp_status(si, SIGP_STAT_INVALID_PARAMETER);
        return;
    }

    /* GS: 0, 10, 11, 12 are valid */
    if (s390_has_feat(S390_FEAT_GUARDED_STORAGE) &&
        lc != 0 &&
        lc != 10 &&
        lc != 11 &&
        lc != 12) {
        set_sigp_status(si, SIGP_STAT_INVALID_PARAMETER);
        return;
    }

    cpu_synchronize_state(cs);

    if (s390_store_adtl_status(cpu, addr, len)) {
        set_sigp_status(si, SIGP_STAT_INVALID_PARAMETER);
        return;
    }
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_restart(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;

    switch (s390_cpu_get_state(cpu)) {
    case S390_CPU_STATE_STOPPED:
        /* the restart irq has to be delivered prior to any other pending irq */
        cpu_synchronize_state(cs);
        /*
         * Set OPERATING (and unhalting) before loading the restart PSW.
         * s390_cpu_set_psw() will then properly halt the CPU again if
         * necessary (TCG).
         */
        s390_cpu_set_state(S390_CPU_STATE_OPERATING, cpu);
        do_restart_interrupt(&cpu->env);
        break;
    case S390_CPU_STATE_OPERATING:
        cpu_inject_restart(cpu);
        break;
    default:
        break;
    }
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_initial_cpu_reset(CPUState *cs, run_on_cpu_data arg)
{
    SigpInfo *si = arg.host_ptr;

    cpu_synchronize_state(cs);
    resettable_reset(OBJECT(cs), RESET_TYPE_S390_CPU_INITIAL);
    cpu_synchronize_post_reset(cs);
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_cpu_reset(CPUState *cs, run_on_cpu_data arg)
{
    SigpInfo *si = arg.host_ptr;

    cpu_synchronize_state(cs);
    resettable_reset(OBJECT(cs), RESET_TYPE_S390_CPU_NORMAL);
    cpu_synchronize_post_reset(cs);
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_set_prefix(CPUState *cs, run_on_cpu_data arg)
{
    S390CPU *cpu = S390_CPU(cs);
    SigpInfo *si = arg.host_ptr;
    uint32_t addr = si->param & 0x7fffe000u;
    hwaddr len = sizeof(struct LowCore);

    cpu_synchronize_state(cs);

    /* the prefix area of the ESA/390 mode is one 4K block */
    if (cpu->env.esa_mode) {
        addr = si->param & 0x7ffff000u;
        len = 4096;
    }

    if (!address_space_access_valid(&address_space_memory, addr, len, false,
                                    MEMTXATTRS_UNSPECIFIED)) {
        set_sigp_status(si, SIGP_STAT_INVALID_PARAMETER);
        return;
    }

    /* cpu has to be stopped */
    if (s390_cpu_get_state(cpu) != S390_CPU_STATE_STOPPED) {
        set_sigp_status(si, SIGP_STAT_INCORRECT_STATE);
        return;
    }

    cpu->env.psa = addr;
    tlb_flush(cs);
    cpu_synchronize_post_init(cs);
    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_cond_emergency(S390CPU *src_cpu, S390CPU *dst_cpu,
                                SigpInfo *si)
{
    const uint64_t psw_int_mask = PSW_MASK_IO | PSW_MASK_EXT;
    uint16_t p_asn, s_asn, asn;
    uint64_t psw_addr, psw_mask;
    bool idle;

    if (!tcg_enabled()) {
        /* handled in KVM */
        set_sigp_status(si, SIGP_STAT_INVALID_ORDER);
        return;
    }

    /* this looks racy, but these values are only used when STOPPED */
    idle = CPU(dst_cpu)->halted;
    psw_addr = dst_cpu->env.psw.addr;
    psw_mask = dst_cpu->env.psw.mask;
    asn = si->param;
    p_asn = dst_cpu->env.cregs[4] & 0xffff;  /* Primary ASN */
    s_asn = dst_cpu->env.cregs[3] & 0xffff;  /* Secondary ASN */

    if (s390_cpu_get_state(dst_cpu) != S390_CPU_STATE_STOPPED ||
        (psw_mask & psw_int_mask) != psw_int_mask ||
        (idle && psw_addr != 0) ||
        (!idle && (asn == p_asn || asn == s_asn))) {
        cpu_inject_emergency_signal(dst_cpu, src_cpu->env.core_id);
    } else {
        set_sigp_status(si, SIGP_STAT_INCORRECT_STATE);
    }

    si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
}

static void sigp_sense_running(S390CPU *dst_cpu, SigpInfo *si)
{
    if (!tcg_enabled()) {
        /* handled in KVM */
        set_sigp_status(si, SIGP_STAT_INVALID_ORDER);
        return;
    }

    /* sensing without locks is racy, but it's the same for real hw */
    if (!s390_has_feat(S390_FEAT_SENSE_RUNNING_STATUS)) {
        set_sigp_status(si, SIGP_STAT_INVALID_ORDER);
        return;
    }

    /* If halted (which includes also STOPPED), it is not running */
    if (CPU(dst_cpu)->halted) {
        set_sigp_status(si, SIGP_STAT_NOT_RUNNING);
    } else {
        si->cc = SIGP_CC_ORDER_CODE_ACCEPTED;
    }
}

static int handle_sigp_single_dst(S390CPU *cpu, S390CPU *dst_cpu, uint8_t order,
                                  uint64_t param, uint64_t *status_reg)
{
    SigpInfo si = {
        .param = param,
        .status_reg = status_reg,
    };

    /* cpu available? */
    if (dst_cpu == NULL) {
        return SIGP_CC_NOT_OPERATIONAL;
    }

    /* only resets can break pending orders */
    if (dst_cpu->env.sigp_order != 0 &&
        order != SIGP_CPU_RESET &&
        order != SIGP_INITIAL_CPU_RESET) {
        return SIGP_CC_BUSY;
    }

    switch (order) {
    case SIGP_SENSE:
        sigp_sense(dst_cpu, &si);
        break;
    case SIGP_EXTERNAL_CALL:
        sigp_external_call(cpu, dst_cpu, &si);
        break;
    case SIGP_EMERGENCY:
        sigp_emergency(cpu, dst_cpu, &si);
        break;
    case SIGP_START:
        run_on_cpu(CPU(dst_cpu), sigp_start, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_STOP:
        run_on_cpu(CPU(dst_cpu), sigp_stop, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_RESTART:
        run_on_cpu(CPU(dst_cpu), sigp_restart, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_STOP_STORE_STATUS:
        run_on_cpu(CPU(dst_cpu), sigp_stop_and_store_status, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_STORE_STATUS_ADDR:
        run_on_cpu(CPU(dst_cpu), sigp_store_status_at_address, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_STORE_EXT_STATUS_ADDR:
        /* the mode is that of the whole configuration */
        if (!cpu->env.esa_mode) {
            set_sigp_status(&si, SIGP_STAT_INVALID_ORDER);
            break;
        }
        run_on_cpu(CPU(dst_cpu), sigp_store_ext_status_at_address,
                   RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_STORE_ADTL_STATUS:
        /* an order of the z/Architecture architectural mode */
        if (cpu->env.esa_mode) {
            set_sigp_status(&si, SIGP_STAT_INVALID_ORDER);
            break;
        }
        run_on_cpu(CPU(dst_cpu), sigp_store_adtl_status, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_SET_PREFIX:
        run_on_cpu(CPU(dst_cpu), sigp_set_prefix, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_INITIAL_CPU_RESET:
        run_on_cpu(CPU(dst_cpu), sigp_initial_cpu_reset, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_CPU_RESET:
        run_on_cpu(CPU(dst_cpu), sigp_cpu_reset, RUN_ON_CPU_HOST_PTR(&si));
        break;
    case SIGP_COND_EMERGENCY:
        sigp_cond_emergency(cpu, dst_cpu, &si);
        break;
    case SIGP_SENSE_RUNNING:
        sigp_sense_running(dst_cpu, &si);
        break;
    default:
        set_sigp_status(&si, SIGP_STAT_INVALID_ORDER);
    }

    return si.cc;
}

static int sigp_set_architecture(S390CPU *cpu, uint32_t param,
                                 uint64_t *status_reg)
{
    /* under KVM the order exists for a configuration that has the mode */
    const bool kvm_cm = kvm_enabled() && kvm_s390_esa390_cm_enabled();
    CPUState *cs;
    uint8_t mode = param & 0xff;
    bool esa_mode = cpu->env.esa_mode;
    int ret;

    if (!tcg_enabled() && !kvm_cm) {
        if (mode == 1) {
            return SIGP_CC_ORDER_CODE_ACCEPTED;
        }
        *status_reg = (*status_reg & 0xffffffff00000000ULL) |
                      SIGP_STAT_INVALID_PARAMETER;
        return SIGP_CC_STATUS_STORED;
    }

    CPU_FOREACH(cs) {
        S390CPU *other = S390_CPU(cs);

        if (other != cpu &&
            s390_cpu_get_state(other) != S390_CPU_STATE_STOPPED) {
            *status_reg = (*status_reg & 0xffffffff00000000ULL) |
                          SIGP_STAT_INCORRECT_STATE;
            return SIGP_CC_STATUS_STORED;
        }
    }

    if ((mode == 0 && esa_mode) || ((mode == 1 || mode == 2) && !esa_mode) ||
        mode > 2) {
        *status_reg = (*status_reg & 0xffffffff00000000ULL) |
                      SIGP_STAT_INVALID_PARAMETER;
        return SIGP_CC_STATUS_STORED;
    }

    if (kvm_cm) {
        /*
         * The kernel only changes the format in which it runs the CPUs.
         * It can do so now: the other CPUs are stopped, this one is
         * handling its interception.  What the order defines for the
         * registers is done here.
         */
        CPU_FOREACH(cs) {
            cpu_synchronize_state(cs);
        }
        ret = kvm_s390_set_arch_mode(mode == 0);
        if (ret == -EBUSY) {
            /* a vCPU is still on its way out of the kernel */
            return SIGP_CC_BUSY;
        } else if (ret) {
            error_report_once("SIGP set-architecture: the kernel does not "
                              "change the architectural mode: %s",
                              strerror(-ret));
            *status_reg = (*status_reg & 0xffffffff00000000ULL) |
                          SIGP_STAT_INVALID_PARAMETER;
            return SIGP_CC_STATUS_STORED;
        }
        CPU_FOREACH(cs) {
            S390CPU *target = S390_CPU(cs);
            CPUS390XState *env = &target->env;
            int i;

            if (mode == 0) {
                s390_cpu_capture_z_state(env);
                env->esa_mode = true;
            } else {
                env->esa_mode = false;
                /*
                 * The PSW is the same PSW in the longer format.  With code
                 * 2 the CPUs other than this one get back what was
                 * captured when they left z/Architecture.
                 */
                if (mode == 2 && target != cpu) {
                    env->psw = env->captured_z_psw;
                    for (i = 0; i < 16; i++) {
                        env->cregs[i] = (env->cregs[i] & 0xffffffffULL) |
                                        (uint64_t)env->captured_z_cregs[i] << 32;
                    }
                }
                /* bit 51 of the prefix becomes zero */
                env->psa &= 0x7fffe000;
            }
            kvm_s390_esa390_put_arch_regs(cs);
        }
        return SIGP_CC_ORDER_CODE_ACCEPTED;
    }

    CPU_FOREACH(cs) {
        S390CPU *target = S390_CPU(cs);
        CPUS390XState *env = &target->env;

        if (mode == 0) {
            env->captured_z_psw = env->psw;
            env->esa_mode = true;
        } else {
            env->esa_mode = false;
            if (mode == 2) {
                env->psw = env->captured_z_psw;
            } else {
                env->psw.mask &= ~PSW_MASK_64;
                env->psw.addr &= PSW_MASK_SHORT_ADDR;
            }
        }
        env->psa &= 0x7ffff000;
        tlb_flush(cs);
    }

    return SIGP_CC_ORDER_CODE_ACCEPTED;
}

S390CPU *s390_cpu_addr2state(uint16_t cpu_addr)
{
    static MachineState *ms;

    if (!ms) {
        ms = MACHINE(qdev_get_machine());
        g_assert(ms->possible_cpus);
    }

    /* CPU address corresponds to the core_id and the index */
    if (cpu_addr >= ms->possible_cpus->len) {
        return NULL;
    }
    return S390_CPU(ms->possible_cpus->cpus[cpu_addr].cpu);
}

int handle_sigp(CPUS390XState *env, uint8_t order, uint64_t r1, uint64_t r3)
{
    uint64_t *status_reg = &env->regs[r1];
    uint64_t param = (r1 % 2) ? env->regs[r1] : env->regs[r1 + 1];
    S390CPU *cpu = env_archcpu(env);
    S390CPU *dst_cpu = NULL;
    int ret;

    if (qemu_mutex_trylock(&qemu_sigp_mutex)) {
        ret = SIGP_CC_BUSY;
        goto out;
    }

    switch (order) {
    case SIGP_SET_ARCH:
        ret = sigp_set_architecture(cpu, param, status_reg);
        break;
    default:
        /* all other sigp orders target a single vcpu */
        dst_cpu = s390_cpu_addr2state(env->regs[r3]);
        ret = handle_sigp_single_dst(cpu, dst_cpu, order, param, status_reg);
    }
    qemu_mutex_unlock(&qemu_sigp_mutex);

out:
    trace_sigp_finished(order, CPU(cpu)->cpu_index,
                        dst_cpu ? CPU(dst_cpu)->cpu_index : -1, ret);
    g_assert(ret >= 0);

    return ret;
}

int s390_cpu_restart(S390CPU *cpu)
{
    SigpInfo si = {};

    run_on_cpu(CPU(cpu), sigp_restart, RUN_ON_CPU_HOST_PTR(&si));
    return 0;
}

void do_stop_interrupt(CPUS390XState *env)
{
    S390CPU *cpu = env_archcpu(env);

    /*
     * Complete the STOP operation before exposing the CPU as
     * STOPPED to the system.
     */
    if (cpu->env.sigp_order == SIGP_STOP_STORE_STATUS) {
        s390_store_status(cpu, S390_STORE_STATUS_DEF_ADDR, true);
    }
    env->sigp_order = 0;
    s390_cpu_set_state(S390_CPU_STATE_STOPPED, cpu);
    if (s390_count_running_cpus() == 0) {
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
    }
    env->pending_int &= ~INTERRUPT_STOP;
}

void s390_init_sigp(void)
{
    qemu_mutex_init(&qemu_sigp_mutex);
}
