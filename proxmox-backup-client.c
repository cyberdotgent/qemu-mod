#include "proxmox-backup-client.h"
#include "qemu/main-loop.h"
#include "qemu/aio-wait.h"
#include "qapi/error.h"

/* Proxmox Backup Server client bindings using coroutines */

// This is called from another thread, so we use aio_co_schedule()
static void proxmox_backup_schedule_wake(void *data) {
    CoCtxData *waker = (CoCtxData *)data;
    aio_co_schedule(waker->ctx, waker->co);
}

int coroutine_fn
proxmox_backup_co_connect(ProxmoxBackupHandle *pbs, Error **errp)
{
    Coroutine *co = qemu_coroutine_self();
    AioContext *ctx = qemu_get_current_aio_context();
    CoCtxData waker = { .co = co, .ctx = ctx };
    char *pbs_err = NULL;
    int pbs_res = -1;

    proxmox_backup_connect_async(pbs, proxmox_backup_schedule_wake, &waker, &pbs_res, &pbs_err);
    qemu_coroutine_yield();
    if (pbs_res < 0) {
        if (errp) error_setg(errp, "backup connect failed: %s", pbs_err ? pbs_err : "unknown error");
        if (pbs_err) proxmox_backup_free_error(pbs_err);
    }
    return pbs_res;
}

int coroutine_fn
proxmox_backup_co_add_config(
    ProxmoxBackupHandle *pbs,
    const char *name,
    const uint8_t *data,
    uint64_t size,
    Error **errp)
{
    Coroutine *co = qemu_coroutine_self();
    AioContext *ctx = qemu_get_current_aio_context();
    CoCtxData waker = { .co = co, .ctx = ctx };
    char *pbs_err = NULL;
    int pbs_res = -1;

    proxmox_backup_add_config_async(
        pbs, name, data, size ,proxmox_backup_schedule_wake, &waker, &pbs_res, &pbs_err);
    qemu_coroutine_yield();
    if (pbs_res < 0) {
        if (errp) error_setg(errp, "backup add_config %s failed: %s", name, pbs_err ? pbs_err : "unknown error");
        if (pbs_err) proxmox_backup_free_error(pbs_err);
    }
    return pbs_res;
}

int coroutine_fn
proxmox_backup_co_register_image(
    ProxmoxBackupHandle *pbs,
    const char *device_name,
    uint64_t size,
    bool incremental,
    Error **errp)
{
    Coroutine *co = qemu_coroutine_self();
    AioContext *ctx = qemu_get_current_aio_context();
    CoCtxData waker = { .co = co, .ctx = ctx };
    char *pbs_err = NULL;
    int pbs_res = -1;

    proxmox_backup_register_image_async(
        pbs, device_name, size, incremental, proxmox_backup_schedule_wake, &waker, &pbs_res, &pbs_err);
    qemu_coroutine_yield();
    if (pbs_res < 0) {
        if (errp) error_setg(errp, "backup register image failed: %s", pbs_err ? pbs_err : "unknown error");
        if (pbs_err) proxmox_backup_free_error(pbs_err);
    }
    return pbs_res;
}

int coroutine_fn
proxmox_backup_co_finish(
    ProxmoxBackupHandle *pbs,
    Error **errp)
{
    Coroutine *co = qemu_coroutine_self();
    AioContext *ctx = qemu_get_current_aio_context();
    CoCtxData waker = { .co = co, .ctx = ctx };
    char *pbs_err = NULL;
    int pbs_res = -1;

    proxmox_backup_finish_async(
        pbs, proxmox_backup_schedule_wake, &waker, &pbs_res, &pbs_err);
    qemu_coroutine_yield();
    if (pbs_res < 0) {
        if (errp) error_setg(errp, "backup finish failed: %s", pbs_err ? pbs_err : "unknown error");
        if (pbs_err) proxmox_backup_free_error(pbs_err);
    }
    return pbs_res;
}

int coroutine_fn
proxmox_backup_co_close_image(
    ProxmoxBackupHandle *pbs,
    uint8_t dev_id,
    Error **errp)
{
    Coroutine *co = qemu_coroutine_self();
    AioContext *ctx = qemu_get_current_aio_context();
    CoCtxData waker = { .co = co, .ctx = ctx };
    char *pbs_err = NULL;
    int pbs_res = -1;

    proxmox_backup_close_image_async(
        pbs, dev_id, proxmox_backup_schedule_wake, &waker, &pbs_res, &pbs_err);
    qemu_coroutine_yield();
    if (pbs_res < 0) {
        if (errp) error_setg(errp, "backup close image failed: %s", pbs_err ? pbs_err : "unknown error");
        if (pbs_err) proxmox_backup_free_error(pbs_err);
    }
    return pbs_res;
}

int coroutine_fn
proxmox_backup_co_write_data(
    ProxmoxBackupHandle *pbs,
    uint8_t dev_id,
    const uint8_t *data,
    uint64_t offset,
    uint64_t size,
    Error **errp)
{
    Coroutine *co = qemu_coroutine_self();
    AioContext *ctx = qemu_get_current_aio_context();
    CoCtxData waker = { .co = co, .ctx = ctx };
    char *pbs_err = NULL;
    int pbs_res = -1;

    proxmox_backup_write_data_async(
        pbs, dev_id, data, offset, size, proxmox_backup_schedule_wake, &waker, &pbs_res, &pbs_err);
    qemu_coroutine_yield();
    if (pbs_res < 0) {
        if (errp) error_setg(errp, "backup write data failed: %s", pbs_err ? pbs_err : "unknown error");
        if (pbs_err) proxmox_backup_free_error(pbs_err);
    }
    return pbs_res;
}
