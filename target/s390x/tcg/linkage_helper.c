/*
 * z/Architecture linkage-stack helpers
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "s390x-internal.h"
#include "tcg_s390x.h"
#include "exec/cpu-common.h"
#include "exec/helper-proto.h"
#include "exec/target_page.h"
#include "exec/cputlb.h"
#include "system/memory.h"
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/helper-retaddr.h"

#ifndef CONFIG_USER_ONLY

#define LINKAGE_DESC_SIZE 8
#define LINKAGE_HEADER_SIZE 16
#define LINKAGE_ENTRY_HEADER 0x09
#define LINKAGE_ENTRY_TRAILER 0x0a
#define LINKAGE_SECTION_VALID 0x01
#define LINKAGE_STATE_ENTRY_SIZE 296
#define LINKAGE_ACCESS_MAX_PAGES 2

#define ASTE_INVALID 0x80000000U
#define ASTE_ATO 0x7ffffffcU
#define ASTE_AX 0xffff0000U
#define ASTE_ATL 0x0000fff0U
#define ASTE_ORIGIN 0x7fffffc0U

typedef enum S390LinkageEntryType {
    S390_LINKAGE_ENTRY_BRANCH = 0x0c,
    S390_LINKAGE_ENTRY_PC = 0x0d,
} S390LinkageEntryType;

typedef struct S390LinkageState {
    uint64_t psw_mask;
    uint64_t psw_addr;
    uint64_t call_addr;
    uint32_t called_space_id;
    uint32_t pc_number;
} S390LinkageState;

typedef struct S390LinkageAccess {
    uint64_t vaddr;
    uint16_t len;
    uint8_t nr_pages;
    bool is_write;
    hwaddr pages[LINKAGE_ACCESS_MAX_PAGES];
} S390LinkageAccess;

typedef struct S390LinkageWrite {
    S390LinkageAccess access;
    const void *data;
} S390LinkageWrite;

typedef struct S390ASTE {
    uint32_t words[16];
    uint32_t origin;
} S390ASTE;

static void linkage_set_tea(CPUS390XState *env, uint64_t tea);
static void linkage_set_exception_access_id(CPUS390XState *env, uint8_t id);
static int asn_translate(CPUS390XState *env, uint16_t asn, S390ASTE *aste);
static uint64_t aste_asce(const S390ASTE *aste);
static int subspace_replace(CPUS390XState *env, uint64_t asce,
                            uint32_t base_aste_origin, uint64_t *result);
static int aste_authorize_secondary(CPUS390XState *env,
                                    const S390ASTE *aste, uint16_t ax,
                                    uint16_t asn);

static int linkage_trace_store(CPUS390XState *env, const void *data,
                               uint16_t len)
{
    uint64_t raddr = env->cregs[12] & CR12_TRACE_ENTRY_MASK;
    uint64_t tec;
    hwaddr abs;
    int flags;
    int exc;

    exc = mmu_translate_real(env, raddr, MMU_DATA_STORE,
                             &abs, &flags, &tec);
    if (exc) {
        env->tlb_fill_exc = exc;
        env->tlb_fill_tec = tec;
        return exc;
    }
    if ((raddr & TARGET_PAGE_MASK) !=
        ((raddr + len - 1) & TARGET_PAGE_MASK)) {
        return PGM_TRACE_TABLE;
    }
    if (address_space_write(env_cpu(env)->as,
                            abs | (raddr & ~TARGET_PAGE_MASK),
                            MEMTXATTRS_UNSPECIFIED, data, len) != MEMTX_OK) {
        return PGM_ADDRESSING;
    }
    env->cregs[12] = (env->cregs[12] & ~CR12_TRACE_ENTRY_MASK) |
                     ((raddr + len) & CR12_TRACE_ENTRY_MASK);
    return 0;
}

static int linkage_trace_pc(CPUS390XState *env, uint32_t pc_number,
                            uint64_t return_addr)
{
    uint8_t entry[12] = { 0 };
    uint8_t key = extract64(env->psw.mask, PSW_SHIFT_KEY, 4) << 4;

    if (!(env->cregs[12] & CR12_ASN_TRACE)) {
        return 0;
    }
    if (env->psw.mask & PSW_MASK_64) {
        entry[0] = 0x22;
        entry[1] = key | ((pc_number >> 16) & 0xf);
        stw_be_p(entry + 2, pc_number);
        stq_be_p(entry + 4, return_addr |
                 !!(env->psw.mask & PSW_MASK_PSTATE));
        return linkage_trace_store(env, entry, 12);
    }

    entry[0] = 0x21;
    entry[1] = key | ((pc_number >> 16) & 0xf);
    stw_be_p(entry + 2, pc_number);
    stl_be_p(entry + 4,
             (env->psw.mask & PSW_MASK_32 ? 0x80000000U : 0) |
             (return_addr & 0x7ffffffeU) |
             !!(env->psw.mask & PSW_MASK_PSTATE));
    return linkage_trace_store(env, entry, 8);
}

static int linkage_trace_branch(CPUS390XState *env, uint64_t target)
{
    uint8_t entry[12] = { 0 };

    if (!(env->cregs[12] & CR12_BRANCH_TRACE)) {
        return 0;
    }
    if ((env->psw.mask & PSW_MASK_64) && target > UINT32_MAX) {
        entry[0] = 0x52;
        entry[1] = 0xc0;
        stq_be_p(entry + 4, target);
        return linkage_trace_store(env, entry, 12);
    }
    stl_be_p(entry, (env->psw.mask & PSW_MASK_32 ? 0x80000000U : 0) |
                      (target & 0x7fffffffU));
    return linkage_trace_store(env, entry, 4);
}

static int linkage_trace_pr(CPUS390XState *env, uint64_t new_mask,
                            uint64_t new_addr, uint16_t new_pasn)
{
    uint8_t entry[20] = { 0 };
    uint8_t key = extract64(env->psw.mask, PSW_SHIFT_KEY, 4) << 4;
    uint64_t old_addr = env->gbea;
    bool old64 = env->psw.mask & PSW_MASK_64;
    bool new64 = new_mask & PSW_MASK_64;
    int len;

    if (!(env->cregs[12] & CR12_ASN_TRACE)) {
        return 0;
    }

    if (!old64 && !new64) {
        entry[0] = 0x32;
        entry[1] = key;
        len = 12;
    } else if (old64 && old_addr <= UINT32_MAX && !new64) {
        entry[0] = 0x32;
        entry[1] = key | 0x02;
        len = 12;
    } else if (old64 && !new64) {
        entry[0] = 0x33;
        entry[1] = key | 0x03;
        len = 16;
    } else if (!old64 && new64 && new_addr <= UINT32_MAX) {
        entry[0] = 0x32;
        entry[1] = key | 0x08;
        len = 12;
    } else if (old64 && old_addr <= UINT32_MAX &&
               new64 && new_addr <= UINT32_MAX) {
        entry[0] = 0x32;
        entry[1] = key | 0x0a;
        len = 12;
    } else if (old64 && new64 && new_addr <= UINT32_MAX) {
        entry[0] = 0x33;
        entry[1] = key | 0x0b;
        len = 16;
    } else if (!old64 && new64) {
        entry[0] = 0x33;
        entry[1] = key | 0x0c;
        len = 16;
    } else if (old64 && old_addr <= UINT32_MAX && new64) {
        entry[0] = 0x33;
        entry[1] = key | 0x0e;
        len = 16;
    } else {
        entry[0] = 0x34;
        entry[1] = key | 0x0f;
        len = 20;
    }
    stw_be_p(entry + 2, new_pasn);

    if (new64 && new_addr > UINT32_MAX) {
        stq_be_p(entry + 4, new_addr |
                 !!(new_mask & PSW_MASK_PSTATE));
        if (old64 && old_addr > UINT32_MAX) {
            stq_be_p(entry + 12, old_addr);
        } else {
            stl_be_p(entry + 12, old_addr);
        }
    } else {
        stl_be_p(entry + 4,
                 (!new64 && (new_mask & PSW_MASK_32) ? 0x80000000U : 0) |
                 (new_addr & 0x7ffffffeU) |
                 !!(new_mask & PSW_MASK_PSTATE));
        if (old64 && old_addr > UINT32_MAX) {
            stq_be_p(entry + 8, old_addr);
        } else {
            stl_be_p(entry + 8,
                     (!old64 && (env->psw.mask & PSW_MASK_32) ?
                      0x80000000U : 0) |
                     (old_addr & 0x7fffffffU));
        }
    }
    return linkage_trace_store(env, entry, len);
}

static int linkage_trace_mode(CPUS390XState *env, uint64_t next_addr)
{
    uint8_t entry[12] = { 0 };

    if (!(env->cregs[12] & CR12_MODE_TRACE)) {
        return 0;
    }
    if (!(env->psw.mask & PSW_MASK_64)) {
        entry[0] = 0x51;
        entry[1] = 0x30;
        stl_be_p(entry + 4,
                 (env->psw.mask & PSW_MASK_32 ? 0x80000000U : 0) |
                 (next_addr & 0x7fffffffU));
        return linkage_trace_store(env, entry, 8);
    }
    if (next_addr <= 0x7fffffffU) {
        entry[0] = 0x51;
        entry[1] = 0x20;
        stl_be_p(entry + 4, next_addr);
        return linkage_trace_store(env, entry, 8);
    }
    entry[0] = 0x52;
    entry[1] = 0x60;
    stq_be_p(entry + 4, next_addr);
    return linkage_trace_store(env, entry, 12);
}

static G_NORETURN void linkage_completed_exception(CPUS390XState *env,
                                                    uint32_t code,
                                                    uint64_t target,
                                                    uint16_t ilen)
{
    env->psw.addr = target;
    env->int_pgm_ilen = ilen;
    trigger_pgm_exception(env, code);
    cpu_loop_exit(env_cpu(env));
}

static bool linkage_record_per_branch(CPUS390XState *env, uint64_t target)
{
    if (!(env->psw.mask & PSW_MASK_PER) ||
        !(env->cregs[9] & PER_CR9_EVENT_BRANCH)) {
        return false;
    }
    if ((env->cregs[9] & PER_CR9_CONTROL_BRANCH_ADDRESS) &&
        (env->cregs[10] <= env->cregs[11] ?
         target < env->cregs[10] || target > env->cregs[11] :
         target < env->cregs[10] && target > env->cregs[11])) {
        return false;
    }

    env->per_address = env->gbea;
    env->per_perc_atmid = PER_CODE_EVENT_BRANCH | get_per_atmid(env);
    return true;
}

/*
 * Linkage-stack addresses are always 64-bit home virtual addresses.  They are
 * not wrapped according to the current PSW addressing mode.  Key-controlled
 * protection does not apply, while DAT page protection and low-address
 * protection do apply.
 *
 * Keep this separate from normal TCG loads: s390_cpu_tlb_fill() masks virtual
 * addresses in 24-bit and 31-bit modes before calling mmu_translate().
 */
static int linkage_access_prepare(CPUS390XState *env,
                                  S390LinkageAccess *access,
                                  uint64_t vaddr, uint16_t len,
                                  bool is_write)
{
    uint64_t addr = vaddr;
    uint32_t remaining = len;
    int rw = is_write ? MMU_DATA_STORE : MMU_DATA_LOAD;
    int nr_pages = 0;

    g_assert(len && len <= TARGET_PAGE_SIZE);

    memset(access, 0, sizeof(*access));
    access->vaddr = vaddr;
    access->len = len;
    access->is_write = is_write;

    while (remaining) {
        uint64_t tec;
        hwaddr raddr;
        int flags;
        int exc;
        uint32_t chunk;

        g_assert(nr_pages < LINKAGE_ACCESS_MAX_PAGES);
        exc = mmu_translate(env, addr, rw, PSW_ASC_HOME,
                            &raddr, &flags, &tec);
        if (exc) {
            env->tlb_fill_exc = exc;
            env->tlb_fill_tec = tec;
            return exc;
        }

        access->pages[nr_pages++] = raddr;
        chunk = MIN(remaining,
                    TARGET_PAGE_SIZE - (uint32_t)(addr & ~TARGET_PAGE_MASK));
        remaining -= chunk;
        addr += chunk;
    }

    access->nr_pages = nr_pages;
    return 0;
}

/* Execute an access which has already been translated and checked. */
static int linkage_access_rw(CPUS390XState *env,
                             const S390LinkageAccess *access,
                             void *hostbuf)
{
    AddressSpace *as = env_cpu(env)->as;
    uint64_t addr = access->vaddr;
    uint32_t remaining = access->len;
    uint8_t *buf = hostbuf;

    for (int i = 0; i < access->nr_pages; i++) {
        uint32_t chunk = MIN(remaining,
                            TARGET_PAGE_SIZE -
                            (uint32_t)(addr & ~TARGET_PAGE_MASK));
        MemTxResult result;

        result = address_space_rw(as,
                                  access->pages[i] |
                                  (addr & ~TARGET_PAGE_MASK),
                                  MEMTXATTRS_UNSPECIFIED, buf, chunk,
                                  access->is_write);
        if (result != MEMTX_OK) {
            return PGM_ADDRESSING;
        }
        remaining -= chunk;
        addr += chunk;
        buf += chunk;
    }

    return 0;
}

static int linkage_read(CPUS390XState *env, uint64_t addr,
                        void *buf, uint16_t len)
{
    S390LinkageAccess access;
    int exc;

    exc = linkage_access_prepare(env, &access, addr, len, false);
    if (exc) {
        return exc;
    }
    return linkage_access_rw(env, &access, buf);
}

static int linkage_prepare_write(CPUS390XState *env,
                                 S390LinkageWrite *write,
                                 uint64_t addr, const void *data,
                                 uint16_t len)
{
    int exc = linkage_access_prepare(env, &write->access, addr, len, true);

    write->data = data;
    return exc;
}

static int linkage_commit_writes(CPUS390XState *env,
                                 const S390LinkageWrite *writes,
                                 int nr_writes)
{
    for (int i = 0; i < nr_writes; i++) {
        int exc = linkage_access_rw(env, &writes[i].access,
                                    (void *)writes[i].data);
        if (exc) {
            env->tlb_fill_exc = exc;
            env->tlb_fill_tec = 0;
            return exc;
        }
    }
    return 0;
}

static uint16_t linkage_desc_rfs(const uint8_t desc[LINKAGE_DESC_SIZE])
{
    return lduw_be_p(desc + 2);
}

static void linkage_desc_set_nes(uint8_t desc[LINKAGE_DESC_SIZE],
                                 uint16_t nes)
{
    stw_be_p(desc + 4, nes);
}

static void linkage_build_state_entry(CPUS390XState *env,
                                      S390LinkageEntryType type,
                                      const S390LinkageState *state,
                                      uint8_t section_id, uint16_t rfs,
                                      uint8_t entry[LINKAGE_STATE_ENTRY_SIZE])
{
    memset(entry, 0, LINKAGE_STATE_ENTRY_SIZE);

    for (int i = 0; i < 16; i++) {
        stq_be_p(entry + i * 8, env->regs[i]);
        stl_be_p(entry + 224 + i * 4, env->aregs[i]);
    }

    stl_be_p(entry + 128, env->cregs[3]);
    stw_be_p(entry + 132, env->cregs[8] >> 16);
    stw_be_p(entry + 134, env->cregs[4]);
    stq_be_p(entry + 136, state->psw_mask);

    if (type == S390_LINKAGE_ENTRY_PC) {
        stl_be_p(entry + 144, state->called_space_id);
        stl_be_p(entry + 148, state->pc_number);
    } else {
        stq_be_p(entry + 144, state->call_addr);
    }

    stq_be_p(entry + 168, state->psw_addr);
    if (s390_has_feat(S390_FEAT_ASN_LX_REUSE) &&
        (env->cregs[0] & CR0_ASN_LX_REUSE)) {
        stl_be_p(entry + 176, env->cregs[3] >> 32);
        stl_be_p(entry + 180, env->cregs[4] >> 32);
    }

    entry[288] = type;
    entry[289] = section_id;
    stw_be_p(entry + 290, rfs - LINKAGE_STATE_ENTRY_SIZE);
}

/*
 * Form an architected z/Architecture linkage-stack state entry.  The caller
 * supplies the instruction-specific saved PSW and call information.
 *
 * All fetches and store preflights are ordered according to the stacking
 * process.  No guest-visible write or CR15 update occurs until every access
 * which can fault has been validated.
 */
static int linkage_stack_push(CPUS390XState *env,
                              S390LinkageEntryType type,
                              const S390LinkageState *state)
{
    uint8_t current_desc[LINKAGE_DESC_SIZE];
    uint8_t new_entry[LINKAGE_STATE_ENTRY_SIZE];
    uint8_t backward[LINKAGE_DESC_SIZE];
    S390LinkageWrite writes[3];
    uint64_t original_desc_addr = env->cregs[15] & ~0x7ULL;
    uint64_t current_desc_addr = original_desc_addr;
    uint64_t new_entry_addr;
    uint64_t new_desc_addr;
    uint64_t trailer;
    uint16_t rfs;
    int nr_writes = 0;
    int exc;

    g_assert(type == S390_LINKAGE_ENTRY_BRANCH ||
             type == S390_LINKAGE_ENTRY_PC);

    exc = linkage_read(env, current_desc_addr, current_desc,
                       sizeof(current_desc));
    if (exc) {
        return exc;
    }
    rfs = linkage_desc_rfs(current_desc);

    if (rfs < LINKAGE_STATE_ENTRY_SIZE) {
        uint64_t next_desc_addr;

        if (rfs & 7) {
            return PGM_STACK_SPEC;
        }

        exc = linkage_read(env, current_desc_addr + LINKAGE_DESC_SIZE + rfs,
                           &trailer, sizeof(trailer));
        if (exc) {
            return exc;
        }
        trailer = be64_to_cpu(trailer);
        if (!(trailer & LINKAGE_SECTION_VALID)) {
            return PGM_STACK_FULL;
        }

        next_desc_addr = trailer & ~0x7ULL;
        exc = linkage_read(env, next_desc_addr, current_desc,
                           sizeof(current_desc));
        if (exc) {
            return exc;
        }
        rfs = linkage_desc_rfs(current_desc);
        if (rfs < LINKAGE_STATE_ENTRY_SIZE) {
            return PGM_STACK_SPEC;
        }

        stq_be_p(backward, original_desc_addr | LINKAGE_SECTION_VALID);
        exc = linkage_prepare_write(env, &writes[nr_writes],
                                    next_desc_addr - LINKAGE_DESC_SIZE,
                                    backward, sizeof(backward));
        if (exc) {
            return exc;
        }
        nr_writes++;
        current_desc_addr = next_desc_addr;
    }

    new_entry_addr = current_desc_addr + LINKAGE_DESC_SIZE;
    new_desc_addr = new_entry_addr + LINKAGE_STATE_ENTRY_SIZE -
                    LINKAGE_DESC_SIZE;
    linkage_build_state_entry(env, type, state, current_desc[1], rfs,
                              new_entry);
    linkage_desc_set_nes(current_desc, LINKAGE_STATE_ENTRY_SIZE);

    exc = linkage_prepare_write(env, &writes[nr_writes], new_entry_addr,
                                new_entry, sizeof(new_entry));
    if (exc) {
        return exc;
    }
    nr_writes++;
    exc = linkage_prepare_write(env, &writes[nr_writes], current_desc_addr,
                                current_desc, sizeof(current_desc));
    if (exc) {
        return exc;
    }
    nr_writes++;

    exc = linkage_commit_writes(env, writes, nr_writes);
    if (exc) {
        return exc;
    }

    env->cregs[15] = new_desc_addr & ~0x7ULL;
    return 0;
}

uint64_t HELPER(bakr)(CPUS390XState *env, uint32_t r1, uint32_t r2,
                      uint64_t next_pc)
{
    S390LinkageState state = {
        .psw_mask = s390_cpu_get_psw_mask(env),
        .psw_addr = next_pc,
    };
    uint64_t branch_addr = r2 ? wrap_address(env, env->regs[r2]) : next_pc;
    uint64_t old_cr12 = env->cregs[12];
    uint64_t new_cr12 = 0;
    int exc;

    if (!(env->psw.mask & PSW_MASK_DAT) ||
        ((env->psw.mask & PSW_MASK_ASC) != PSW_ASC_PRIMARY &&
         (env->psw.mask & PSW_MASK_ASC) != PSW_ASC_ACCREG)) {
        tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
    }

    if (r1) {
        uint64_t return_spec = env->regs[r1];

        if (return_spec & 1) {
            state.psw_mask |= PSW_MASK_64 | PSW_MASK_32;
            state.psw_addr = return_spec & ~1ULL;
        } else if (return_spec & 0x80000000ULL) {
            state.psw_mask &= ~PSW_MASK_64;
            state.psw_mask |= PSW_MASK_32;
            state.psw_addr = return_spec & 0x7fffffffULL;
        } else {
            state.psw_mask &= ~(PSW_MASK_64 | PSW_MASK_32);
            state.psw_addr = return_spec & 0xffffffULL;
        }
    }

    if (env->psw.mask & PSW_MASK_64) {
        state.call_addr = (branch_addr & ~1ULL) | 1;
    } else {
        state.call_addr = branch_addr;
        if (env->psw.mask & PSW_MASK_32) {
            state.call_addr |= 0x80000000ULL;
        }
    }

    if (r2) {
        exc = linkage_trace_branch(env, branch_addr);
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
        new_cr12 = env->cregs[12];
        env->cregs[12] = old_cr12;
    }

    exc = linkage_stack_push(env, S390_LINKAGE_ENTRY_BRANCH, &state);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    if (new_cr12) {
        env->cregs[12] = new_cr12;
    }
    return branch_addr;
}

static int linkage_locate_state(CPUS390XState *env, bool program_return,
                                uint64_t *state_desc_addr, uint8_t *state_type)
{
    uint8_t desc[LINKAGE_DESC_SIZE];
    uint64_t desc_addr = env->cregs[15] & ~0x7ULL;
    uint8_t type;
    int exc;

    exc = linkage_read(env, desc_addr, desc, sizeof(desc));
    if (exc) {
        return exc;
    }
    type = desc[0] & 0x7f;

    if (type == LINKAGE_ENTRY_HEADER) {
        uint64_t backward;

        if (program_return && (desc[0] & 0x80)) {
            return PGM_STACK_OP;
        }
        exc = linkage_read(env, desc_addr - LINKAGE_DESC_SIZE,
                           &backward, sizeof(backward));
        if (exc) {
            return exc;
        }
        backward = be64_to_cpu(backward);
        if (!(backward & LINKAGE_SECTION_VALID)) {
            return PGM_STACK_EMPTY;
        }
        desc_addr = backward & ~0x7ULL;
        exc = linkage_read(env, desc_addr, desc, sizeof(desc));
        if (exc) {
            return exc;
        }
        type = desc[0] & 0x7f;
        if (type == LINKAGE_ENTRY_HEADER) {
            return PGM_STACK_SPEC;
        }
    }

    if (type != S390_LINKAGE_ENTRY_BRANCH &&
        type != S390_LINKAGE_ENTRY_PC) {
        return PGM_STACK_TYPE;
    }
    if (program_return && (desc[0] & 0x80)) {
        return PGM_STACK_OP;
    }

    *state_desc_addr = desc_addr;
    *state_type = type;
    return 0;
}

static void linkage_check_unstack_mode(CPUS390XState *env, bool allow_home)
{
    uint64_t asc = env->psw.mask & PSW_MASK_ASC;

    if (!(env->psw.mask & PSW_MASK_DAT) || asc == PSW_ASC_SECONDARY ||
        (!allow_home && asc == PSW_ASC_HOME)) {
        tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
    }
}

void HELPER(pr)(CPUS390XState *env)
{
    uint8_t state[LINKAGE_STATE_ENTRY_SIZE];
    static const uint16_t zero;
    S390LinkageWrite write;
    uint64_t state_desc_addr;
    uint64_t preceding_desc_addr;
    uint64_t new_mask;
    uint64_t new_addr;
    S390ASTE paste;
    S390ASTE saste;
    uint64_t new_pasce = env->cregs[1];
    uint64_t new_sasce = env->cregs[7];
    uint32_t new_pasteo = env->cregs[5];
    uint16_t new_ax = env->cregs[4] >> 16;
    bool translated_primary = false;
    bool translated_secondary = false;
    uint64_t old_pasce = env->cregs[1];
    uint64_t old_cr12 = env->cregs[12];
    uint64_t new_cr12 = old_cr12;
    uint16_t old_pasn = env->cregs[4];
    bool space_switch = false;
    bool reuse = s390_has_feat(S390_FEAT_ASN_LX_REUSE) &&
                 (env->cregs[0] & CR0_ASN_LX_REUSE);
    uint8_t type;
    int exc;

    linkage_check_unstack_mode(env, false);

    exc = linkage_locate_state(env, true, &state_desc_addr, &type);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    exc = linkage_read(env,
                       state_desc_addr -
                       (LINKAGE_STATE_ENTRY_SIZE - LINKAGE_DESC_SIZE),
                       state, LINKAGE_STATE_ENTRY_SIZE);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    preceding_desc_addr = state_desc_addr - LINKAGE_STATE_ENTRY_SIZE;

    /*
     * Preflight the only unstacking store before changing CPU state.  A
     * two-byte store avoids an extra descriptor fetch not specified by PoP.
     */
    exc = linkage_prepare_write(env, &write, preceding_desc_addr + 4,
                                &zero, sizeof(zero));
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }

    new_mask = ldq_be_p(state + 136);
    new_mask = (new_mask & ~PSW_MASK_PER) | (env->psw.mask & PSW_MASK_PER);
    new_addr = ldq_be_p(state + 168);

    if (type == S390_LINKAGE_ENTRY_PC &&
        (env->cregs[12] & CR12_ASN_TRACE)) {
        exc = linkage_trace_pr(env, new_mask, new_addr,
                               lduw_be_p(state + 134));
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
        new_cr12 = env->cregs[12];
        env->cregs[12] = old_cr12;
    } else if ((env->cregs[12] & CR12_MODE_TRACE) &&
               !!(env->psw.mask & PSW_MASK_64) !=
               !!(new_mask & PSW_MASK_64)) {
        exc = linkage_trace_mode(env, env->gbea);
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
        new_cr12 = env->cregs[12];
        env->cregs[12] = old_cr12;
    }

    if (type == S390_LINKAGE_ENTRY_PC) {
        uint16_t new_sasn = lduw_be_p(state + 130);
        uint16_t new_pasn = lduw_be_p(state + 134);
        space_switch = new_pasn != old_pasn;

        if ((new_pasn != old_pasn || new_sasn != new_pasn) &&
            !(env->cregs[14] & CR14_ASN_TRANSLATION)) {
            tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
        }

        if (new_pasn != old_pasn) {
            exc = asn_translate(env, new_pasn, &paste);
            if (exc) {
                tcg_s390_program_interrupt(env, exc, GETPC());
            }
            if (reuse && ldl_be_p(state + 180) != paste.words[11]) {
                linkage_set_tea(env, new_pasn);
                linkage_set_exception_access_id(env, 0x20);
                tcg_s390_program_interrupt(env, PGM_ASTE_INSTANCE, GETPC());
            }
            new_pasce = aste_asce(&paste);
            exc = subspace_replace(env, new_pasce, paste.origin, &new_pasce);
            if (exc) {
                tcg_s390_program_interrupt(env, exc, GETPC());
            }
            new_pasteo = paste.origin;
            new_ax = paste.words[1] >> 16;
            translated_primary = true;
        }

        if (new_sasn == new_pasn) {
            new_sasce = new_pasce;
        } else {
            exc = asn_translate(env, new_sasn, &saste);
            if (exc) {
                tcg_s390_program_interrupt(env, exc, GETPC());
            }
            if (reuse && ldl_be_p(state + 176) != saste.words[11]) {
                linkage_set_tea(env, new_sasn);
                linkage_set_exception_access_id(env, 0x10);
                tcg_s390_program_interrupt(env, PGM_ASTE_INSTANCE, GETPC());
            }
            exc = aste_authorize_secondary(env, &saste, new_ax, new_sasn);
            if (exc) {
                tcg_s390_program_interrupt(env, exc, GETPC());
            }
            new_sasce = aste_asce(&saste);
            exc = subspace_replace(env, new_sasce, saste.origin, &new_sasce);
            if (exc) {
                tcg_s390_program_interrupt(env, exc, GETPC());
            }
            translated_secondary = true;
        }
    }

    exc = linkage_commit_writes(env, &write, 1);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }

    for (int i = 2; i <= 14; i++) {
        env->regs[i] = ldq_be_p(state + i * 8);
        env->aregs[i] = ldl_be_p(state + 224 + i * 4);
    }

    if (type == S390_LINKAGE_ENTRY_PC) {
        env->cregs[3] = deposit64(env->cregs[3], 0, 32,
                                  ldl_be_p(state + 128));
        env->cregs[8] = deposit64(env->cregs[8], 16, 16,
                                  lduw_be_p(state + 132));
        env->cregs[4] = deposit64(env->cregs[4], 0, 16,
                                  lduw_be_p(state + 134));
        if (translated_primary) {
            env->cregs[1] = new_pasce;
            env->cregs[4] = deposit64(env->cregs[4], 16, 16, new_ax);
            env->cregs[5] = deposit64(env->cregs[5], 0, 32, new_pasteo);
        }
        if (reuse) {
            env->cregs[3] = deposit64(env->cregs[3], 32, 32,
                                      ldl_be_p(state + 176));
            env->cregs[4] = deposit64(env->cregs[4], 32, 32,
                                      ldl_be_p(state + 180));
        }
        env->cregs[7] = new_sasce;
        if (translated_primary || translated_secondary) {
            tlb_flush(env_cpu(env));
        }
    }

    env->cregs[15] = preceding_desc_addr & ~0x7ULL;
    env->cregs[12] = new_cr12;
    tlb_flush(env_cpu(env));
    s390_cpu_set_psw(env, new_mask, new_addr);
    linkage_record_per_branch(env, new_addr);
    if (space_switch &&
        (((old_pasce | env->cregs[1]) & ASCE_SPACE_SWITCH_EVENT) ||
         env->per_perc_atmid)) {
        linkage_set_tea(env, old_pasn |
                        (old_pasce & ASCE_SPACE_SWITCH_EVENT ?
                         TEA_SPACE_SWITCH_EVENT : 0));
        linkage_completed_exception(env, PGM_SPACE_SWITCH, new_addr, 2);
    }
    if (env->per_perc_atmid) {
        linkage_completed_exception(env, PGM_PER, new_addr, 2);
    }
    cpu_loop_exit(env_cpu(env));
}

void HELPER(ereg)(CPUS390XState *env, uint32_t r1, uint32_t r2,
                  uint32_t is_64)
{
    uint64_t state_desc_addr;
    uint64_t state_addr;
    uint8_t type;
    int reg = r1;
    int exc;

    linkage_check_unstack_mode(env, true);
    exc = linkage_locate_state(env, false, &state_desc_addr, &type);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    state_addr = state_desc_addr -
                 (LINKAGE_STATE_ENTRY_SIZE - LINKAGE_DESC_SIZE);

    for (;;) {
        uint64_t gpr;
        uint32_t ar;

        exc = linkage_read(env, state_addr + reg * 8, &gpr, sizeof(gpr));
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
        exc = linkage_read(env, state_addr + 224 + reg * 4,
                           &ar, sizeof(ar));
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
        gpr = be64_to_cpu(gpr);
        ar = be32_to_cpu(ar);
        if (is_64) {
            env->regs[reg] = gpr;
        } else {
            env->regs[reg] = deposit64(env->regs[reg], 0, 32, gpr);
        }
        env->aregs[reg] = ar;

        if (reg == r2) {
            break;
        }
        reg = (reg + 1) & 15;
    }
    tlb_flush(env_cpu(env));
}

uint32_t HELPER(esta)(CPUS390XState *env, uint32_t r1, uint32_t r2)
{
    uint64_t state_desc_addr;
    uint64_t state_addr;
    uint8_t type;
    uint8_t data[16];
    uint8_t code = env->regs[r2];
    bool reuse = s390_has_feat(S390_FEAT_ASN_LX_REUSE);
    int exc;

    linkage_check_unstack_mode(env, true);
    if ((r1 & 1) || code > (reuse ? 5 : 4)) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
    }
    exc = linkage_locate_state(env, false, &state_desc_addr, &type);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    state_addr = state_desc_addr -
                 (LINKAGE_STATE_ENTRY_SIZE - LINKAGE_DESC_SIZE);

    switch (code) {
    case 0:
    case 2:
    case 3: {
        static const uint16_t offsets[] = { 128, 0, 144, 152 };

        exc = linkage_read(env, state_addr + offsets[code], data, 8);
        if (!exc) {
            env->regs[r1] = deposit64(env->regs[r1], 0, 32,
                                      ldl_be_p(data));
            env->regs[r1 + 1] = deposit64(env->regs[r1 + 1], 0, 32,
                                          ldl_be_p(data + 4));
        }
        break;
    }
    case 1: {
        uint64_t saved_addr;
        uint32_t word0;
        uint32_t word1;

        exc = linkage_read(env, state_addr + 136, data, 5);
        if (exc) {
            break;
        }
        exc = linkage_read(env, state_addr + 168, &saved_addr,
                           sizeof(saved_addr));
        if (exc) {
            break;
        }
        saved_addr = be64_to_cpu(saved_addr);
        word0 = ldl_be_p(data) | 0x00080000;
        word1 = (data[4] & 0x80 ? 0x80000000 : 0) |
                (saved_addr & 0x7fffffff);
        if (saved_addr >> 31) {
            word1 |= 1;
        }
        env->regs[r1] = deposit64(env->regs[r1], 0, 32, word0);
        env->regs[r1 + 1] = deposit64(env->regs[r1 + 1], 0, 32, word1);
        break;
    }
    case 4:
        exc = linkage_read(env, state_addr + 136, data, 8);
        if (exc) {
            break;
        }
        exc = linkage_read(env, state_addr + 168, data + 8, 8);
        if (!exc) {
            env->regs[r1] = ldq_be_p(data);
            env->regs[r1 + 1] = ldq_be_p(data + 8);
        }
        break;
    case 5:
        exc = linkage_read(env, state_addr + 176, data, 8);
        if (!exc) {
            env->regs[r1] = deposit64(env->regs[r1], 32, 32,
                                      ldl_be_p(data));
            env->regs[r1 + 1] = deposit64(env->regs[r1 + 1], 32, 32,
                                          ldl_be_p(data + 4));
        }
        break;
    default:
        g_assert_not_reached();
    }
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    return type == S390_LINKAGE_ENTRY_PC;
}

void HELPER(msta)(CPUS390XState *env, uint32_t r1)
{
    uint64_t state_desc_addr;
    uint8_t modify[8];
    uint8_t type;
    S390LinkageWrite write;
    int exc;

    linkage_check_unstack_mode(env, true);
    if (r1 & 1) {
        tcg_s390_program_interrupt(env, PGM_SPECIFICATION, GETPC());
    }
    exc = linkage_locate_state(env, false, &state_desc_addr, &type);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }

    stl_be_p(modify, env->regs[r1]);
    stl_be_p(modify + 4, env->regs[r1 + 1]);
    exc = linkage_prepare_write(env, &write, state_desc_addr - 136,
                                modify, sizeof(modify));
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    exc = linkage_commit_writes(env, &write, 1);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
}

static int linkage_real_read(CPUS390XState *env, uint64_t raddr,
                             void *buf, uint16_t len)
{
    AddressSpace *as = env_cpu(env)->as;
    uint8_t *p = buf;
    uint32_t remaining = len;

    while (remaining) {
        uint64_t tec;
        hwaddr abs;
        int flags;
        int exc;
        uint32_t chunk;

        exc = mmu_translate_real(env, raddr, MMU_DATA_LOAD,
                                 &abs, &flags, &tec);
        if (exc) {
            env->tlb_fill_exc = exc;
            env->tlb_fill_tec = tec;
            return exc;
        }
        chunk = MIN(remaining,
                    TARGET_PAGE_SIZE -
                    (uint32_t)(raddr & ~TARGET_PAGE_MASK));
        if (address_space_read(as, abs | (raddr & ~TARGET_PAGE_MASK),
                               MEMTXATTRS_UNSPECIFIED, p, chunk) != MEMTX_OK) {
            return PGM_ADDRESSING;
        }
        remaining -= chunk;
        raddr = (raddr + chunk) & 0x7fffffff;
        p += chunk;
    }
    return 0;
}

static void linkage_set_tea(CPUS390XState *env, uint64_t tea)
{
    env->tlb_fill_exc = 0;
    env->tlb_fill_tec = tea;
}

static void linkage_set_exception_access_id(CPUS390XState *env, uint8_t id)
{
    address_space_stb(env_cpu(env)->as,
                      env->psa + offsetof(LowCore, exc_access_id), id,
                      MEMTXATTRS_UNSPECIFIED, NULL);
}

static int aste_read(CPUS390XState *env, uint32_t origin, uint16_t asn,
                     S390ASTE *aste)
{
    uint32_t raw[16];
    int exc;

    exc = linkage_real_read(env, origin, raw, sizeof(raw));
    if (exc) {
        return exc;
    }
    for (int i = 0; i < ARRAY_SIZE(raw); i++) {
        aste->words[i] = be32_to_cpu(raw[i]);
    }
    aste->origin = origin;
    if (aste->words[0] & ASTE_INVALID) {
        linkage_set_tea(env, asn);
        return PGM_ASX_TRANS;
    }
    return 0;
}

static int asn_translate(CPUS390XState *env, uint16_t asn, S390ASTE *aste)
{
    uint32_t afte;
    uint32_t afte_addr;
    uint32_t aste_addr;
    int exc;

    afte_addr = ((env->cregs[14] & CR14_ASN_FIRST_ORIGIN) << 12) +
                ((asn & 0xffc0) >> 4);
    exc = linkage_real_read(env, afte_addr, &afte, sizeof(afte));
    if (exc) {
        return exc;
    }
    afte = be32_to_cpu(afte);
    if (afte & 0x80000000U) {
        linkage_set_tea(env, asn);
        return PGM_AFX_TRANS;
    }

    aste_addr = ((afte & ASTE_ORIGIN) + ((asn & 0x3f) << 6)) &
                0x7fffffffU;
    return aste_read(env, aste_addr, asn, aste);
}

static uint64_t aste_asce(const S390ASTE *aste)
{
    return (uint64_t)aste->words[2] << 32 | aste->words[3];
}

static int subspace_replace(CPUS390XState *env, uint64_t asce,
                            uint32_t base_aste_origin, uint64_t *result)
{
    uint32_t duct[4];
    uint32_t subspace_aste[16];
    uint32_t duct_origin = env->cregs[2] & 0x7fffffc0U;
    uint32_t subspace_origin;
    uint64_t event_bits;
    int exc;

    *result = asce;
    if (!(asce & ASCE_SUBSPACE)) {
        return 0;
    }

    exc = linkage_real_read(env, duct_origin, duct, sizeof(duct));
    if (exc) {
        return exc;
    }
    for (int i = 0; i < ARRAY_SIZE(duct); i++) {
        duct[i] = be32_to_cpu(duct[i]);
    }
    if (!(duct[1] & 0x80000000U) ||
        (duct[0] & ASTE_ORIGIN) != base_aste_origin) {
        return 0;
    }

    subspace_origin = duct[1] & ASTE_ORIGIN;
    exc = linkage_real_read(env, subspace_origin, subspace_aste,
                            sizeof(subspace_aste));
    if (exc) {
        return exc;
    }
    for (int i = 0; i < ARRAY_SIZE(subspace_aste); i++) {
        subspace_aste[i] = be32_to_cpu(subspace_aste[i]);
    }
    env->tlb_fill_arn = 0;
    if (subspace_aste[0] & ASTE_INVALID) {
        return PGM_ASTE_VALID;
    }
    if (subspace_aste[5] != duct[3]) {
        return PGM_ASTE_SEQ;
    }

    event_bits = asce & (ASCE_SPACE_SWITCH_EVENT | ASCE_ALT_EVENT);
    *result = ((uint64_t)subspace_aste[2] << 32 | subspace_aste[3]);
    *result &= ~(ASCE_SPACE_SWITCH_EVENT | ASCE_ALT_EVENT);
    *result |= event_bits;
    return 0;
}

static int aste_authorize_secondary(CPUS390XState *env,
                                    const S390ASTE *aste, uint16_t ax,
                                    uint16_t asn)
{
    uint8_t entry;
    uint32_t atl = (aste->words[1] & ASTE_ATL) >> 4;
    uint32_t addr;
    int exc;

    if ((ax >> 4) > atl) {
        linkage_set_tea(env, asn);
        return PGM_SEC_AUTH;
    }

    addr = ((aste->words[0] & ASTE_ATO) + (ax >> 2)) & 0x7fffffffU;
    exc = linkage_real_read(env, addr, &entry, sizeof(entry));
    if (exc) {
        return exc;
    }
    if (!(entry & (0x40 >> ((ax & 3) * 2)))) {
        linkage_set_tea(env, asn);
        return PGM_SEC_AUTH;
    }
    return 0;
}

static int pc_translate(CPUS390XState *env, uint64_t effective_addr,
                        uint64_t next_pc, uint32_t *translated_pc,
                        uint64_t *new_cr12, uint8_t ete[32])
{
    uint32_t designation;
    uint32_t table_entry;
    uint64_t primary_aste = env->cregs[5] & 0x7fffffc0ULL;
    uint32_t pc_number;
    uint32_t pctea;
    uint32_t ex;
    uint64_t entry_table_origin;
    uint32_t entry_table_length;
    uint64_t old_cr12 = env->cregs[12];
    bool reuse = s390_has_feat(S390_FEAT_ASN_LX_REUSE) &&
                 (env->cregs[0] & CR0_ASN_LX_REUSE);
    int exc;

    if (reuse && (effective_addr & 0x00080000ULL)) {
        pc_number = ((effective_addr & 0xfff00000ULL) >> 1) |
                    (effective_addr & 0x0007ffffULL);
        pctea = effective_addr;
    } else {
        pc_number = effective_addr & 0x000fffffULL;
        pctea = pc_number;
    }
    ex = pc_number & 0xff;
    *translated_pc = pc_number;

    exc = linkage_real_read(env, primary_aste + 24,
                            &designation, sizeof(designation));
    if (exc) {
        return exc;
    }
    designation = be32_to_cpu(designation);
    if (!(designation & 0x80000000U)) {
        return PGM_SPECIAL_OP;
    }

    /*
     * ASN tracing precedes linkage-table lookup.  Keep the updated CR12
     * staged until the PC completes, but retain the trace-table store if a
     * later table lookup raises an exception.
     */
    exc = linkage_trace_pc(env, pctea, next_pc);
    if (exc) {
        return exc;
    }
    *new_cr12 = env->cregs[12];
    env->cregs[12] = old_cr12;

    if (!reuse) {
        uint32_t lx = (pc_number >> 8) & 0xfff;

        if ((lx >> 5) > (designation & 0x7f)) {
            linkage_set_tea(env, pctea);
            return PGM_LX_TRANS;
        }
        exc = linkage_real_read(env,
                                ((designation & 0x7fffff80U) + lx * 4) &
                                0x7fffffffU,
                                &table_entry, sizeof(table_entry));
        if (exc) {
            return exc;
        }
        table_entry = be32_to_cpu(table_entry);
        if (table_entry & 0x80000000U) {
            linkage_set_tea(env, pctea);
            return PGM_LX_TRANS;
        }
    } else {
        uint32_t lfx = pc_number & 0x7fffe000U;
        uint32_t lsx = (pc_number >> 8) & 0x1f;
        uint64_t lste;

        if ((effective_addr & 0x00080000ULL) &&
            (pc_number >> 19) > (designation & 0xff)) {
            linkage_set_tea(env, pctea);
            return PGM_LFX_TRANS;
        }

        exc = linkage_real_read(env,
                                ((designation & 0x7fffff00U) +
                                 (lfx >> 11)) & 0x7fffffffU,
                                &table_entry, sizeof(table_entry));
        if (exc) {
            return exc;
        }
        table_entry = be32_to_cpu(table_entry);
        if (table_entry & 0x80000000U) {
            linkage_set_tea(env, pctea);
            return PGM_LFX_TRANS;
        }

        exc = linkage_real_read(env,
                                ((table_entry & 0x7fffff00U) + lsx * 8) &
                                0x7fffffffU,
                                &lste, sizeof(lste));
        if (exc) {
            return exc;
        }
        lste = be64_to_cpu(lste);
        table_entry = lste >> 32;
        if (table_entry & 0x80000000U) {
            linkage_set_tea(env, pctea);
            return PGM_LSX_TRANS;
        }
        if ((uint32_t)lste && (uint32_t)lste != (env->regs[15] >> 32)) {
            linkage_set_tea(env, pctea);
            return PGM_LSTE_SEQ;
        }
    }

    entry_table_origin = table_entry & 0x7fffffc0U;
    entry_table_length = table_entry & 0x3f;
    if ((ex >> 2) > entry_table_length) {
        linkage_set_tea(env, pctea);
        return PGM_EX_TRANS;
    }
    return linkage_real_read(env,
                             (entry_table_origin + ex * 32) & 0x7fffffffU,
                             ete, 32);
}

uint64_t HELPER(pc)(CPUS390XState *env, uint64_t pc_number, uint64_t next_pc)
{
    uint8_t ete[32];
    S390ASTE aste = { 0 };
    S390LinkageState state = {
        .psw_mask = s390_cpu_get_psw_mask(env),
        .psw_addr = next_pc,
    };
    uint64_t old_mask = state.psw_mask;
    uint64_t old_cr12 = env->cregs[12];
    uint64_t new_cr12 = 0;
    uint64_t old_pasce = env->cregs[1];
    uint16_t old_pasn = env->cregs[4];
    uint64_t new_mask = old_mask;
    uint64_t q0;
    uint64_t entry_parameter;
    uint64_t target;
    uint64_t called_pasce = 0;
    uint16_t akm;
    uint16_t asn;
    uint16_t ekm;
    uint16_t pkm = env->cregs[3] >> 16;
    bool stacking;
    bool result_64;
    bool result_31;
    bool problem;
    bool reuse = s390_has_feat(S390_FEAT_ASN_LX_REUSE) &&
                 (env->cregs[0] & CR0_ASN_LX_REUSE);
    uint32_t translated_pc;
    int exc;

    if (!(env->psw.mask & PSW_MASK_DAT) ||
        (env->psw.mask & PSW_MASK_ASC) == PSW_ASC_SECONDARY ||
        (env->psw.mask & PSW_MASK_ASC) == PSW_ASC_HOME) {
        tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
    }

    exc = pc_translate(env, pc_number, next_pc, &translated_pc,
                       &new_cr12, ete);
    if (exc) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }

    q0 = ldq_be_p(ete);
    akm = lduw_be_p(ete + 8);
    asn = lduw_be_p(ete + 10);
    ekm = lduw_be_p(ete + 12);
    stacking = ete[16] & 0x80;
    result_64 = ete[16] & 0x40;
    result_31 = q0 & 0x80000000ULL;
    problem = q0 & 1;

    if (!stacking &&
        ((env->psw.mask & PSW_MASK_ASC) == PSW_ASC_ACCREG ||
         !!(env->psw.mask & PSW_MASK_64) != result_64)) {
        tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
    }
    if (!result_64 && !result_31 && (q0 & 0x7f000000ULL)) {
        tcg_s390_program_interrupt(env, PGM_PC_TRANS_SPEC, GETPC());
    }
    if ((env->psw.mask & PSW_MASK_PSTATE) && !(pkm & akm)) {
        tcg_s390_program_interrupt(env, PGM_PRIVILEGED, GETPC());
    }

    if (asn) {
        uint32_t aste_origin;

        if (!(env->cregs[14] & CR14_ASN_TRANSLATION)) {
            tcg_s390_program_interrupt(env, PGM_SPECIAL_OP, GETPC());
        }
        aste_origin = ldl_be_p(ete + 20) & ASTE_ORIGIN;
        exc = aste_read(env, aste_origin, asn, &aste);
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
        called_pasce = aste_asce(&aste);
        exc = subspace_replace(env, called_pasce, aste.origin,
                               &called_pasce);
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
    }

    if (stacking && (old_cr12 & CR12_MODE_TRACE) &&
        !!(old_mask & PSW_MASK_64) != result_64) {
        env->cregs[12] = new_cr12;
        exc = linkage_trace_mode(env, next_pc);
        if (exc) {
            env->cregs[12] = old_cr12;
            tcg_s390_program_interrupt(env, exc, GETPC());
        }
        new_cr12 = env->cregs[12];
        env->cregs[12] = old_cr12;
    }

    if (result_64) {
        target = q0 & ~1ULL;
        new_mask |= PSW_MASK_64 | PSW_MASK_32;
        entry_parameter = ldq_be_p(ete + 24);
    } else {
        target = q0 & 0x7ffffffeULL;
        new_mask &= ~PSW_MASK_64;
        if (result_31) {
            new_mask |= PSW_MASK_32;
        } else {
            new_mask &= ~PSW_MASK_32;
        }
        entry_parameter = ldl_be_p(ete + 28);
    }
    if (problem) {
        new_mask |= PSW_MASK_PSTATE;
    } else {
        new_mask &= ~PSW_MASK_PSTATE;
    }

    if (stacking) {
        state.call_addr = target;
        state.called_space_id = asn ?
            (uint32_t)asn << 16 | (aste.words[11] & 0xffff) : 0;
        state.pc_number = translated_pc & 0x7fffffff;
        if (result_64) {
            state.pc_number |= 0x80000000U;
        }
        exc = linkage_stack_push(env, S390_LINKAGE_ENTRY_PC, &state);
        if (exc) {
            tcg_s390_program_interrupt(env, exc, GETPC());
        }

        if (ete[16] & 0x10) {
            new_mask = deposit64(new_mask, PSW_SHIFT_KEY, 4, ete[17] >> 4);
        }
        if (ete[16] & 0x08) {
            pkm = ekm;
        } else {
            pkm |= ekm;
        }
        if (ete[16] & 0x04) {
            env->cregs[8] = deposit64(env->cregs[8], 16, 16,
                                      lduw_be_p(ete + 18));
        }
        new_mask &= ~PSW_MASK_ASC;
        if (ete[16] & 0x02) {
            new_mask |= PSW_ASC_ACCREG;
        }
    } else {
        if (env->psw.mask & PSW_MASK_64) {
            env->regs[14] = (next_pc & ~1ULL) |
                            !!(old_mask & PSW_MASK_PSTATE);
        } else {
            uint32_t link = (old_mask & PSW_MASK_32 ? 0x80000000U : 0) |
                            (next_pc & 0x7ffffffeU) |
                            !!(old_mask & PSW_MASK_PSTATE);
            env->regs[14] = deposit64(env->regs[14], 0, 32, link);
        }
        env->regs[3] = deposit64(env->regs[3], 0, 32,
                                 (uint32_t)pkm << 16 |
                                 (uint16_t)env->cregs[4]);
        pkm |= ekm;
    }

    env->regs[4] = result_64 ? entry_parameter :
                   deposit64(env->regs[4], 0, 32, entry_parameter);
    env->cregs[3] = deposit64(env->cregs[3], 16, 16, pkm);

    if (!asn) {
        env->cregs[3] = deposit64(env->cregs[3], 0, 16,
                                  (uint16_t)env->cregs[4]);
        env->cregs[7] = env->cregs[1];
        if (reuse) {
            env->cregs[3] = deposit64(env->cregs[3], 32, 32,
                                      env->cregs[4] >> 32);
        }
    } else {
        uint64_t old_primary = env->cregs[1];
        uint64_t old_cr4 = env->cregs[4];

        env->cregs[3] = deposit64(env->cregs[3], 0, 16,
                                  (uint16_t)old_cr4);
        env->cregs[7] = old_primary;
        if (reuse) {
            env->cregs[3] = deposit64(env->cregs[3], 32, 32, old_cr4 >> 32);
        }

        env->cregs[4] = deposit64(env->cregs[4], 0, 32,
                                  (aste.words[1] & ASTE_AX) | asn);
        if (reuse) {
            env->cregs[4] = deposit64(env->cregs[4], 32, 32,
                                      aste.words[11]);
        }
        env->cregs[1] = called_pasce;
        env->cregs[5] = deposit64(env->cregs[5], 0, 32, aste.origin);

        if (stacking && (ete[16] & 0x01)) {
            env->cregs[3] = deposit64(env->cregs[3], 0, 16, asn);
            env->cregs[7] = env->cregs[1];
            if (reuse) {
                env->cregs[3] = deposit64(env->cregs[3], 32, 32,
                                          env->cregs[4] >> 32);
            }
        }
        tlb_flush(env_cpu(env));
    }

    env->psw.mask = new_mask;
    if (new_cr12 != old_cr12) {
        env->cregs[12] = new_cr12;
    }
    linkage_record_per_branch(env, target);
    if (asn &&
        (((old_pasce | env->cregs[1]) & ASCE_SPACE_SWITCH_EVENT) ||
         env->per_perc_atmid)) {
        linkage_set_tea(env, old_pasn |
                        (old_pasce & ASCE_SPACE_SWITCH_EVENT ?
                         TEA_SPACE_SWITCH_EVENT : 0));
        linkage_completed_exception(env, PGM_SPACE_SWITCH, target, 4);
    }
    return target;
}

#endif /* !CONFIG_USER_ONLY */
