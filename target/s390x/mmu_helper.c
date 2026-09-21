/*
 * S390x MMU related functions
 *
 * Copyright (c) 2011 Alexander Graf
 * Copyright (c) 2015 Thomas Huth, IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "cpu.h"
#include "s390x-internal.h"
#include "tcg/tcg_s390x.h"
#include "kvm/kvm_s390x.h"
#include "system/kvm.h"
#include "system/tcg.h"
#include "system/memory.h"
#ifdef CONFIG_TCG
#include "accel/tcg/cpu-loop.h"
#include "accel/tcg/helper-retaddr.h"
#include "exec/helper-proto.h"
#endif
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "exec/cputlb.h"
#include "hw/core/hw-error.h"
#include "hw/s390x/storage-keys.h"
#include "hw/core/boards.h"

/* Fetch/store bits in the translation exception code: */
#define FS_READ  0x800
#define FS_WRITE 0x400

#define ALET_RESERVED      0xfe000000U
#define ALET_PRIMARY_LIST  0x01000000U
#define ALET_SEQUENCE      0x00ff0000U
#define ALET_ALEN          0x0000ffffU
#define ALD_ORIGIN         0x7fffff80U
#define ALD_LENGTH         0x0000007fU
#define ALD_LENGTH_SHIFT   3
#define ALE_INVALID        0x80000000U
#define ALE_FETCH_ONLY     0x02000000U
#define ALE_PRIVATE        0x01000000U
#define ALE_SEQUENCE       0x00ff0000U
#define ALE_AUTH_INDEX     0x0000ffffU
#define ALE_ASTE_ORIGIN    0x7fffffc0U
#define ASTE_INVALID       0x80000000U
#define ASTE_AUTH_ORIGIN   0x7ffffffcU
#define ASTE_AUTH_LENGTH   0x0000fff0U

static void trigger_access_exception(CPUS390XState *env, uint32_t type,
                                     uint64_t tec, uint8_t arn)
{
    S390CPU *cpu = env_archcpu(env);

    if (kvm_enabled()) {
        kvm_s390_access_exception(cpu, type, tec);
    } else {
        env->tlb_fill_exc = type;
        env->tlb_fill_tec = tec;
        env->tlb_fill_arn = arn;
        trigger_pgm_exception(env, type);
    }
}

/* check whether the address would be proteted by Low-Address Protection */
static bool is_low_address(uint64_t addr)
{
    return addr <= 511 || (addr >= 4096 && addr <= 4607);
}

/* check whether Low-Address Protection is enabled for mmu_translate() */
static bool lowprot_enabled(const CPUS390XState *env, uint64_t asce)
{
    if (!(env->cregs[0] & CR0_LOWPROT)) {
        return false;
    }
    if (!(env->psw.mask & PSW_MASK_DAT)) {
        return true;
    }
    return !(asce & ASCE_PRIVATE_SPACE);
}

/**
 * Translate real address to absolute (= physical)
 * address by taking care of the prefix mapping.
 */
hwaddr mmu_real2abs(CPUS390XState *env, hwaddr raddr)
{
    if (raddr < 0x2000) {
        return raddr + env->psa;    /* Map the lowcore. */
    } else if (raddr >= env->psa && raddr < env->psa + 0x2000) {
        return raddr - env->psa;    /* Map the 0 page. */
    }
    return raddr;
}

bool mmu_absolute_addr_valid(hwaddr addr, bool is_write)
{
    return address_space_access_valid(&address_space_memory,
                                      addr & TARGET_PAGE_MASK,
                                      TARGET_PAGE_SIZE, is_write,
                                      MEMTXATTRS_UNSPECIFIED);
}

static inline bool read_table_entry(CPUS390XState *env, hwaddr gaddr,
                                    uint64_t *entry)
{
    CPUState *cs = env_cpu(env);
    MemTxResult ret;

    /*
     * According to the PoP, these table addresses are "unpredictably real
     * or absolute". Also, "it is unpredictable whether the address wraps
     * or an addressing exception is recognized".
     *
     * We treat them as absolute addresses and don't wrap them.
     */
    *entry = address_space_ldq_be(cs->as, gaddr, MEMTXATTRS_UNSPECIFIED, &ret);

    return ret == MEMTX_OK;
}

static bool mmu_read_real(CPUS390XState *env, uint32_t raddr,
                          void *buf, size_t len)
{
    return address_space_read(env_cpu(env)->as, mmu_real2abs(env, raddr),
                              MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

static int mmu_authorize_extended(CPUS390XState *env,
                                  const uint32_t aste[16], uint16_t eax)
{
    uint32_t atl = (aste[1] & ASTE_AUTH_LENGTH) >> 4;
    uint32_t addr;
    uint8_t entry;

    if ((eax >> 4) > atl) {
        return PGM_EXT_AUTH;
    }
    addr = ((aste[0] & ASTE_AUTH_ORIGIN) + (eax >> 2)) & 0x7fffffffU;
    if (!mmu_read_real(env, addr, &entry, sizeof(entry))) {
        return PGM_ADDRESSING;
    }
    return entry & (0x40 >> ((eax & 3) * 2)) ? 0 : PGM_EXT_AUTH;
}

int s390_mmu_translate_alet(CPUS390XState *env, uint32_t alet, uint16_t eax,
                            int rw, bool special_art, uint64_t *asce,
                            bool *fetch_only, uint32_t *aste_origin,
                            uint32_t aste_words[16])
{
    uint32_t ale[4];
    uint32_t aste[16];
    uint32_t cb;
    uint32_t ald;
    uint32_t ale_addr;
    uint32_t aste_addr;
    int exc;

    *fetch_only = false;
    if (alet == 0) {
        *asce = env->cregs[1];
        if (aste_origin) {
            *aste_origin = env->cregs[5] & ALE_ASTE_ORIGIN;
        }
        return 0;
    }
    if (alet == 1) {
        *asce = env->cregs[7];
        return 0;
    }
    if (alet & ALET_RESERVED) {
        return PGM_ALET_SPEC;
    }

    cb = alet & ALET_PRIMARY_LIST ? env->cregs[5] : env->cregs[2];
    cb &= 0x7fffffc0U;
    if (!mmu_read_real(env, cb + 16, &ald, sizeof(ald))) {
        return PGM_ADDRESSING;
    }
    ald = be32_to_cpu(ald);
    if (((alet & ALET_ALEN) >> ALD_LENGTH_SHIFT) >
        (ald & ALD_LENGTH)) {
        return PGM_ALEN_SPEC;
    }

    ale_addr = ((ald & ALD_ORIGIN) + (alet & ALET_ALEN) * 16) &
               0x7fffffffU;
    if (!mmu_read_real(env, ale_addr, ale, sizeof(ale))) {
        return PGM_ADDRESSING;
    }
    for (int i = 0; i < ARRAY_SIZE(ale); i++) {
        ale[i] = be32_to_cpu(ale[i]);
    }
    if (ale[0] & ALE_INVALID) {
        return PGM_ALEN_SPEC;
    }
    if (!special_art &&
        (ale[0] & ALE_SEQUENCE) != (alet & ALET_SEQUENCE)) {
        return PGM_ALE_SEQ;
    }

    aste_addr = ale[2] & ALE_ASTE_ORIGIN;
    if (!mmu_read_real(env, aste_addr, aste, sizeof(aste))) {
        return PGM_ADDRESSING;
    }
    for (int i = 0; i < ARRAY_SIZE(aste); i++) {
        aste[i] = be32_to_cpu(aste[i]);
    }
    if (aste[0] & ASTE_INVALID) {
        return PGM_ASTE_VALID;
    }
    if (aste[5] != ale[3]) {
        return PGM_ASTE_SEQ;
    }

    if (!special_art && (ale[0] & ALE_PRIVATE) &&
        (ale[0] & ALE_AUTH_INDEX) != eax) {
        exc = mmu_authorize_extended(env, aste, eax);
        if (exc) {
            return exc;
        }
    }
    *fetch_only = (ale[0] & ALE_FETCH_ONLY) &&
                  (rw == MMU_DATA_STORE || rw == MMU_S390_TPROT);

    *asce = (uint64_t)aste[2] << 32 | aste[3];
    if (aste_origin) {
        *aste_origin = aste_addr;
    }
    if (aste_words) {
        memcpy(aste_words, aste, sizeof(aste));
    }
    return 0;
}

static int mmu_translate_arn(CPUS390XState *env, unsigned int arn, int rw,
                             uint64_t *asce, bool *fetch_only)
{
    uint32_t alet = arn ? env->aregs[arn] : 0;
    uint16_t eax = env->cregs[8] >> 16;

    return s390_mmu_translate_alet(env, alet, eax, rw, false,
                                   asce, fetch_only, NULL, NULL);
}

#ifdef CONFIG_TCG
uint32_t HELPER(tar)(CPUS390XState *env, uint32_t r1, uint32_t r2)
{
    uint32_t alet = env->aregs[r1];
    uint16_t eax = env->regs[r2] >> 16;
    uint64_t asce;
    bool fetch_only;
    int exc;

    if (alet == 0) {
        return 0;
    }
    if (alet == 1) {
        return 3;
    }

    exc = s390_mmu_translate_alet(env, alet, eax, MMU_DATA_LOAD, false,
                                  &asce, &fetch_only, NULL, NULL);
    if (exc == PGM_ADDRESSING) {
        tcg_s390_program_interrupt(env, exc, GETPC());
    }
    if (exc) {
        return 3;
    }
    return alet & ALET_PRIMARY_LIST ? 2 : 1;
}
#endif

static int mmu_translate_asce(CPUS390XState *env, vaddr vaddr,
                              uint64_t asc, uint64_t asce, hwaddr *raddr,
                              int *flags, int *lra_cc)
{
    const bool edat1 = (env->cregs[0] & CR0_EDAT) &&
                       s390_has_feat(S390_FEAT_EDAT);
    const bool edat2 = edat1 && s390_has_feat(S390_FEAT_EDAT_2);
    const bool iep = (env->cregs[0] & CR0_IEP) &&
                     s390_has_feat(S390_FEAT_INSTRUCTION_EXEC_PROT);
    const int asce_tl = asce & ASCE_TABLE_LENGTH;
    const int asce_p = asce & ASCE_PRIVATE_SPACE;
    hwaddr gaddr = asce & ASCE_ORIGIN;
    uint64_t entry;

    if (asce & ASCE_REAL_SPACE) {
        /* direct mapping */
        *raddr = vaddr;
        return 0;
    }

    switch (asce & ASCE_TYPE_MASK) {
    case ASCE_TYPE_REGION1:
        if (VADDR_REGION1_TL(vaddr) > asce_tl) {
            return PGM_REG_FIRST_TRANS;
        }
        gaddr += VADDR_REGION1_TX(vaddr) * 8;
        break;
    case ASCE_TYPE_REGION2:
        if (VADDR_REGION1_TX(vaddr)) {
            return PGM_ASCE_TYPE;
        }
        if (VADDR_REGION2_TL(vaddr) > asce_tl) {
            return PGM_REG_SEC_TRANS;
        }
        gaddr += VADDR_REGION2_TX(vaddr) * 8;
        break;
    case ASCE_TYPE_REGION3:
        if (VADDR_REGION1_TX(vaddr) || VADDR_REGION2_TX(vaddr)) {
            return PGM_ASCE_TYPE;
        }
        if (VADDR_REGION3_TL(vaddr) > asce_tl) {
            return PGM_REG_THIRD_TRANS;
        }
        gaddr += VADDR_REGION3_TX(vaddr) * 8;
        break;
    case ASCE_TYPE_SEGMENT:
        if (VADDR_REGION1_TX(vaddr) || VADDR_REGION2_TX(vaddr) ||
            VADDR_REGION3_TX(vaddr)) {
            return PGM_ASCE_TYPE;
        }
        gaddr += VADDR_SEGMENT_TX(vaddr) * 8;
        if (VADDR_SEGMENT_TL(vaddr) > asce_tl) {
            *raddr = gaddr;
            *lra_cc = 3;
            return PGM_SEGMENT_TRANS;
        }
        break;
    }

    switch (asce & ASCE_TYPE_MASK) {
    case ASCE_TYPE_REGION1:
        if (!read_table_entry(env, gaddr, &entry)) {
            return PGM_ADDRESSING;
        }
        if (entry & REGION_ENTRY_I) {
            return PGM_REG_FIRST_TRANS;
        }
        if ((entry & REGION_ENTRY_TT) != REGION_ENTRY_TT_REGION1) {
            return PGM_TRANS_SPEC;
        }
        if (VADDR_REGION2_TL(vaddr) < (entry & REGION_ENTRY_TF) >> 6 ||
            VADDR_REGION2_TL(vaddr) > (entry & REGION_ENTRY_TL)) {
            return PGM_REG_SEC_TRANS;
        }
        if (edat1 && (entry & REGION_ENTRY_P)) {
            *flags &= ~PAGE_WRITE;
        }
        gaddr = (entry & REGION_ENTRY_ORIGIN) + VADDR_REGION2_TX(vaddr) * 8;
        /* fall through */
    case ASCE_TYPE_REGION2:
        if (!read_table_entry(env, gaddr, &entry)) {
            return PGM_ADDRESSING;
        }
        if (entry & REGION_ENTRY_I) {
            return PGM_REG_SEC_TRANS;
        }
        if ((entry & REGION_ENTRY_TT) != REGION_ENTRY_TT_REGION2) {
            return PGM_TRANS_SPEC;
        }
        if (VADDR_REGION3_TL(vaddr) < (entry & REGION_ENTRY_TF) >> 6 ||
            VADDR_REGION3_TL(vaddr) > (entry & REGION_ENTRY_TL)) {
            return PGM_REG_THIRD_TRANS;
        }
        if (edat1 && (entry & REGION_ENTRY_P)) {
            *flags &= ~PAGE_WRITE;
        }
        gaddr = (entry & REGION_ENTRY_ORIGIN) + VADDR_REGION3_TX(vaddr) * 8;
        /* fall through */
    case ASCE_TYPE_REGION3:
        if (!read_table_entry(env, gaddr, &entry)) {
            return PGM_ADDRESSING;
        }
        if (entry & REGION_ENTRY_I) {
            return PGM_REG_THIRD_TRANS;
        }
        if ((entry & REGION_ENTRY_TT) != REGION_ENTRY_TT_REGION3) {
            return PGM_TRANS_SPEC;
        }
        if (edat2 && (entry & REGION3_ENTRY_CR) && asce_p) {
            return PGM_TRANS_SPEC;
        }
        if (edat1 && (entry & REGION_ENTRY_P)) {
            *flags &= ~PAGE_WRITE;
        }
        if (edat2 && (entry & REGION3_ENTRY_FC)) {
            if (iep && (entry & REGION3_ENTRY_IEP)) {
                *flags &= ~PAGE_EXEC;
            }
            *raddr = (entry & REGION3_ENTRY_RFAA) |
                     (vaddr & ~REGION3_ENTRY_RFAA);
            return 0;
        }
        gaddr = (entry & REGION_ENTRY_ORIGIN) +
                VADDR_SEGMENT_TX(vaddr) * 8;
        if (VADDR_SEGMENT_TL(vaddr) < (entry & REGION_ENTRY_TF) >> 6 ||
            VADDR_SEGMENT_TL(vaddr) > (entry & REGION_ENTRY_TL)) {
            *raddr = gaddr;
            *lra_cc = 3;
            return PGM_SEGMENT_TRANS;
        }
        /* fall through */
    case ASCE_TYPE_SEGMENT:
        if (!read_table_entry(env, gaddr, &entry)) {
            return PGM_ADDRESSING;
        }
        if (entry & SEGMENT_ENTRY_I) {
            *raddr = gaddr;
            *lra_cc = 1;
            return PGM_SEGMENT_TRANS;
        }
        if ((entry & SEGMENT_ENTRY_TT) != SEGMENT_ENTRY_TT_SEGMENT) {
            return PGM_TRANS_SPEC;
        }
        if ((entry & SEGMENT_ENTRY_CS) && asce_p) {
            return PGM_TRANS_SPEC;
        }
        if (entry & SEGMENT_ENTRY_P) {
            *flags &= ~PAGE_WRITE;
        }
        if (edat1 && (entry & SEGMENT_ENTRY_FC)) {
            if (iep && (entry & SEGMENT_ENTRY_IEP)) {
                *flags &= ~PAGE_EXEC;
            }
            *raddr = (entry & SEGMENT_ENTRY_SFAA) |
                     (vaddr & ~SEGMENT_ENTRY_SFAA);
            return 0;
        }
        gaddr = (entry & SEGMENT_ENTRY_ORIGIN) + VADDR_PAGE_TX(vaddr) * 8;
        break;
    }

    if (!read_table_entry(env, gaddr, &entry)) {
        return PGM_ADDRESSING;
    }
    if (entry & PAGE_ENTRY_I) {
        *raddr = gaddr;
        *lra_cc = 2;
        return PGM_PAGE_TRANS;
    }
    if (entry & PAGE_ENTRY_0) {
        return PGM_TRANS_SPEC;
    }
    if (entry & PAGE_ENTRY_P) {
        *flags &= ~PAGE_WRITE;
    }
    if (iep && (entry & PAGE_ENTRY_IEP)) {
        *flags &= ~PAGE_EXEC;
    }

    *raddr = entry & TARGET_PAGE_MASK;
    return 0;
}

static bool skey_store_protected(const CPUS390XState *env, uint8_t skey,
                                 int access_key)
{
    if (access_key < 0 || !access_key ||
        access_key == (skey & SK_ACC_MASK)) {
        return false;
    }
    return !((env->cregs[0] & CR0_STORE_PROT_OVERRIDE) &&
             (skey & SK_ACC_MASK) == 0x90);
}

static bool skey_fetch_protected(const CPUS390XState *env, uint64_t addr,
                                 bool private, uint8_t skey,
                                 int access_key)
{
    if (access_key < 0 || !access_key ||
        access_key == (skey & SK_ACC_MASK) || !(skey & SK_F)) {
        return false;
    }
    if ((env->cregs[0] & CR0_FETCH_PROT_OVERRIDE) && addr < 2048 && !private) {
        return false;
    }
    return !((env->cregs[0] & CR0_STORE_PROT_OVERRIDE) &&
             (skey & SK_ACC_MASK) == 0x90);
}

static bool mmu_handle_skey(CPUS390XState *env, uint64_t logical_addr,
                            bool private, hwaddr addr, int rw, int access_key,
                            int *flags)
{
    static S390SKeysClass *skeyclass;
    static S390SKeysState *ss;
    uint8_t key, old_key;

    /*
     * We expect to be called with an absolute address that has already been
     * validated, such that we can reliably use it to lookup the storage key.
     */
    if (unlikely(!ss)) {
        ss = s390_get_skeys_device();
        skeyclass = S390_SKEYS_GET_CLASS(ss);
    }

    /*
     * Don't enable storage keys if they are still disabled, i.e., no actual
     * storage key instruction was issued yet.
     */
    if (!skeyclass->skeys_are_enabled(ss)) {
        return false;
    }

    /*
     * Whenever we create a new TLB entry, we set the storage key reference
     * bit. In case we allow write accesses, we set the storage key change
     * bit. Whenever the guest changes the storage key, we have to flush the
     * TLBs of all CPUs (the whole TLB or all affected entries), so that the
     * next reference/change will result in an MMU fault and make us properly
     * update the storage key here.
     *
     * Note 1: "record of references ... is not necessarily accurate",
     *         "change bit may be set in case no storing has occurred".
     *         -> We can set reference/change bits even on exceptions.
     * Note 2: certain accesses seem to ignore storage keys. For example,
     *         DAT translation does not set reference bits for table accesses.
     *
     * TODO: we have races between getting and setting the key.
     */
    if (s390_skeys_get(ss, addr / TARGET_PAGE_SIZE, 1, &key)) {
        return false;
    }
    old_key = key;

    if (rw == MMU_DATA_STORE) {
        if (skey_store_protected(env, key, access_key)) {
            return true;
        }
    } else if (skey_fetch_protected(env, logical_addr, private, key,
                                    access_key)) {
        return true;
    }

    switch (rw) {
    case MMU_DATA_LOAD:
    case MMU_INST_FETCH:
        /*
         * The TLB entry has to remain write-protected on read-faults if
         * the storage key does not indicate a change already. Otherwise
         * we might miss setting the change bit on write accesses.
         */
        if (!(key & SK_C)) {
            *flags &= ~PAGE_WRITE;
        }
        if (skey_store_protected(env, key, access_key)) {
            *flags &= ~PAGE_WRITE;
        }
        break;
    case MMU_DATA_STORE:
        key |= SK_C;
        break;
    default:
        g_assert_not_reached();
    }

    /* Any store/fetch sets the reference bit */
    key |= SK_R;

    if (key != old_key) {
        s390_skeys_set(ss, addr / TARGET_PAGE_SIZE, 1, &key);
    }
    return false;
}

/**
 * Translate a virtual (logical) address into a physical (absolute) address.
 * @param vaddr  the virtual address
 * @param rw     0 = read, 1 = write, 2 = code fetch, < 0 = load real address
 * @param asc    address space control (one of the PSW_ASC_* modes)
 * @param access_key  storage access key in bits 0-3, followed by four zeroes;
 *                    a negative value bypasses key-controlled protection
 * @param raddr  the translated address is stored to this pointer
 * @param flags  the PAGE_READ/WRITE/EXEC flags are stored to this pointer
 * @param tec    the translation exception code if stored to this pointer if
 *               there is an exception to raise
 * @param lra_cc the LRA condition code for a translation exception, or -1
 *               when the exception code must be returned; may be NULL
 * @return       0 = success, != 0, the exception to raise
 */
int mmu_translate_with_key(CPUS390XState *env, vaddr vaddr, int rw,
                           uint64_t asc, int access_key, hwaddr *raddr,
                           int *flags, uint64_t *tec, int *lra_cc)
{
    uint64_t logical_addr = vaddr;
    uint64_t asc_mode = asc & PSW_MASK_ASC;
    uint64_t asce = 0;
    unsigned int arn = asc & 0xf;
    bool art_fetch_only = false;
    int unused_lra_cc;
    int r;

    *tec = (vaddr & TARGET_PAGE_MASK) | (asc_mode >> 46) |
            (rw == MMU_DATA_STORE ? FS_WRITE : FS_READ);
    *flags = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    if (!lra_cc) {
        lra_cc = &unused_lra_cc;
    }
    *lra_cc = -1;

    vaddr &= TARGET_PAGE_MASK;

    if (rw != MMU_S390_LRA && !(env->psw.mask & PSW_MASK_DAT)) {
        *raddr = vaddr;
    } else {
        switch (asc_mode) {
        case PSW_ASC_PRIMARY:
            asce = env->cregs[1];
            break;
        case PSW_ASC_HOME:
            asce = env->cregs[13];
            break;
        case PSW_ASC_SECONDARY:
            asce = env->cregs[7];
            break;
        case PSW_ASC_ACCREG:
            r = mmu_translate_arn(env, arn, rw, &asce, &art_fetch_only);
            if (r) {
                return r;
            }
            break;
        default:
            g_assert_not_reached();
        }
    }

    if (is_low_address(vaddr & TARGET_PAGE_MASK) &&
        lowprot_enabled(env, asce)) {
        /*
         * If any part of this page is currently protected, make sure the
         * TLB entry will not be reused.
         */
        *flags |= PAGE_WRITE_INV;
        if (is_low_address(logical_addr) && rw == MMU_DATA_STORE) {
            *tec |= 0x80;
            return PGM_PROTECTION;
        }
    }

    if (env->psw.mask & PSW_MASK_DAT || rw == MMU_S390_LRA) {
        r = mmu_translate_asce(env, vaddr, asc_mode, asce, raddr, flags,
                               lra_cc);
        if (unlikely(r)) {
            return r;
        }

        if (art_fetch_only) {
            *flags &= ~PAGE_WRITE;
        }
        if (unlikely(rw == MMU_DATA_STORE && !(*flags & PAGE_WRITE))) {
            *tec |= 0x4;
            return PGM_PROTECTION;
        }
        if (unlikely(rw == MMU_INST_FETCH && !(*flags & PAGE_EXEC))) {
            *tec |= 0x84;
            return PGM_PROTECTION;
        }
    }

    if (rw >= 0) {
        /* Convert real address -> absolute address */
        *raddr = mmu_real2abs(env, *raddr);

        if (!mmu_absolute_addr_valid(*raddr, rw == MMU_DATA_STORE)) {
            *tec = 0; /* unused */
            return PGM_ADDRESSING;
        }

        if (mmu_handle_skey(env, logical_addr, asce & ASCE_PRIVATE_SPACE,
                            *raddr, rw, access_key, flags)) {
            return PGM_PROTECTION;
        }
    }
    return 0;
}

int s390_tprot(CPUS390XState *env, vaddr addr, uint64_t asc,
               uint8_t access_key, uint64_t *tec)
{
    static S390SKeysClass *skeyclass;
    static S390SKeysState *ss;
    uint64_t asc_mode = asc & PSW_MASK_ASC;
    uint64_t asce = 0;
    hwaddr raddr;
    uint8_t skey = 0;
    bool private = false;
    bool fetch_only = false;
    int flags;
    int exc;

    exc = mmu_translate_with_key(env, addr, MMU_S390_TPROT, asc, -1,
                                 &raddr, &flags, tec, NULL);
    if (exc) {
        return -exc;
    }

    if (env->psw.mask & PSW_MASK_DAT) {
        switch (asc_mode) {
        case PSW_ASC_PRIMARY:
            asce = env->cregs[1];
            break;
        case PSW_ASC_SECONDARY:
            asce = env->cregs[7];
            break;
        case PSW_ASC_HOME:
            asce = env->cregs[13];
            break;
        case PSW_ASC_ACCREG:
            exc = mmu_translate_arn(env, asc & 0xf, MMU_S390_TPROT,
                                    &asce, &fetch_only);
            if (exc) {
                return -exc;
            }
            break;
        default:
            g_assert_not_reached();
        }
        private = asce & ASCE_PRIVATE_SPACE;
    }

    raddr = mmu_real2abs(env, raddr);
    if (!mmu_absolute_addr_valid(raddr, false)) {
        return -PGM_ADDRESSING;
    }

    if (!ss) {
        ss = s390_get_skeys_device();
        skeyclass = S390_SKEYS_GET_CLASS(ss);
    }
    if (skeyclass->skeys_are_enabled(ss)) {
        s390_skeys_get(ss, raddr / TARGET_PAGE_SIZE, 1, &skey);
    }

    if (skey_fetch_protected(env, addr, private, skey, access_key)) {
        return 2;
    }
    if (fetch_only || !(flags & PAGE_WRITE) ||
        (is_low_address(addr) && lowprot_enabled(env, asce)) ||
        skey_store_protected(env, skey, access_key)) {
        return 1;
    }
    return 0;
}

int mmu_translate(CPUS390XState *env, vaddr vaddr, int rw, uint64_t asc,
                  hwaddr *raddr, int *flags, uint64_t *tec, int *lra_cc)
{
    int access_key =
        ((env->psw.mask & PSW_MASK_KEY) >> PSW_SHIFT_KEY) << 4;

    return mmu_translate_with_key(env, vaddr, rw, asc, access_key, raddr,
                                  flags, tec, lra_cc);
}

/**
 * translate_pages: Translate a set of consecutive logical page addresses
 * to absolute addresses. This function is used for TCG and old KVM without
 * the MEMOP interface.
 */
static int translate_pages(S390CPU *cpu, vaddr addr, uint8_t ar, int nr_pages,
                           hwaddr *pages, bool is_write, uint64_t *tec)
{
    uint64_t asc = cpu->env.psw.mask & PSW_MASK_ASC;
    CPUS390XState *env = &cpu->env;
    int ret, i, pflags;

    if (asc == PSW_ASC_ACCREG) {
        asc |= ar;
    }

    for (i = 0; i < nr_pages; i++) {
        ret = mmu_translate(env, addr, is_write, asc, &pages[i], &pflags, tec,
                            NULL);
        if (ret) {
            return ret;
        }
        addr += TARGET_PAGE_SIZE;
    }

    return 0;
}

int s390_cpu_pv_mem_rw(S390CPU *cpu, unsigned int offset, void *hostbuf,
                       int len, bool is_write)
{
    int ret;

    if (kvm_enabled()) {
        ret = kvm_s390_mem_op_pv(cpu, offset, hostbuf, len, is_write);
    } else {
        /* Protected Virtualization is a KVM/Hardware only feature */
        g_assert_not_reached();
    }
    return ret;
}

/**
 * s390_cpu_virt_mem_rw:
 * @laddr:     the logical start address
 * @ar:        the access register number
 * @hostbuf:   buffer in host memory. NULL = do only checks w/o copying
 * @len:       length that should be transferred
 * @is_write:  true = write, false = read
 * Returns:    0 on success, non-zero if an exception occurred
 *
 * Copy from/to guest memory using logical addresses. Note that we inject a
 * program interrupt in case there is an error while accessing the memory.
 *
 * This function will always return (also for TCG), make sure to call
 * s390_cpu_virt_mem_handle_exc() to properly exit the CPU loop.
 */
int s390_cpu_virt_mem_rw(S390CPU *cpu, vaddr laddr, uint8_t ar, void *hostbuf,
                         int len, bool is_write)
{
    const MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    uint8_t arn = (cpu->env.psw.mask & PSW_MASK_ASC) == PSW_ASC_ACCREG ?
                  ar : 0;
    int currlen, nr_pages, i;
    hwaddr *pages;
    uint64_t tec;
    int ret;

    if (kvm_enabled()) {
        ret = kvm_s390_mem_op(cpu, laddr, ar, hostbuf, len, is_write);
        if (ret >= 0) {
            return ret;
        }
    }

    nr_pages = (((laddr & ~TARGET_PAGE_MASK) + len - 1) >> TARGET_PAGE_BITS)
               + 1;
    pages = g_malloc(nr_pages * sizeof(*pages));

    ret = translate_pages(cpu, laddr, ar, nr_pages, pages, is_write, &tec);
    if (ret == 0 && hostbuf != NULL) {
        AddressSpace *as = CPU(cpu)->as;

        /* Copy data by stepping through the area page by page */
        for (i = 0; i < nr_pages; i++) {
            MemTxResult res;

            currlen = MIN(len, TARGET_PAGE_SIZE - (laddr % TARGET_PAGE_SIZE));
            res = address_space_rw(as, pages[i] | (laddr & ~TARGET_PAGE_MASK),
                                   attrs, hostbuf, currlen, is_write);
            if (res != MEMTX_OK) {
                ret = PGM_ADDRESSING;
                break;
            }
            laddr += currlen;
            hostbuf += currlen;
            len -= currlen;
        }
    }
    if (ret) {
        trigger_access_exception(&cpu->env, ret, tec, arn);
    }

    g_free(pages);
    return ret;
}

static void s390_tx_discard_pages(CPUS390XState *env)
{
    for (unsigned int i = 0; i < env->tx_page_count; i++) {
        g_free(env->tx_page_data[i]);
        env->tx_page_data[i] = NULL;
        env->tx_pages[i] = 0;
    }
    env->tx_page_count = 0;
}

void s390_tx_reset(CPUS390XState *env)
{
    s390_tx_discard_pages(env);
    env->tx_depth = 0;
    env->tx_constrained = false;
    env->tx_gprmask = 0;
    env->tx_start_addr = 0;
}

void s390_tx_begin(CPUS390XState *env, uint64_t start_addr, uint8_t gprmask)
{
    s390_tx_reset(env);
    memcpy(env->tx_saved_regs, env->regs, sizeof(env->tx_saved_regs));
    env->tx_start_addr = start_addr;
    env->tx_gprmask = gprmask;
    env->tx_depth = 1;
    env->tx_constrained = true;

    /*
     * Every transactional store must fault through s390_cpu_tlb_fill() at
     * least once so that the original page can be retained for rollback.
     */
    tlb_flush(env_cpu(env));
}

int s390_tx_track_page(CPUS390XState *env, hwaddr page)
{
    AddressSpace *as = env_cpu(env)->as;
    uint8_t *copy;

    page &= TARGET_PAGE_MASK;
    for (unsigned int i = 0; i < env->tx_page_count; i++) {
        if (env->tx_pages[i] == page) {
            return 0;
        }
    }
    if (env->tx_page_count == S390_TX_MAX_PAGES) {
        return PGM_TRANSACTION_CONSTRAINT;
    }

    copy = g_malloc(TARGET_PAGE_SIZE);
    if (address_space_read(as, page, MEMTXATTRS_UNSPECIFIED, copy,
                           TARGET_PAGE_SIZE) != MEMTX_OK) {
        g_free(copy);
        return PGM_ADDRESSING;
    }
    env->tx_pages[env->tx_page_count] = page;
    env->tx_page_data[env->tx_page_count] = copy;
    env->tx_page_count++;
    return 0;
}

void s390_tx_commit(CPUS390XState *env)
{
    s390_tx_discard_pages(env);
    env->tx_depth = 0;
    env->tx_constrained = false;
    env->tx_gprmask = 0;
    env->tx_start_addr = 0;
    tlb_flush(env_cpu(env));
}

void s390_tx_abort(CPUS390XState *env)
{
    AddressSpace *as = env_cpu(env)->as;
    uint8_t mask = env->tx_gprmask;
    uint64_t start_addr = env->tx_start_addr;

    for (unsigned int i = 0; i < env->tx_page_count; i++) {
        address_space_write(as, env->tx_pages[i], MEMTXATTRS_UNSPECIFIED,
                            env->tx_page_data[i], TARGET_PAGE_SIZE);
    }
    for (unsigned int i = 0; i < 16; i += 2, mask <<= 1) {
        if (mask & 0x80) {
            env->regs[i] = env->tx_saved_regs[i];
            env->regs[i + 1] = env->tx_saved_regs[i + 1];
        }
    }
    s390_tx_reset(env);
    env->psw.addr = start_addr;
    tlb_flush(env_cpu(env));
}

void s390_cpu_virt_mem_handle_exc(S390CPU *cpu, uintptr_t ra)
{
    /* KVM will handle the interrupt automatically, TCG has to exit the TB */
#ifdef CONFIG_TCG
    if (tcg_enabled()) {
        cpu_loop_exit_restore(CPU(cpu), ra);
    }
#endif
}

/**
 * Translate a real address into a physical (absolute) address.
 * @param raddr  the real address
 * @param rw     0 = read, 1 = write, 2 = code fetch
 * @param addr   the translated address is stored to this pointer
 * @param flags  the PAGE_READ/WRITE/EXEC flags are stored to this pointer
 * @return       0 = success, != 0, the exception to raise
 */
int mmu_translate_real_with_key(CPUS390XState *env, hwaddr raddr, int rw,
                                int access_key, hwaddr *addr, int *flags,
                                uint64_t *tec)
{
    const bool lowprot_enabled = env->cregs[0] & CR0_LOWPROT;

    *flags = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    if (is_low_address(raddr & TARGET_PAGE_MASK) && lowprot_enabled) {
        /* see comment in mmu_translate() how this works */
        *flags |= PAGE_WRITE_INV;
        if (is_low_address(raddr) && rw == MMU_DATA_STORE) {
            /* LAP sets bit 56 */
            *tec = (raddr & TARGET_PAGE_MASK) | FS_WRITE | 0x80;
            return PGM_PROTECTION;
        }
    }

    *addr = mmu_real2abs(env, raddr & TARGET_PAGE_MASK);

    if (!mmu_absolute_addr_valid(*addr, rw == MMU_DATA_STORE)) {
        /* unused */
        *tec = 0;
        return PGM_ADDRESSING;
    }

    if (mmu_handle_skey(env, raddr, false, *addr, rw, access_key, flags)) {
        return PGM_PROTECTION;
    }
    return 0;
}

int mmu_translate_real(CPUS390XState *env, hwaddr raddr, int rw,
                       hwaddr *addr, int *flags, uint64_t *tec)
{
    int access_key =
        ((env->psw.mask & PSW_MASK_KEY) >> PSW_SHIFT_KEY) << 4;

    return mmu_translate_real_with_key(env, raddr, rw, access_key,
                                       addr, flags, tec);
}
