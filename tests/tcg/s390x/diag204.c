/*
 * Test the DIAG 204 LPAR RMF interface.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdint.h>

#define PAGE_SIZE 4096

struct __attribute__((packed)) diag204_header {
    uint8_t partitions;
    uint8_t flags;
    uint16_t time_slice;
    uint16_t physical_cpus;
    uint16_t own_partition_offset;
    uint64_t tod;
};

struct __attribute__((packed)) diag204_partition {
    uint8_t partition_number;
    uint8_t cpus;
    uint8_t reserved[6];
    uint8_t name[8];
};

struct __attribute__((packed)) diag204_cpu {
    uint16_t address;
    uint8_t reserved[2];
    uint8_t type_index;
    uint8_t flags;
    uint16_t weight;
    uint64_t accumulated_time;
    uint64_t lpar_time;
};

struct __attribute__((packed)) diag204_x_header {
    uint8_t partitions;
    uint8_t flags;
    uint16_t time_slice;
    uint16_t physical_cpus;
    uint16_t own_partition_offset;
    uint64_t tod_high;
    uint64_t tod_low;
    uint8_t reserved[40];
};

struct __attribute__((packed)) diag204_x_partition {
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
};

struct __attribute__((packed)) diag204_x_cpu {
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
};

static uint8_t buffer[8 * PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));

static const uint8_t qemu_tcg[] = {
    0xd8, 0xc5, 0xd4, 0xe4, 0x40, 0xe3, 0xc3, 0xc7,
};

static int bytes_equal(const uint8_t *a, const uint8_t *b, unsigned int len)
{
    unsigned int i;

    for (i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void diag204(uint64_t *subcode, uint64_t *pages, void *addr)
{
    register uint64_t r0 asm("0") = (unsigned long)addr;
    register uint64_t r2 asm("2") = *subcode;
    register uint64_t r3 asm("3") = *pages;

    asm volatile("diag %0,%1,0x204"
                 : "+d"(r0), "+d"(r2), "+d"(r3)
                 :
                 : "cc", "memory");
    *subcode = r2;
    *pages = r3;
}

static uint64_t diag204_same_register(uint64_t *subcode, uint64_t *pages)
{
    register uint64_t r0 asm("0") = *subcode;
    register uint64_t r1 asm("1") = *pages;
    uint64_t program_mask = 0;

    asm volatile("diag %0,%0,0x204\n"
                 "ipm %2"
                 : "+d"(r0), "+d"(r1), "+d"(program_mask)
                 :
                 : "cc", "memory");
    *subcode = r0;
    *pages = r1;
    return program_mask;
}

int main(void)
{
    struct diag204_x_partition *x_partition;
    struct diag204_x_header *x_header;
    struct diag204_x_cpu *x_cpu;
    struct diag204_partition *partition;
    struct diag204_header *header;
    struct diag204_cpu *cpu;
    uint64_t subcode, pages, required_pages;
    uint16_t cpus;

    subcode = 0x10005;
    pages = 0;
    if (((diag204_same_register(&subcode, &pages) >> 28) & 3) != 0 ||
        subcode != 0 || pages == 0 || pages > 8) {
        return 1;
    }
    required_pages = pages;

    subcode = 0x10007;
    diag204(&subcode, &pages, buffer);
    if (subcode != 0) {
        return 2;
    }
    x_header = (struct diag204_x_header *)buffer;
    x_partition = (struct diag204_x_partition *)(x_header + 1);
    x_cpu = (struct diag204_x_cpu *)(x_partition + 1);
    cpus = x_header->physical_cpus;
    if (x_header->partitions != 1 || x_header->flags != 0 ||
        cpus == 0 ||
        required_pages != (sizeof(*x_header) + sizeof(*x_partition) +
                           cpus * sizeof(*x_cpu) + PAGE_SIZE - 1) / PAGE_SIZE ||
        x_header->own_partition_offset != sizeof(*x_header) ||
        x_partition->partition_number != 1 || x_partition->cpus != cpus ||
        x_partition->real_cpus != cpus ||
        !bytes_equal(x_partition->name, qemu_tcg, sizeof(qemu_tcg)) ||
        x_cpu->address != 0 || !(x_cpu->flags & 0x20) ||
        x_cpu->weight != 100 || x_cpu->current_weight != 1000 ||
        x_cpu->online_time == 0 || x_cpu->wait_time != x_cpu->online_time ||
        x_cpu[cpus - 1].address != cpus - 1) {
        return 3;
    }

    subcode = 4;
    pages = 1;
    diag204(&subcode, &pages, buffer);
    if (subcode != 0) {
        return 4;
    }
    header = (struct diag204_header *)buffer;
    partition = (struct diag204_partition *)(header + 1);
    cpu = (struct diag204_cpu *)(partition + 1);
    if (header->partitions != 1 || header->flags != 0 ||
        header->physical_cpus != cpus ||
        header->own_partition_offset != sizeof(*header) ||
        partition->partition_number != 1 || partition->cpus != cpus ||
        !bytes_equal(partition->name, qemu_tcg, sizeof(qemu_tcg)) ||
        cpu->address != 0 || !(cpu->flags & 0x20) || cpu->weight != 100 ||
        cpu[cpus - 1].address != cpus - 1) {
        return 5;
    }

    subcode = 0xdead;
    pages = 0;
    diag204(&subcode, &pages, 0);
    return subcode == 4 ? 0 : 6;
}
