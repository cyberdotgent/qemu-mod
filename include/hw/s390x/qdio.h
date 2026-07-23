/*
 * Minimal format-0 Queued Direct I/O support
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_S390X_QDIO_H
#define HW_S390X_QDIO_H

#include "qemu/typedefs.h"

#define S390_QDIO_MAX_QUEUES 4
#define S390_QDIO_MAX_BUFFERS 128
#define S390_QDIO_MAX_ELEMENTS 16

#define S390_QDIO_SLSB_INPUT_EMPTY       0x41
#define S390_QDIO_SLSB_INPUT_PRIMED      0x82
#define S390_QDIO_SLSB_OUTPUT_PRIMED     0x62
#define S390_QDIO_SLSB_OUTPUT_EMPTY      0xa1
#define S390_QDIO_SLSB_ERROR             0x8f

#define S390_QDIO_EFLAGS_LAST_ENTRY      0x40
#define S390_QDIO_SFLAGS_PCI_REQ         0x40

typedef struct S390QdioQueue {
    uint64_t sliba;
    uint64_t sla;
    uint64_t slsba;
    uint8_t slib_key;
    uint8_t sl_key;
    uint8_t sbal_key;
    uint8_t slsb_key;
    uint8_t cursor;
    uint32_t mask;
} S390QdioQueue;

typedef struct S390QdioState {
    bool established;
    bool active;
    uint64_t qiba;
    uint8_t qib_key;
    uint8_t input_count;
    uint8_t output_count;
    S390QdioQueue input[S390_QDIO_MAX_QUEUES];
    S390QdioQueue output[S390_QDIO_MAX_QUEUES];
} S390QdioState;

int s390_qdio_establish(S390QdioState *s, const uint8_t *qdr, size_t len);
void s390_qdio_reset(S390QdioState *s);
int s390_qdio_read(uint64_t addr, void *buf, size_t len);
int s390_qdio_write(uint64_t addr, const void *buf, size_t len);
int s390_qdio_find_buffer(S390QdioQueue *q, uint8_t wanted, uint8_t *index);
int s390_qdio_get_sbal(S390QdioQueue *q, uint8_t index, uint64_t *addr);
int s390_qdio_set_state(S390QdioQueue *q, uint8_t index, uint8_t state);

#endif
