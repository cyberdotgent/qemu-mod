/*
 * S390x DIAG instruction helper functions
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
#include "cpu.h"
#include "s390x-internal.h"
#include "hw/core/boards.h"
#include "hw/s390x/ebcdic.h"
#include "hw/watchdog/wdt_diag288.h"
#include "system/cpus.h"
#include "hw/s390x/ipl.h"
#include "hw/s390x/s390-virtio-ccw.h"
#include "hw/s390x/tod.h"
#include "system/kvm.h"
#include "kvm/kvm_s390x.h"
#include "target/s390x/kvm/pv.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/units.h"

#define DIAG204_SUBCODE_STIB4  0x00000004
#define DIAG204_SUBCODE_RSI    0x00010005
#define DIAG204_SUBCODE_STIB6  0x00010006
#define DIAG204_SUBCODE_STIB7  0x00010007

#define DIAG204_RC_OK          0
#define DIAG204_RC_UNSUPPORTED 4
#define DIAG204_CPU_ONLINE     0x20
#define DIAG204_SHARED_WEIGHT  100
#define DIAG204_X_WEIGHT       1000
#define DIAG204_DED_WEIGHT     0xffff

typedef struct QEMU_PACKED Diag204Header {
    uint8_t partitions;
    uint8_t flags;
    uint16_t time_slice;
    uint16_t physical_cpus;
    uint16_t own_partition_offset;
    uint64_t tod;
} Diag204Header;

typedef struct QEMU_PACKED Diag204Partition {
    uint8_t partition_number;
    uint8_t cpus;
    uint8_t reserved[6];
    uint8_t name[8];
} Diag204Partition;

typedef struct QEMU_PACKED Diag204CPU {
    uint16_t address;
    uint8_t reserved[2];
    uint8_t type_index;
    uint8_t flags;
    uint16_t weight;
    uint64_t accumulated_time;
    uint64_t lpar_time;
} Diag204CPU;

typedef struct QEMU_PACKED Diag204XHeader {
    uint8_t partitions;
    uint8_t flags;
    uint16_t time_slice;
    uint16_t physical_cpus;
    uint16_t own_partition_offset;
    uint64_t tod_high;
    uint64_t tod_low;
    uint8_t reserved[40];
} Diag204XHeader;

typedef struct QEMU_PACKED Diag204XPartition {
    uint8_t partition_number;
    uint8_t cpus;
    uint8_t real_cpus;
    uint8_t flags;
    uint32_t machine_limit;
    uint8_t name[8];
    uint8_t cpc_name[8];
    uint8_t os_name[8];
    uint64_t central_storage_mb;
    uint64_t expanded_storage_mb;
    uint8_t user_partition_id;
    uint8_t mtid;
    uint8_t reserved1[2];
    uint32_t group_machine_limit;
    uint8_t group_name[8];
    uint8_t hardware_group_name[8];
    uint8_t reserved2[24];
} Diag204XPartition;

typedef struct QEMU_PACKED Diag204XCPU {
    uint16_t address;
    uint8_t reserved1[2];
    uint8_t type_index;
    uint8_t flags;
    uint16_t weight;
    uint64_t accumulated_time;
    uint64_t lpar_time;
    uint16_t minimum_weight;
    uint16_t current_weight;
    uint16_t maximum_weight;
    uint8_t reserved2[2];
    uint64_t online_time;
    uint64_t wait_time;
    uint32_t pma_weight;
    uint32_t polar_weight;
    uint32_t cpu_type_cap;
    uint32_t group_cpu_type_cap;
    uint8_t reserved3[32];
} Diag204XCPU;

QEMU_BUILD_BUG_ON(sizeof(Diag204Header) != 16);
QEMU_BUILD_BUG_ON(sizeof(Diag204Partition) != 16);
QEMU_BUILD_BUG_ON(sizeof(Diag204CPU) != 24);
QEMU_BUILD_BUG_ON(sizeof(Diag204XHeader) != 64);
QEMU_BUILD_BUG_ON(sizeof(Diag204XPartition) != 96);
QEMU_BUILD_BUG_ON(sizeof(Diag204XCPU) != 96);

static uint8_t diag204_cpu_count(void)
{
    CPUState *cs;
    unsigned int count = 0;

    CPU_FOREACH(cs) {
        count++;
    }
    return count;
}

static void diag204_ebcdic_name(uint8_t *dest, size_t len, const char *name)
{
    size_t name_len = MIN(strlen(name), len);

    memset(dest, 0x40, len);
    ebcdic_put(dest, name, name_len);
}

static const char *diag204_partition_name(void)
{
    return kvm_enabled() ? "QEMU KVM" : "QEMU TCG";
}

static void diag204_get_tod(uint64_t *tod, uint64_t *etod_high,
                            uint64_t *etod_low)
{
    S390TODState *td = s390_get_todstate();
    S390TODClass *tdc = S390_TOD_GET_CLASS(td);
    S390TOD value;

    tdc->get(td, &value, &error_abort);
    *tod = value.low;
    *etod_high = value.low >> 8;
    *etod_low = (value.low << 56) | 0x10000;
}

static void diag204_set_return(CPUS390XState *env, uint64_t r3, uint32_t rc)
{
    env->regs[r3] = deposit64(env->regs[r3], 0, 32, rc);
    if (kvm_enabled()) {
        setcc(env_archcpu(env), 0);
    }
}

static void *diag204_build_simple(size_t *size)
{
    uint8_t cpu_count = diag204_cpu_count();
    size_t data_size = sizeof(Diag204Header) + sizeof(Diag204Partition) +
                       cpu_count * sizeof(Diag204CPU);
    Diag204Partition *partition;
    Diag204Header *header;
    CPUState *cs;
    Diag204CPU *entry;
    uint64_t tod, unused_high, unused_low;
    uint8_t *data;

    if (data_size > TARGET_PAGE_SIZE) {
        return NULL;
    }
    data = g_malloc0(TARGET_PAGE_SIZE);
    header = (Diag204Header *)data;
    header->partitions = 1;
    header->physical_cpus = cpu_to_be16(cpu_count);
    header->own_partition_offset = cpu_to_be16(sizeof(*header));
    diag204_get_tod(&tod, &unused_high, &unused_low);
    header->tod = cpu_to_be64(tod);

    partition = (Diag204Partition *)(header + 1);
    partition->partition_number = 1;
    partition->cpus = cpu_count;
    diag204_ebcdic_name(partition->name, sizeof(partition->name),
                        diag204_partition_name());

    entry = (Diag204CPU *)(partition + 1);
    CPU_FOREACH(cs) {
        S390CPU *cpu = S390_CPU(cs);

        entry->address = cpu_to_be16(cpu->env.core_id);
        entry->flags = DIAG204_CPU_ONLINE;
        entry->weight = cpu_to_be16(cpu->env.dedicated ?
                                    DIAG204_DED_WEIGHT :
                                    DIAG204_SHARED_WEIGHT);
        entry++;
    }
    *size = TARGET_PAGE_SIZE;
    return data;
}

static void *diag204_build_extended(size_t *size)
{
    const MachineState *ms = MACHINE(qdev_get_machine());
    uint8_t cpu_count = diag204_cpu_count();
    size_t data_size = sizeof(Diag204XHeader) + sizeof(Diag204XPartition) +
                       cpu_count * sizeof(Diag204XCPU);
    size_t alloc_size = ROUND_UP(data_size, TARGET_PAGE_SIZE);
    uint64_t uptime_us = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;
    Diag204XPartition *partition;
    Diag204XHeader *header;
    CPUState *cs;
    Diag204XCPU *entry;
    uint64_t tod, etod_high, etod_low;
    uint8_t *data = g_malloc0(alloc_size);

    header = (Diag204XHeader *)data;
    header->partitions = 1;
    header->physical_cpus = cpu_to_be16(cpu_count);
    header->own_partition_offset = cpu_to_be16(sizeof(*header));
    diag204_get_tod(&tod, &etod_high, &etod_low);
    header->tod_high = cpu_to_be64(etod_high);
    header->tod_low = cpu_to_be64(etod_low);

    partition = (Diag204XPartition *)(header + 1);
    partition->partition_number = 1;
    partition->cpus = cpu_count;
    partition->real_cpus = cpu_count;
    diag204_ebcdic_name(partition->name, sizeof(partition->name),
                        diag204_partition_name());
    diag204_ebcdic_name(partition->cpc_name, sizeof(partition->cpc_name),
                        "QEMU");
    diag204_ebcdic_name(partition->os_name, sizeof(partition->os_name),
                        "QEMU");
    diag204_ebcdic_name(partition->group_name,
                        sizeof(partition->group_name), "");
    diag204_ebcdic_name(partition->hardware_group_name,
                        sizeof(partition->hardware_group_name), "");
    partition->central_storage_mb = cpu_to_be64(ms->ram_size / MiB);

    entry = (Diag204XCPU *)(partition + 1);
    CPU_FOREACH(cs) {
        S390CPU *cpu = S390_CPU(cs);
        uint16_t weight = cpu->env.dedicated ? DIAG204_DED_WEIGHT :
                                              DIAG204_X_WEIGHT;

        entry->address = cpu_to_be16(cpu->env.core_id);
        entry->flags = DIAG204_CPU_ONLINE;
        entry->weight = cpu_to_be16(cpu->env.dedicated ?
                                    DIAG204_DED_WEIGHT :
                                    DIAG204_SHARED_WEIGHT);
        entry->minimum_weight = cpu_to_be16(weight);
        entry->current_weight = cpu_to_be16(weight);
        entry->maximum_weight = cpu_to_be16(weight);
        entry->online_time = cpu_to_be64(uptime_us);
        entry->wait_time = cpu_to_be64(uptime_us);
        entry->pma_weight = cpu_to_be32(weight);
        entry->polar_weight = cpu_to_be32(weight);
        entry++;
    }
    *size = alloc_size;
    return data;
}

bool handle_diag_204(CPUS390XState *env, uint64_t r1, uint64_t r3,
                     uintptr_t ra)
{
    S390CPU *cpu = env_archcpu(env);
    uint32_t subcode = env->regs[r3];
    uint64_t addr = wrap_address(env, env->regs[r1]);
    uint32_t pages = env->regs[(r3 + 1) & 15];
    AddressSpace *as = cpu_get_address_space(CPU(cpu), 0);
    MemTxResult tx_result;
    size_t size;
    void *data;

    switch (subcode) {
    case DIAG204_SUBCODE_RSI:
        {
            BQL_LOCK_GUARD();

            size = ROUND_UP(sizeof(Diag204XHeader) +
                            sizeof(Diag204XPartition) +
                            diag204_cpu_count() * sizeof(Diag204XCPU),
                            TARGET_PAGE_SIZE);
        }
        env->regs[(r3 + 1) & 15] =
            deposit64(env->regs[(r3 + 1) & 15], 0, 32,
                      size / TARGET_PAGE_SIZE);
        diag204_set_return(env, r3, DIAG204_RC_OK);
        return true;
    case DIAG204_SUBCODE_STIB4:
        addr = mmu_real2abs(env, addr);
        if (addr & ~TARGET_PAGE_MASK) {
            s390_program_interrupt(env, PGM_SPECIFICATION, ra);
            return false;
        }
        {
            BQL_LOCK_GUARD();

            data = diag204_build_simple(&size);
        }
        if (!data) {
            diag204_set_return(env, r3, DIAG204_RC_UNSUPPORTED);
            return true;
        }
        if (!address_space_access_valid(as, addr, size, true,
                                        MEMTXATTRS_UNSPECIFIED)) {
            g_free(data);
            s390_program_interrupt(env, PGM_ADDRESSING, ra);
            return false;
        }
        tx_result = address_space_write(as, addr, MEMTXATTRS_UNSPECIFIED,
                                        data, size);
        g_free(data);
        if (tx_result != MEMTX_OK) {
            s390_program_interrupt(env, PGM_ADDRESSING, ra);
            return false;
        }
        break;
    case DIAG204_SUBCODE_STIB6:
    case DIAG204_SUBCODE_STIB7:
        if (addr & ~TARGET_PAGE_MASK) {
            s390_program_interrupt(env, PGM_SPECIFICATION, ra);
            return false;
        }
        {
            BQL_LOCK_GUARD();

            data = diag204_build_extended(&size);
        }
        if (pages < size / TARGET_PAGE_SIZE) {
            g_free(data);
            diag204_set_return(env, r3, DIAG204_RC_UNSUPPORTED);
            return true;
        }
        if (s390_cpu_virt_mem_write(cpu, addr, r1, data, size)) {
            g_free(data);
            s390_cpu_virt_mem_handle_exc(cpu, ra);
            return false;
        }
        g_free(data);
        break;
    default:
        diag204_set_return(env, r3, DIAG204_RC_UNSUPPORTED);
        return true;
    }

    diag204_set_return(env, r3, DIAG204_RC_OK);
    return true;
}


int handle_diag_288(CPUS390XState *env, uint64_t r1, uint64_t r3)
{
    uint64_t func = env->regs[r1];
    uint64_t timeout = env->regs[r1 + 1];
    uint64_t action = env->regs[r3];
    Object *obj;
    DIAG288State *diag288;
    DIAG288Class *diag288_class;

    if (r1 % 2 || action != 0) {
        return -1;
    }

    /* Timeout must be more than 15 seconds except for timer deletion */
    if (func != WDT_DIAG288_CANCEL && timeout < 15) {
        return -1;
    }

    obj = object_resolve_path_type("", TYPE_WDT_DIAG288, NULL);
    if (!obj) {
        return -1;
    }

    diag288 = DIAG288(obj);
    diag288_class = DIAG288_GET_CLASS(diag288);
    return diag288_class->handle_timer(diag288, func, timeout);
}

static int diag308_parm_check(CPUS390XState *env, uint64_t r1, uint64_t addr,
                              uintptr_t ra, bool write)
{
    /* Handled by the Ultravisor */
    if (s390_is_pv()) {
        return 0;
    }
    if ((r1 & 1) || (addr & ~TARGET_PAGE_MASK)) {
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return -1;
    }
    if (!address_space_access_valid(&address_space_memory, addr,
                                    sizeof(IplParameterBlock), write,
                                    MEMTXATTRS_UNSPECIFIED)) {
        s390_program_interrupt(env, PGM_ADDRESSING, ra);
        return -1;
    }
    return 0;
}

static void s390_ipl_read(CPUS390XState *env, uint64_t addr,
                          IplParameterBlock *iplb, size_t size)
{
    if (s390_is_pv()) {
        s390_cpu_pv_mem_read(env_archcpu(env), 0, iplb, size);
    } else {
        address_space_read(cpu_get_address_space(env_cpu(env), 0), addr,
                           MEMTXATTRS_UNSPECIFIED, iplb, size);
    }
}

static void s390_ipl_write(CPUS390XState *env, uint64_t addr,
                           IplParameterBlock *iplb, size_t size)
{
    if (s390_is_pv()) {
        s390_cpu_pv_mem_write(env_archcpu(env), 0, iplb, size);
    } else {
        address_space_write(cpu_get_address_space(env_cpu(env), 0), addr,
                            MEMTXATTRS_UNSPECIFIED, iplb, size);
    }
}

bool handle_diag_308(CPUS390XState *env, uint64_t r1, uint64_t r3, uintptr_t ra)
{
    bool valid;
    CPUState *cs = env_cpu(env);
    uint64_t addr =  env->regs[r1];
    uint64_t subcode = env->regs[r3];
    IplParameterBlock *iplb;

    if (env->psw.mask & PSW_MASK_PSTATE) {
        s390_program_interrupt(env, PGM_PRIVILEGED, ra);
        return false;
    }

    if (subcode & ~0x0ffffULL) {
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return false;
    }

    if (subcode >= DIAG308_PV_SET && !s390_has_feat(S390_FEAT_UNPACK)) {
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return false;
    }

    switch (subcode) {
    case DIAG308_RESET_MOD_CLR:
        s390_ipl_reset_request(cs, S390_RESET_MODIFIED_CLEAR);
        return true;
    case DIAG308_RESET_LOAD_NORM:
        s390_ipl_reset_request(cs, S390_RESET_LOAD_NORMAL);
        return true;
    case DIAG308_LOAD_CLEAR:
        /* Well we still lack the clearing bit... */
        s390_ipl_reset_request(cs, S390_RESET_REIPL);
        return true;
    case DIAG308_SET:
    case DIAG308_PV_SET:
        if (diag308_parm_check(env, r1, addr, ra, false)) {
            return false;
        }
        iplb = g_new0(IplParameterBlock, 1);
        s390_ipl_read(env, addr, iplb, sizeof(iplb->len));
        if (!iplb_valid_len(iplb)) {
            env->regs[r1 + 1] = DIAG_308_RC_INVALID;
            goto out;
        }
        s390_ipl_read(env, addr, iplb, be32_to_cpu(iplb->len));

        valid = subcode == DIAG308_PV_SET ? iplb_valid_pv(iplb) : iplb_valid(iplb);
        if (!valid) {
            if (subcode == DIAG308_SET && iplb->pbt == S390_IPL_TYPE_QEMU_SCSI) {
                s390_rebuild_iplb(iplb->devno, iplb);
                s390_ipl_update_diag308(iplb);
                env->regs[r1 + 1] = DIAG_308_RC_OK;
            } else {
                env->regs[r1 + 1] = DIAG_308_RC_INVALID;
            }

            goto out;
        }

        s390_ipl_update_diag308(iplb);
        env->regs[r1 + 1] = DIAG_308_RC_OK;
out:
        g_free(iplb);
        return false;
    case DIAG308_STORE:
    case DIAG308_PV_STORE:
        if (diag308_parm_check(env, r1, addr, ra, true)) {
            return false;
        }
        if (subcode == DIAG308_PV_STORE) {
            iplb = s390_ipl_get_iplb_pv();
        } else {
            iplb = s390_ipl_get_iplb();
        }
        if (!iplb) {
            env->regs[r1 + 1] = DIAG_308_RC_NO_CONF;
            return false;
        }

        s390_ipl_write(env, addr, iplb, be32_to_cpu(iplb->len));
        env->regs[r1 + 1] = DIAG_308_RC_OK;
        return false;
    case DIAG308_PV_START:
        iplb = s390_ipl_get_iplb_pv();
        if (!iplb) {
            env->regs[r1 + 1] = DIAG_308_RC_NO_PV_CONF;
            return false;
        }

        if (kvm_enabled() && kvm_s390_get_hpage_1m()) {
            error_report("Protected VMs can currently not be backed with "
                         "huge pages");
            env->regs[r1 + 1] = DIAG_308_RC_INVAL_FOR_PV;
            return false;
        }

        s390_ipl_reset_request(cs, S390_RESET_PV);
        return true;
    default:
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return false;
    }
}
