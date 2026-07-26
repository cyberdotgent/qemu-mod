/*
 *  S/390 helpers - system only
 *
 *  Copyright (c) 2009 Ulrich Hecht
 *  Copyright (c) 2011 Alexander Graf
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
#include "cpu.h"
#include "s390x-internal.h"
#include "qemu/timer.h"
#include "hw/s390x/ioinst.h"
#include "system/hw_accel.h"
#include "system/memory.h"
#include "system/runstate.h"
#include "exec/target_page.h"
#include "exec/watchpoint.h"

void s390x_tod_timer(void *opaque)
{
    cpu_inject_clock_comparator((S390CPU *) opaque);
}

void s390x_cpu_timer(void *opaque)
{
    cpu_inject_cpu_timer((S390CPU *) opaque);
}

hwaddr s390_cpu_get_phys_addr_debug(CPUState *cs, vaddr addr)
{
    S390CPU *cpu = S390_CPU(cs);
    CPUS390XState *env = &cpu->env;
    hwaddr raddr;
    int prot;
    uint64_t asc = env->psw.mask & PSW_MASK_ASC;
    uint64_t tec;
    vaddr page = addr & TARGET_PAGE_MASK;

    /* 31-Bit mode */
    if (!(env->psw.mask & PSW_MASK_64)) {
        page &= 0x7fffffff;
    }

    /* We want to read the code (e.g., see what we are single-stepping).*/
    if (asc != PSW_ASC_HOME) {
        asc = PSW_ASC_PRIMARY;
    }

    /*
     * We want to read code even if IEP is active. Use MMU_DATA_LOAD instead
     * of MMU_INST_FETCH.
     */
    if (mmu_translate(env, page, MMU_DATA_LOAD, asc, &raddr, &prot, &tec,
                      NULL)) {
        return -1;
    }
    raddr += (addr & ~TARGET_PAGE_MASK);
    return raddr;
}

static inline bool is_special_wait_psw(uint64_t psw_addr)
{
    /* signal quiesce */
    return (psw_addr & 0xfffUL) == 0xfffUL;
}

void s390_handle_wait(S390CPU *cpu)
{
    CPUState *cs = CPU(cpu);

    s390_cpu_halt(cpu);

    if (s390_count_running_cpus() == 0) {
        if (is_special_wait_psw(cpu->env.psw.addr)) {
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
        } else {
            cpu->env.crash_reason = S390_CRASH_REASON_DISABLED_WAIT;
            qemu_system_guest_panicked(cpu_get_crash_info(cs));
        }
    }
}

LowCore *cpu_map_lowcore(CPUS390XState *env)
{
    LowCore *lowcore;
    hwaddr len = sizeof(LowCore);
    CPUState *cs = env_cpu(env);
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    lowcore = address_space_map(cs->as, env->psa, &len, true, attrs);

    if (len < sizeof(LowCore)) {
        cpu_abort(cs, "Could not map lowcore\n");
    }

    return lowcore;
}

void cpu_unmap_lowcore(CPUS390XState *env, LowCore *lowcore)
{
    AddressSpace *as = env_cpu(env)->as;

    address_space_unmap(as, lowcore, sizeof(LowCore), true, sizeof(LowCore));
}

void s390_lowcore_store_psw(CPUS390XState *env, LowCore *lowcore,
                            size_t z_offset, size_t esa_offset,
                            uint64_t mask, uint64_t addr)
{
    uint8_t *p = (uint8_t *)lowcore;

    if (env->esa_mode) {
        uint64_t short_psw = ((mask ^ PSW_MASK_SHORTPSW) &
                              PSW_MASK_SHORT_CTRL) |
                             (addr & PSW_MASK_SHORT_ADDR);

        stq_be_p(p + esa_offset, short_psw);
    } else {
        stq_be_p(p + z_offset, mask);
        stq_be_p(p + z_offset + 8, addr);
    }
}

void s390_lowcore_load_psw(CPUS390XState *env, const LowCore *lowcore,
                           size_t z_offset, size_t esa_offset,
                           uint64_t *mask, uint64_t *addr)
{
    const uint8_t *p = (const uint8_t *)lowcore;

    if (env->esa_mode) {
        uint64_t short_psw = ldq_be_p(p + esa_offset);

        *mask = (short_psw & PSW_MASK_SHORT_CTRL) ^ PSW_MASK_SHORTPSW;
        *addr = short_psw & PSW_MASK_SHORT_ADDR;
    } else {
        *mask = ldq_be_p(p + z_offset);
        *addr = ldq_be_p(p + z_offset + 8);
    }
}

void do_restart_interrupt(CPUS390XState *env)
{
    uint64_t mask, addr;
    LowCore *lowcore;

    lowcore = cpu_map_lowcore(env);

    s390_lowcore_store_psw(env, lowcore,
                           offsetof(LowCore, restart_old_psw), 0x008,
                           s390_cpu_get_psw_mask(env), env->psw.addr);
    s390_lowcore_load_psw(env, lowcore,
                          offsetof(LowCore, restart_new_psw), 0x000,
                          &mask, &addr);

    cpu_unmap_lowcore(env, lowcore);
    env->pending_int &= ~INTERRUPT_RESTART;

    s390_cpu_set_psw(env, mask, addr);
}
