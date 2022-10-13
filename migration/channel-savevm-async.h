/*
 * QEMU I/O channels driver for savevm-async.c
 *
 * Copyright (c) 2022 Proxmox Server Solutions
 *
 * Authors:
 *  Fiona Ebner (f.ebner@proxmox.com)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QIO_CHANNEL_SAVEVM_ASYNC_H
#define QIO_CHANNEL_SAVEVM_ASYNC_H

#include "io/channel.h"
#include "qom/object.h"

#define TYPE_QIO_CHANNEL_SAVEVM_ASYNC "qio-channel-savevm-async"
OBJECT_DECLARE_SIMPLE_TYPE(QIOChannelSavevmAsync, QIO_CHANNEL_SAVEVM_ASYNC)


/**
 * QIOChannelSavevmAsync:
 *
 * The QIOChannelBlock object provides a channel implementation that is able to
 * perform I/O on any BlockBackend whose BlockDriverState directly contains a
 * VMState (as opposed to indirectly, like qcow2). It allows tracking the
 * current position from the outside.
 */
struct QIOChannelSavevmAsync {
    QIOChannel parent;
    BlockBackend *be;
    size_t *bs_pos;
};


/**
 * qio_channel_savevm_async_new:
 * @be: the block backend
 * @bs_pos: used to keep track of the IOChannels current position
 *
 * Create a new IO channel object that can perform I/O on a BlockBackend object
 * whose BlockDriverState directly contains a VMState.
 *
 * Returns: the new channel object
 */
QIOChannelSavevmAsync *
qio_channel_savevm_async_new(BlockBackend *be, size_t *bs_pos);

#endif /* QIO_CHANNEL_SAVEVM_ASYNC_H */
