/*
 * Filter to fake a zero-initialized block device.
 *
 * Copyright (c) 2016 Wolfgang Bumiller <w.bumiller@proxmox.com>
 * Copyright (c) 2016 Proxmox Server Solutions GmbH
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block_int.h"
#include "block/block-io.h"
#include "block/graph-lock.h"
#include "qobject/qdict.h"
#include "qobject/qstring.h"
#include "qemu/cutils.h"
#include "qemu/option.h"
#include "qemu/module.h"

typedef struct {
    bool has_zero_init;
    int64_t extents;
} BDRVZeroinitState;

/* Valid blkverify filenames look like blkverify:path/to/raw_image:path/to/image */
static void zeroinit_parse_filename(const char *filename, QDict *options,
                                     Error **errp)
{
    QString *raw_path;

    /* Parse the blkverify: prefix */
    if (!strstart(filename, "zeroinit:", &filename)) {
        /* There was no prefix; therefore, all options have to be already
           present in the QDict (except for the filename) */
        return;
    }

    raw_path = qstring_from_str(filename);
    qdict_put(options, "x-next", raw_path);
}

static QemuOptsList runtime_opts = {
    .name = "zeroinit",
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = "x-next",
            .type = QEMU_OPT_STRING,
            .help = "[internal use only, will be removed]",
        },
        {
            .name = "x-zeroinit",
            .type = QEMU_OPT_BOOL,
            .help = "set has_initialized_zero flag",
        },
        { /* end of list */ }
    },
};

static int zeroinit_open(BlockDriverState *bs, QDict *options, int flags,
                          Error **errp)
{
    BDRVZeroinitState *s = bs->opaque;
    QemuOpts *opts;
    Error *local_err = NULL;
    int ret;
    const char *next = NULL;

    s->extents = 0;

    opts = qemu_opts_create(&runtime_opts, NULL, 0, &error_abort);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto fail;
    }


    next = qemu_opt_get(opts, "x-next");

    if (next) {
        ret = bdrv_open_file_child(next, options, "next", bs, &local_err);
    } else { /* when opened as a blockdev, there is no 'next' option */
        ret = bdrv_open_file_child(NULL, options, "file", bs, &local_err);
    }
    if (ret < 0) {
        error_propagate(errp, local_err);
        goto fail;
    }

    /* set the options */
    s->has_zero_init = qemu_opt_get_bool(opts, "x-zeroinit", true);

    ret = 0;
fail:
    if (ret < 0) {
        bdrv_graph_wrlock();
        bdrv_unref_child(bs, bs->file);
        bdrv_graph_wrunlock();
    }
    qemu_opts_del(opts);
    return ret;
}

static void zeroinit_close(BlockDriverState *bs)
{
    BDRVZeroinitState *s = bs->opaque;
    (void)s;
}

static coroutine_fn int64_t GRAPH_RDLOCK
zeroinit_co_getlength(BlockDriverState *bs)
{
    return bdrv_co_getlength(bs->file->bs);
}

static int coroutine_fn GRAPH_RDLOCK
zeroinit_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                   QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    return bdrv_co_preadv(bs->file, offset, bytes, qiov, flags);
}

static int coroutine_fn GRAPH_RDLOCK
zeroinit_co_pwrite_zeroes(BlockDriverState *bs, int64_t offset, int64_t bytes,
                          BdrvRequestFlags flags)
{
    BDRVZeroinitState *s = bs->opaque;
    if (offset >= s->extents)
        return 0;
    return bdrv_pwrite_zeroes(bs->file, offset, bytes, flags);
}

static int coroutine_fn GRAPH_RDLOCK
zeroinit_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
                    QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVZeroinitState *s = bs->opaque;
    int64_t extents = offset + bytes;
    if (extents > s->extents)
        s->extents = extents;
    return bdrv_co_pwritev(bs->file, offset, bytes, qiov, flags);
}

static coroutine_fn int GRAPH_RDLOCK
zeroinit_co_flush(BlockDriverState *bs)
{
    return bdrv_co_flush(bs->file->bs);
}

static int GRAPH_RDLOCK
zeroinit_has_zero_init(BlockDriverState *bs)
{
    BDRVZeroinitState *s = bs->opaque;
    return s->has_zero_init;
}

static int coroutine_fn GRAPH_RDLOCK
zeroinit_co_pdiscard(BlockDriverState *bs, int64_t offset, int64_t bytes)
{
    return bdrv_co_pdiscard(bs->file, offset, bytes);
}

static int GRAPH_RDLOCK
zeroinit_co_truncate(BlockDriverState *bs, int64_t offset, _Bool exact,
                     PreallocMode prealloc, BdrvRequestFlags req_flags,
                     Error **errp)
{
    return bdrv_co_truncate(bs->file, offset, exact, prealloc, req_flags, errp);
}

static coroutine_fn int GRAPH_RDLOCK
zeroinit_co_get_info(BlockDriverState *bs, BlockDriverInfo *bdi)
{
    return bdrv_co_get_info(bs->file->bs, bdi);
}

static BlockDriver bdrv_zeroinit = {
    .format_name                      = "zeroinit",
    .protocol_name                    = "zeroinit",
    .instance_size                    = sizeof(BDRVZeroinitState),

    .bdrv_parse_filename              = zeroinit_parse_filename,
    .bdrv_open                        = zeroinit_open,
    .bdrv_close                       = zeroinit_close,
    .bdrv_co_getlength                = zeroinit_co_getlength,
    .bdrv_child_perm                  = bdrv_default_perms,
    .bdrv_co_flush_to_disk            = zeroinit_co_flush,

    .bdrv_co_pwrite_zeroes            = zeroinit_co_pwrite_zeroes,
    .bdrv_co_pwritev                  = zeroinit_co_pwritev,
    .bdrv_co_preadv                   = zeroinit_co_preadv,
    .bdrv_co_flush                    = zeroinit_co_flush,

    .is_filter                        = true,

    .bdrv_has_zero_init               = zeroinit_has_zero_init,

    .bdrv_co_pdiscard                 = zeroinit_co_pdiscard,

    .bdrv_co_truncate                 = zeroinit_co_truncate,
    .bdrv_co_get_info                 = zeroinit_co_get_info,
};

static void bdrv_zeroinit_init(void)
{
    bdrv_register(&bdrv_zeroinit);
}

block_init(bdrv_zeroinit_init);
