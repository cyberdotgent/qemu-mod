/*
 * Node to allow backing images to be applied to any node. Assumes a blank
 * image to begin with, only new writes are tracked as allocated, thus this
 * must never be put on a node that already contains data.
 *
 * Copyright (c) 2020 Proxmox Server Solutions GmbH
 * Copyright (c) 2020 Stefan Reiter <s.reiter@proxmox.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block_int.h"
#include "block/dirty-bitmap.h"
#include "block/graph-lock.h"
#include "qobject/qdict.h"
#include "qobject/qstring.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/option.h"
#include "qemu/module.h"
#include "system/block-backend.h"

#define TRACK_OPT_AUTO_REMOVE "auto-remove"

typedef struct {
    BdrvDirtyBitmap *bitmap;
    uint64_t granularity;
} BDRVAllocTrackState;

static QemuOptsList runtime_opts = {
    .name = "alloc-track",
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = TRACK_OPT_AUTO_REMOVE,
            .type = QEMU_OPT_BOOL,
            .help = "automatically replace this node with 'file' when 'backing'"
                    "is detached",
        },
        { /* end of list */ }
    },
};

static void GRAPH_RDLOCK
track_refresh_limits(BlockDriverState *bs, Error **errp)
{
    BDRVAllocTrackState *s = bs->opaque;

    if (!bs->file) {
        return;
    }

    /*
     * Always use alignment from underlying write device so RMW cycle for
     * bdrv_pwritev reads data from our backing via track_co_preadv. Also use at
     * least the bitmap granularity.
     */
    bs->bl.request_alignment = MAX(bs->file->bs->bl.request_alignment,
                                   s->granularity);
}

static int track_open(BlockDriverState *bs, QDict *options, int flags,
                      Error **errp)
{
    BDRVAllocTrackState *s = bs->opaque;
    BdrvChild *file = NULL;
    QemuOpts *opts;
    Error *local_err = NULL;
    int ret = 0;

    opts = qemu_opts_create(&runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto fail;
    }

    if (!qemu_opt_get_bool(opts, TRACK_OPT_AUTO_REMOVE, false)) {
        error_setg(errp, "alloc-track: requires auto-remove option to be set to on");
        ret = -EINVAL;
        goto fail;
    }

    /* open the target (write) node, backing will be attached by block layer */
    file = bdrv_open_child(NULL, options, "file", bs, &child_of_bds,
                           BDRV_CHILD_DATA | BDRV_CHILD_METADATA, false,
                           &local_err);
    bdrv_graph_wrlock();
    bs->file = file;
    bdrv_graph_wrunlock();
    if (local_err) {
        ret = -EINVAL;
        error_propagate(errp, local_err);
        goto fail;
    }

    bdrv_graph_rdlock_main_loop();
    BlockDriverInfo bdi = {0};
    ret = bdrv_get_info(bs->file->bs, &bdi);
    if (ret < 0) {
        /*
         * Not a hard failure. Worst that can happen is partial cluster
         * allocation in the write target. However, the driver here returns its
         * allocation status based on the dirty bitmap, so any other data that
         * maps to such a cluster will still be copied later by a stream job (or
         * during writes to that cluster).
         */
        warn_report("alloc-track: unable to query cluster size for write target: %s",
                    strerror(ret));
    }
    ret = 0;
    /*
     * Always consider alignment from underlying write device so RMW cycle for
     * bdrv_pwritev reads data from our backing via track_co_preadv. Also try to
     * avoid partial cluster allocation in the write target by considering the
     * cluster size.
     */
    s->granularity = MAX(bs->file->bs->bl.request_alignment,
                         MAX(bdi.cluster_size, BDRV_SECTOR_SIZE));
    track_refresh_limits(bs, errp);
    s->bitmap = bdrv_create_dirty_bitmap(bs->file->bs, s->granularity, NULL,
                                         &local_err);
    bdrv_graph_rdunlock_main_loop();
    if (local_err) {
        ret = -EIO;
        error_propagate(errp, local_err);
        goto fail;
    }

fail:
    if (ret < 0) {
        bdrv_graph_wrlock();
        bdrv_unref_child(bs, bs->file);
        bdrv_graph_wrunlock();
        if (s->bitmap) {
            bdrv_release_dirty_bitmap(s->bitmap);
        }
    }
    qemu_opts_del(opts);
    return ret;
}

static void track_close(BlockDriverState *bs)
{
    BDRVAllocTrackState *s = bs->opaque;
    if (s->bitmap) {
        bdrv_release_dirty_bitmap(s->bitmap);
    }
}

static coroutine_fn int64_t GRAPH_RDLOCK
track_co_getlength(BlockDriverState *bs)
{
    return bdrv_co_getlength(bs->file->bs);
}

static int coroutine_fn GRAPH_RDLOCK
track_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVAllocTrackState *s = bs->opaque;
    QEMUIOVector local_qiov;
    int ret;

    /* 'cur_offset' is relative to 'offset', 'local_offset' to image start */
    uint64_t cur_offset, local_offset;
    int64_t local_bytes;
    bool alloc;

    if (offset < 0 || bytes < 0) {
        fprintf(stderr, "unexpected negative 'offset' or 'bytes' value!\n");
        return -EIO;
    }

    /* a read request can span multiple granularity-sized chunks, and can thus
     * contain blocks with different allocation status - we could just iterate
     * granularity-wise, but for better performance use bdrv_dirty_bitmap_next_X
     * to find the next flip and consider everything up to that in one go */
    for (cur_offset = 0; cur_offset < bytes; cur_offset += local_bytes) {
        local_offset = offset + cur_offset;
        alloc = bdrv_dirty_bitmap_get(s->bitmap, local_offset);
        if (alloc) {
            local_bytes = bdrv_dirty_bitmap_next_zero(s->bitmap, local_offset,
                                                      bytes - cur_offset);
        } else {
            local_bytes = bdrv_dirty_bitmap_next_dirty(s->bitmap, local_offset,
                                                       bytes - cur_offset);
        }

        /* _bitmap_next_X return is -1 if no end found within limit, otherwise
         * offset of next flip (to start of image) */
        local_bytes = local_bytes < 0 ?
            bytes - cur_offset :
            local_bytes - local_offset;

        qemu_iovec_init_slice(&local_qiov, qiov, cur_offset, local_bytes);

        if (alloc) {
            ret = bdrv_co_preadv(bs->file, local_offset, local_bytes,
                                 &local_qiov, flags);
        } else if (bs->backing) {
            ret = bdrv_co_preadv(bs->backing, local_offset, local_bytes,
                                 &local_qiov, flags);
        } else {
            qemu_iovec_memset(&local_qiov, cur_offset, 0, local_bytes);
            ret = 0;
        }

        if (ret != 0) {
            break;
        }
    }

    return ret;
}

static int coroutine_fn GRAPH_RDLOCK
track_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
                 QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    return bdrv_co_pwritev(bs->file, offset, bytes, qiov, flags);
}

static int coroutine_fn GRAPH_RDLOCK
track_co_pwrite_zeroes(BlockDriverState *bs, int64_t offset, int64_t bytes,
                       BdrvRequestFlags flags)
{
    return bdrv_co_pwrite_zeroes(bs->file, offset, bytes, flags);
}

static int coroutine_fn GRAPH_RDLOCK
track_co_pdiscard(BlockDriverState *bs, int64_t offset, int64_t bytes)
{
    return bdrv_co_pdiscard(bs->file, offset, bytes);
}

static coroutine_fn int GRAPH_RDLOCK
track_co_flush(BlockDriverState *bs)
{
    return bdrv_co_flush(bs->file->bs);
}

static int coroutine_fn GRAPH_RDLOCK
track_co_block_status(BlockDriverState *bs, unsigned int mode,
                                            int64_t offset,
                                            int64_t bytes,
                                            int64_t *pnum,
                                            int64_t *map,
                                            BlockDriverState **file)
{
    BDRVAllocTrackState *s = bs->opaque;

    bool alloc = bdrv_dirty_bitmap_get(s->bitmap, offset);
    int64_t next_flipped;
    if (alloc) {
        next_flipped = bdrv_dirty_bitmap_next_zero(s->bitmap, offset, bytes);
    } else {
        next_flipped = bdrv_dirty_bitmap_next_dirty(s->bitmap, offset, bytes);
    }

    /* in case not the entire region has the same state, we need to set pnum to
     * indicate for how many bytes our result is valid */
    *pnum = next_flipped == -1 ? bytes : next_flipped - offset;
    *map = offset;

    if (alloc) {
        *file = bs->file->bs;
        return BDRV_BLOCK_RAW | BDRV_BLOCK_OFFSET_VALID;
    } else if (bs->backing) {
        *file = bs->backing->bs;
    }
    return 0;
}

static void GRAPH_RDLOCK
track_child_perm(BlockDriverState *bs, BdrvChild *c, BdrvChildRole role,
                 BlockReopenQueue *reopen_queue, uint64_t perm, uint64_t shared,
                 uint64_t *nperm, uint64_t *nshared)
{
    *nshared = BLK_PERM_ALL;

    if (role & BDRV_CHILD_DATA) {
        *nperm = perm & DEFAULT_PERM_PASSTHROUGH;
    } else {
        /* 'backing' is also a child of our BDS, but we don't expect it to be
         * writeable, so we only forward 'consistent read' */
        *nperm = perm & BLK_PERM_CONSISTENT_READ;
    }
}

static int coroutine_fn GRAPH_RDLOCK
track_co_change_backing_file(BlockDriverState *bs, const char *backing_file,
                             const char *backing_fmt)
{
    /*
     * Note that the actual backing file graph change is already done in the
     * stream job itself with bdrv_set_backing_hd_drained(), so no need to
     * actually do anything here. But still needs to be implemented, to make
     * our caller (i.e. bdrv_co_change_backing_file() do the right thing).
     *
     * FIXME
     * We'd like to auto-remove ourselves from the block graph, but it cannot
     * be done from a coroutine. Currently done in the stream job, where it
     * kinda fits better, but in the long-term, a special parameter would be
     * nice (or done via qemu-server via upcoming blockdev-replace QMP command).
     */
    return 0;
}

static BlockDriver bdrv_alloc_track = {
    .format_name                      = "alloc-track",
    .instance_size                    = sizeof(BDRVAllocTrackState),

    .bdrv_open                        = track_open,
    .bdrv_close                       = track_close,
    .bdrv_co_getlength                = track_co_getlength,
    .bdrv_child_perm                  = track_child_perm,
    .bdrv_refresh_limits              = track_refresh_limits,

    .bdrv_co_pwrite_zeroes            = track_co_pwrite_zeroes,
    .bdrv_co_pwritev                  = track_co_pwritev,
    .bdrv_co_preadv                   = track_co_preadv,
    .bdrv_co_pdiscard                 = track_co_pdiscard,

    .bdrv_co_flush                    = track_co_flush,
    .bdrv_co_flush_to_disk            = track_co_flush,

    .supports_backing                 = true,

    .bdrv_co_block_status             = track_co_block_status,
    .bdrv_co_change_backing_file      = track_co_change_backing_file,
};

static void bdrv_alloc_track_init(void)
{
    bdrv_register(&bdrv_alloc_track);
}

block_init(bdrv_alloc_track_init);
