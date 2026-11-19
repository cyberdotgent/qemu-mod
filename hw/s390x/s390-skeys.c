/*
 * s390 storage key device
 *
 * Copyright 2015 IBM Corp.
 * Author(s): Jason J. Herne <jjherne@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or (at
 * your option) any later version. See the COPYING file in the top-level
 * directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "exec/target_page.h"
#include "hw/s390x/s390-virtio-ccw.h"
#include "hw/core/qdev-properties.h"
#include "hw/s390x/storage-keys.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-machine.h"
#include "qobject/qdict.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "hw/core/boards.h"
#include "system/memory_mapping.h"
#include "system/address-spaces.h"
#include "system/kvm.h"
#include "migration/qemu-file-types.h"
#include "migration/register.h"
#include "monitor/hmp.h"
#include "monitor/monitor.h"
#include "trace.h"

#define S390_SKEYS_BUFFER_SIZE (128 * KiB)  /* Room for 128k storage keys */
#define S390_SKEYS_SAVE_FLAG_EOS 0x01
#define S390_SKEYS_SAVE_FLAG_SKEYS 0x02
#define S390_SKEYS_SAVE_FLAG_ERROR 0x04

S390SKeysState *s390_get_skeys_device(void)
{
    S390SKeysState *ss;

    ss = S390_SKEYS(object_resolve_path_type("", TYPE_S390_SKEYS, NULL));
    assert(ss);
    return ss;
}

void s390_skeys_init(void)
{
    Object *obj;

    if (kvm_enabled()) {
        obj = object_new(TYPE_KVM_S390_SKEYS);
    } else {
        obj = object_new(TYPE_QEMU_S390_SKEYS);
    }
    object_property_add_child(qdev_get_machine(), TYPE_S390_SKEYS,
                              obj);
    object_unref(obj);

    qdev_realize(DEVICE(obj), NULL, &error_fatal);
}

int s390_skeys_get(S390SKeysState *ks, uint64_t start_gfn,
                   uint64_t count, uint8_t *keys)
{
    S390SKeysClass *kc = S390_SKEYS_GET_CLASS(ks);
    int rc;

    rc = kc->get_skeys(ks, start_gfn, count, keys);
    if (rc) {
        trace_s390_skeys_get_nonzero(rc);
    }
    return rc;
}

int s390_skeys_set(S390SKeysState *ks, uint64_t start_gfn,
                   uint64_t count, uint8_t *keys)
{
    S390SKeysClass *kc = S390_SKEYS_GET_CLASS(ks);
    int rc;

    rc = kc->set_skeys(ks, start_gfn, count, keys);
    if (rc) {
        trace_s390_skeys_set_nonzero(rc);
    }
    return rc;
}

int s390_skeys_update_access(uint64_t addr, uint64_t len, bool is_write)
{
    S390SKeysState *ss = s390_get_skeys_device();
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    uint64_t start_gfn, end_gfn, count;
    g_autofree uint8_t *keys = NULL;
    uint8_t bits = 0x04; /* Reference */
    bool changed = false;
    int rc;

    if (!len || !skeyclass->skeys_are_enabled(ss)) {
        return 0;
    }
    if (addr > UINT64_MAX - (len - 1)) {
        return -EINVAL;
    }

    start_gfn = addr / TARGET_PAGE_SIZE;
    end_gfn = (addr + len - 1) / TARGET_PAGE_SIZE;
    count = end_gfn - start_gfn + 1;
    keys = g_new(uint8_t, count);

    rc = s390_skeys_get(ss, start_gfn, count, keys);
    if (rc) {
        return rc;
    }

    if (is_write) {
        bits |= 0x02; /* Change */
    }
    for (uint64_t i = 0; i < count; i++) {
        uint8_t old_key = keys[i];

        keys[i] |= bits;
        changed |= keys[i] != old_key;
    }

    return changed ? s390_skeys_set(ss, start_gfn, count, keys) : 0;
}

static void write_keys(FILE *f, uint8_t *keys, uint64_t startgfn,
                       uint64_t count, Error **errp)
{
    uint64_t curpage = startgfn;
    uint64_t maxpage = curpage + count - 1;

    for (; curpage <= maxpage; curpage++) {
        uint8_t acc = (*keys & 0xF0) >> 4;
        int fp =  (*keys & 0x08);
        int ref = (*keys & 0x04);
        int ch = (*keys & 0x02);
        int res = (*keys & 0x01);

        fprintf(f, "page=%03" PRIx64 ": key(%d) => ACC=%X, FP=%d, REF=%d,"
                " ch=%d, reserved=%d\n",
                curpage, *keys, acc, fp, ref, ch, res);
        keys++;
    }
}

#ifdef CONFIG_HMP
void hmp_info_skeys(MonitorHMP *hmp, const QDict *qdict)
{
    S390SKeysState *ss = s390_get_skeys_device();
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    uint64_t addr = qdict_get_int(qdict, "addr");
    uint8_t key;
    int r;

    /* Quick check to see if guest is using storage keys*/
    if (!skeyclass->skeys_are_enabled(ss)) {
        monitor_hmp_printf(hmp, "Error: This guest is not using storage keys\n");
        return;
    }

    if (!address_space_access_valid(&address_space_memory,
                                    addr & TARGET_PAGE_MASK, TARGET_PAGE_SIZE,
                                    false, MEMTXATTRS_UNSPECIFIED)) {
        monitor_hmp_printf(hmp, "Error: The given address is not valid\n");
        return;
    }

    r = skeyclass->get_skeys(ss, addr / TARGET_PAGE_SIZE, 1, &key);
    if (r < 0) {
        monitor_hmp_printf(hmp, "Error: %s\n", strerror(-r));
        return;
    }

    monitor_hmp_printf(hmp, "  key: 0x%X\n", key);
}

void hmp_dump_skeys(MonitorHMP *hmp, const QDict *qdict)
{
    const char *filename = qdict_get_str(qdict, "filename");
    Error *err = NULL;

    qmp_dump_skeys(filename, &err);
    if (err) {
        error_report_err(err);
    }
}
#endif

void s390_qmp_dump_skeys(const char *filename, Error **errp)
{
    S390SKeysState *ss = s390_get_skeys_device();
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    GuestPhysBlockList guest_phys_blocks;
    GuestPhysBlock *block;
    uint64_t pages, gfn;
    Error *lerr = NULL;
    uint8_t *buf;
    int ret;
    int fd;
    FILE *f;

    /* Quick check to see if guest is using storage keys*/
    if (!skeyclass->skeys_are_enabled(ss)) {
        error_setg(errp, "This guest is not using storage keys - "
                         "nothing to dump");
        return;
    }

    fd = qemu_open_old(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        error_setg_file_open(errp, errno, filename);
        return;
    }
    f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        error_setg_file_open(errp, errno, filename);
        return;
    }

    buf = g_try_malloc(S390_SKEYS_BUFFER_SIZE);
    if (!buf) {
        error_setg(errp, "Could not allocate memory");
        goto out;
    }

    assert(bql_locked());
    guest_phys_blocks_init(&guest_phys_blocks);
    guest_phys_blocks_append(&guest_phys_blocks);

    QTAILQ_FOREACH(block, &guest_phys_blocks.head, next) {
        assert(QEMU_IS_ALIGNED(block->target_start, TARGET_PAGE_SIZE));
        assert(QEMU_IS_ALIGNED(block->target_end, TARGET_PAGE_SIZE));

        gfn = block->target_start / TARGET_PAGE_SIZE;
        pages = (block->target_end - block->target_start) / TARGET_PAGE_SIZE;

        while (pages) {
            const uint64_t cur_pages = MIN(pages, S390_SKEYS_BUFFER_SIZE);

            ret = skeyclass->get_skeys(ss, gfn, cur_pages, buf);
            if (ret < 0) {
                error_setg_errno(errp, -ret, "get_keys error");
                goto out_free;
            }

            /* write keys to stream */
            write_keys(f, buf, gfn, cur_pages, &lerr);
            if (lerr) {
                goto out_free;
            }

            gfn += cur_pages;
            pages -= cur_pages;
        }
    }

out_free:
    guest_phys_blocks_free(&guest_phys_blocks);
    error_propagate(errp, lerr);
    g_free(buf);
out:
    fclose(f);
}

static bool qemu_s390_skeys_are_enabled(S390SKeysState *ss)
{
    QEMUS390SKeysState *skeys = QEMU_S390_SKEYS(ss);

    /* Lockless check is sufficient. */
    return !!skeys->keydata;
}

static bool qemu_s390_enable_skeys(S390SKeysState *ss)
{
    QEMUS390SKeysState *skeys = QEMU_S390_SKEYS(ss);
    static gsize initialized;

    if (likely(skeys->keydata)) {
        return true;
    }

    /*
     * TODO: Modern Linux doesn't use storage keys unless running KVM guests
     *       that use storage keys. Therefore, we keep it simple for now.
     *
     * 1) We should initialize to "referenced+changed" for an initial
     *    over-indication. Let's avoid touching megabytes of data for now and
     *    assume that any sane user will issue a storage key instruction before
     *    actually relying on this data.
     * 2) Relying on ram_size and allocating a big array is ugly. We should
     *    allocate and manage storage key data per RAMBlock or optimally using
     *    some sparse data structure.
     * 3) We only ever have a single S390SKeysState, so relying on
     *    g_once_init_enter() is good enough.
     */
    if (g_once_init_enter(&initialized)) {
        S390CcwMachineState *s390ms = S390_CCW_MACHINE(qdev_get_machine());

        skeys->key_count = s390_get_memory_limit(s390ms) / TARGET_PAGE_SIZE;
        skeys->keydata = g_malloc0(skeys->key_count);
        g_once_init_leave(&initialized, 1);
    }
    return false;
}

static int qemu_s390_skeys_set(S390SKeysState *ss, uint64_t start_gfn,
                              uint64_t count, uint8_t *keys)
{
    QEMUS390SKeysState *skeydev = QEMU_S390_SKEYS(ss);
    int i;

    /* Check for uint64 overflow and access beyond end of key data */
    if (unlikely(!skeydev->keydata || start_gfn + count > skeydev->key_count ||
                  start_gfn + count < count)) {
        error_report("Error: Setting storage keys for pages with unallocated "
                     "storage key memory: gfn=%" PRIx64 " count=%" PRId64,
                     start_gfn, count);
        return -EINVAL;
    }

    for (i = 0; i < count; i++) {
        skeydev->keydata[start_gfn + i] = keys[i];
    }
    return 0;
}

static int qemu_s390_skeys_get(S390SKeysState *ss, uint64_t start_gfn,
                               uint64_t count, uint8_t *keys)
{
    QEMUS390SKeysState *skeydev = QEMU_S390_SKEYS(ss);
    int i;

    /* Check for uint64 overflow and access beyond end of key data */
    if (unlikely(!skeydev->keydata || start_gfn + count > skeydev->key_count ||
                  start_gfn + count < count)) {
        error_report("Error: Getting storage keys for pages with unallocated "
                     "storage key memory: gfn=%" PRIx64 " count=%" PRId64,
                     start_gfn, count);
        return -EINVAL;
    }

    for (i = 0; i < count; i++) {
        keys[i] = skeydev->keydata[start_gfn + i];
    }
    return 0;
}

static void qemu_s390_skeys_class_init(ObjectClass *oc, const void *data)
{
    S390SKeysClass *skeyclass = S390_SKEYS_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    skeyclass->skeys_are_enabled = qemu_s390_skeys_are_enabled;
    skeyclass->enable_skeys = qemu_s390_enable_skeys;
    skeyclass->get_skeys = qemu_s390_skeys_get;
    skeyclass->set_skeys = qemu_s390_skeys_set;

    /* Reason: Internal device (only one skeys device for the whole memory) */
    dc->user_creatable = false;
}

/*
 * Migration and snapshots of the storage keys.
 *
 * Under KVM, every key is a page table walk and an ISKE or SSKE in the
 * kernel, once the guest uses storage keys (z/OS, z/VSE and z/VM do; Linux
 * does not, KVM then reports that keys are not in use and nothing is sent).
 * Reading the keys of 2 GiB took 1.9 s, setting them on the destination as
 * long: setting a key is a quiescing SSKE, and it faults in the pages the
 * incoming RAM left unpopulated (the zero pages of the source), which is not
 * faster with more threads.  As one non-iterable section, that was all
 * downtime.
 *
 * So the keys are sent twice.  A first pass during precopy (iterable
 * section, chunks of S390_SKEYS_ITERATE_PAGES) lets the destination set
 * them while the VM still runs on the source.  The final pass with the VM
 * stopped sends all keys again; the destination reads its keys (ISKE, in
 * parallel) and only sets those that differ: keys the guest changed since
 * the first pass, and R/C bits that changed on the destination meanwhile
 * (pages written by the RAM migration; also, the C bit of pages that the
 * first pass faulted in was seen lost again by the final pass, so what was
 * set cannot be trusted without reading it back).  The keys are never
 * tracked between the passes: the final pass alone is complete, as before.
 *
 * The records are unchanged (S390_SKEYS_SAVE_FLAG_SKEYS ranges, terminated
 * by EOS in every section), so a destination with the former single-section
 * loader loads each section in turn, and this loader loads the stream of
 * the former single section.
 */
#define S390_SKEYS_MAX_THREADS 8
#define S390_SKEYS_THREAD_MIN_PAGES (16 * KiB)  /* 64 MiB of guest memory  */
#define S390_SKEYS_ITERATE_PAGES (1 * MiB)      /* 4 GiB per iteration    */
#define S390_SKEYS_FINAL_PAGES (128 * KiB)      /* 512 MiB per record     */
#define S390_SKEYS_SET_GAP 16

typedef struct S390SKeysJob {
    S390SKeysState *ss;
    bool set;
    uint64_t gfn;
    uint64_t count;
    uint8_t *keys;
    int ret;
} S390SKeysJob;

static void *s390_skeys_job_run(void *opaque)
{
    S390SKeysJob *job = opaque;
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(job->ss);
    uint64_t done = 0;

    while (done < job->count && !job->ret) {
        const uint64_t n = MIN(job->count - done, S390_SKEYS_BUFFER_SIZE);

        if (job->set) {
            job->ret = skeyclass->set_skeys(job->ss, job->gfn + done, n,
                                            job->keys + done);
        } else {
            job->ret = skeyclass->get_skeys(job->ss, job->gfn + done, n,
                                            job->keys + done);
        }
        done += n;
    }
    return NULL;
}

static int s390_skeys_num_threads(uint64_t count)
{
    uint64_t threads = DIV_ROUND_UP(count, S390_SKEYS_THREAD_MIN_PAGES);
    long host_cpus = 1;

#ifdef _SC_NPROCESSORS_ONLN
    host_cpus = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (host_cpus < 1) {
        host_cpus = 1;
    }
    return MAX(MIN(MIN(threads, S390_SKEYS_MAX_THREADS), host_cpus), 1);
}

/* Get or set the keys of count pages from gfn on; the first error. */
static int s390_skeys_get_set_parallel(S390SKeysState *ss, bool set,
                                       uint64_t gfn, uint64_t count,
                                       uint8_t *keys)
{
    const int nthreads = s390_skeys_num_threads(count);
    const uint64_t per_thread = DIV_ROUND_UP(count, nthreads);
    S390SKeysJob jobs[S390_SKEYS_MAX_THREADS] = {};
    QemuThread threads[S390_SKEYS_MAX_THREADS];
    uint64_t start = 0;
    int i, ret = 0;

    for (i = 0; i < nthreads; i++) {
        jobs[i] = (S390SKeysJob) {
            .ss = ss,
            .set = set,
            .gfn = gfn + start,
            .count = MIN(per_thread, count - start),
            .keys = keys + start,
        };
        start += jobs[i].count;
        if (i > 0) {
            qemu_thread_create(&threads[i], "s390-skeys", s390_skeys_job_run,
                               &jobs[i], QEMU_THREAD_JOINABLE);
        }
    }
    s390_skeys_job_run(&jobs[0]);
    for (i = 0; i < nthreads; i++) {
        if (i > 0) {
            qemu_thread_join(&threads[i]);
        }
        if (jobs[i].ret && !ret) {
            ret = jobs[i].ret;
        }
    }
    return ret;
}

/*
 * Set the keys of count pages from gfn on, but only where they differ from
 * the keys the pages have (see above).  The first key is always set: under
 * KVM, setting a key enables storage keys for the VM, as before.  Equal
 * keys between two differing ones closer than S390_SKEYS_SET_GAP are set
 * as well, to save ioctls.
 */
static int s390_skeys_set_changed(S390SKeysState *ss, uint64_t gfn,
                                  uint64_t count, uint8_t *keys)
{
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    g_autofree uint8_t *cur = g_try_malloc0(count);
    uint64_t i, first, last, set = 0;

    if (!cur ||
        (skeyclass->skeys_are_enabled(ss) &&
         s390_skeys_get_set_parallel(ss, false, gfn, count, cur))) {
        return s390_skeys_get_set_parallel(ss, true, gfn, count, keys);
    }
    /* (while keys are not enabled, all are 0 once they are) */
    cur[0] = ~keys[0];

    i = 0;
    while (i < count) {
        S390SKeysJob job;

        if (cur[i] == keys[i]) {
            i++;
            continue;
        }
        first = last = i;
        for (i++; i < count && i - last <= S390_SKEYS_SET_GAP; i++) {
            if (cur[i] != keys[i]) {
                last = i;
            }
        }
        job = (S390SKeysJob) {
            .ss = ss,
            .set = true,
            .gfn = gfn + first,
            .count = last + 1 - first,
            .keys = keys + first,
        };
        s390_skeys_job_run(&job);
        if (job.ret) {
            return job.ret;
        }
        set += job.count;
        i = last + 1;
    }
    trace_s390_skeys_load(count, gfn, set);
    return 0;
}

/* Send the keys of count pages from gfn on as one record; false: error. */
static bool s390_skeys_put_range(QEMUFile *f, S390SKeysState *ss,
                                 uint64_t gfn, uint64_t count)
{
    g_autofree uint8_t *buf = g_try_malloc(count);
    int error;

    if (!buf) {
        error_report("storage key save could not allocate memory");
        return false;
    }
    error = s390_skeys_get_set_parallel(ss, false, gfn, count, buf);
    if (error) {
        /*
         * Create a valid stream with all 0x00 and indicate
         * S390_SKEYS_SAVE_FLAG_ERROR to the destination.
         */
        error_report("S390_GET_KEYS error %d", error);
        memset(buf, 0, count);
    }
    qemu_put_be64(f, (gfn * TARGET_PAGE_SIZE) | S390_SKEYS_SAVE_FLAG_SKEYS);
    qemu_put_be64(f, count);
    qemu_put_buffer(f, buf, count);
    return !error;
}

/* The first pass of the source: the next range to send. */
static struct {
    GuestPhysBlockList blocks;
    bool blocks_valid;
    GuestPhysBlock *block;  /* NULL: done (or not needed) */
    uint64_t offset;        /* pages of the block already sent */
    uint64_t pending;       /* pages still to be sent */
} skeys_precopy;

static void s390_storage_keys_save_cleanup(void *opaque)
{
    if (skeys_precopy.blocks_valid) {
        guest_phys_blocks_free(&skeys_precopy.blocks);
    }
    memset(&skeys_precopy, 0, sizeof(skeys_precopy));
}

static int s390_storage_keys_save_setup(QEMUFile *f, void *opaque,
                                        Error **errp)
{
    S390SKeysState *ss = S390_SKEYS(opaque);
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    GuestPhysBlock *block;

    s390_storage_keys_save_cleanup(opaque);
    if (skeyclass->skeys_are_enabled(ss)) {
        guest_phys_blocks_init(&skeys_precopy.blocks);
        guest_phys_blocks_append(&skeys_precopy.blocks);
        skeys_precopy.blocks_valid = true;
        QTAILQ_FOREACH(block, &skeys_precopy.blocks.head, next) {
            skeys_precopy.pending +=
                (block->target_end - block->target_start) / TARGET_PAGE_SIZE;
        }
        skeys_precopy.block = QTAILQ_FIRST(&skeys_precopy.blocks.head);
    }
    qemu_put_be64(f, S390_SKEYS_SAVE_FLAG_EOS);
    return 0;
}

static bool s390_storage_keys_is_active_iterate(void *opaque)
{
    return skeys_precopy.block != NULL;
}

static void s390_storage_keys_query_pending(void *opaque,
                                            MigPendingData *pending,
                                            bool exact, bool final)
{
    pending->precopy_bytes += skeys_precopy.pending;
}

/* One chunk of the first pass; an error only ends it. */
static int s390_storage_keys_save_iterate(QEMUFile *f, void *opaque)
{
    S390SKeysState *ss = S390_SKEYS(opaque);
    GuestPhysBlock *block = skeys_precopy.block;

    if (block) {
        const uint64_t gfn = block->target_start / TARGET_PAGE_SIZE +
                             skeys_precopy.offset;
        const uint64_t pages = (block->target_end - block->target_start) /
                               TARGET_PAGE_SIZE - skeys_precopy.offset;
        const uint64_t count = MIN(pages, S390_SKEYS_ITERATE_PAGES);

        if (s390_skeys_put_range(f, ss, gfn, count)) {
            skeys_precopy.offset += count;
            skeys_precopy.pending -= count;
            if (count == pages) {
                skeys_precopy.block = QTAILQ_NEXT(block, next);
                skeys_precopy.offset = 0;
            }
        } else {
            /* the final pass sends (and checks) all keys anyway */
            skeys_precopy.block = NULL;
        }
    }
    qemu_put_be64(f, S390_SKEYS_SAVE_FLAG_EOS);
    if (!skeys_precopy.block) {
        skeys_precopy.pending = 0;
        return 1;
    }
    return 0;
}

/* All keys, with the VM stopped. */
static int s390_storage_keys_save_complete(QEMUFile *f, void *opaque)
{
    S390SKeysState *ss = S390_SKEYS(opaque);
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    GuestPhysBlockList guest_phys_blocks;
    GuestPhysBlock *block;
    uint64_t gfn, pages;
    bool ok = true;

    skeys_precopy.block = NULL;
    skeys_precopy.pending = 0;
    if (!skeyclass->skeys_are_enabled(ss)) {
        goto end_stream;
    }

    guest_phys_blocks_init(&guest_phys_blocks);
    guest_phys_blocks_append(&guest_phys_blocks);

    /* Send each contiguous physical memory range separately. */
    QTAILQ_FOREACH(block, &guest_phys_blocks.head, next) {
        assert(QEMU_IS_ALIGNED(block->target_start, TARGET_PAGE_SIZE));
        assert(QEMU_IS_ALIGNED(block->target_end, TARGET_PAGE_SIZE));

        gfn = block->target_start / TARGET_PAGE_SIZE;
        pages = (block->target_end - block->target_start) / TARGET_PAGE_SIZE;
        /* in records, the destination sets one while the next is read */
        while (ok && pages) {
            const uint64_t count = MIN(pages, S390_SKEYS_FINAL_PAGES);

            ok = s390_skeys_put_range(f, ss, gfn, count);
            gfn += count;
            pages -= count;
        }
        if (!ok) {
            break;
        }
    }

    guest_phys_blocks_free(&guest_phys_blocks);
end_stream:
    qemu_put_be64(f, ok ? S390_SKEYS_SAVE_FLAG_EOS
                        : S390_SKEYS_SAVE_FLAG_ERROR);
    return 0;
}

static int s390_storage_keys_load(QEMUFile *f, void *opaque, int version_id)
{
    S390SKeysState *ss = S390_SKEYS(opaque);
    S390SKeysClass *skeyclass = S390_SKEYS_GET_CLASS(ss);
    const uint64_t max_pages =
        MACHINE(qdev_get_machine())->maxram_size / TARGET_PAGE_SIZE;
    int ret = 0;

    /*
     * Make sure to lazy-enable if required to be done explicitly. No need to
     * flush any TLB as the VM is not running yet.
     */
    if (skeyclass->enable_skeys) {
        skeyclass->enable_skeys(ss);
    }

    while (!ret) {
        ram_addr_t addr;
        int flags;

        addr = qemu_get_be64(f);
        flags = addr & ~TARGET_PAGE_MASK;
        addr &= TARGET_PAGE_MASK;

        switch (flags) {
        case S390_SKEYS_SAVE_FLAG_SKEYS: {
            const uint64_t total_count = qemu_get_be64(f);
            const uint64_t gfn = addr / TARGET_PAGE_SIZE;
            g_autofree uint8_t *buf = NULL;

            /* at most the keys of the guest memory, one byte per page */
            if (gfn > max_pages || total_count > max_pages - gfn) {
                error_report("Storage key data for too many pages: %" PRIu64,
                             total_count);
                ret = -EINVAL;
                break;
            }
            buf = g_try_malloc(total_count);
            if (!buf) {
                error_report("storage key load could not allocate memory");
                ret = -ENOMEM;
                break;
            }
            if (qemu_get_buffer(f, buf, total_count) != total_count) {
                ret = qemu_file_get_error(f) ?: -EIO;
                error_report("Storage key data is incomplete");
                break;
            }

            ret = s390_skeys_set_changed(ss, gfn, total_count, buf);
            if (ret < 0) {
                error_report("S390_SET_KEYS error %d", ret);
            }
            break;
        }
        case S390_SKEYS_SAVE_FLAG_ERROR: {
            error_report("Storage key data is incomplete");
            ret = -EINVAL;
            break;
        }
        case S390_SKEYS_SAVE_FLAG_EOS:
            /* normal exit */
            return 0;
        default:
            error_report("Unexpected storage key flag data: %#x", flags);
            ret = -EINVAL;
        }
    }

    return ret;
}

static SaveVMHandlers savevm_s390_storage_keys = {
    .save_setup = s390_storage_keys_save_setup,
    .save_cleanup = s390_storage_keys_save_cleanup,
    .save_complete = s390_storage_keys_save_complete,
    .is_active_iterate = s390_storage_keys_is_active_iterate,
    .save_query_pending = s390_storage_keys_query_pending,
    .save_live_iterate = s390_storage_keys_save_iterate,
    .load_state = s390_storage_keys_load,
};

static void s390_skeys_realize(DeviceState *dev, Error **errp)
{
    S390SKeysState *ss = S390_SKEYS(dev);

    register_savevm_live(TYPE_S390_SKEYS, 0, 1, &savevm_s390_storage_keys, ss);
}

static void s390_skeys_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->hotpluggable = false;
    dc->realize = s390_skeys_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo s390_skeys_types[] = {
    {
        .name           = TYPE_DUMP_SKEYS_INTERFACE,
        .parent         = TYPE_INTERFACE,
        .class_size     = sizeof(DumpSKeysInterface),
    },
    {
        .name           = TYPE_S390_SKEYS,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(S390SKeysState),
        .class_init     = s390_skeys_class_init,
        .class_size     = sizeof(S390SKeysClass),
        .abstract       = true,
    },
    {
        .name           = TYPE_QEMU_S390_SKEYS,
        .parent         = TYPE_S390_SKEYS,
        .instance_size  = sizeof(QEMUS390SKeysState),
        .class_init     = qemu_s390_skeys_class_init,
        .class_size     = sizeof(S390SKeysClass),
    },
};

DEFINE_TYPES(s390_skeys_types)
