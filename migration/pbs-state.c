/*
 * PBS (dirty-bitmap) state migration
 */

#include "qemu/osdep.h"
#include "migration/misc.h"
#include "qemu-file.h"
#include "migration/vmstate.h"
#include "migration/register.h"
#include "proxmox-backup-qemu.h"

typedef struct PBSState {
    bool active;
} PBSState;

/* state is accessed via this static variable directly, 'opaque' is NULL */
static PBSState pbs_state;

static void pbs_state_pending(void *opaque, MigPendingData *data, bool exact,
                              bool final)
{
    /* we send everything in save_setup, so nothing is ever pending */
}

/* receive PBS state via f and deserialize, called on target */
static int pbs_state_load(QEMUFile *f, void *opaque, int version_id)
{
    /* safe cast, we cannot migrate to target with less bits than source */
    size_t buf_size = (size_t)qemu_get_be64(f);
    if (buf_size == 0) {
        return 0;
    }

    g_autofree uint8_t *buf = g_try_malloc(buf_size);
    if (!buf) {
        fprintf(stderr,
                "error receiving PBS state: cannot allocate %zu bytes\n",
                buf_size);
        return -ENOMEM;
    }

    size_t read = qemu_get_buffer(f, buf, buf_size);
    if (read < buf_size) {
        fprintf(stderr, "error receiving PBS state: not enough data\n");
        return -EIO;
    }

    proxmox_import_state(buf, buf_size);
    return 0;
}

/* serialize PBS state and send to target via f, called on source */
static int pbs_state_save_setup(QEMUFile *f, void *opaque, Error **errp)
{
    size_t buf_size;
    uint8_t *buf = proxmox_export_state(&buf_size);

    /* LV encoding */
    qemu_put_be64(f, buf_size);
    qemu_put_buffer(f, buf, buf_size);

    proxmox_free_state_buf(buf);
    pbs_state.active = false;
    return 0;
}

static bool pbs_state_is_active(void *opaque)
{
    /* we need to return active exactly once, else .save_setup is never called,
     * but if we'd just return true the migration doesn't make progress since
     * it'd be waiting for us */
    return pbs_state.active;
}

static bool pbs_state_is_active_iterate(void *opaque)
{
    /* we don't iterate, everything is sent in save_setup */
    return pbs_state_is_active(opaque);
}

static bool pbs_state_has_postcopy(void *opaque)
{
    /* PBS state can't change during a migration (since that's blocking any
     * potential backups), so we can copy everything before the VM is stopped */
    return false;
}

static void pbs_state_save_cleanup(void *opaque)
{
    /* reset active after migration succeeds or fails */
    pbs_state.active = false;
}

static SaveVMHandlers savevm_pbs_state_handlers = {
    .save_setup = pbs_state_save_setup,
    .has_postcopy = pbs_state_has_postcopy,
    .save_query_pending = pbs_state_pending,
    .is_active_iterate = pbs_state_is_active_iterate,
    .load_state = pbs_state_load,
    .is_active = pbs_state_is_active,
    .save_cleanup = pbs_state_save_cleanup,
};

void pbs_state_mig_init(void)
{
    pbs_state.active = true;
    register_savevm_live("pbs-state", 0, 1,
                         &savevm_pbs_state_handlers,
                         NULL);
}
