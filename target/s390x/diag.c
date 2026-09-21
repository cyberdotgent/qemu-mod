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

#include <inttypes.h>
#include "qemu/osdep.h"
#include "cpu.h"
#include "s390x-internal.h"
#include "hw/core/boards.h"
#include "hw/s390x/css.h"
#include "hw/s390x/ebcdic.h"
#include "hw/s390x/sclp.h"
#include "hw/watchdog/wdt_diag288.h"
#include "system/cpus.h"
#include "hw/s390x/cert-store.h"
#include "hw/s390x/ipl.h"
#include "hw/s390x/ipl/diag320.h"
#include "hw/s390x/ipl/diag508.h"
#include "hw/s390x/s390-virtio-ccw.h"
#include "hw/s390x/tod.h"
#include "system/kvm.h"
#include "kvm/kvm_s390x.h"
#include "target/s390x/kvm/pv.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "crypto/x509-utils.h"

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

#define MSSF_READ_CONFIG_INFO  0x00020001
#define MSSF_READ_CHP_STATUS   0x00030001
#define MSSF_CONFIG_MIN_LEN    64
#define MSSF_CHP_STATUS_LEN    (8 + 32 + 32 + 32 + 152)
#define MSSF_RESP_REASON       6
#define MSSF_RESP_CODE         7
#define MSSF_REASON_COMPLETE   0x00
#define MSSF_RESPONSE_COMPLETE 0x10
#define MSSF_REASON_BAD_LENGTH 0x01
#define MSSF_RESPONSE_REJECT   0xf0
#define MSSF_REASON_UNASSIGNED 0x06

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

bool handle_diag_080(CPUS390XState *env, uint64_t r1, uint64_t r3,
                     uintptr_t ra)
{
    S390CPU *cpu = env_archcpu(env);
    AddressSpace *as = cpu_get_address_space(CPU(cpu), 0);
    MachineState *machine = MACHINE(qdev_get_machine());
    uint64_t addr = mmu_real2abs(env, (uint32_t)env->regs[r1]);
    uint32_t command = env->regs[r3];
    uint8_t header[8];
    g_autofree uint8_t *spccb = NULL;
    uint16_t length;
    MemTxResult tx_result;
    unsigned int ssid;
    unsigned int schid;

    if (addr & 7) {
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return false;
    }
    if (!address_space_access_valid(as, addr, sizeof(header), false,
                                    MEMTXATTRS_UNSPECIFIED)) {
        s390_program_interrupt(env, PGM_ADDRESSING, ra);
        return false;
    }
    tx_result = address_space_read(as, addr, MEMTXATTRS_UNSPECIFIED,
                                   header, sizeof(header));
    if (tx_result != MEMTX_OK) {
        s390_program_interrupt(env, PGM_ADDRESSING, ra);
        return false;
    }
    length = lduw_be_p(header);
    if (length < sizeof(header) ||
        !address_space_access_valid(as, addr, length, true,
                                    MEMTXATTRS_UNSPECIFIED)) {
        s390_program_interrupt(env, PGM_ADDRESSING, ra);
        return false;
    }
    spccb = g_malloc(length);
    tx_result = address_space_read(as, addr, MEMTXATTRS_UNSPECIFIED,
                                   spccb, length);
    if (tx_result != MEMTX_OK) {
        s390_program_interrupt(env, PGM_ADDRESSING, ra);
        return false;
    }

    if (addr & 0x7ff) {
        spccb[MSSF_RESP_REASON] = MSSF_REASON_BAD_LENGTH;
        spccb[MSSF_RESP_CODE] = 0;
    } else {
        switch (command) {
        case MSSF_READ_CONFIG_INFO:
            if (length < MAX(MSSF_CONFIG_MIN_LEN,
                             32 + machine->smp.max_cpus * 2)) {
                spccb[MSSF_RESP_REASON] = MSSF_REASON_BAD_LENGTH;
                spccb[MSSF_RESP_CODE] = MSSF_RESPONSE_REJECT;
                break;
            }
            /*
             * This legacy format has one-byte increment-count and
             * increment-size fields.  Pick an exact power-of-two increment
             * large enough to represent contemporary storage sizes.
             */
            {
                uint64_t memory_mb = machine->ram_size / MiB;
                uint64_t increment_mb = 1;
                uint64_t cpu_count = machine->smp.max_cpus;
                unsigned int i;

                while (DIV_ROUND_UP(memory_mb, increment_mb) > UINT8_MAX) {
                    increment_mb <<= 1;
                }
                memset(spccb + 8, 0, 24 + cpu_count * 2);
                spccb[8] = DIV_ROUND_UP(memory_mb, increment_mb);
                spccb[9] = increment_mb;
                spccb[10] = 0x04;
                spccb[11] = 0x01;
                stw_be_p(spccb + 16, cpu_count);
                stw_be_p(spccb + 18, 32);
                stw_be_p(spccb + 20, 0);
                stw_be_p(spccb + 22, 32 + cpu_count * 2);
                s390_ipl_convert_loadparm(
                    (char *)S390_CCW_MACHINE(machine)->loadparm, spccb + 24);
                for (i = 0; i < cpu_count; i++) {
                    spccb[32 + i * 2] = i;
                }
            }
            spccb[MSSF_RESP_REASON] = MSSF_REASON_COMPLETE;
            spccb[MSSF_RESP_CODE] = MSSF_RESPONSE_COMPLETE;
            break;
        case MSSF_READ_CHP_STATUS:
            if (length < MSSF_CHP_STATUS_LEN) {
                spccb[MSSF_RESP_REASON] = MSSF_REASON_BAD_LENGTH;
                spccb[MSSF_RESP_CODE] = MSSF_RESPONSE_REJECT;
                break;
            }
            memset(spccb + 8, 0, MSSF_CHP_STATUS_LEN - 8);
            for (ssid = 0; ssid <= MAX_SSID; ssid++) {
                for (schid = 0; schid <= MAX_SCHID; schid++) {
                    SubchDev *sch = css_find_subch(1, 0, ssid, schid);
                    SCHIB status;
                    unsigned int path;

                    if (!sch || !css_subch_visible(sch)) {
                        continue;
                    }
                    memcpy(&status, &sch->curr_status, sizeof(status));
                    if (!(status.pmcw.flags & PMCW_FLAGS_MASK_DNV)) {
                        continue;
                    }
                    for (path = 0;
                         path < ARRAY_SIZE(status.pmcw.chpid); path++) {
                        uint8_t bit = 0x80 >> path;
                        uint8_t chpid;

                        if (!(status.pmcw.pim & bit)) {
                            continue;
                        }
                        chpid = status.pmcw.chpid[path];
                        bit = 0x80 >> (chpid % 8);
                        spccb[8 + chpid / 8] |= bit;
                        spccb[40 + chpid / 8] |= bit;
                        spccb[72 + chpid / 8] |= bit;
                    }
                }
            }
            spccb[MSSF_RESP_REASON] = MSSF_REASON_COMPLETE;
            spccb[MSSF_RESP_CODE] = MSSF_RESPONSE_COMPLETE;
            break;
        default:
            spccb[MSSF_RESP_REASON] = MSSF_REASON_UNASSIGNED;
            spccb[MSSF_RESP_CODE] = MSSF_RESPONSE_REJECT;
            break;
        }
    }

    tx_result = address_space_write(as, addr, MEMTXATTRS_UNSPECIFIED,
                                    spccb, length);
    if (tx_result != MEMTX_OK) {
        s390_program_interrupt(env, PGM_ADDRESSING, ra);
        return false;
    }
    setcc(cpu, 0);
    sclp_service_interrupt(addr);
    return true;
}

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


static inline bool diag_parm_addr_valid(uint64_t addr, size_t size, bool write)
{
    return address_space_access_valid(&address_space_memory, addr,
                                      size, write, MEMTXATTRS_UNSPECIFIED);
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
    if (!diag_parm_addr_valid(addr, sizeof(IplParameterBlock), write)) {
        s390_program_interrupt(env, PGM_ADDRESSING, ra);
        return -1;
    }
    return 0;
}

static void s390_ipl_read(CPUS390XState *env, uint64_t addr,
                          void *data, size_t size)
{
    if (s390_is_pv()) {
        s390_cpu_pv_mem_read(env_archcpu(env), 0, data, size);
    } else {
        address_space_read(cpu_get_address_space(env_cpu(env), 0), addr,
                           MEMTXATTRS_UNSPECIFIED, data, size);
    }
}

static void s390_ipl_write(CPUS390XState *env, uint64_t addr,
                           void *data, size_t size)
{
    if (s390_is_pv()) {
        s390_cpu_pv_mem_write(env_archcpu(env), 0, data, size);
    } else {
        address_space_write(cpu_get_address_space(env_cpu(env), 0), addr,
                            MEMTXATTRS_UNSPECIFIED, data, size);
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

        if (kvm_enabled() && kvm_s390_get_hpage()) {
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

static int handle_diag320_query_vcsi(S390CPU *cpu, uint64_t addr, uint64_t r1,
                                     uintptr_t ra, S390IPLCertificateStore *cs)
{
    g_autofree VCStorageSizeBlock *vcssb = NULL;

    vcssb = g_new0(VCStorageSizeBlock, 1);
    if (s390_cpu_virt_mem_read(cpu, addr, r1, vcssb, sizeof(*vcssb))) {
        s390_cpu_virt_mem_handle_exc(cpu, ra);
        return -1;
    }

    if (be32_to_cpu(vcssb->length) != VCSSB_LEN_VALID) {
        return DIAG_320_RC_INVAL_VCSSB_LEN;
    }

    if (!cs->count) {
        vcssb->length = cpu_to_be32(VCSSB_NO_VC);
    } else {
        vcssb->version = 0;
        vcssb->total_vc_ct = cpu_to_be16(cs->count);
        vcssb->max_vc_ct = cpu_to_be16(MAX_CERTIFICATES);
        vcssb->max_single_vcb_len = cpu_to_be32(sizeof(VCBlockHeader) +
                                                sizeof(VCEntryHeader) +
                                                cs->largest_cert_size);
        vcssb->total_vcb_len = cpu_to_be32(sizeof(VCBlockHeader) +
                                           cs->count * sizeof(VCEntryHeader) +
                                           cs->total_bytes);
    }

    if (s390_cpu_virt_mem_write(cpu, addr, r1, vcssb, be32_to_cpu(vcssb->length))) {
        s390_cpu_virt_mem_handle_exc(cpu, ra);
        return -1;
    }
    return DIAG_320_RC_OK;
}

static bool is_cert_valid(const S390IPLCertificate *cert)
{
    int rc;
    Error *err = NULL;

    rc = qcrypto_x509_check_cert_times(cert->raw, cert->size, &err);
    if (rc != 0) {
        error_report_err(err);
        return false;
    }

    return true;
}

static int handle_key_id(VCEntry *vce, const S390IPLCertificate *cert)
{
    int rc;
    g_autofree unsigned char *key_id_data = NULL;
    size_t key_id_len;
    Error *err = NULL;

    rc = qcrypto_x509_get_cert_key_id(cert->raw, cert->size,
                                      QCRYPTO_HASH_ALGO_SHA256,
                                      &key_id_data, &key_id_len, &err);
    if (rc < 0) {
        error_report_err(err);
        return 0;
    }

    if (sizeof(VCEntryHeader) + key_id_len > be32_to_cpu(vce->vce_hdr.len)) {
        error_report("Unable to write key ID: exceeds buffer bounds");
        return 0;
    }

    vce->vce_hdr.keyid_len = cpu_to_be16(key_id_len);

    memcpy(vce->cert_buf, key_id_data, key_id_len);

    return ROUND_UP(key_id_len, 4);
}

static int handle_hash(VCEntry *vce, const S390IPLCertificate *cert,
                       uint16_t keyid_field_len)
{
    int rc;
    uint16_t hash_offset;
    g_autofree void *hash_data = NULL;
    size_t hash_len;
    Error *err = NULL;

    hash_len = CERT_HASH_LEN;
    hash_data = g_malloc0(hash_len);
    rc = qcrypto_get_x509_cert_fingerprint(cert->raw, cert->size,
                                           QCRYPTO_HASH_ALGO_SHA256,
                                           hash_data, &hash_len, &err);
    if (rc < 0) {
        error_report_err(err);
        return 0;
    }

    hash_offset = sizeof(VCEntryHeader) + keyid_field_len;
    if (hash_offset + hash_len > be32_to_cpu(vce->vce_hdr.len)) {
        error_report("Unable to write hash: exceeds buffer bounds");
        return 0;
    }

    vce->vce_hdr.hash_len = cpu_to_be16(hash_len);
    vce->vce_hdr.hash_type = DIAG_320_VCE_HASHTYPE_SHA2_256;
    vce->vce_hdr.hash_offset = cpu_to_be16(hash_offset);

    memcpy((uint8_t *)vce + hash_offset, hash_data, hash_len);

    return ROUND_UP(hash_len, 4);
}

static int handle_cert(VCEntry *vce, const S390IPLCertificate *cert,
                       uint16_t hash_field_len)
{
    int rc;
    uint16_t cert_offset;
    g_autofree uint8_t *cert_der = NULL;
    size_t der_size;
    Error *err = NULL;

    rc = qcrypto_x509_convert_cert_der(cert->raw, cert->size,
                                       &cert_der, &der_size, &err);
    if (rc < 0) {
        error_report_err(err);
        return 0;
    }

    cert_offset = be16_to_cpu(vce->vce_hdr.hash_offset) + hash_field_len;
    if (cert_offset + der_size > be32_to_cpu(vce->vce_hdr.len)) {
        error_report("Unable to write certificate: exceeds buffer bounds");
        return 0;
    }

    vce->vce_hdr.format = DIAG_320_VCE_FORMAT_X509_DER;
    vce->vce_hdr.cert_len = cpu_to_be32(der_size);
    vce->vce_hdr.cert_offset = cpu_to_be16(cert_offset);

    memcpy((uint8_t *)vce + cert_offset, cert_der, der_size);

    return ROUND_UP(der_size, 4);
}

static int get_key_type(const S390IPLCertificate *cert)
{
    int rc;
    Error *err = NULL;

    rc = qcrypto_x509_check_ecc_curve_p521(cert->raw, cert->size, &err);
    if (rc == -1) {
        error_report_err(err);
        return -1;
    }

    return (rc == 1) ? DIAG_320_VCE_KEYTYPE_ECDSA_P521 :
                       DIAG_320_VCE_KEYTYPE_SELF_DESCRIBING;
}

static int build_vce_header(VCEntry *vce, const S390IPLCertificate *cert, int idx)
{
    int key_type;

    vce->vce_hdr.len = cpu_to_be32(sizeof(VCEntryHeader));
    vce->vce_hdr.cert_idx = cpu_to_be16(idx + 1);
    memcpy(vce->vce_hdr.name, cert->name, CERT_NAME_MAX_LEN);

    if (!is_cert_valid(cert)) {
        return -1;
    }

    key_type = get_key_type(cert);
    if (key_type == -1) {
        return -1;
    }
    vce->vce_hdr.key_type = key_type;

    return 0;
}

static int build_vce_data(VCEntry *vce, const S390IPLCertificate *cert,
                          uint32_t vce_max_len)
{
    uint16_t keyid_field_len;
    uint16_t hash_field_len;
    uint32_t cert_field_len;
    uint32_t vce_len;

    vce->vce_hdr.len = cpu_to_be32(vce_max_len);

    keyid_field_len = handle_key_id(vce, cert);
    if (!keyid_field_len) {
        return -1;
    }

    hash_field_len = handle_hash(vce, cert, keyid_field_len);
    if (!hash_field_len) {
        return -1;
    }

    cert_field_len = handle_cert(vce, cert, hash_field_len);
    if (!cert_field_len) {
        return -1;
    }

    vce_len = sizeof(VCEntryHeader) + keyid_field_len + hash_field_len + cert_field_len;
    if (vce_len > vce_max_len) {
        return -1;
    }

    vce->vce_hdr.flags |= DIAG_320_VCE_FLAGS_VALID;

    /* Update vce length to reflect the actual size used by vce */
    vce->vce_hdr.len = cpu_to_be32(vce_len);

    return 0;
}

static int handle_diag320_store_vc(S390CPU *cpu, uint64_t addr, uint64_t r1, uintptr_t ra,
                                   S390IPLCertificateStore *cs)
{
    g_autofree VCBlockHeader *vcb_hdr = NULL;
    size_t remaining_space;
    uint16_t first_vc_index;
    uint16_t last_vc_index;
    int cs_start_index;
    int cs_end_index;
    uint32_t vce_max_len;
    uint32_t vce_len;
    uint32_t in_len;

    vcb_hdr = g_new0(VCBlockHeader, 1);
    if (s390_cpu_virt_mem_read(cpu, addr, r1, vcb_hdr, sizeof(*vcb_hdr))) {
        s390_cpu_virt_mem_handle_exc(cpu, ra);
        return -1;
    }

    in_len = be32_to_cpu(vcb_hdr->in_len);
    first_vc_index = be16_to_cpu(vcb_hdr->first_vc_index);
    last_vc_index = be16_to_cpu(vcb_hdr->last_vc_index);

    if (in_len % TARGET_PAGE_SIZE != 0) {
        return DIAG_320_RC_INVAL_VCB_LEN;
    }

    if (first_vc_index > last_vc_index) {
        return DIAG_320_RC_BAD_RANGE;
    }

    vcb_hdr->out_len = sizeof(VCBlockHeader);

    /*
     * DIAG 320 subcode 2 expects to query a certificate store that
     * maintains an index origin of 1. However, the S390IPLCertificateStore
     * maintains an index origin of 0. Thus, the indices must be adjusted
     * for correct access into the cert store. A couple of special cases
     * must also be accounted for.
     */

    /* Both indices are 0; return header with no certs */
    if (first_vc_index == 0 && last_vc_index == 0) {
        goto out;
    }

    /* Normalize indices */
    cs_start_index = (first_vc_index == 0) ? 0 : first_vc_index - 1;
    cs_end_index = last_vc_index - 1;

    /* Requested range is outside the cert store; return header with no certs */
    if (cs_start_index >= cs->count || cs_end_index >= cs->count) {
        goto out;
    }

    remaining_space = in_len - sizeof(VCBlockHeader);

    for (int i = cs_start_index; i <= cs_end_index; i++) {
        const S390IPLCertificate *cert = &cs->certs[i];
        /*
         * Each field of the VCE is word-aligned.
         * Allocate enough space for the largest possible size for this VCE.
         * As the certificate fields (key-id, hash, data) are parsed, the
         * VCE's length field will be updated accordingly.
         */
        vce_max_len = sizeof(VCEntryHeader) + ROUND_UP(CERT_KEY_ID_LEN, 4) +
                      ROUND_UP(CERT_HASH_LEN, 4) + ROUND_UP(cert->der_size, 4);
        g_autofree VCEntry *vce = g_malloc0(vce_max_len);

        /*
         * Bit 0 of the VCE flags indicates whether the certificate is valid.
         * The caller of DIAG320 subcode 2 is responsible for verifying that
         * the VCE contains a valid certificate.
         */
        if (build_vce_header(vce, cert, i) || build_vce_data(vce, cert, vce_max_len)) {
            /*
             * Error occurs - VCE does not contain a valid certificate.
             * Bit 0 of the VCE flags is 0 and the VCE length is set.
             */
            vce->vce_hdr.len = cpu_to_be32(VCE_INVALID_LEN);
        }
        vce_len = be32_to_cpu(vce->vce_hdr.len);

        /*
         * If there is no more space to store the cert,
         * set the remaining verification cert count and
         * break early.
         */
        if (remaining_space < vce_len) {
            vcb_hdr->remain_ct = cpu_to_be16(last_vc_index - i);
            break;
        }

        /* Write VCE */
        if (s390_cpu_virt_mem_write(cpu, addr + vcb_hdr->out_len, r1, vce, vce_len)) {
            s390_cpu_virt_mem_handle_exc(cpu, ra);
            return -1;
        }

        vcb_hdr->out_len += vce_len;
        remaining_space -= vce_len;
        vcb_hdr->stored_ct++;
    }
    vcb_hdr->stored_ct = cpu_to_be16(vcb_hdr->stored_ct);

out:
    vcb_hdr->out_len = cpu_to_be32(vcb_hdr->out_len);

    if (s390_cpu_virt_mem_write(cpu, addr, r1, vcb_hdr, sizeof(VCBlockHeader))) {
        s390_cpu_virt_mem_handle_exc(cpu, ra);
        return -1;
    }

    return DIAG_320_RC_OK;
}

QEMU_BUILD_BUG_MSG(sizeof(VCStorageSizeBlock) != VCSSB_LEN_VALID,
                   "size of VCStorageSizeBlock is wrong");
QEMU_BUILD_BUG_MSG(sizeof(VCBlock) != 64, "size of VCBlock is wrong");
QEMU_BUILD_BUG_MSG(sizeof(VCEntry) != 128, "size of VCEntry is wrong");

void handle_diag_320(CPUS390XState *env, uint64_t r1, uint64_t r3, uintptr_t ra)
{
    S390CPU *cpu = env_archcpu(env);
    S390IPLCertificateStore *cs = s390_ipl_get_certificate_store();
    uint64_t subcode = env->regs[r3];
    uint64_t addr = env->regs[r1];
    uint32_t ism_word0;
    int rc;

    if (env->psw.mask & PSW_MASK_PSTATE) {
        s390_program_interrupt(env, PGM_PRIVILEGED, ra);
        return;
    }

    if (!s390_has_feat(S390_FEAT_CERT_STORE) ||
        (subcode & ~0x000ffULL) ||
        (r1 & 1)) {
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return;
    }

    switch (subcode) {
    case DIAG_320_SUBC_QUERY_ISM:
        /*
         * The Installed Subcode Block (ISB) can be up 8 words in size,
         * but the current set of subcodes can fit within a single word
         * for now.
         */
        ism_word0 = cpu_to_be32(DIAG_320_ISM_QUERY_SUBCODES |
                                         DIAG_320_ISM_QUERY_VCSI |
                                         DIAG_320_ISM_STORE_VC);

        if (s390_cpu_virt_mem_write(cpu, addr, r1, &ism_word0, sizeof(ism_word0))) {
            s390_cpu_virt_mem_handle_exc(cpu, ra);
            return;
        }

        env->regs[r1 + 1] = DIAG_320_RC_OK;
        break;
    case DIAG_320_SUBC_QUERY_VCSI:
        if (addr & 0x7) {
            s390_program_interrupt(env, PGM_SPECIFICATION, ra);
            return;
        }

        if (!diag_parm_addr_valid(addr, sizeof(VCStorageSizeBlock), true)) {
            s390_program_interrupt(env, PGM_ADDRESSING, ra);
            return;
        }

        rc = handle_diag320_query_vcsi(cpu, addr, r1, ra, cs);
        if (rc == -1) {
            return;
        }
        env->regs[r1 + 1] = rc;
        break;
    case DIAG_320_SUBC_STORE_VC:
        if (addr & ~TARGET_PAGE_MASK) {
            s390_program_interrupt(env, PGM_SPECIFICATION, ra);
            return;
        }

        rc = handle_diag320_store_vc(cpu, addr, r1, ra, cs);
        if (rc == -1) {
            return;
        }
        env->regs[r1 + 1] = rc;
        break;
    default:
        env->regs[r1 + 1] = DIAG_320_RC_NOT_SUPPORTED;
        break;
    }
}

static bool diag_508_verify_sig(uint8_t *cert, size_t cert_size,
                                uint8_t *comp, size_t comp_size,
                                uint8_t *sig, size_t sig_size)
{
    g_autofree uint8_t *sig_pem = NULL;
    size_t sig_size_pem;
    int rc;

    /*
     * PKCS#7 signature with DER format
     * Convert to PEM format for signature verification
     *
     * Ignore errors during qcrypto signature format conversion and verification
     * Return false on any error, treating it as a verification failure
     */
    rc = qcrypto_pkcs7_convert_sig_pem(sig, sig_size, &sig_pem, &sig_size_pem, NULL);
    if (rc < 0) {
        return false;
    }

    rc = qcrypto_x509_verify_sig(cert, cert_size,
                                 comp, comp_size,
                                 sig_pem, sig_size_pem, NULL);
    if (rc < 0) {
        return false;
    }

    return true;
}

static int handle_diag508_sig_verif(CPUS390XState *env, uint64_t addr)
{
    int verified;
    uint32_t svb_len;
    uint64_t comp_len, comp_addr;
    uint64_t sig_len, sig_addr;
    g_autofree uint8_t *comp = NULL;
    g_autofree uint8_t *sig = NULL;
    g_autofree Diag508SigVerifBlock *svb = NULL;
    size_t svb_size = sizeof(Diag508SigVerifBlock);
    S390IPLCertificateStore *cs = s390_ipl_get_certificate_store();

    if (!cs->count) {
        return DIAG_508_RC_NO_CERTS;
    }

    svb = g_new0(Diag508SigVerifBlock, 1);
    s390_ipl_read(env, addr, svb, svb_size);

    svb_len = be32_to_cpu(svb->length);
    if (svb_len != svb_size) {
        return DIAG_508_RC_INVAL_LEN;
    }

    comp_len = be64_to_cpu(svb->comp_len);
    comp_addr = be64_to_cpu(svb->comp_addr);
    sig_len = be64_to_cpu(svb->sig_len);
    sig_addr = be64_to_cpu(svb->sig_addr);

    if (!comp_len || !comp_addr || comp_len > DIAG_508_MAX_COMP_LEN) {
        if (comp_len > DIAG_508_MAX_COMP_LEN) {
            warn_report("DIAG 0x508: component length %" PRIu64
                        " exceeds current maximum %u",
                        comp_len, DIAG_508_MAX_COMP_LEN);
        }
        return DIAG_508_RC_INVAL_COMP_DATA;
    }

    if (!sig_len || !sig_addr || sig_len > DIAG_508_MAX_SIG_LEN) {
        if (sig_len > DIAG_508_MAX_SIG_LEN) {
            warn_report("DIAG 0x508: signature length %" PRIu64
                        " exceeds current maximum %u",
                        sig_len, DIAG_508_MAX_SIG_LEN);
        }
        return DIAG_508_RC_INVAL_PKCS7_SIG;
    }

    comp = g_malloc0(comp_len);
    s390_ipl_read(env, comp_addr, comp, comp_len);

    sig = g_malloc0(sig_len);
    s390_ipl_read(env, sig_addr, sig, sig_len);

    for (int i = 0; i < cs->count; i++) {
        verified = diag_508_verify_sig(cs->certs[i].raw,
                                       cs->certs[i].size,
                                       comp, comp_len,
                                       sig, sig_len);
        if (verified) {
            svb->cert_store_index = i;
            svb->cert_len = cpu_to_be64(cs->certs[i].der_size);
            s390_ipl_write(env, addr, svb, svb_size);
            return DIAG_508_RC_OK;
        }
    }

    return DIAG_508_RC_FAIL_VERIF;
}

QEMU_BUILD_BUG_MSG(sizeof(Diag508SigVerifBlock) != 64,
                   "size of Diag508SigVerifBlock is wrong");

void handle_diag_508(CPUS390XState *env, uint64_t r1, uint64_t r3, uintptr_t ra)
{
    uint64_t subcode = env->regs[r3];
    uint64_t addr = env->regs[r1];
    int rc;

    if (env->psw.mask & PSW_MASK_PSTATE) {
        s390_program_interrupt(env, PGM_PRIVILEGED, ra);
        return;
    }

    if ((subcode & ~0x0ffffULL) || (r1 & 1)) {
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return;
    }

    switch (subcode) {
    case DIAG_508_SUBC_QUERY_SUBC:
        rc = DIAG_508_SUBC_SIG_VERIF;
        break;
    case DIAG_508_SUBC_SIG_VERIF:
        if (!diag_parm_addr_valid(addr, sizeof(Diag508SigVerifBlock), true)) {
            s390_program_interrupt(env, PGM_ADDRESSING, ra);
            return;
        }

        rc = handle_diag508_sig_verif(env, addr);
        break;
    default:
        s390_program_interrupt(env, PGM_SPECIFICATION, ra);
        return;
    }
    env->regs[r1 + 1] = rc;
}
