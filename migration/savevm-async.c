#include "qemu/osdep.h"
#include "migration/channel-savevm-async.h"
#include "migration/migration.h"
#include "migration/migration-stats.h"
#include "migration/options.h"
#include "migration/savevm.h"
#include "migration/snapshot.h"
#include "migration/global_state.h"
#include "migration/ram.h"
#include "migration/qemu-file.h"
#include "system/cpu-throttle.h"
#include "system/system.h"
#include "system/runstate.h"
#include "block/block.h"
#include "system/block-backend.h"
#include "qapi/error.h"
#include "qapi/qmp/qerror.h"
#include "qobject/qdict.h"
#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "qapi/qapi-commands-block.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/yank.h"
#include "system/iothread.h"

/* #define DEBUG_SAVEVM_STATE */

#ifdef DEBUG_SAVEVM_STATE
#define DPRINTF(fmt, ...) \
    do { printf("savevm-async: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

enum {
    SAVE_STATE_DONE,
    SAVE_STATE_ERROR,
    SAVE_STATE_ACTIVE,
    SAVE_STATE_COMPLETED,
    SAVE_STATE_CANCELLED
};


static struct SnapshotState {
    BlockBackend *target;
    size_t bs_pos;
    int state;
    Error *error;
    Error *blocker;
    bool vm_needs_start;
    bool skip_vm_start;
    QEMUFile *file;
    int64_t total_time;
    QEMUBH *finalize_bh;
    Coroutine *co;
    QemuCoSleep target_close_wait;
    IOThread *iothread;
} snap_state;

static bool savevm_aborted(void)
{
    return snap_state.state == SAVE_STATE_CANCELLED ||
        snap_state.state == SAVE_STATE_ERROR;
}

static bool should_skip_vm_start(void) {
    return snap_state.state == SAVE_STATE_COMPLETED && snap_state.skip_vm_start;
}

SaveVMInfo *qmp_query_savevm(Error **errp)
{
    SaveVMInfo *info = g_malloc0(sizeof(*info));
    struct SnapshotState *s = &snap_state;

    if (s->state != SAVE_STATE_DONE) {
        info->has_bytes = true;
        info->bytes = s->bs_pos;
        switch (s->state) {
        case SAVE_STATE_ERROR:
            info->status = g_strdup("failed");
            info->has_total_time = true;
            info->total_time = s->total_time;
            if (s->error) {
                info->error = g_strdup(error_get_pretty(s->error));
            }
            break;
        case SAVE_STATE_ACTIVE:
            info->status = g_strdup("active");
            info->has_total_time = true;
            info->total_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                - s->total_time;
            break;
        case SAVE_STATE_COMPLETED:
            info->status = g_strdup("completed");
            info->has_total_time = true;
            info->total_time = s->total_time;
            break;
        }
    }

    return info;
}

static int save_snapshot_cleanup(void)
{
    int ret = 0;

    DPRINTF("save_snapshot_cleanup\n");

    snap_state.total_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) -
        snap_state.total_time;

    if (snap_state.file) {
        ret = qemu_fclose(snap_state.file);
        snap_state.file = NULL;
    }

    if (snap_state.target) {
        BlockDriverState *target_bs = blk_bs(snap_state.target);
        if (!savevm_aborted()) {
            /* try to truncate, but ignore errors (will fail on block devices).
            * note1: bdrv_read() need whole blocks, so we need to round up
            * note2: PVE requires 1024 (BDRV_SECTOR_SIZE*2) alignment
            */
            size_t size = QEMU_ALIGN_UP(snap_state.bs_pos, BDRV_SECTOR_SIZE*2);
            blk_truncate(snap_state.target, size, false, PREALLOC_MODE_OFF, 0, NULL);
        }
        if (target_bs) {
            bdrv_op_unblock_all(target_bs, snap_state.blocker);
        }
        error_free(snap_state.blocker);
        snap_state.blocker = NULL;
        blk_unref(snap_state.target);
        snap_state.target = NULL;

        qemu_co_sleep_wake(&snap_state.target_close_wait);
    }

    return ret;
}

static void G_GNUC_PRINTF(1, 2) save_snapshot_error(const char *fmt, ...)
{
    va_list ap;
    char *msg;

    va_start(ap, fmt);
    msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);

    DPRINTF("save_snapshot_error: %s\n", msg);

    if (!snap_state.error) {
        error_setg(&snap_state.error, "%s", msg);
    }

    g_free (msg);

    snap_state.state = SAVE_STATE_ERROR;
}

static void process_savevm_finalize(void *opaque)
{
    int ret;
    MigrationState *ms = migrate_get_current();
    MigPendingData pending = {};
    Error *local_err = NULL;

    bool aborted = savevm_aborted();

#ifdef DEBUG_SAVEVM_STATE
    int64_t start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
#endif

    qemu_bh_delete(snap_state.finalize_bh);
    snap_state.finalize_bh = NULL;
    snap_state.co = NULL;

    /* We need to own the target bdrv's context for the following functions,
     * so move it back. It can stay in the main context and live out its live
     * there, since we're done with it after this method ends anyway.
     */
    blk_set_aio_context(snap_state.target, qemu_get_aio_context(), NULL);

    snap_state.vm_needs_start = runstate_is_running();
    ret = vm_stop_force_state(RUN_STATE_FINISH_MIGRATE);
    if (ret < 0) {
        save_snapshot_error("vm_stop_force_state error %d", ret);
    }

    /*
     * Need a final query to collect the latest state before calling
     * qemu_savevm_state_complete_precopy(). The migration code does this in
     * migration_switchover_start().
     */
    if (!qemu_savevm_query_pending_final(ms, &pending, &local_err)) {
        ret = -1;
        save_snapshot_error("qemu_savevm_query_pending_final error: %s",
                            error_get_pretty(local_err));
        error_free(local_err);
        local_err = NULL;
    } else if (!aborted) {
        /* skip state saving if we aborted, snapshot will be invalid anyway */
        if (!qemu_savevm_state_complete_precopy(ms, &local_err)) {
            ret = -1;
            save_snapshot_error("qemu_savevm_state_complete_precopy error: %s",
                                error_get_pretty(local_err));
            error_free(local_err);
            local_err = NULL;
        }
    }

    DPRINTF("state saving complete\n");
    DPRINTF("timing: process_savevm_finalize (state saving) took %ld ms\n",
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_time);

    /* clear migration state */
    migrate_set_state(&ms->state, MIGRATION_STATUS_SETUP,
        ret || aborted ? MIGRATION_STATUS_FAILED : MIGRATION_STATUS_COMPLETED);
    ms->to_dst_file = NULL;

    /*
     * Same as in migration_iteration_finish(): saving RAM might've turned on CPU throttling for
     * auto-converge, make sure to disable it.
     */
    cpu_throttle_stop();

    qemu_savevm_state_cleanup();

    ret = save_snapshot_cleanup();
    if (ret < 0) {
        save_snapshot_error("save_snapshot_cleanup error %d", ret);
    } else if (snap_state.state == SAVE_STATE_ACTIVE) {
        snap_state.state = SAVE_STATE_COMPLETED;
    } else if (aborted) {
        /*
         * If there was an error, there's no need to set a new one here.
         * If the snapshot was canceled, leave setting the state to
         * qmp_savevm_end(), which is waked by save_snapshot_cleanup().
         */
    } else {
        save_snapshot_error("process_savevm_cleanup: invalid state: %d",
                            snap_state.state);
    }
    if (snap_state.vm_needs_start) {
        if (!should_skip_vm_start()) {
            vm_start();
        }
        snap_state.vm_needs_start = false;
    }

    DPRINTF("timing: process_savevm_finalize (full) took %ld ms\n",
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_time);
}

static void coroutine_fn process_savevm_co(void *opaque)
{
    int ret;
    int64_t maxlen;
    BdrvNextIterator it;
    BlockDriverState *bs = NULL;
    MigrationState *ms = migrate_get_current();

#ifdef DEBUG_SAVEVM_STATE
    int64_t start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
#endif

    ret = qemu_file_get_error(snap_state.file);
    if (ret < 0) {
        save_snapshot_error("qemu_savevm_state_setup failed");
        return;
    }

    while (snap_state.state == SAVE_STATE_ACTIVE) {
        MigPendingData pending = { };
        uint64_t threshold = 400 * 1000;

        /*
         * Similar to what is done in migration.c, call the exact variant only
         * once pending.precopy_bytes in the estimate is below the threshold.
         */
        qemu_savevm_query_pending_iter(ms, &pending, false);
        if (pending.precopy_bytes <= threshold) {
            qemu_savevm_query_pending_iter(ms, &pending, true);
        }

        /*
         * A guest reaching this cutoff is dirtying lots of RAM. It should be
         * large enough so that the guest can't dirty this much between the
         * check and the guest actually being stopped, but it should be small
         * enough to avoid long downtimes for non-hibernation snapshots.
         */
        maxlen = blk_getlength(snap_state.target) - 100*1024*1024;

        /* Note that there is no progress for {post,stop}copy_bytes when iterating */
        if (pending.precopy_bytes > threshold && snap_state.bs_pos + pending.total_bytes < maxlen) {
            ret = qemu_savevm_state_iterate(snap_state.file, false);
            if (ret < 0) {
                save_snapshot_error("qemu_savevm_state_iterate error %d", ret);
                break;
            }
            DPRINTF("savevm iterate pending size %lu ret %d\n", pending.total_bytes, ret);
        } else {
            qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER, NULL);
            global_state_store();

            DPRINTF("savevm iterate complete\n");
            break;
        }
    }

    DPRINTF("timing: process_savevm_co took %ld ms\n",
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_time);

#ifdef DEBUG_SAVEVM_STATE
    int64_t start_time_flush = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
#endif
    /* If a drive runs in an IOThread we can flush it async, and only
     * need to sync-flush whatever IO happens between now and
     * vm_stop_force_state. bdrv_next can only be called from main AioContext,
     * so move there now and after every flush.
     */
    aio_co_reschedule_self(qemu_get_aio_context());
    bdrv_graph_co_rdlock();
    bs = bdrv_first(&it);
    bdrv_graph_co_rdunlock();
    while (bs) {
        /* target has BDRV_O_NO_FLUSH, no sense calling bdrv_flush on it */
        if (bs != blk_bs(snap_state.target)) {
            AioContext *bs_ctx = bdrv_get_aio_context(bs);
            if (bs_ctx != qemu_get_aio_context()) {
                DPRINTF("savevm: async flushing drive %s\n", bs->filename);
                aio_co_reschedule_self(bs_ctx);
                bdrv_graph_co_rdlock();
                bdrv_flush(bs);
                bdrv_graph_co_rdunlock();
                aio_co_reschedule_self(qemu_get_aio_context());
            }
        }
        bdrv_graph_co_rdlock();
        bs = bdrv_next(&it);
        bdrv_graph_co_rdunlock();
    }

    DPRINTF("timing: async flushing took %ld ms\n",
        qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - start_time_flush);

    qemu_bh_schedule(snap_state.finalize_bh);
}

static void savevm_cleanup_iothread(void) {
    if (snap_state.iothread) {
        iothread_destroy(snap_state.iothread);
        snap_state.iothread = NULL;
    }
}

void qmp_savevm_start(const char *statefile, bool has_skip_vm_start,
                      bool skip_vm_start, Error **errp)
{
    Error *local_err = NULL;
    MigrationState *ms = migrate_get_current();
    BlockDriverState *target_bs = NULL;
    int ret = 0;

    int bdrv_oflags = BDRV_O_RDWR | BDRV_O_RESIZE | BDRV_O_NO_FLUSH;

    if (snap_state.state != SAVE_STATE_DONE) {
        error_setg(errp, "VM snapshot already started\n");
        return;
    }

    if (migration_is_running()) {
        error_setg(errp, "There's a migration process in progress");
        return;
    }

    /* initialize snapshot info */
    snap_state.bs_pos = 0;
    snap_state.total_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    snap_state.blocker = NULL;
    snap_state.target_close_wait = (QemuCoSleep){ .to_wake = NULL };
    snap_state.skip_vm_start = has_skip_vm_start && skip_vm_start;

    if (snap_state.error) {
        error_free(snap_state.error);
        snap_state.error = NULL;
    }

    if (!statefile) {
        snap_state.vm_needs_start = runstate_is_running();
        vm_stop(RUN_STATE_SAVE_VM);
        snap_state.state = SAVE_STATE_COMPLETED;
        return;
    }

    if (qemu_savevm_state_blocked(errp)) {
        goto fail;
    }

    if (snap_state.iothread) {
        /* This is not expected, so warn about it, but no point in re-creating a new iothread. */
        warn_report("iothread for snapshot already exists - re-using");
    } else {
        snap_state.iothread =
            iothread_create("proxmox_savevm_async_iothread__", &local_err);
        if (!snap_state.iothread) {
            error_setg(errp, "creating iothread failed: %s",
                       local_err ? error_get_pretty(local_err) : "unknown error");
            goto fail;
        }
    }

    /* Open the image */
    QDict *options = NULL;
    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    snap_state.target = blk_new_open(statefile, NULL, options, bdrv_oflags, &local_err);
    if (!snap_state.target) {
        error_setg(errp, "failed to open '%s'", statefile);
        goto fail;
    }
    target_bs = blk_bs(snap_state.target);
    if (!target_bs) {
        error_setg(errp, "failed to open '%s' - no block driver state", statefile);
        goto fail;
    }

    QIOChannel *ioc = QIO_CHANNEL(qio_channel_savevm_async_new(snap_state.target,
                                                               &snap_state.bs_pos));
    snap_state.file = qemu_file_new_output(ioc);
    object_unref(ioc);

    if (!snap_state.file) {
        error_setg(errp, "failed to open '%s'", statefile);
        goto fail;
    }

    /*
     * qemu_savevm_* paths use migration code and expect a migration state.
     * State is cleared in process_savevm_co, but has to be initialized
     * here (blocking main thread, from QMP) to avoid race conditions.
     */
    if (migrate_init(ms, errp) != 0) {
        goto fail;
    }
    memset(&mig_stats, 0, sizeof(mig_stats));
    ms->to_dst_file = snap_state.file;

    error_setg(&snap_state.blocker, "block device is in use by savevm");
    bdrv_op_block_all(target_bs, snap_state.blocker);

    snap_state.state = SAVE_STATE_ACTIVE;
    snap_state.finalize_bh = qemu_bh_new(process_savevm_finalize, &snap_state);
    qemu_savevm_state_header(snap_state.file);
    ret = qemu_savevm_state_do_setup(snap_state.file, &local_err);
    if (ret != 0) {
        error_setg_errno(errp, -ret, "savevm state setup failed: %s",
                         local_err ? error_get_pretty(local_err) : "unknown error");
        goto fail;
    }

    ret = blk_set_aio_context(snap_state.target, snap_state.iothread->ctx, &local_err);
    if (ret != 0) {
        error_setg_errno(errp, -ret, "failed to set iothread context for VM state target: %s",
                         local_err ? error_get_pretty(local_err) : "unknown error");
        goto fail;
    }

    snap_state.co = qemu_coroutine_create(&process_savevm_co, NULL);
    aio_co_schedule(snap_state.iothread->ctx, snap_state.co);

    return;

fail:
    savevm_cleanup_iothread();
    save_snapshot_error("setup failed");
}

static void coroutine_fn wait_for_close_co(void *opaque)
{
    int64_t timeout;

    if (snap_state.target) {
        /* wait until cleanup is done before returning, this ensures that after this
         * call exits the statefile will be closed and can be removed immediately */
        DPRINTF("savevm-end: waiting for cleanup\n");
        timeout = 30L * 1000 * 1000 * 1000;
        qemu_co_sleep_ns_wakeable(&snap_state.target_close_wait,
                                  QEMU_CLOCK_REALTIME, timeout);
        if (snap_state.target) {
            save_snapshot_error("timeout waiting for target file close in "
                                "qmp_savevm_end");
            /* we cannot assume the snapshot finished in this case, so leave the
             * state alone - caller has to figure something out */
            return;
        }
    } else {
        DPRINTF("savevm-end: no target file open\n");
    }

    savevm_cleanup_iothread();

    // File closed and no other error, so ensure next snapshot can be started.
    if (snap_state.state != SAVE_STATE_ERROR) {
        snap_state.state = SAVE_STATE_DONE;
    }

    DPRINTF("savevm-end: cleanup done\n");
}

void qmp_savevm_end(Error **errp)
{
    if (snap_state.state == SAVE_STATE_DONE) {
        error_setg(errp, "VM snapshot not started\n");
        return;
    }

    Coroutine *wait_for_close = qemu_coroutine_create(wait_for_close_co, NULL);

    if (snap_state.state == SAVE_STATE_ACTIVE) {
        snap_state.state = SAVE_STATE_CANCELLED;
        qemu_coroutine_enter(wait_for_close);
        return;
    }

    if (snap_state.vm_needs_start) {
        if (!should_skip_vm_start()) {
            vm_start();
        }
        snap_state.vm_needs_start = false;
    }

    qemu_coroutine_enter(wait_for_close);
}

int load_snapshot_from_blockdev(const char *filename, Error **errp)
{
    BlockBackend *be;
    BlockDriverState *bs = NULL;
    Error *local_err = NULL;
    Error *blocker = NULL;
    QDict *options;

    QEMUFile *f;
    size_t bs_pos = 0;
    int ret = -EINVAL;

    options = qdict_new();
    qdict_put_str(options, "driver", "raw");

    be = blk_new_open(filename, NULL, options, 0, &local_err);

    if (!be) {
        error_setg(errp, "Could not open VM state file");
        goto the_end;
    }

    bs = blk_bs(be);
    if (!bs) {
        error_setg(errp, "Could not open VM state file - missing block driver state");
        goto the_end;
    }

    error_setg(&blocker, "block device is in use by load state");
    bdrv_op_block_all(bs, blocker);

    /* restore the VM state */
    QIOChannel *ioc = QIO_CHANNEL(qio_channel_savevm_async_new(be, &bs_pos));
    f = qemu_file_new_input(ioc);
    object_unref(ioc);
    if (!f) {
        error_setg(errp, "Could not open VM state file");
        goto the_end;
    }

    qemu_system_reset(SHUTDOWN_CAUSE_NONE);
    ret = qemu_loadvm_state(f, &local_err);

    /* dirty bitmap migration has a special case we need to trigger manually */
    dirty_bitmap_mig_before_vm_start();

    qemu_fclose(f);

    /* state_destroy assumes a real migration which would have added a yank */
    yank_register_instance(MIGRATION_YANK_INSTANCE, &error_abort);

    migration_incoming_state_destroy();
    if (ret < 0) {
        if (local_err) {
            error_setg_errno(errp, -ret, "Error while loading VM state - %s",
                             error_get_pretty(local_err));
        } else {
            error_setg_errno(errp, -ret, "Error while loading VM state");
        }
        goto the_end;
    }

    ret = 0;

 the_end:
    if (be) {
        if (bs) {
            bdrv_op_unblock_all(bs, blocker);
        }
        error_free(blocker);
        blk_unref(be);
    }
    return ret;
}
