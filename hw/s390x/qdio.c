/*
 * Minimal format-0 Queued Direct I/O support
 *
 * The wire layout follows the Linux s390 QDIO definitions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"

#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/s390x/qdio.h"
#include "qemu/bswap.h"

int s390_qdio_read(uint64_t addr, void *buf, size_t len)
{
    if (addr + len < addr) {
        return -EFAULT;
    }
    return address_space_read(&address_space_memory, addr,
                              MEMTXATTRS_UNSPECIFIED, buf, len) ==
           MEMTX_OK ? 0 : -EFAULT;
}

int s390_qdio_write(uint64_t addr, const void *buf, size_t len)
{
    if (addr + len < addr) {
        return -EFAULT;
    }
    return address_space_write(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf, len) ==
           MEMTX_OK ? 0 : -EFAULT;
}

void s390_qdio_reset(S390QdioState *s)
{
    memset(s, 0, sizeof(*s));
}

static void qdio_load_desc(S390QdioQueue *q, const uint8_t *desc)
{
    q->sliba = ldq_be_p(desc);
    q->sla = ldq_be_p(desc + 8);
    q->slsba = ldq_be_p(desc + 16);
    q->slib_key = desc[28] >> 4;
    q->sl_key = desc[28] & 0xf;
    q->sbal_key = desc[29] >> 4;
    q->slsb_key = desc[29] & 0xf;
}

int s390_qdio_establish(S390QdioState *s, const uint8_t *qdr, size_t len)
{
    uint8_t iq, oq, iqsz, oqsz;
    const uint8_t *desc;
    uint8_t qib_ac;
    unsigned int i;

    if (len < 64 || qdr[0] != 0 || qdr[3] != 0) {
        return -EINVAL;
    }
    iq = qdr[5];
    oq = qdr[7];
    iqsz = qdr[9];
    oqsz = qdr[11];
    if (iq != 1 || oq < 1 || oq > S390_QDIO_MAX_QUEUES ||
        iqsz != 8 || oqsz != 8 ||
        len < 64 + (size_t)(iq + oq) * 32) {
        return -EINVAL;
    }

    s390_qdio_reset(s);
    s->qiba = ldq_be_p(qdr + 48);
    s->qib_key = qdr[60] >> 4;
    s->input_count = iq;
    s->output_count = oq;
    desc = qdr + 64;
    for (i = 0; i < iq; i++, desc += 32) {
        qdio_load_desc(&s->input[i], desc);
        s->input[i].mask = 0x80000000U >> i;
    }
    for (i = 0; i < oq; i++, desc += 32) {
        qdio_load_desc(&s->output[i], desc);
        s->output[i].mask = 0x80000000U >> i;
    }
    if (!s->qiba || s390_qdio_read(s->qiba + 3, &qib_ac, 1)) {
        s390_qdio_reset(s);
        return -EFAULT;
    }
    qib_ac |= 0x40;
    if (s390_qdio_write(s->qiba + 3, &qib_ac, 1)) {
        s390_qdio_reset(s);
        return -EFAULT;
    }
    s->established = true;
    return 0;
}

int s390_qdio_find_buffer(S390QdioQueue *q, uint8_t wanted, uint8_t *index)
{
    unsigned int n;

    for (n = 0; n < S390_QDIO_MAX_BUFFERS; n++) {
        uint8_t pos = (q->cursor + n) & 0x7f;
        uint8_t state;

        if (s390_qdio_read(q->slsba + pos, &state, 1)) {
            return -EFAULT;
        }
        if (state == wanted) {
            *index = pos;
            return 0;
        }
    }
    return -ENOBUFS;
}

int s390_qdio_get_sbal(S390QdioQueue *q, uint8_t index, uint64_t *addr)
{
    uint64_t be_addr;

    if (s390_qdio_read(q->sla + (uint64_t)index * 8, &be_addr,
                       sizeof(be_addr))) {
        return -EFAULT;
    }
    *addr = be64_to_cpu(be_addr);
    return *addr ? 0 : -EFAULT;
}

int s390_qdio_set_state(S390QdioQueue *q, uint8_t index, uint8_t state)
{
    int ret = s390_qdio_write(q->slsba + index, &state, 1);

    if (!ret) {
        q->cursor = (index + 1) & 0x7f;
    }
    return ret;
}
