/*
 * QEMU KVM support -- s390x specific functions.
 *
 * Copyright (c) 2009 Ulrich Hecht
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef KVM_S390X_H
#define KVM_S390X_H

#include "target/s390x/cpu-qom.h"

struct kvm_s390_irq;

void kvm_s390_floating_interrupt_legacy(struct kvm_s390_irq *irq);
void kvm_s390_vcpu_interrupt(S390CPU *cpu, struct kvm_s390_irq *irq);
void kvm_s390_access_exception(S390CPU *cpu, uint16_t code, uint64_t te_code);
int kvm_s390_mem_op(S390CPU *cpu, vaddr addr, uint8_t ar, void *hostbuf,
                    int len, bool is_write);
int kvm_s390_mem_op_pv(S390CPU *cpu, vaddr addr, void *hostbuf, int len,
                       bool is_write);
void kvm_s390_program_interrupt(S390CPU *cpu, uint16_t code);
int kvm_s390_set_cpu_state(S390CPU *cpu, S390CpuState cpu_state);
void kvm_s390_vcpu_interrupt_pre_save(S390CPU *cpu);
int kvm_s390_vcpu_interrupt_post_load(S390CPU *cpu);
int kvm_s390_get_hpage(void);
int kvm_s390_get_protected_dump(void);

/* ESA/390-compatibility mode (esa390.c) */
struct kvm_guest_debug;
int kvm_s390_esa390_init(MachineState *ms, struct KVMState *s);
bool kvm_s390_esa390_cm_enabled(void);
void kvm_s390_esa390_fac_list(uint8_t *fac_list);
int kvm_s390_set_arch_mode(bool esa);
int kvm_s390_get_arch_mode(void);
void kvm_s390_esa390_put_arch_regs(CPUState *cs);
void kvm_s390_esa390_note_exit(void);
void kvm_s390_esa390_run_failed(CPUState *cs, int run_ret);
void kvm_s390_esa390_guest_debug(struct kvm_guest_debug *dbg);

int kvm_s390_get_ri(void);
int kvm_s390_get_zpci_op(void);
int kvm_s390_get_clock(uint8_t *tod_high, uint64_t *tod_clock);
int kvm_s390_get_clock_ext(uint8_t *tod_high, uint64_t *tod_clock);
int kvm_s390_set_clock(uint8_t tod_high, uint64_t tod_clock);
int kvm_s390_set_clock_ext(uint8_t tod_high, uint64_t tod_clock);
void kvm_s390_enable_css_support(S390CPU *cpu);
int kvm_s390_assign_subch_ioeventfd(EventNotifier *notifier, uint32_t sch,
                                    int vq, bool assign);
int kvm_s390_cmma_active(void);
void kvm_s390_cmma_reset(void);
void kvm_s390_reset_vcpu_clear(S390CPU *cpu);
void kvm_s390_reset_vcpu_normal(S390CPU *cpu);
void kvm_s390_reset_vcpu_initial(S390CPU *cpu);
int kvm_s390_set_mem_limit(uint64_t new_limit, uint64_t *hw_limit);
void kvm_s390_set_max_pagesize(uint64_t pagesize, Error **errp);
void kvm_s390_crypto_reset(void);
void kvm_s390_restart_interrupt(S390CPU *cpu);
bool kvm_s390_sclp_event_signal(bool after_completion);
void kvm_s390_stop_interrupt(S390CPU *cpu);
void kvm_s390_set_diag318(CPUState *cs, uint64_t diag318_info);
int kvm_s390_topology_set_mtcr(uint64_t attr);

/*
 * Interruptions and instructions emulated by devices (for instance a
 * hypervisor service such as IUCV).
 */
struct kvm_s390_ext_user_info;
typedef struct S390UserExtIrqOps {
    /* Without the BQL: whether an interruption may be waiting for @cpu. */
    bool (*pending)(S390CPU *cpu);
    /*
     * With the BQL, when KVM can present an interruption to @cpu before it
     * executes another instruction: choose one, perform what belongs to
     * its presentation (for instance storing data into guest memory) and
     * describe it in @info. Return false if there is none for @cpu.
     */
    bool (*present)(S390CPU *cpu, struct kvm_s390_ext_user_info *info);
} S390UserExtIrqOps;
bool kvm_s390_user_ext_irq_init(const S390UserExtIrqOps *ops,
                                uint64_t cr0_subclass, Error **errp);
void kvm_s390_user_ext_irq_kick(S390CPU *cpu);

/*
 * Handler for an instruction that the machine does not have: called with
 * the BQL and the registers synchronized; the PSW addresses the next
 * instruction. Returns 0 or an EXCP_ code as the intercept handlers do.
 */
typedef int (*S390UserOpExcHandler)(S390CPU *cpu, uint16_t ipa, uint32_t ipb);
bool kvm_s390_user_operexec_init(uint16_t ipa, S390UserOpExcHandler fn,
                                 Error **errp);

/* itrace.c: KVM single-step instruction tracer */
struct kvm_run;
void s390_kvm_itrace_arm(void);
void s390_kvm_itrace_pre_run(CPUState *cs);
bool s390_kvm_itrace_exit(CPUState *cs, struct kvm_run *run);

#endif /* KVM_S390X_H */
