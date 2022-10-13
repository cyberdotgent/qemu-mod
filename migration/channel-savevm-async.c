/*
 * QIO Channel implementation to be used by savevm-async QMP calls
 */
#include "qemu/osdep.h"
#include "migration/channel-savevm-async.h"
#include "qapi/error.h"
#include "system/block-backend.h"
#include "trace.h"

QIOChannelSavevmAsync *
qio_channel_savevm_async_new(BlockBackend *be, size_t *bs_pos)
{
    QIOChannelSavevmAsync *ioc;

    ioc = QIO_CHANNEL_SAVEVM_ASYNC(object_new(TYPE_QIO_CHANNEL_SAVEVM_ASYNC));

    bdrv_ref(blk_bs(be));
    ioc->be = be;
    ioc->bs_pos = bs_pos;

    return ioc;
}


static void
qio_channel_savevm_async_finalize(Object *obj)
{
    QIOChannelSavevmAsync *ioc = QIO_CHANNEL_SAVEVM_ASYNC(obj);

    if (ioc->be) {
        bdrv_unref(blk_bs(ioc->be));
        ioc->be = NULL;
    }
    ioc->bs_pos = NULL;
}


static ssize_t
qio_channel_savevm_async_readv(QIOChannel *ioc,
                               const struct iovec *iov,
                               size_t niov,
                               int **fds,
                               size_t *nfds,
                               int flags,
                               Error **errp)
{
    QIOChannelSavevmAsync *saioc = QIO_CHANNEL_SAVEVM_ASYNC(ioc);
    BlockBackend *be = saioc->be;
    int64_t maxlen = blk_getlength(be);
    QEMUIOVector qiov;
    size_t size;
    int ret;

    qemu_iovec_init_external(&qiov, (struct iovec *)iov, niov);

    if (*saioc->bs_pos >= maxlen) {
        error_setg(errp, "cannot read beyond maxlen");
        return -1;
    }

    if (maxlen - *saioc->bs_pos < qiov.size) {
        size = maxlen - *saioc->bs_pos;
    } else {
        size = qiov.size;
    }

    // returns 0 on success
    ret = blk_preadv(be, *saioc->bs_pos, size, &qiov, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "blk_preadv failed");
        return -1;
    }

    *saioc->bs_pos += size;
    return size;
}


static ssize_t
qio_channel_savevm_async_writev(QIOChannel *ioc,
                                const struct iovec *iov,
                                size_t niov,
                                int *fds,
                                size_t nfds,
                                int flags,
                                Error **errp)
{
    QIOChannelSavevmAsync *saioc = QIO_CHANNEL_SAVEVM_ASYNC(ioc);
    BlockBackend *be = saioc->be;
    QEMUIOVector qiov;
    int ret;

    qemu_iovec_init_external(&qiov, (struct iovec *)iov, niov);

    if (qemu_in_coroutine()) {
        ret = blk_co_pwritev(be, *saioc->bs_pos, qiov.size, &qiov, 0);
        aio_wait_kick();
    } else {
        ret = blk_pwritev(be, *saioc->bs_pos, qiov.size, &qiov, 0);
    }

    if (ret < 0) {
        error_setg_errno(errp, -ret, "blk(_co)_pwritev failed");
        return -1;
    }

    *saioc->bs_pos += qiov.size;
    return qiov.size;
}


static int
qio_channel_savevm_async_set_blocking(QIOChannel *ioc,
                                      bool enabled,
                                      Error **errp)
{
    if (!enabled) {
        error_setg(errp, "Non-blocking mode not supported for savevm-async");
        return -1;
    }
    return 0;
}


static int
qio_channel_savevm_async_close(QIOChannel *ioc,
                               Error **errp)
{
    QIOChannelSavevmAsync *saioc = QIO_CHANNEL_SAVEVM_ASYNC(ioc);
    int rv = bdrv_flush(blk_bs(saioc->be));

    if (rv < 0) {
        error_setg_errno(errp, -rv, "Unable to flush VMState");
        return -1;
    }

    bdrv_unref(blk_bs(saioc->be));
    saioc->be = NULL;
    saioc->bs_pos = NULL;

    return 0;
}


static void
qio_channel_savevm_async_set_aio_fd_handler(QIOChannel *ioc,
                                            AioContext *read_ctx,
                                            IOHandler *io_read,
                                            AioContext *write_ctx,
                                            IOHandler *io_write,
                                            void *opaque)
{
    // if channel-block starts doing something, check if this needs adaptation
}


static void
qio_channel_savevm_async_class_init(ObjectClass *klass,
                                    const void *class_data G_GNUC_UNUSED)
{
    QIOChannelClass *ioc_klass = QIO_CHANNEL_CLASS(klass);

    ioc_klass->io_writev = qio_channel_savevm_async_writev;
    ioc_klass->io_readv = qio_channel_savevm_async_readv;
    ioc_klass->io_set_blocking = qio_channel_savevm_async_set_blocking;
    ioc_klass->io_close = qio_channel_savevm_async_close;
    ioc_klass->io_set_aio_fd_handler = qio_channel_savevm_async_set_aio_fd_handler;
}

static const TypeInfo qio_channel_savevm_async_info = {
    .parent = TYPE_QIO_CHANNEL,
    .name = TYPE_QIO_CHANNEL_SAVEVM_ASYNC,
    .instance_size = sizeof(QIOChannelSavevmAsync),
    .instance_finalize = qio_channel_savevm_async_finalize,
    .class_init = qio_channel_savevm_async_class_init,
};

static void
qio_channel_savevm_async_register_types(void)
{
    type_register_static(&qio_channel_savevm_async_info);
}

type_init(qio_channel_savevm_async_register_types);
