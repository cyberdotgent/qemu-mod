#include "proxmox-backup-client.h"
#include "pve-backup.h"
#include "vma.h"

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "block/block_int-global-state.h"
#include "block/blockjob.h"
#include "block/copy-before-write.h"
#include "block/dirty-bitmap.h"
#include "block/graph-lock.h"
#include "qapi/qapi-commands-block.h"
#include "qobject/qdict.h"
#include "qapi/qmp/qerror.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"

#if defined(CONFIG_MALLOC_TRIM)
#include <malloc.h>
#endif

#include <proxmox-backup-qemu.h>

/* PVE backup state and related function */

/*
 * Note: A resume from a qemu_coroutine_yield can happen in a different thread,
 * so you may not use normal mutexes within coroutines:
 *
 * ---bad-example---
 * qemu_rec_mutex_lock(lock)
 * ...
 * qemu_coroutine_yield() // wait for something
 * // we are now inside a different thread
 * qemu_rec_mutex_unlock(lock) // Crash - wrong thread!!
 * ---end-bad-example--
 *
 * ==> Always use CoMutext inside coroutines.
 *
 */

const char *PBS_BITMAP_NAME = "pbs-incremental-dirty-bitmap";
const char *BACKGROUND_BITMAP_NAME = "backup-access-background-bitmap";

static struct PVEBackupState {
    struct {
        // Everything accessed from qmp_backup_query command is protected using
        // this lock. Do NOT hold this lock for long times, as it is sometimes
        // acquired from coroutines, and thus any wait time may block the guest.
        QemuMutex lock;
        Error *error;
        time_t start_time;
        time_t end_time;
        char *backup_file;
        uuid_t uuid;
        char uuid_str[37];
        size_t total;
        size_t dirty;
        size_t transferred;
        size_t reused;
        size_t zero_bytes;
        GList *bitmap_list;
        bool finishing;
        bool starting;
    } stat;
    int64_t speed;
    BackupPerf perf;
    VmaWriter *vmaw;
    ProxmoxBackupHandle *pbs;
    GList *di_list;
    JobTxn *txn;
    CoMutex backup_mutex;
    CoMutex dump_callback_mutex;
    char *target_id;
} backup_state;

static void pvebackup_init(void)
{
    qemu_mutex_init(&backup_state.stat.lock);
    qemu_co_mutex_init(&backup_state.backup_mutex);
    qemu_co_mutex_init(&backup_state.dump_callback_mutex);
}

// initialize PVEBackupState at startup
opts_init(pvebackup_init);

typedef struct PVEBackupFleecingInfo {
    BlockDriverState *bs;
    BlockDriverState *cbw;
    BlockDriverState *snapshot_access;
} PVEBackupFleecingInfo;

typedef struct PVEBackupDevInfo {
    BlockDriverState *bs;
    PVEBackupFleecingInfo fleecing;
    size_t size;
    uint64_t block_size;
    uint8_t dev_id;
    char* device_name;
    int completed_ret; // INT_MAX if not completed
    BdrvDirtyBitmap *bitmap;
    BdrvDirtyBitmap *background_bitmap; // used for external backup access
    PBSBitmapAction bitmap_action;
    BlockDriverState *target;
    BlockJob *job;
    BackupAccessSetupBitmapMode requested_bitmap_mode;
} PVEBackupDevInfo;

static void pvebackup_propagate_error(Error *err)
{
    qemu_mutex_lock(&backup_state.stat.lock);
    error_propagate(&backup_state.stat.error, err);
    qemu_mutex_unlock(&backup_state.stat.lock);
}

static bool pvebackup_error_or_canceled(void)
{
    qemu_mutex_lock(&backup_state.stat.lock);
    bool error_or_canceled = !!backup_state.stat.error;
    qemu_mutex_unlock(&backup_state.stat.lock);

    return error_or_canceled;
}

static void pvebackup_add_transferred_bytes(size_t transferred, size_t zero_bytes, size_t reused)
{
    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.zero_bytes += zero_bytes;
    backup_state.stat.transferred += transferred;
    backup_state.stat.reused += reused;
    qemu_mutex_unlock(&backup_state.stat.lock);
}

// This may get called from multiple coroutines in multiple io-threads
// Note1: this may get called after job_cancel()
static int coroutine_fn
pvebackup_co_dump_pbs_cb(
    void *opaque,
    uint64_t start,
    uint64_t bytes,
    const void *pbuf)
{
    assert(qemu_in_coroutine());

    const uint64_t size = bytes;
    const unsigned char *buf = pbuf;
    PVEBackupDevInfo *di = opaque;

    assert(backup_state.pbs);
    assert(buf);

    Error *local_err = NULL;
    int pbs_res = -1;

    bool is_zero_block = size == di->block_size && buffer_is_zero(buf, size);

    qemu_co_mutex_lock(&backup_state.dump_callback_mutex);

    // avoid deadlock if job is cancelled
    if (pvebackup_error_or_canceled()) {
        qemu_co_mutex_unlock(&backup_state.dump_callback_mutex);
        return -1;
    }

    uint64_t transferred = 0;
    uint64_t reused = 0;
    while (transferred < size) {
        uint64_t left = size - transferred;
        uint64_t to_transfer = left < di->block_size ? left : di->block_size;

        pbs_res = proxmox_backup_co_write_data(backup_state.pbs, di->dev_id,
            is_zero_block ? NULL : buf + transferred, start + transferred,
            to_transfer, &local_err);
        transferred += to_transfer;

        if (pbs_res < 0) {
            pvebackup_propagate_error(local_err);
            qemu_co_mutex_unlock(&backup_state.dump_callback_mutex);
            return pbs_res;
        }

        reused += pbs_res == 0 ? to_transfer : 0;
    }

    qemu_co_mutex_unlock(&backup_state.dump_callback_mutex);
    pvebackup_add_transferred_bytes(size, is_zero_block ? size : 0, reused);

    return size;
}

// This may get called from multiple coroutines in multiple io-threads
static int coroutine_fn
pvebackup_co_dump_vma_cb(
    void *opaque,
    uint64_t start,
    uint64_t bytes,
    const void *pbuf)
{
    assert(qemu_in_coroutine());

    const uint64_t size = bytes;
    const unsigned char *buf = pbuf;
    PVEBackupDevInfo *di = opaque;

    int ret = -1;

    assert(backup_state.vmaw);
    assert(buf);

    uint64_t remaining = size;

    uint64_t cluster_num = start / VMA_CLUSTER_SIZE;
    if ((cluster_num * VMA_CLUSTER_SIZE) != start) {
        Error *local_err = NULL;
        error_setg(&local_err,
                   "got unaligned write inside backup dump "
                   "callback (sector %ld)", start);
        pvebackup_propagate_error(local_err);
        return -1; // not aligned to cluster size
    }

    while (remaining > 0) {
        qemu_co_mutex_lock(&backup_state.dump_callback_mutex);
        // avoid deadlock if job is cancelled
        if (pvebackup_error_or_canceled()) {
            qemu_co_mutex_unlock(&backup_state.dump_callback_mutex);
            return -1;
        }

        size_t zero_bytes = 0;
        ret = vma_writer_write(backup_state.vmaw, di->dev_id, cluster_num, buf, &zero_bytes);
        qemu_co_mutex_unlock(&backup_state.dump_callback_mutex);

        ++cluster_num;
        buf += VMA_CLUSTER_SIZE;
        if (ret < 0) {
            Error *local_err = NULL;
            vma_writer_error_propagate(backup_state.vmaw, &local_err);
            pvebackup_propagate_error(local_err);
            return ret;
        } else {
            if (remaining >= VMA_CLUSTER_SIZE) {
                assert(ret == VMA_CLUSTER_SIZE);
                pvebackup_add_transferred_bytes(VMA_CLUSTER_SIZE, zero_bytes, 0);
                remaining -= VMA_CLUSTER_SIZE;
            } else {
                assert(ret == remaining);
                pvebackup_add_transferred_bytes(remaining, zero_bytes, 0);
                remaining = 0;
            }
        }
    }

    return size;
}

// assumes the caller holds backup_mutex
static void coroutine_fn pvebackup_co_cleanup(void)
{
    assert(qemu_in_coroutine());

    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.finishing = true;
    qemu_mutex_unlock(&backup_state.stat.lock);

    if (backup_state.vmaw) {
        Error *local_err = NULL;
        vma_writer_close(backup_state.vmaw, &local_err);

        if (local_err != NULL) {
            pvebackup_propagate_error(local_err);
         }

        backup_state.vmaw = NULL;
    }

    if (backup_state.pbs) {
        if (!pvebackup_error_or_canceled()) {
            Error *local_err = NULL;
            proxmox_backup_co_finish(backup_state.pbs, &local_err);
            if (local_err != NULL) {
                pvebackup_propagate_error(local_err);
            }
        }

        proxmox_backup_disconnect(backup_state.pbs);
        backup_state.pbs = NULL;
    }

    g_list_free(backup_state.di_list);
    backup_state.di_list = NULL;

    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.end_time = time(NULL);
    backup_state.stat.finishing = false;
    qemu_mutex_unlock(&backup_state.stat.lock);

#if defined(CONFIG_MALLOC_TRIM)
    /*
     * Try to reclaim memory for buffers (and, in case of PBS, Rust futures), etc.
     * Won't happen by default if there is fragmentation.
     */
    malloc_trim(4 * 1024 * 1024);
#endif
}

static void coroutine_fn pvebackup_co_complete_stream(void *opaque)
{
    PVEBackupDevInfo *di = opaque;
    int ret = di->completed_ret;

    qemu_mutex_lock(&backup_state.stat.lock);
    bool starting = backup_state.stat.starting;
    qemu_mutex_unlock(&backup_state.stat.lock);
    if (starting) {
        /* in 'starting' state, no tasks have been run yet, meaning we can (and
         * must) skip all cleanup, as we don't know what has and hasn't been
         * initialized yet. */
        return;
    }

    qemu_co_mutex_lock(&backup_state.backup_mutex);

    /*
     * All jobs in the transaction will be canceled when one receives an error.
     * The first error wins, so only set it for ECANCELED if it was the last
     * job. This allows more interesting errors from other jobs to win.
     */
    if (ret < 0 && (ret != -ECANCELED || !g_list_nth(backup_state.di_list, 1))) {
        Error *local_err = NULL;
        error_setg(&local_err, "job failed with err %d - %s", ret, strerror(-ret));
        pvebackup_propagate_error(local_err);
    }

    di->bs = NULL;
    g_free(di->device_name);
    di->device_name = NULL;

    assert(di->target == NULL);

    bool error_or_canceled = pvebackup_error_or_canceled();

    if (backup_state.vmaw) {
        vma_writer_close_stream(backup_state.vmaw, di->dev_id);
    }

    if (backup_state.pbs && !error_or_canceled) {
        Error *local_err = NULL;
        proxmox_backup_co_close_image(backup_state.pbs, di->dev_id, &local_err);
        if (local_err != NULL) {
            pvebackup_propagate_error(local_err);
        }
    }

    // remove self from job list
    backup_state.di_list = g_list_remove(backup_state.di_list, di);

    g_free(di);

    /* call cleanup if we're the last job */
    if (!g_list_first(backup_state.di_list)) {
        pvebackup_co_cleanup();
    }

    qemu_co_mutex_unlock(&backup_state.backup_mutex);
}

/*
 * New writes since the backup access was set up are in the background bitmap. Because of failure,
 * the previously tracked writes in di->bitmap are still required too. Thus, merge with the
 * background bitmap to get all new writes since the last backup.
 */
static void handle_backup_access_bitmaps_in_error_case(PVEBackupDevInfo *di)
{
    Error *local_err = NULL;

    if (di->bs && di->background_bitmap) {
        bdrv_drained_begin(di->bs);
        if (di->bitmap) {
            bdrv_enable_dirty_bitmap(di->bitmap);
            if (!bdrv_merge_dirty_bitmap(di->bitmap, di->background_bitmap, NULL, &local_err)) {
                warn_report("backup access: %s - could not merge bitmaps in error path - %s",
                            di->device_name,
                            local_err ? error_get_pretty(local_err) : "unknown error");
                /*
                 * Could not merge, drop original bitmap too.
                 */
                bdrv_release_dirty_bitmap(di->bitmap);
            }
        } else {
            warn_report("backup access: %s - expected bitmap not present", di->device_name);
        }
        bdrv_release_dirty_bitmap(di->background_bitmap);
        bdrv_drained_end(di->bs);
    }
}

/*
 * Continue tracking for next incremental backup in di->bitmap. New writes since the backup access
 * was set up are in the background bitmap. Because the backup was successful, clear di->bitmap and
 * merge back the background bitmap to get only the new writes.
 */
static void handle_backup_access_bitmaps_after_success(PVEBackupDevInfo *di)
{
    Error *local_err = NULL;

    if (di->bs && di->background_bitmap) {
        bdrv_drained_begin(di->bs);
        if (di->bitmap) {
            bdrv_enable_dirty_bitmap(di->bitmap);
            bdrv_clear_dirty_bitmap(di->bitmap, NULL);
            if (!bdrv_merge_dirty_bitmap(di->bitmap, di->background_bitmap, NULL, &local_err)) {
                warn_report("backup access: %s - could not merge bitmaps after backup - %s",
                            di->device_name,
                            local_err ? error_get_pretty(local_err) : "unknown error");
                /*
                 * Could not merge, drop original bitmap too.
                 */
                bdrv_release_dirty_bitmap(di->bitmap);
            }
        } else {
            warn_report("backup access: %s - expected bitmap not present", di->device_name);
        }
        bdrv_release_dirty_bitmap(di->background_bitmap);
        bdrv_drained_end(di->bs);
    }
}

static void cleanup_snapshot_access(PVEBackupDevInfo *di)
{
    if (di->fleecing.snapshot_access) {
        bdrv_unref(di->fleecing.snapshot_access);
        di->fleecing.snapshot_access = NULL;
    }
    if (di->fleecing.cbw) {
        bdrv_cbw_drop(di->fleecing.cbw);
        di->fleecing.cbw = NULL;
    }
}

static void pvebackup_complete_cb(void *opaque, int ret)
{
    PVEBackupDevInfo *di = opaque;
    di->completed_ret = ret;

    if (di->fleecing.cbw) {
        /*
         * With fleecing, failure for cbw does not fail the guest write, but only sets the snapshot
         * error, making further requests to the snapshot fail with EACCES, which then also fail the
         * job. But that code is not the root cause and just confusing, so update it.
         */
        int snapshot_error = bdrv_cbw_snapshot_error(di->fleecing.cbw);
        if (di->completed_ret == -EACCES && snapshot_error) {
            di->completed_ret = snapshot_error;
        }
    }

    /*
     * Handle block-graph specific cleanup (for fleecing) outside of the coroutine, because the work
     * won't be done as a coroutine anyways:
     * - For snapshot_access, allows doing bdrv_unref() directly. Doing it via bdrv_co_unref() would
     *   just spawn a BH calling bdrv_unref().
     * - For cbw, draining would need to spawn a BH.
     */
    cleanup_snapshot_access(di);

    /*
     * Needs to happen outside of coroutine, because it takes the graph write lock.
     */
    if (di->job) {
        WITH_JOB_LOCK_GUARD() {
            job_unref_locked(&di->job->job);
            di->job = NULL;
        }
    }

    /*
     * Schedule stream cleanup in async coroutine. close_image and finish might
     * take a while, so we can't block on them here. This way it also doesn't
     * matter if we're already running in a coroutine or not.
     * Note: di is a pointer to an entry in the global backup_state struct, so
     * it stays valid.
     */
    Coroutine *co = qemu_coroutine_create(pvebackup_co_complete_stream, di);
    aio_co_enter(qemu_get_aio_context(), co);
}

/*
 * job_cancel(_sync) does not like to be called from coroutines, so defer to
 * main loop processing via a bottom half. Assumes that caller holds
 * backup_mutex.
 */
static void job_cancel_bh(void *opaque) {
    CoCtxData *data = (CoCtxData*)opaque;

    /*
     * Be careful to pick a valid job to cancel:
     * 1. job_cancel_sync() does not expect the job to be finalized already.
     * 2. job_exit() might run between scheduling and running job_cancel_bh()
     *    and pvebackup_co_complete_stream() might not have removed the job from
     *    the list yet (in fact, cannot, because it waits for the backup_mutex).
     * Requiring !job_is_completed() ensures that no finalized job is picked.
     */
    GList *bdi = g_list_first(backup_state.di_list);
    while (bdi) {
        if (bdi->data) {
            BlockJob *bj = ((PVEBackupDevInfo *)bdi->data)->job;
            if (bj) {
                Job *job = &bj->job;
                WITH_JOB_LOCK_GUARD() {
                    if (!job_is_completed_locked(job)) {
                        job_cancel_sync_locked(job, true);
                        /*
                         * It's enough to cancel one job in the transaction, the
                         * rest will follow automatically.
                         */
                        break;
                    }
                }
            }
        }
        bdi = g_list_next(bdi);
    }

    aio_co_enter(data->ctx, data->co);
}

void coroutine_fn qmp_backup_cancel(Error **errp)
{
    Error *cancel_err = NULL;
    error_setg(&cancel_err, "backup canceled");
    pvebackup_propagate_error(cancel_err);

    qemu_co_mutex_lock(&backup_state.backup_mutex);

    if (backup_state.vmaw) {
        /* make sure vma writer does not block anymore */
        vma_writer_set_error(backup_state.vmaw, "backup canceled");
    }

    if (backup_state.pbs) {
        proxmox_backup_abort(backup_state.pbs, "backup canceled");
    }

    CoCtxData data = {
        .ctx = qemu_get_current_aio_context(),
        .co = qemu_coroutine_self(),
    };
    aio_bh_schedule_oneshot(data.ctx, job_cancel_bh, &data);
    qemu_coroutine_yield();

    qemu_co_mutex_unlock(&backup_state.backup_mutex);
}

// assumes the caller holds backup_mutex
static int coroutine_fn pvebackup_co_add_config(
    const char *file,
    const char *name,
    BackupFormat format,
    VmaWriter *vmaw,
    ProxmoxBackupHandle *pbs,
    Error **errp)
{
    int res = 0;

    char *cdata = NULL;
    gsize clen = 0;
    GError *err = NULL;
    if (!g_file_get_contents(file, &cdata, &clen, &err)) {
        error_setg(errp, "unable to read file '%s'", file);
        return 1;
    }

    char *basename = g_path_get_basename(file);
    if (name == NULL) name = basename;

    if (format == BACKUP_FORMAT_VMA) {
        if (vma_writer_add_config(vmaw, name, cdata, clen) != 0) {
            error_setg(errp, "unable to add %s config data to vma archive", file);
            goto err;
        }
    } else if (format == BACKUP_FORMAT_PBS) {
        if (proxmox_backup_co_add_config(pbs, name, (unsigned char *)cdata, clen, errp) < 0)
            goto err;
    }

 out:
    g_free(basename);
    g_free(cdata);
    return res;

 err:
    res = -1;
    goto out;
}

/*
 * Setup a snapshot-access block node for a device with associated fleecing image.
 */
static int setup_snapshot_access(PVEBackupDevInfo *di, Error **errp)
{
    Error *local_err = NULL;

    if (!di->fleecing.bs) {
        error_setg(errp, "no associated fleecing image");
        return -1;
    }

    QDict *cbw_opts = qdict_new();
    qdict_put_str(cbw_opts, "driver", "copy-before-write");
    qdict_put_str(cbw_opts, "file", bdrv_get_node_name(di->bs));
    qdict_put_str(cbw_opts, "target", bdrv_get_node_name(di->fleecing.bs));

    if (di->bitmap) {
        /*
         * Only guest writes to parts relevant for the backup need to be intercepted with
         * old data being copied to the fleecing image.
         */
        qdict_put_str(cbw_opts, "bitmap.node", bdrv_get_node_name(di->bs));
        qdict_put_str(cbw_opts, "bitmap.name", bdrv_dirty_bitmap_name(di->bitmap));
    }
    /*
     * Fleecing storage is supposed to be fast and it's better to break backup than guest
     * writes. Certain guest drivers like VirtIO-win have 60 seconds timeout by default, so
     * abort a bit before that.
     */
    qdict_put_str(cbw_opts, "on-cbw-error", "break-snapshot");
    qdict_put_int(cbw_opts, "cbw-timeout", 45);

    di->fleecing.cbw = bdrv_insert_node(di->bs, cbw_opts, BDRV_O_RDWR, &local_err);

    if (!di->fleecing.cbw) {
        error_setg(errp, "appending cbw node for fleecing failed: %s",
                   local_err ? error_get_pretty(local_err) : "unknown error");
        return -1;
    }

    QDict *snapshot_access_opts = qdict_new();
    qdict_put_str(snapshot_access_opts, "driver", "snapshot-access");
    qdict_put_str(snapshot_access_opts, "file", bdrv_get_node_name(di->fleecing.cbw));

    di->fleecing.snapshot_access =
        bdrv_open(NULL, NULL, snapshot_access_opts, BDRV_O_RDWR | BDRV_O_UNMAP, &local_err);
    if (!di->fleecing.snapshot_access) {
        bdrv_cbw_drop(di->fleecing.cbw);
        di->fleecing.cbw = NULL;

        error_setg(errp, "setting up snapshot access for fleecing failed: %s",
                   local_err ? error_get_pretty(local_err) : "unknown error");
        return -1;
    }

    return 0;
}

static void setup_all_snapshot_access_bh(void *opaque)
{
    assert(!qemu_in_coroutine());

    CoCtxData *data = (CoCtxData*)opaque;
    Error **errp = (Error**)data->data;

    Error *local_err = NULL;

    GList *l =  backup_state.di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        bdrv_drained_begin(di->bs);

        if (di->bitmap) {
            BdrvDirtyBitmap *background_bitmap =
                bdrv_create_dirty_bitmap(di->bs, PROXMOX_BACKUP_DEFAULT_CHUNK_SIZE,
                                         BACKGROUND_BITMAP_NAME, &local_err);
            if (!background_bitmap) {
                error_setg(errp, "%s - creating background bitmap for backup access failed: %s",
                           di->device_name,
                           local_err ? error_get_pretty(local_err) : "unknown error");
                bdrv_drained_end(di->bs);
                break;
            }
            di->background_bitmap = background_bitmap;
            bdrv_disable_dirty_bitmap(di->bitmap);
        }

        if (setup_snapshot_access(di, &local_err) < 0) {
            bdrv_drained_end(di->bs);
            error_setg(errp, "%s - setting up snapshot access failed: %s", di->device_name,
                       local_err ? error_get_pretty(local_err) : "unknown error");
            break;
        }

        bdrv_drained_end(di->bs);
    }

    /* return */
    aio_co_enter(data->ctx, data->co);
}

/*
 * backup_job_create can *not* be run from a coroutine, so this can't either.
 * The caller is responsible that backup_mutex is held nonetheless.
 */
static void create_backup_jobs_bh(void *opaque) {

    assert(!qemu_in_coroutine());

    CoCtxData *data = (CoCtxData*)opaque;
    Error **errp = (Error**)data->data;

    Error *local_err = NULL;

    /* create job transaction to synchronize bitmap commit and cancel all
     * jobs in case one errors */
    if (backup_state.txn) {
        job_txn_unref(backup_state.txn);
    }
    backup_state.txn = job_txn_new_seq();

    /* create and start all jobs (paused state) */
    GList *l =  backup_state.di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        assert(di->target != NULL);

        MirrorSyncMode sync_mode = MIRROR_SYNC_MODE_FULL;
        BitmapSyncMode bitmap_mode = BITMAP_SYNC_MODE_NEVER;
        if (di->bitmap) {
            sync_mode = MIRROR_SYNC_MODE_BITMAP;
            bitmap_mode = BITMAP_SYNC_MODE_ON_SUCCESS;
        }
        bdrv_drained_begin(di->bs);

        BackupPerf perf = (BackupPerf){ .max_workers = backup_state.perf.max_workers };

        BlockDriverState *source_bs = di->bs;
        bool discard_source = false;
        if (di->fleecing.bs) {
            if (setup_snapshot_access(di, &local_err) < 0) {
                error_setg(errp, "%s - setting up snapshot access for fleecing failed: %s",
                           di->device_name,
                           local_err ? error_get_pretty(local_err) : "unknown error");
                bdrv_drained_end(di->bs);
                break;
            }

            source_bs = di->fleecing.snapshot_access;
            discard_source = true;

            /*
             * bdrv_get_info() just retuns 0 (= doesn't matter) for RBD when using krbd. But discard
             * on the fleecing image won't work if the backup job's granularity is less than the RBD
             * object size (default 4 MiB), so it does matter. Always use at least 4 MiB. With a PBS
             * target, the backup job granularity would already be at least this much.
             */
            perf.min_cluster_size = 4 * 1024 * 1024;
            /*
             * For discard to work, cluster size for the backup job must be at least the same as for
             * the fleecing image.
             */
            BlockDriverInfo bdi;
            if (bdrv_get_info(di->fleecing.bs, &bdi) >= 0) {
                perf.min_cluster_size = MAX(perf.min_cluster_size, bdi.cluster_size);
            }
        }

        BlockJob *job = backup_job_create(
            di->device_name, source_bs, di->target, backup_state.speed, sync_mode, di->bitmap,
            bitmap_mode, false, discard_source, NULL, &perf, BLOCKDEV_ON_ERROR_REPORT,
            BLOCKDEV_ON_ERROR_REPORT, ON_CBW_ERROR_BREAK_GUEST_WRITE, JOB_DEFAULT,
            pvebackup_complete_cb, di, backup_state.txn, &local_err);

        bdrv_drained_end(di->bs);

        di->job = job;
        if (job) {
            WITH_JOB_LOCK_GUARD() {
                job_ref_locked(&job->job);
            }
        }

        if (!job || local_err) {
            cleanup_snapshot_access(di);
            error_setg(errp, "backup_job_create failed: %s",
                       local_err ? error_get_pretty(local_err) : "null");
            break;
        }

        bdrv_unref(di->target);
        di->target = NULL;
    }

    if (*errp) {
        /*
         * It's enough to cancel one job in the transaction, the rest will
         * follow automatically.
         */
        bool canceled = false;
        l = backup_state.di_list;
        while (l) {
            PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
            l = g_list_next(l);

            if (di->target) {
                bdrv_unref(di->target);
                di->target = NULL;
            }

            if (di->job) {
                WITH_JOB_LOCK_GUARD() {
                    if (!canceled) {
                        job_cancel_sync_locked(&di->job->job, true);
                        canceled = true;
                    }
                    job_unref_locked(&di->job->job);
                    di->job = NULL;
                }
            }
        }
    }

    /* return */
    aio_co_enter(data->ctx, data->co);
}

/*
 * EFI disk and TPM state are small and it's just not worth setting up fleecing for them.
 */
static bool fleecing_no_efi_tpm(const char *device_id)
{
    return strncmp(device_id, "drive-efidisk", 13) && strncmp(device_id, "drive-tpmstate", 14);
}

static bool fleecing_all(const char *device_id)
{
    return true;
}

static PVEBackupDevInfo coroutine_fn GRAPH_RDLOCK *get_single_device_info(
    const char *device,
    bool (*device_uses_fleecing)(const char*),
    Error **errp)
{
    BlockBackend *blk = blk_by_name(device);
    BlockDriverState *root_bs, *bs;

    if (blk) {
        root_bs = bs = blk_bs(blk);
    } else {
        /* TODO PVE 10 - fleecing will always be attached without blk */
        root_bs = bs = bdrv_find_node(device);
        if (!bs) {
            error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                      "Device '%s' not found", device);
            return NULL;
        }
        /* For TPM, bs is already correct, otherwise need the file child. */
        if (!strncmp(bs->drv->format_name, "throttle", 8)) {
            if (!bs->file || !bs->file->bs) {
                error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                          "Device '%s' not found (no file child)", device);
                return NULL;
            }
            bs = bs->file->bs;
        }
    }

    if (!bdrv_co_is_inserted(bs)) {
        error_setg(errp, "Device '%s' has no medium", device);
        return NULL;
    }

    PVEBackupDevInfo *di = g_new0(PVEBackupDevInfo, 1);
    di->bs = bs;
    /* Need the name of the root node, e.g. drive-scsi0 */
    di->device_name = g_strdup(bdrv_get_device_or_node_name(root_bs));

    if (device_uses_fleecing && device_uses_fleecing(device)) {
        g_autofree gchar *fleecing_devid = g_strconcat(device, "-fleecing", NULL);
        BlockDriverState *fleecing_bs;

        BlockBackend *fleecing_blk = blk_by_name(fleecing_devid);
        if (fleecing_blk) {
            fleecing_bs = blk_bs(fleecing_blk);
        } else {
            /* TODO PVE 10 - fleecing will always be attached without blk */
            fleecing_bs = bdrv_find_node(fleecing_devid);
            if (!fleecing_bs) {
                error_set(errp, ERROR_CLASS_DEVICE_NOT_FOUND,
                          "Device '%s' not found", fleecing_devid);
                goto fail;
            }
        }

        if (!bdrv_co_is_inserted(fleecing_bs)) {
            error_setg(errp, "Device '%s' has no medium", fleecing_devid);
            goto fail;
        }
        /*
         * Fleecing image needs to be the same size to act as a cbw target.
         */
        if (bs->total_sectors != fleecing_bs->total_sectors) {
            error_setg(errp, "Size mismatch for '%s' - sector count %ld != %ld",
                       fleecing_devid, fleecing_bs->total_sectors, bs->total_sectors);
            goto fail;
        }
        di->fleecing.bs = fleecing_bs;
    }

    return di;
fail:
    g_free(di->device_name);
    g_free(di);
    return NULL;
}

/*
 * Returns a list of device infos, which needs to be freed by the caller. In
 * case of an error, errp will be set, but the returned value might still be a
 * list.
 */
static GList coroutine_fn GRAPH_RDLOCK *get_device_info(
    const char *devlist,
    bool (*device_uses_fleecing)(const char*),
    Error **errp)
{
    gchar **devs = NULL;
    GList *di_list = NULL;

    if (devlist) {
        devs = g_strsplit_set(devlist, ",;:", -1);

        gchar **d = devs;
        while (d && *d) {
            PVEBackupDevInfo *di = get_single_device_info(*d, device_uses_fleecing, errp);
            if (!di) {
                goto err;
            }
            di_list = g_list_append(di_list, di);
            d++;
        }
    } else {
        BdrvNextIterator it;

        for (BlockDriverState *bs = bdrv_first(&it); bs; bs = bdrv_next(&it)) {
            if (!bdrv_co_is_inserted(bs) || bdrv_is_read_only(bs)) {
                continue;
            }

            PVEBackupDevInfo *di = g_new0(PVEBackupDevInfo, 1);
            di->bs = bs;
            di->device_name = g_strdup(bdrv_get_device_or_node_name(bs));
            di_list = g_list_append(di_list, di);
        }
    }

    if (!di_list) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR, "empty device list");
        goto err;
    }

err:
    if (devs) {
        g_strfreev(devs);
    }

    return di_list;
}

/*
 * To be called with the backup_state.stat mutex held.
 */
static void clear_backup_state_bitmap_list(void) {

    if (backup_state.stat.bitmap_list) {
        GList *bl = backup_state.stat.bitmap_list;
        while (bl) {
            g_free(((PBSBitmapInfo *)bl->data)->drive);
            g_free(bl->data);
            bl = g_list_next(bl);
        }
        g_list_free(backup_state.stat.bitmap_list);
        backup_state.stat.bitmap_list = NULL;
    }
}

/*
 * Initializes most of the backup state 'stat' struct. Note that 'reused' and
 * 'bitmap_list' are not changed by this function and need to be handled by
 * the caller. In particular, 'reused' needs to be set before calling this
 * function.
 *
 * To be called with the backup_state.stat mutex held.
 */
static void initialize_backup_state_stat(
    const char *backup_file,
    uuid_t *uuid,
    size_t total,
    bool starting)
{
    if (backup_state.stat.error) {
        error_free(backup_state.stat.error);
        backup_state.stat.error = NULL;
    }

    backup_state.stat.start_time = time(NULL);
    backup_state.stat.end_time = 0;

    if (backup_state.stat.backup_file) {
        g_free(backup_state.stat.backup_file);
    }
    backup_state.stat.backup_file = g_strdup(backup_file);

    if (uuid) {
        uuid_copy(backup_state.stat.uuid, *uuid);
        uuid_unparse_lower(*uuid, backup_state.stat.uuid_str);
    } else {
        backup_state.stat.uuid_str[0] = '\0';
    }

    backup_state.stat.total = total;
    backup_state.stat.dirty = total - backup_state.stat.reused;
    backup_state.stat.transferred = 0;
    backup_state.stat.zero_bytes = 0;
    backup_state.stat.finishing = false;
    backup_state.stat.starting = starting;
}

/*
 * To be called with the backup_state mutex held.
 */
static void backup_state_set_target_id(const char *target_id) {
    if (backup_state.target_id) {
        g_free(backup_state.target_id);
    }
    backup_state.target_id = g_strdup(target_id);
}

BackupAccessInfoList *coroutine_fn qmp_backup_access_setup(
    const char *target_id,
    BackupAccessSourceDeviceList *devices,
    Error **errp)
{
    assert(qemu_in_coroutine());

    qemu_co_mutex_lock(&backup_state.backup_mutex);

    Error *local_err = NULL;
    GList *di_list = NULL;
    GList *l;

    if (backup_state.di_list) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "previous backup for target '%s' not finished", backup_state.target_id);
        qemu_co_mutex_unlock(&backup_state.backup_mutex);
        return NULL;
    }

    bdrv_graph_co_rdlock();
    for (BackupAccessSourceDeviceList *it = devices; it; it = it->next) {
        PVEBackupDevInfo *di = get_single_device_info(it->value->device, fleecing_all, &local_err);
        if (!di) {
            bdrv_graph_co_rdunlock();
            error_propagate(errp, local_err);
            goto err;
        }
        di->requested_bitmap_mode = it->value->bitmap_mode;
        di_list = g_list_append(di_list, di);
    }
    bdrv_graph_co_rdunlock();
    assert(di_list);

    size_t total = 0;

    l = di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        ssize_t size = bdrv_getlength(di->bs);
        if (size < 0) {
            error_setg_errno(errp, -size, "bdrv_getlength failed");
            goto err;
        }
        di->size = size;
        total += size;

        di->completed_ret = INT_MAX;
    }

    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.reused = 0;

    /* clear previous backup's bitmap_list */
    clear_backup_state_bitmap_list();

    const char *bitmap_name = target_id;

    /* create bitmaps if requested */
    l = di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        di->block_size = PROXMOX_BACKUP_DEFAULT_CHUNK_SIZE;

        PBSBitmapAction action = PBS_BITMAP_ACTION_NOT_USED;
        size_t dirty = di->size;

        if (di->requested_bitmap_mode == BACKUP_ACCESS_SETUP_BITMAP_MODE_NONE ||
            di->requested_bitmap_mode == BACKUP_ACCESS_SETUP_BITMAP_MODE_NEW) {
            BdrvDirtyBitmap *old_bitmap = bdrv_find_dirty_bitmap(di->bs, bitmap_name);
            if (old_bitmap) {
                bdrv_release_dirty_bitmap(old_bitmap);
                action = PBS_BITMAP_ACTION_NOT_USED_REMOVED; // set below for new
            }
        }

        BdrvDirtyBitmap *bitmap = NULL;
        if (di->requested_bitmap_mode == BACKUP_ACCESS_SETUP_BITMAP_MODE_NEW ||
            di->requested_bitmap_mode == BACKUP_ACCESS_SETUP_BITMAP_MODE_USE) {
            bitmap = bdrv_find_dirty_bitmap(di->bs, bitmap_name);
            if (!bitmap) {
                bitmap = bdrv_create_dirty_bitmap(di->bs, PROXMOX_BACKUP_DEFAULT_CHUNK_SIZE,
                                                  bitmap_name, errp);
                if (!bitmap) {
                    qemu_mutex_unlock(&backup_state.stat.lock);
                    goto err;
                }
                bdrv_set_dirty_bitmap(bitmap, 0, di->size);
                if (di->requested_bitmap_mode == BACKUP_ACCESS_SETUP_BITMAP_MODE_USE) {
                    action = PBS_BITMAP_ACTION_MISSING_RECREATED;
                } else {
                    action = PBS_BITMAP_ACTION_NEW;
                }
            } else {
                if (di->requested_bitmap_mode == BACKUP_ACCESS_SETUP_BITMAP_MODE_NEW) {
                    qemu_mutex_unlock(&backup_state.stat.lock);
                    error_setg(errp, "internal error - removed old bitmap still present");
                    goto err;
                }
                /* track clean chunks as reused */
                dirty = MIN(bdrv_get_dirty_count(bitmap), di->size);
                backup_state.stat.reused += di->size - dirty;
                action = PBS_BITMAP_ACTION_USED;
            }
        }

        PBSBitmapInfo *info = g_malloc(sizeof(*info));
        info->drive = g_strdup(di->device_name);
        info->action = action;
        info->size = di->size;
        info->dirty = dirty;
        backup_state.stat.bitmap_list = g_list_append(backup_state.stat.bitmap_list, info);

        di->bitmap = bitmap;
        di->bitmap_action = action;
    }

    /* starting=false, because there is no associated QEMU job */
    initialize_backup_state_stat(NULL, NULL, total, false);

    qemu_mutex_unlock(&backup_state.stat.lock);

    backup_state_set_target_id(target_id);

    backup_state.vmaw = NULL;
    backup_state.pbs = NULL;

    backup_state.di_list = di_list;

    /* Run setup_all_snapshot_access_bh outside of coroutine (in BH) but keep
    * backup_mutex locked. This is fine, a CoMutex can be held across yield
    * points, and we'll release it as soon as the BH reschedules us.
    */
    CoCtxData waker = {
        .co = qemu_coroutine_self(),
        .ctx = qemu_get_current_aio_context(),
        .data = &local_err,
    };
    aio_bh_schedule_oneshot(waker.ctx, setup_all_snapshot_access_bh, &waker);
    qemu_coroutine_yield();

    if (local_err) {
        error_propagate(errp, local_err);
        goto err;
    }

    qemu_co_mutex_unlock(&backup_state.backup_mutex);

    BackupAccessInfoList *bai_head = NULL, **p_bai_next = &bai_head;

    l = di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        BackupAccessInfoList *info = g_malloc0(sizeof(*info));
        info->value = g_malloc0(sizeof(*info->value));
        info->value->node_name = g_strdup(bdrv_get_node_name(di->fleecing.snapshot_access));
        info->value->device = g_strdup(di->device_name);
        info->value->size = di->size;
        if (di->bitmap) {
            info->value->bitmap_node_name = g_strdup(bdrv_get_node_name(di->bs));
            info->value->bitmap_name = g_strdup(bitmap_name);
            info->value->bitmap_action = di->bitmap_action;
            info->value->has_bitmap_action = true;
        }

        *p_bai_next = info;
        p_bai_next = &info->next;
    }

    return bai_head;

err:

    l = di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        handle_backup_access_bitmaps_in_error_case(di);

        g_free(di->device_name);
        di->device_name = NULL;

        g_free(di);
    }
    g_list_free(di_list);
    backup_state.di_list = NULL;

    qemu_co_mutex_unlock(&backup_state.backup_mutex);
    return NULL;
}

/*
 * Caller needs to hold the backup mutex or the BQL.
 */
void backup_access_teardown(bool success)
{
    GList *l = backup_state.di_list;

    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.finishing = true;
    qemu_mutex_unlock(&backup_state.stat.lock);

    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        if (di->fleecing.snapshot_access) {
            bdrv_unref(di->fleecing.snapshot_access);
            di->fleecing.snapshot_access = NULL;
        }
        if (di->fleecing.cbw) {
            bdrv_cbw_drop(di->fleecing.cbw);
            di->fleecing.cbw = NULL;
        }

        if (success) {
            handle_backup_access_bitmaps_after_success(di);
        } else {
            handle_backup_access_bitmaps_in_error_case(di);
        }

        g_free(di->device_name);
        di->device_name = NULL;

        g_free(di);
    }
    g_list_free(backup_state.di_list);
    backup_state.di_list = NULL;

    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.end_time = time(NULL);
    backup_state.stat.finishing = false;
    qemu_mutex_unlock(&backup_state.stat.lock);
}

// Not done in a coroutine, because bdrv_co_unref() and cbw_drop() would just spawn BHs anyways.
// Caller needs to hold the backup_state.backup_mutex lock
static void backup_access_teardown_bh(void *opaque)
{
    CoCtxData *data = (CoCtxData*)opaque;

    backup_access_teardown(*((bool*)data->data));

    /* return */
    aio_co_enter(data->ctx, data->co);
}

void coroutine_fn qmp_backup_access_teardown(const char *target_id, bool success, Error **errp)
{
    assert(qemu_in_coroutine());

    qemu_co_mutex_lock(&backup_state.backup_mutex);

    if (!backup_state.target_id) { // nothing to do
        qemu_co_mutex_unlock(&backup_state.backup_mutex);
        return;
    }

    /*
     * Continue with target_id == NULL, used by the callback registered for qemu_cleanup()
     */
    if (target_id && strcmp(backup_state.target_id, target_id)) {
        error_setg(errp, "cannot teardown backup access - got target %s instead of %s",
                   target_id, backup_state.target_id);
        qemu_co_mutex_unlock(&backup_state.backup_mutex);
        return;
    }

    if (!strcmp(backup_state.target_id, "Proxmox VE")) {
        error_setg(errp, "cannot teardown backup access for PVE - use backup-cancel instead");
        qemu_co_mutex_unlock(&backup_state.backup_mutex);
        return;
    }

    CoCtxData waker = {
        .co = qemu_coroutine_self(),
        .ctx = qemu_get_current_aio_context(),
        .data = &success,
    };
    aio_bh_schedule_oneshot(waker.ctx, backup_access_teardown_bh, &waker);
    qemu_coroutine_yield();

    qemu_co_mutex_unlock(&backup_state.backup_mutex);
    return;
}

UuidInfo coroutine_fn *qmp_backup(
    const char *backup_file,
    const char *password,
    const char *keyfile,
    const char *key_password,
    const char *master_keyfile,
    const char *fingerprint,
    const char *backup_ns,
    const char *backup_id,
    bool has_backup_time, int64_t backup_time,
    bool has_use_dirty_bitmap, bool use_dirty_bitmap,
    bool has_compress, bool compress,
    bool has_encrypt, bool encrypt,
    bool has_format, BackupFormat format,
    const char *config_file,
    const char *firewall_file,
    const char *devlist,
    bool has_speed, int64_t speed,
    bool has_max_workers, int64_t max_workers,
    bool has_fleecing, bool fleecing,
    Error **errp)
{
    assert(qemu_in_coroutine());

    qemu_co_mutex_lock(&backup_state.backup_mutex);

    Error *local_err = NULL;
    uuid_t uuid;
    VmaWriter *vmaw = NULL;
    ProxmoxBackupHandle *pbs = NULL;
    GList *di_list = NULL;
    GList *l;
    UuidInfo *uuid_info;

    const char *config_name = "qemu-server.conf";
    const char *firewall_name = "qemu-server.fw";

    if (backup_state.di_list) {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                  "previous backup for target '%s' not finished", backup_state.target_id);
        qemu_co_mutex_unlock(&backup_state.backup_mutex);
        return NULL;
    }

    /* Todo: try to auto-detect format based on file name */
    format = has_format ? format : BACKUP_FORMAT_VMA;

    bdrv_graph_co_rdlock();
    di_list = get_device_info(devlist, (has_fleecing && fleecing) ? fleecing_no_efi_tpm : NULL,
                              &local_err);
    bdrv_graph_co_rdunlock();
    if (local_err) {
        error_propagate(errp, local_err);
        goto err;
    }
    assert(di_list);

    size_t total = 0;

    l = di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        bdrv_graph_co_rdlock();
        bool blocked = bdrv_op_is_blocked(di->bs, BLOCK_OP_TYPE_BACKUP_SOURCE, errp);
        bdrv_graph_co_rdunlock();
        if (blocked) {
            goto err;
        }

        ssize_t size = bdrv_getlength(di->bs);
        if (size < 0) {
            error_setg_errno(errp, -size, "bdrv_getlength failed");
            goto err;
        }
        di->size = size;
        total += size;

        di->completed_ret = INT_MAX;
    }

    uuid_generate(uuid);

    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.reused = 0;

    /* clear previous backup's bitmap_list */
    clear_backup_state_bitmap_list();

    if (format == BACKUP_FORMAT_PBS) {
        if (!password) {
            error_set(errp, ERROR_CLASS_GENERIC_ERROR, "missing parameter 'password'");
            goto err_mutex;
        }
        if (!backup_id) {
            error_set(errp, ERROR_CLASS_GENERIC_ERROR, "missing parameter 'backup-id'");
            goto err_mutex;
        }
        if (!has_backup_time) {
            error_set(errp, ERROR_CLASS_GENERIC_ERROR, "missing parameter 'backup-time'");
            goto err_mutex;
        }

        int dump_cb_block_size = PROXMOX_BACKUP_DEFAULT_CHUNK_SIZE; // Hardcoded (4M)
        firewall_name = "fw.conf";

        char *pbs_err = NULL;
        pbs = proxmox_backup_new_ns(
            backup_file,
            backup_ns,
            backup_id,
            backup_time,
            dump_cb_block_size,
            password,
            keyfile,
            key_password,
            master_keyfile,
            has_compress ? compress : true,
            has_encrypt ? encrypt : !!keyfile,
            fingerprint,
            &pbs_err);

        if (!pbs) {
            error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                      "proxmox_backup_new failed: %s", pbs_err);
            proxmox_backup_free_error(pbs_err);
            goto err_mutex;
        }

        int connect_result = proxmox_backup_co_connect(pbs, errp);
        if (connect_result < 0)
            goto err_mutex;

        /* register all devices */
        l = di_list;
        while (l) {
            PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
            l = g_list_next(l);

            di->block_size = dump_cb_block_size;

            PBSBitmapAction action = PBS_BITMAP_ACTION_NOT_USED;
            size_t dirty = di->size;

            BdrvDirtyBitmap *bitmap = bdrv_find_dirty_bitmap(di->bs, PBS_BITMAP_NAME);
            bool expect_only_dirty = false;

            if (has_use_dirty_bitmap && use_dirty_bitmap) {
                if (bitmap == NULL) {
                    bitmap = bdrv_create_dirty_bitmap(di->bs, dump_cb_block_size, PBS_BITMAP_NAME, errp);
                    if (!bitmap) {
                        goto err_mutex;
                    }
                    action = PBS_BITMAP_ACTION_NEW;
                } else {
                    expect_only_dirty =
                        proxmox_backup_check_incremental(pbs, di->device_name, di->size) != 0;
                }

                if (expect_only_dirty) {
                    /* track clean chunks as reused */
                    dirty = MIN(bdrv_get_dirty_count(bitmap), di->size);
                    backup_state.stat.reused += di->size - dirty;
                    action = PBS_BITMAP_ACTION_USED;
                } else {
                    /* mark entire bitmap as dirty to make full backup */
                    bdrv_set_dirty_bitmap(bitmap, 0, di->size);
                    if (action != PBS_BITMAP_ACTION_NEW) {
                        action = PBS_BITMAP_ACTION_INVALID;
                    }
                }
                di->bitmap = bitmap;
            } else {
                /* after a full backup the old dirty bitmap is invalid anyway */
                if (bitmap != NULL) {
                    bdrv_release_dirty_bitmap(bitmap);
                    action = PBS_BITMAP_ACTION_NOT_USED_REMOVED;
                }
            }

            int dev_id = proxmox_backup_co_register_image(pbs, di->device_name, di->size,
                                                          expect_only_dirty, errp);
            if (dev_id < 0) {
                goto err_mutex;
            }

            if (!(di->target = bdrv_co_backup_dump_create(dump_cb_block_size, di->size, pvebackup_co_dump_pbs_cb, di, errp))) {
                goto err_mutex;
            }

            di->dev_id = dev_id;
            di->bitmap_action = action;

            PBSBitmapInfo *info = g_malloc(sizeof(*info));
            info->drive = g_strdup(di->device_name);
            info->action = action;
            info->size = di->size;
            info->dirty = dirty;
            backup_state.stat.bitmap_list = g_list_append(backup_state.stat.bitmap_list, info);
        }
    } else if (format == BACKUP_FORMAT_VMA) {
        vmaw = vma_writer_create(backup_file, uuid, &local_err);
        if (!vmaw) {
            error_propagate(errp, local_err);
            goto err_mutex;
        }

        /* register all devices for vma writer */
        l = di_list;
        while (l) {
            PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
            l = g_list_next(l);

            if (!(di->target = bdrv_co_backup_dump_create(VMA_CLUSTER_SIZE, di->size, pvebackup_co_dump_vma_cb, di, errp))) {
                goto err_mutex;
            }

            di->dev_id = vma_writer_register_stream(vmaw, di->device_name, di->size);
            if (di->dev_id <= 0) {
                error_set(errp, ERROR_CLASS_GENERIC_ERROR,
                          "register_stream failed");
                goto err_mutex;
            }
        }
    } else {
        error_set(errp, ERROR_CLASS_GENERIC_ERROR, "unknown backup format");
        goto err_mutex;
    }

    /* add configuration file to archive */
    if (config_file) {
        if (pvebackup_co_add_config(config_file, config_name, format, vmaw, pbs, errp) != 0) {
            goto err_mutex;
        }
    }

    /* add firewall file to archive */
    if (firewall_file) {
        if (pvebackup_co_add_config(firewall_file, firewall_name, format, vmaw, pbs, errp) != 0) {
            goto err_mutex;
        }
    }
    /* initialize global backup_state now */
    initialize_backup_state_stat(backup_file, &uuid, total, true);
    char *uuid_str = g_strdup(backup_state.stat.uuid_str);

    qemu_mutex_unlock(&backup_state.stat.lock);

    backup_state.speed = (has_speed && speed > 0) ? speed : 0;

    backup_state.perf = (BackupPerf){ .max_workers = 16 };
    if (has_max_workers) {
        backup_state.perf.max_workers = max_workers;
    }

    backup_state.vmaw = vmaw;
    backup_state.pbs = pbs;

    backup_state_set_target_id("Proxmox");

    backup_state.di_list = di_list;

    uuid_info = g_malloc0(sizeof(*uuid_info));
    uuid_info->UUID = uuid_str;

    /* Run create_backup_jobs_bh outside of coroutine (in BH) but keep
    * backup_mutex locked. This is fine, a CoMutex can be held across yield
    * points, and we'll release it as soon as the BH reschedules us.
    */
    CoCtxData waker = {
        .co = qemu_coroutine_self(),
        .ctx = qemu_get_current_aio_context(),
        .data = &local_err,
    };
    aio_bh_schedule_oneshot(waker.ctx, create_backup_jobs_bh, &waker);
    qemu_coroutine_yield();

    if (local_err) {
        error_propagate(errp, local_err);
        goto err;
    }

    qemu_co_mutex_unlock(&backup_state.backup_mutex);

    qemu_mutex_lock(&backup_state.stat.lock);
    backup_state.stat.starting = false;
    qemu_mutex_unlock(&backup_state.stat.lock);

    /* start the first job in the transaction */
    job_txn_start_seq(backup_state.txn);

    return uuid_info;

err_mutex:
    qemu_mutex_unlock(&backup_state.stat.lock);

err:

    l = di_list;
    while (l) {
        PVEBackupDevInfo *di = (PVEBackupDevInfo *)l->data;
        l = g_list_next(l);

        if (di->target) {
            bdrv_co_unref(di->target);
        }

        g_free(di->device_name);
        di->device_name = NULL;

        g_free(di);
    }
    g_list_free(di_list);
    backup_state.di_list = NULL;

    if (vmaw) {
        Error *err = NULL;
        vma_writer_close(vmaw, &err);
        unlink(backup_file);
    }

    if (pbs) {
        proxmox_backup_disconnect(pbs);
        backup_state.pbs = NULL;
    }

    qemu_co_mutex_unlock(&backup_state.backup_mutex);
    return NULL;
}

BackupStatus *qmp_query_backup(Error **errp)
{
    BackupStatus *info = g_malloc0(sizeof(*info));

    qemu_mutex_lock(&backup_state.stat.lock);

    if (!backup_state.stat.start_time) {
        /* not started, return {} */
        qemu_mutex_unlock(&backup_state.stat.lock);
        return info;
    }

    info->has_start_time = true;
    info->start_time = backup_state.stat.start_time;

    if (backup_state.stat.backup_file) {
        info->backup_file = g_strdup(backup_state.stat.backup_file);
    }

    info->uuid = g_strdup(backup_state.stat.uuid_str);

    if (backup_state.stat.end_time) {
        if (backup_state.stat.error) {
            info->status = g_strdup("error");
            info->errmsg = g_strdup(error_get_pretty(backup_state.stat.error));
        } else {
            info->status = g_strdup("done");
        }
        info->has_end_time = true;
        info->end_time = backup_state.stat.end_time;
    } else {
        info->status = g_strdup("active");
    }

    info->has_total = true;
    info->total = backup_state.stat.total;
    info->has_dirty = true;
    info->dirty = backup_state.stat.dirty;
    info->has_zero_bytes = true;
    info->zero_bytes = backup_state.stat.zero_bytes;
    info->has_transferred = true;
    info->transferred = backup_state.stat.transferred;
    info->has_reused = true;
    info->reused = backup_state.stat.reused;
    info->finishing = backup_state.stat.finishing;

    qemu_mutex_unlock(&backup_state.stat.lock);

    return info;
}

PBSBitmapInfoList *qmp_query_pbs_bitmap_info(Error **errp)
{
    PBSBitmapInfoList *head = NULL, **p_next = &head;

    qemu_mutex_lock(&backup_state.stat.lock);

    GList *l = backup_state.stat.bitmap_list;
    while (l) {
        PBSBitmapInfo *info = (PBSBitmapInfo *)l->data;
        l = g_list_next(l);

        /* clone bitmap info to avoid auto free after QMP marshalling */
        PBSBitmapInfo *info_ret = g_malloc0(sizeof(*info_ret));
        info_ret->drive = g_strdup(info->drive);
        info_ret->action = info->action;
        info_ret->size = info->size;
        info_ret->dirty = info->dirty;

        PBSBitmapInfoList *info_list = g_malloc0(sizeof(*info_list));
        info_list->value = info_ret;

        *p_next = info_list;
        p_next = &info_list->next;
    }

    qemu_mutex_unlock(&backup_state.stat.lock);

    return head;
}

ProxmoxSupportStatus *qmp_query_proxmox_support(Error **errp)
{
    ProxmoxSupportStatus *ret = g_malloc0(sizeof(*ret));
    ret->pbs_library_version = g_strdup(proxmox_backup_qemu_version());
    ret->pbs_dirty_bitmap = true;
    ret->pbs_dirty_bitmap_savevm = true;
    ret->pbs_dirty_bitmap_migration = true;
    ret->query_bitmap_info = true;
    ret->pbs_masterkey = true;
    ret->backup_max_workers = true;
    ret->backup_fleecing = true;
    ret->backup_access_api = true;
    return ret;
}
