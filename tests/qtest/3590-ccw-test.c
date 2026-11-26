/*
 * QTest testcase for emulated 3590 CCW tape drives
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "s390x-ccw-prog.h"

static char *create_aws_image(void)
{
    static const uint8_t image[] = {
        0x18, 0x00, 0x00, 0x00, 0xa0, 0x00,
        0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00,
        0x02, 0x00, 0x30, 0x00, 0x20, 0x00, 0x00, 0x18,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-3590-XXXXXX.aws", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    return path;
}

static char *create_malformed_aws_image(void)
{
    static const uint8_t image[] = {
        0x01, 0x00, 0x00, 0x00, 0x80, 0x00, 0xaa,
    };
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-3590-bad-XXXXXX.aws", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    return path;
}

static char *create_other_aws_image(void)
{
    /* One four-byte record and a tape mark */
    static const uint8_t image[] = {
        0x04, 0x00, 0x00, 0x00, 0xa0, 0x00, 'Q', 'E', 'M', 'U',
        0x00, 0x00, 0x04, 0x00, 0x40, 0x00,
    };
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-3590-other-XXXXXX.aws", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    close(fd);
    return path;
}

static char *wait_migration(QTestState *qts)
{
    for (;;) {
        QDict *rsp = qtest_qmp(qts, "{'execute': 'query-migrate'}");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = ret && qdict_haskey(ret, "status") ?
                             qdict_get_str(ret, "status") : "none";

        if (!strcmp(status, "completed") || !strcmp(status, "failed")) {
            char *result = g_strdup(status);

            qobject_unref(rsp);
            return result;
        }
        qobject_unref(rsp);
        g_usleep(10 * 1000);
    }
}

/* Migrate @from into a file and return the final source status. */
static char *migrate_to_file(QTestState *from, const char *path)
{
    g_autofree char *uri = g_strdup_printf("file:%s", path);

    qtest_qmp_assert_success(from,
        "{'execute': 'migrate', 'arguments': {'uri': %s}}", uri);
    return wait_migration(from);
}

/* Load the migration file into @to and return the final status. */
static char *migrate_from_file(QTestState *to, const char *path)
{
    g_autofree char *uri = g_strdup_printf("file:%s", path);

    qtest_qmp_assert_success(to,
        "{'execute': 'migrate-incoming', 'arguments': "
        " {'uri': %s, 'exit-on-error': false}}", uri);
    return wait_migration(to);
}


/* Create a qcow2 image that can hold internal snapshots */
static char *create_snapshot_image(void)
{
    const char *qemu_img = g_getenv("QTEST_QEMU_IMG");
    g_autofree char *cmd = NULL;
    char *path = NULL;
    int fd;

    if (!qemu_img) {
        return NULL;
    }
    fd = g_file_open_tmp("qemu-snap-XXXXXX.qcow2", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    cmd = g_strdup_printf("%s create -q -f qcow2 %s 16M", qemu_img, path);
    g_assert_cmpint(system(cmd), ==, 0);
    return path;
}

/* savevm + loadvm in the same process; loadvm runs post_load drained */
static void snapshot_roundtrip(QTestState *qts)
{
    g_autofree char *save = qtest_hmp(qts, "savevm s1");
    g_autofree char *load = NULL;

    g_assert_cmpstr(save, ==, "");
    load = qtest_hmp(qts, "loadvm s1");
    g_assert_cmpstr(load, ==, "");
}

static char *qom_get_string(QTestState *qts, const char *path,
                            const char *property)
{
    QDict *response = qtest_qmp(
        qts,
        "{'execute': 'qom-get', 'arguments': "
        " {'path': %s, 'property': %s}}",
        path, property);
    char *value;

    g_assert(qdict_haskey(response, "return"));
    value = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);
    return value;
}

static bool tape_tray_open(QTestState *qts)
{
    QDict *response = qtest_qmp(qts, "{'execute': 'query-block'}");
    QList *blocks = qdict_get_qlist(response, "return");
    const QListEntry *entry = qlist_first(blocks);
    QDict *block;
    bool open;

    g_assert_nonnull(entry);
    block = qobject_to(QDict, qlist_entry_obj(entry));
    g_assert_cmpstr(qdict_get_str(block, "qdev"), ==, "tape0");
    open = qdict_get_bool(block, "tray_open");
    qobject_unref(response);
    return open;
}

static void test_dev3590_shortcut(void)
{
    g_autofree char *path = create_aws_image();
    g_autofree char *dev_id = NULL;
    QTestState *qts;

    qts = qtest_initf("-nodefaults -dev3590 file=%s,devno=580,id=tape0",
                      path);
    dev_id = qom_get_string(qts, "/machine/peripheral/tape0", "dev_id");
    g_assert_cmpstr(dev_id, ==, "fe.0.0580");
    g_assert_false(tape_tray_open(qts));
    qtest_quit(qts);
    unlink(path);
}

static void test_auto_devnos(void)
{
    g_autofree char *dev_id0 = NULL;
    g_autofree char *dev_id1 = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults -dev3590 id=tape0 -dev3590 id=tape1");
    dev_id0 = qom_get_string(qts, "/machine/peripheral/tape0", "dev_id");
    dev_id1 = qom_get_string(qts, "/machine/peripheral/tape1", "dev_id");
    g_assert_cmpstr(dev_id0, ==, "fe.0.0580");
    g_assert_cmpstr(dev_id1, ==, "fe.0.0581");
    qtest_quit(qts);
}

static void test_runtime_media_change(void)
{
    g_autofree char *path = create_aws_image();
    g_autofree char *bad_path = create_malformed_aws_image();
    QTestState *qts = qtest_init(
        "-nodefaults -dev3590 id=tape0,devno=0580 -S");

    g_assert_true(tape_tray_open(qts));
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-add', 'arguments': {"
        " 'node-name': 'tape-file', 'driver': 'file', 'filename': %s}}",
        path);
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-add', 'arguments': {"
        " 'node-name': 'tape-medium', 'driver': 'raw',"
        " 'read-only': true, 'file': 'tape-file'}}");
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-insert-medium', 'arguments': {"
        " 'id': 'tape0', 'node-name': 'tape-medium'}}");
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-close-tray', 'arguments': {'id': 'tape0'}}");
    g_assert_false(tape_tray_open(qts));
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-open-tray', 'arguments': {'id': 'tape0'}}");
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-remove-medium', 'arguments': {'id': 'tape0'}}");
    g_assert_true(tape_tray_open(qts));

    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-add', 'arguments': {"
        " 'node-name': 'bad-file', 'driver': 'file', 'filename': %s}}",
        bad_path);
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-add', 'arguments': {"
        " 'node-name': 'bad-medium', 'driver': 'raw',"
        " 'read-only': true, 'file': 'bad-file'}}");
    qtest_qmp_assert_success(
        qts,
        "{'execute': 'blockdev-insert-medium', 'arguments': {"
        " 'id': 'tape0', 'node-name': 'bad-medium'}}");
    qobject_unref(qtest_qmp_assert_failure_ref(
        qts,
        "{'execute': 'blockdev-close-tray', 'arguments': {'id': 'tape0'}}"));
    g_assert_true(tape_tray_open(qts));

    qtest_quit(qts);
    unlink(path);
    unlink(bad_path);
}

static void test_identity_whitelist(void)
{
    static const char * const identities[] = {
        "3410", "3411", "3420", "3422", "3430", "3480",
        "3490", "3590", "8809", "9347", "9348",
    };
    g_autoptr(GString) args = g_string_new("-nodefaults -S");
    QTestState *qts;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(identities); i++) {
        g_string_append_printf(args,
            " -dev3590 id=tape%zu,devno=%03zx,ident=%s",
            i, 0x580 + i, identities[i]);
    }
    qts = qtest_init(args->str);

    for (i = 0; i < ARRAY_SIZE(identities); i++) {
        g_autofree char *path =
            g_strdup_printf("/machine/peripheral/tape%zu", i);
        g_autofree char *ident = qom_get_string(qts, path, "ident");

        g_assert_cmpstr(ident, ==, identities[i]);
    }
    qtest_quit(qts);
}

/*
 * Migrate a drive with a medium loaded (tray open with @open_tray) into a
 * destination with drive arguments @dst_args and return the status of
 * the destination.
 */
static char *tape_migrate(const char *src_image, bool open_tray,
                          const char *dst_args, bool *dst_tray_open)
{
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    QTestState *src, *dst;
    char *result;
    int fd = g_file_open_tmp("qemu-3590-state-XXXXXX", &state, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    src = qtest_initf("-nodefaults -S -dev3590 file=%s,devno=580,id=tape0",
                      src_image);
    if (open_tray) {
        qtest_qmp_assert_success(src,
            "{'execute': 'blockdev-open-tray', 'arguments': {'id': 'tape0'}}");
    }
    status = migrate_to_file(src, state);
    g_assert_cmpstr(status, ==, "completed");
    qtest_quit(src);

    dst = qtest_initf("-nodefaults -S %s -incoming defer", dst_args);
    result = migrate_from_file(dst, state);
    if (dst_tray_open && !strcmp(result, "completed")) {
        *dst_tray_open = tape_tray_open(dst);
    }
    qtest_quit(dst);
    unlink(state);
    return result;
}

static void test_migration(void)
{
    g_autofree char *path = create_aws_image();
    g_autofree char *args = g_strdup_printf(
        "-dev3590 file=%s,devno=580,id=tape0", path);
    g_autofree char *status = NULL;
    bool open = true;

    status = tape_migrate(path, false, args, &open);
    g_assert_cmpstr(status, ==, "completed");
    g_assert_false(open);
    unlink(path);
}

static void test_migration_tray_open(void)
{
    g_autofree char *path = create_aws_image();
    g_autofree char *args = g_strdup_printf(
        "-dev3590 file=%s,devno=580,id=tape0", path);
    g_autofree char *status = NULL;
    bool open = false;

    status = tape_migrate(path, true, args, &open);
    g_assert_cmpstr(status, ==, "completed");
    g_assert_true(open);
    unlink(path);
}

static void test_migration_no_medium(void)
{
    g_autofree char *path = create_aws_image();
    g_autofree char *status = NULL;

    status = tape_migrate(path, false, "-dev3590 devno=580,id=tape0", NULL);
    g_assert_cmpstr(status, ==, "failed");
    unlink(path);
}

static void test_migration_other_medium(void)
{
    g_autofree char *path = create_aws_image();
    g_autofree char *other = create_other_aws_image();
    g_autofree char *args = g_strdup_printf(
        "-dev3590 file=%s,devno=580,id=tape0", other);
    g_autofree char *status = NULL;

    status = tape_migrate(path, false, args, NULL);
    g_assert_cmpstr(status, ==, "failed");
    unlink(path);
    unlink(other);
}

/*
 * A writable medium may still grow while the destination starts: an
 * incomplete record at its tail must not keep the destination from
 * starting, the migrated position is what counts.
 */
static void test_migration_writable_growing(void)
{
    static const uint8_t partial[] = { 0x00, 0x10, 0x00, 0x00, 0xa0 };
    g_autofree char *path = create_aws_image();
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    QTestState *src, *dst;
    int fd = g_file_open_tmp("qemu-3590-state-XXXXXX", &state, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    src = qtest_initf("-nodefaults -S -dev3590 file=%s,devno=580,"
                      "readonly=off,id=tape0", path);
    g_free(status);
    status = migrate_to_file(src, state);
    g_assert_cmpstr(status, ==, "completed");
    qtest_quit(src);

    fd = open(path, O_WRONLY | O_APPEND);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, partial, sizeof(partial)), ==, sizeof(partial));
    close(fd);

    dst = qtest_initf("-nodefaults -S -dev3590 file=%s,devno=580,"
                      "readonly=off,id=tape0 -incoming defer", path);
    g_free(status);
    status = migrate_from_file(dst, state);
    g_assert_cmpstr(status, ==, "completed");
    g_assert_false(tape_tray_open(dst));
    qtest_quit(dst);
    unlink(state);
    unlink(path);
}

static void test_snapshot(void)
{
    g_autofree char *path = create_aws_image();
    g_autofree char *disk = create_snapshot_image();
    QTestState *qts;

    if (!disk) {
        g_test_skip("QTEST_QEMU_IMG not set");
        unlink(path);
        return;
    }
    qts = qtest_initf("-nodefaults -dev3590 file=%s,devno=580,id=tape0 "
                      "-drive if=none,id=snap,file=%s,format=qcow2",
                      path, disk);
    snapshot_roundtrip(qts);
    g_assert_false(tape_tray_open(qts));
    qtest_quit(qts);
    unlink(path);
    unlink(disk);
}

/*
 * Channel programs on the drive
 *
 * The guest program of s390x-ccw-prog.h runs the channel programs, under
 * KVM only: the tests are skipped on a host without it.
 */

#define TAPE_WRITE           0x01
#define TAPE_READ            0x02
#define TAPE_SENSE           0x04
#define TAPE_REWIND          0x07
#define TAPE_READ_PREVIOUS   0x0a
#define TAPE_REWIND_UNLOAD   0x0f
#define TAPE_ERASE_GAP       0x17
#define TAPE_WRITE_MARK      0x1f
#define TAPE_READ_BLOCK_ID   0x22
#define TAPE_BSB             0x27
#define TAPE_BSF             0x2f
#define TAPE_FSB             0x37
#define TAPE_FSF             0x3f
#define TAPE_LOCATE          0x4f

#define DSTAT_UE             0x01
#define CSTAT_IL             0x40

#define TAPE_BUF  0x10000       /* data buffer, 320 KiB */
#define TAPE_BUF2 0x80000       /* second buffer for compares */
#define TAPE_SENSE_BUF 0xf000

typedef struct TapeIo {
    QTestState *qts;
    char *image;
} TapeIo;

static bool tape_io_start(TapeIo *t, const char *drive_args,
                          const char *dev_args, const char *extra)
{
    g_autofree char *devices = NULL;

    if (!qtest_has_accel("kvm")) {
        g_test_skip("needs KVM");
        return false;
    }
    if (!t->image) {
        t->image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    }
    devices = g_strdup_printf("%s -device 3590-ccw,%sdevno=fe.0.0580,"
                              "id=tape0%s%s %s", drive_args ?: "",
                              drive_args ? "drive=tape," : "",
                              dev_args ? "," : "", dev_args ?: "",
                              extra ?: "");
    t->qts = qtest_initf("-nodefaults -accel kvm -m 64M -kernel %s %s",
                         t->image, devices);
    /* qtest adds "-accel qtest" as a fallback: insist on KVM */
    {
        QDict *rsp = qtest_qmp(t->qts, "{'execute': 'query-kvm'}");

        g_assert_true(qdict_get_bool(qdict_get_qdict(rsp, "return"),
                                     "enabled"));
        qobject_unref(rsp);
    }
    return true;
}

static bool tape_io_start_file(TapeIo *t, const char *aws, bool writable,
                               const char *dev_args)
{
    g_autofree char *drive = g_strdup_printf(
        "-drive if=none,id=tape,format=raw,file=%s,read-only=%s",
        aws, writable ? "off" : "on");

    return tape_io_start(t, drive, dev_args, NULL);
}

static void tape_io_stop(TapeIo *t)
{
    qtest_quit(t->qts);
    t->qts = NULL;
}

static void tape_io_free(TapeIo *t)
{
    if (t->image) {
        unlink(t->image);
        g_clear_pointer(&t->image, g_free);
    }
}

/* Run one CCW; return its IRB */
static CcwProgIrb tape_ccw(TapeIo *t, uint8_t cmd, uint8_t flags,
                           uint16_t count, uint32_t cda)
{
    CcwProgIrb irb;

    ccw_prog_set_ccw(t->qts, 0, cmd, flags, count, cda);
    g_assert_cmpint(ccw_prog_run(t->qts, CCW_ORB_FMT, &irb), ==, 0);
    return irb;
}

static void tape_ok(TapeIo *t, uint8_t cmd, uint16_t count, uint32_t cda)
{
    CcwProgIrb irb = tape_ccw(t, cmd, CCW_FLAG_SLI, count, cda);

    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    g_assert_cmphex(irb.cstat, ==, 0);
}

static void tape_sense(TapeIo *t, uint8_t *sense)
{
    tape_ok(t, TAPE_SENSE, 32, TAPE_SENSE_BUF);
    qtest_memread(t->qts, TAPE_SENSE_BUF, sense, 32);
}

/* Unit check expected: return sense bytes 0, 1 and 3 */
static void tape_expect_uc(TapeIo *t, CcwProgIrb irb, uint8_t sense0,
                           uint8_t era, uint8_t *sense)
{
    uint8_t buf[32];

    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | CCW_DSTAT_UC);
    tape_sense(t, sense ?: buf);
    g_assert_cmphex((sense ?: buf)[0], ==, sense0);
    g_assert_cmphex((sense ?: buf)[3], ==, era);
}

static uint32_t tape_block_id(TapeIo *t)
{
    uint8_t id[8];

    tape_ok(t, TAPE_READ_BLOCK_ID, 8, TAPE_SENSE_BUF);
    qtest_memread(t->qts, TAPE_SENSE_BUF, id, sizeof(id));
    g_assert_cmphex(ldl_be_p(id), ==, ldl_be_p(id + 4));
    return ldl_be_p(id);
}

static void fill_block(uint8_t *buf, size_t len, unsigned int seed)
{
    size_t i;

    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed * 31 + i * 7 + (i >> 8));
    }
}

/*
 * Write one block of @len bytes from @data with as many data-chained CCWs
 * as needed (at most 65535 bytes each).  Return the IRB.
 */
static CcwProgIrb tape_write_block(TapeIo *t, const uint8_t *data, size_t len)
{
    unsigned int n = 0;
    size_t done = 0;
    CcwProgIrb irb;

    qtest_memwrite(t->qts, TAPE_BUF, data, len);
    while (done < len) {
        size_t part = MIN(len - done, (size_t)UINT16_MAX);

        ccw_prog_set_ccw(t->qts, n++, TAPE_WRITE,
                         done + part < len ? CCW_FLAG_DC : 0, part,
                         TAPE_BUF + done);
        done += part;
    }
    g_assert_cmpint(ccw_prog_run(t->qts, CCW_ORB_FMT, &irb), ==, 0);
    return irb;
}

static void tape_write_ok(TapeIo *t, const uint8_t *data, size_t len)
{
    CcwProgIrb irb = tape_write_block(t, data, len);

    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    g_assert_cmphex(irb.cstat, ==, 0);
}

static void tape_write_label(TapeIo *t, const char *text)
{
    uint8_t label[80];

    memset(label, ' ', sizeof(label));
    memcpy(label, text, strlen(text));
    tape_write_ok(t, label, sizeof(label));
}

static void tape_mark(TapeIo *t)
{
    tape_ok(t, TAPE_WRITE_MARK, 1, 0);
}

/*
 * Read the next block with data-chained CCWs covering 256 KiB and compare
 * it with @data / @len.
 */
static void tape_read_expect(TapeIo *t, const uint8_t *data, size_t len)
{
    g_autofree uint8_t *buf = g_malloc(len);
    size_t total = 4 * UINT16_MAX + 4;
    unsigned int i;
    CcwProgIrb irb;

    for (i = 0; i < 5; i++) {
        ccw_prog_set_ccw(t->qts, i, TAPE_READ,
                         i < 4 ? CCW_FLAG_DC : CCW_FLAG_SLI,
                         i < 4 ? UINT16_MAX : 4, TAPE_BUF2 + i * UINT16_MAX);
    }
    g_assert_cmpint(ccw_prog_run(t->qts, CCW_ORB_FMT, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    if (len == total) {
        g_assert_cmphex(irb.cstat, ==, 0);
    } else {
        /* Ended in a data-chained CCW: incorrect length */
        g_assert_cmphex(irb.cstat, ==, CSTAT_IL);
        g_assert_cmphex(irb.cpa, ==,
                        CCW_PROG_CCWS + 8 * (len / UINT16_MAX + 1));
    }
    qtest_memread(t->qts, TAPE_BUF2, buf, len);
    g_assert(memcmp(buf, data, len) == 0);
}

static void tape_read_label(TapeIo *t, const char *text)
{
    uint8_t label[80];

    memset(label, ' ', sizeof(label));
    memcpy(label, text, strlen(text));
    tape_read_expect(t, label, sizeof(label));
}

static void tape_read_mark(TapeIo *t)
{
    CcwProgIrb irb = tape_ccw(t, TAPE_READ, CCW_FLAG_SLI, 80, TAPE_BUF2);

    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
}

static char *tape_new_image(const char *tag)
{
    g_autofree char *name = g_strdup_printf("qemu-3590-%s-XXXXXX.aws", tag);
    char *path = NULL;
    int fd = g_file_open_tmp(name, &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    return path;
}

/*
 * Check the AWS structure of a file written by the drive: chunks of at
 * most 65535 bytes, backward links, record flags.  Return the number of
 * blocks + tape marks.
 */
static unsigned int aws_check_file(const char *path, size_t *size)
{
    g_autofree char *contents = NULL;
    gsize len;
    size_t pos = 0;
    uint16_t prev = 0;
    bool in_record = false;
    unsigned int blocks = 0;

    g_assert(g_file_get_contents(path, &contents, &len, NULL));
    while (pos < len) {
        const uint8_t *h = (const uint8_t *)contents + pos;
        uint16_t clen = lduw_le_p(h);

        g_assert_cmpuint(pos + 6, <=, len);
        g_assert_cmpuint(lduw_le_p(h + 2), ==, prev);
        g_assert_cmphex(h[5], ==, 0);
        if (h[4] & 0x40) {
            g_assert_false(in_record);
            g_assert_cmpuint(clen, ==, 0);
            blocks++;
        } else {
            g_assert_cmpint(!!(h[4] & 0x80), ==, !in_record);
            in_record = !(h[4] & 0x20);
            if (!in_record) {
                blocks++;
            }
        }
        prev = clen;
        pos += 6 + clen;
    }
    g_assert_cmpuint(pos, ==, len);
    g_assert_false(in_record);
    if (size) {
        *size = len;
    }
    return blocks;
}

/* Sizes that cover one chunk, a full chunk, two chunks and five chunks */
static const size_t test_sizes[] = { 80, 32760, 65535, 65536, 262144 };

/*
 * Two files with standard labels as an operating system writes them
 * (label, tape mark, data, tape mark, trailer labels, two tape marks,
 * back over the second one), read back in full, by Forward Space File,
 * Locate Block and Read Previous, also after a remount.
 */
static void test_io_multifile(void)
{
    g_autofree char *aws = tape_new_image("multi");
    uint8_t *data[ARRAY_SIZE(test_sizes)];
    TapeIo t = { 0 };
    size_t i, size;

    if (!tape_io_start_file(&t, aws, true, NULL)) {
        unlink(aws);
        return;
    }
    for (i = 0; i < ARRAY_SIZE(test_sizes); i++) {
        data[i] = g_malloc(test_sizes[i]);
        fill_block(data[i], test_sizes[i], i + 1);
    }

    tape_write_label(&t, "VOL1QEMU01");
    tape_write_label(&t, "HDR1FILE.ONE");
    tape_write_label(&t, "HDR2U");
    tape_mark(&t);
    for (i = 0; i < ARRAY_SIZE(test_sizes); i++) {
        tape_write_ok(&t, data[i], test_sizes[i]);
    }
    tape_mark(&t);
    tape_write_label(&t, "EOF1FILE.ONE");
    tape_write_label(&t, "EOF2U");
    tape_mark(&t);
    tape_mark(&t);
    g_assert_cmphex(tape_block_id(&t), ==, 14);
    tape_ok(&t, TAPE_BSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 13);

    tape_write_label(&t, "HDR1FILE.TWO");
    tape_write_label(&t, "HDR2U");
    tape_mark(&t);
    tape_write_ok(&t, data[0], test_sizes[0]);
    tape_mark(&t);
    tape_write_label(&t, "EOF1FILE.TWO");
    tape_write_label(&t, "EOF2U");
    tape_mark(&t);
    tape_mark(&t);
    tape_ok(&t, TAPE_BSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 21);

    /* Everything, in order */
    tape_ok(&t, TAPE_REWIND, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 0);
    tape_read_label(&t, "VOL1QEMU01");
    tape_read_label(&t, "HDR1FILE.ONE");
    tape_read_label(&t, "HDR2U");
    tape_read_mark(&t);
    for (i = 0; i < ARRAY_SIZE(test_sizes); i++) {
        tape_read_expect(&t, data[i], test_sizes[i]);
    }
    tape_read_mark(&t);
    tape_read_label(&t, "EOF1FILE.ONE");
    tape_read_label(&t, "EOF2U");
    tape_read_mark(&t);
    tape_read_label(&t, "HDR1FILE.TWO");

    /* Forward Space File */
    tape_ok(&t, TAPE_REWIND, 1, 0);
    tape_ok(&t, TAPE_FSF, 1, 0);
    tape_ok(&t, TAPE_FSF, 1, 0);
    tape_ok(&t, TAPE_FSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 13);
    tape_read_label(&t, "HDR1FILE.TWO");
    tape_ok(&t, TAPE_FSF, 1, 0);
    tape_read_expect(&t, data[0], test_sizes[0]);

    /* Locate Block, Read Previous */
    qtest_writel(t.qts, TAPE_SENSE_BUF, cpu_to_be32(6));
    tape_ok(&t, TAPE_LOCATE, 4, TAPE_SENSE_BUF);
    g_assert_cmphex(tape_block_id(&t), ==, 6);
    tape_read_expect(&t, data[2], test_sizes[2]);
    g_assert_cmphex(tape_block_id(&t), ==, 7);
    tape_read_expect(&t, data[3], test_sizes[3]);
    {
        CcwProgIrb irb = tape_ccw(&t, TAPE_READ_PREVIOUS, CCW_FLAG_SLI,
                                  UINT16_MAX, TAPE_BUF2);
        uint8_t buf[80];

        g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
        g_assert_cmphex(tape_block_id(&t), ==, 7);
        qtest_memread(t.qts, TAPE_BUF2, buf, sizeof(buf));
        g_assert(memcmp(buf, data[3], sizeof(buf)) == 0);
    }
    tape_io_stop(&t);

    g_assert_cmpuint(aws_check_file(aws, &size), ==, 22);

    /* Remount: the image the drive wrote is a valid medium */
    g_assert_true(tape_io_start_file(&t, aws, false, NULL));
    tape_ok(&t, TAPE_FSF, 1, 0);
    tape_ok(&t, TAPE_FSF, 1, 0);
    tape_ok(&t, TAPE_FSF, 1, 0);
    tape_read_label(&t, "HDR1FILE.TWO");
    tape_io_stop(&t);

    tape_io_free(&t);
    for (i = 0; i < ARRAY_SIZE(test_sizes); i++) {
        g_free(data[i]);
    }
    unlink(aws);
}

/*
 * Status of the spacing commands and of Read Previous at tape marks, BOT
 * and the end of the data.  Medium: A, tape mark, B, tape mark.
 */
static void test_io_space_status(void)
{
    g_autofree char *aws = tape_new_image("space");
    uint8_t sense[32];
    TapeIo t = { 0 };
    CcwProgIrb irb;

    if (!tape_io_start_file(&t, aws, true, NULL)) {
        unlink(aws);
        return;
    }
    tape_write_label(&t, "A");
    tape_mark(&t);
    tape_write_label(&t, "B");
    tape_mark(&t);
    tape_ok(&t, TAPE_REWIND, 1, 0);

    tape_ok(&t, TAPE_FSB, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 1);
    irb = tape_ccw(&t, TAPE_FSB, 0, 1, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    g_assert_cmphex(tape_block_id(&t), ==, 2);
    irb = tape_ccw(&t, TAPE_BSB, 0, 1, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    g_assert_cmphex(tape_block_id(&t), ==, 1);
    tape_ok(&t, TAPE_BSB, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 0);

    /* Backward at BOT: ERA 39, BOT in sense byte 1 */
    tape_expect_uc(&t, tape_ccw(&t, TAPE_BSB, 0, 1, 0), 0, 0x39, sense);
    g_assert_cmphex(sense[1] & 0x48, ==, 0x48);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_BSF, 0, 1, 0), 0, 0x39, NULL);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_READ_PREVIOUS, CCW_FLAG_SLI, 80,
                                TAPE_BUF2), 0, 0x39, NULL);

    /* Read Previous over a tape mark stops in front of it */
    tape_ok(&t, TAPE_FSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 2);
    irb = tape_ccw(&t, TAPE_READ_PREVIOUS, CCW_FLAG_SLI, 80, TAPE_BUF2);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    g_assert_cmphex(tape_block_id(&t), ==, 1);
    tape_read_mark(&t);
    tape_read_label(&t, "B");
    tape_ok(&t, TAPE_READ_PREVIOUS, 80, TAPE_BUF2);
    g_assert_cmphex(tape_block_id(&t), ==, 2);

    /* Backward Space File from just behind a tape mark */
    tape_ok(&t, TAPE_FSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 4);
    tape_ok(&t, TAPE_BSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 3);
    tape_ok(&t, TAPE_BSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 1);

    /* End of the data: data check, ERA 36, position kept */
    tape_ok(&t, TAPE_FSF, 1, 0);
    tape_ok(&t, TAPE_FSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 4);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_FSB, 0, 1, 0), 0x08, 0x36, sense);
    /* 3590 sense: reason code 3020 (end of data) in bytes 4-5 */
    g_assert_cmphex(lduw_be_p(sense + 4), ==, 0x3020);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_READ, CCW_FLAG_SLI, 80, TAPE_BUF2),
                   0x08, 0x36, NULL);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_FSF, 0, 1, 0), 0x08, 0x36, NULL);
    g_assert_cmphex(tape_block_id(&t), ==, 4);

    /* Locate past the end: locate failure, position kept */
    qtest_writel(t.qts, TAPE_SENSE_BUF, cpu_to_be32(1));
    tape_ok(&t, TAPE_LOCATE, 4, TAPE_SENSE_BUF);
    qtest_writel(t.qts, TAPE_SENSE_BUF, cpu_to_be32(9));
    tape_expect_uc(&t, tape_ccw(&t, TAPE_LOCATE, 0, 4, TAPE_SENSE_BUF),
                   0x08, 0x36, sense);
    g_assert_cmphex(sense[1] & 0x80, ==, 0x80);
    g_assert_cmphex(tape_block_id(&t), ==, 1);

    tape_io_stop(&t);
    tape_io_free(&t);
    unlink(aws);
}

/*
 * Data chaining: a Read that ends exactly at the end of a CCW with chain
 * data set must end the operation at the next CCW (incorrect length) and
 * must not read the next block into the chain.
 */
static void test_io_data_chaining(void)
{
    g_autofree char *aws = tape_new_image("dc");
    uint8_t block[100], buf[100];
    TapeIo t = { 0 };
    CcwProgIrb irb;

    if (!tape_io_start_file(&t, aws, true, NULL)) {
        unlink(aws);
        return;
    }
    fill_block(block, sizeof(block), 7);
    tape_write_ok(&t, block, sizeof(block));
    tape_write_label(&t, "NEXT");
    tape_ok(&t, TAPE_REWIND, 1, 0);

    qtest_memset(t.qts, TAPE_BUF2, 0xee, 200);
    ccw_prog_set_ccw(t.qts, 0, TAPE_READ, CCW_FLAG_DC, 100, TAPE_BUF2);
    ccw_prog_set_ccw(t.qts, 1, TAPE_READ, 0, 100, TAPE_BUF2 + 100);
    g_assert_cmpint(ccw_prog_run(t.qts, CCW_ORB_FMT, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    g_assert_cmphex(irb.cstat, ==, CSTAT_IL);
    g_assert_cmphex(irb.cpa, ==, CCW_PROG_CCWS + 16);
    g_assert_cmphex(irb.count, ==, 100);
    qtest_memread(t.qts, TAPE_BUF2, buf, sizeof(buf));
    g_assert(memcmp(buf, block, sizeof(buf)) == 0);
    g_assert_cmphex(qtest_readb(t.qts, TAPE_BUF2 + 100), ==, 0xee);
    g_assert_cmphex(tape_block_id(&t), ==, 1);
    tape_read_label(&t, "NEXT");

    /* A block larger than 256 KiB is refused, nothing written */
    {
        unsigned int i;

        for (i = 0; i < 5; i++) {
            ccw_prog_set_ccw(t.qts, i, TAPE_WRITE, i < 4 ? CCW_FLAG_DC : 0,
                             UINT16_MAX, TAPE_BUF);
        }
        g_assert_cmpint(ccw_prog_run(t.qts, CCW_ORB_FMT, &irb), ==, 0);
        tape_expect_uc(&t, irb, 0x80, 0x27, NULL);
        g_assert_cmphex(tape_block_id(&t), ==, 2);
    }
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(aws, NULL), ==, 2);
    tape_io_free(&t);
    unlink(aws);
}

/* A Write in the middle of the data ends the data there. */
static void test_io_write_truncates(void)
{
    g_autofree char *aws = tape_new_image("trunc");
    TapeIo t = { 0 };
    char name[8];
    size_t size;
    int i;

    if (!tape_io_start_file(&t, aws, true, NULL)) {
        unlink(aws);
        return;
    }
    for (i = 0; i < 5; i++) {
        snprintf(name, sizeof(name), "B%d", i);
        tape_write_label(&t, name);
    }
    tape_mark(&t);
    tape_ok(&t, TAPE_REWIND, 1, 0);
    tape_read_label(&t, "B0");
    tape_read_label(&t, "B1");
    tape_write_label(&t, "NEW");
    tape_expect_uc(&t, tape_ccw(&t, TAPE_READ, CCW_FLAG_SLI, 80, TAPE_BUF2),
                   0x08, 0x36, NULL);
    tape_ok(&t, TAPE_BSB, 1, 0);
    tape_read_label(&t, "NEW");
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(aws, &size), ==, 3);
    g_assert_cmpuint(size, ==, 3 * 86);
    tape_io_free(&t);
    unlink(aws);
}

/* A read-only medium is file protected. */
static void test_io_read_only(void)
{
    g_autofree char *aws = create_aws_image();
    uint8_t sense[32];
    TapeIo t = { 0 };

    if (!tape_io_start_file(&t, aws, false, NULL)) {
        unlink(aws);
        return;
    }
    tape_sense(&t, sense);
    g_assert_cmphex(sense[1] & 0x02, ==, 0x02);
    qtest_memset(t.qts, TAPE_BUF, 0x40, 80);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_WRITE, 0, 80, TAPE_BUF),
                   0x80, 0x30, sense);
    g_assert_cmphex(sense[1] & 0x02, ==, 0x02);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_WRITE_MARK, 0, 1, 0),
                   0x80, 0x30, NULL);
    tape_io_stop(&t);
    tape_io_free(&t);
    unlink(aws);
}

/*
 * The end of a volume as the operating system handles it: Write completes
 * with unit exception from the early-warning point on, the program writes
 * its trailer labels (tape mark, EOV1, EOV2, two tape marks, each with unit
 * exception) and unloads.  A block beyond the end itself is refused with
 * physical end of tape and is not on the medium.
 */
static void test_io_eot_capacity(void)
{
    g_autofree char *aws = tape_new_image("eot");
    g_autofree uint8_t *block = g_malloc(8192);
    TapeIo t = { 0 };
    CcwProgIrb irb;
    size_t size;
    int i;

    if (!tape_io_start_file(&t, aws, true,
                            "capacity=64K,eot-margin=16K")) {
        unlink(aws);
        return;
    }
    for (i = 1; i <= 7; i++) {
        fill_block(block, 8192, i);
        irb = tape_write_block(&t, block, 8192);
        /* 8198 bytes per block: block 6 ends past 48 KiB */
        g_assert_cmphex(irb.dstat, ==,
                        CCW_DSTAT_CE_DE | (i >= 6 ? DSTAT_UE : 0));
        g_assert_cmphex(irb.cstat, ==, 0);
    }
    irb = tape_write_block(&t, block, 8192);
    tape_expect_uc(&t, irb, 0x10, 0x38, NULL);
    g_assert_cmphex(tape_block_id(&t), ==, 7);

    /* End of volume */
    irb = tape_ccw(&t, TAPE_WRITE_MARK, 0, 1, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    for (i = 0; i < 2; i++) {
        uint8_t label[80];

        memset(label, ' ', sizeof(label));
        memcpy(label, i ? "EOV2" : "EOV1", 4);
        irb = tape_write_block(&t, label, sizeof(label));
        g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    }
    for (i = 0; i < 2; i++) {
        irb = tape_ccw(&t, TAPE_WRITE_MARK, 0, 1, 0);
        g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    }
    irb = tape_ccw(&t, TAPE_ERASE_GAP, 0, 1, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_REWIND_UNLOAD, 0, 1, 0), 0, 0x2b,
                   NULL);
    tape_io_stop(&t);

    g_assert_cmpuint(aws_check_file(aws, &size), ==, 12);
    g_assert_cmpuint(size, ==, 7 * 8198 + 3 * 6 + 2 * 86);

    /* Read back after a remount */
    g_assert_true(tape_io_start_file(&t, aws, false,
                                     "capacity=64K,eot-margin=16K"));
    for (i = 1; i <= 7; i++) {
        fill_block(block, 8192, i);
        tape_read_expect(&t, block, 8192);
    }
    tape_read_mark(&t);
    tape_read_label(&t, "EOV1");
    tape_read_label(&t, "EOV2");
    tape_read_mark(&t);
    tape_read_mark(&t);
    tape_io_stop(&t);
    tape_io_free(&t);
    unlink(aws);
}

/* The same with a limit in blocks: max-blocks, automatic margin (1/8). */
static void test_io_eot_blocks(void)
{
    g_autofree char *aws = tape_new_image("eotb");
    TapeIo t = { 0 };
    CcwProgIrb irb;
    uint8_t label[80];
    int i;

    if (!tape_io_start_file(&t, aws, true, "max-blocks=40")) {
        unlink(aws);
        return;
    }
    memset(label, 'x', sizeof(label));
    for (i = 1; i <= 40; i++) {
        irb = tape_write_block(&t, label, sizeof(label));
        g_assert_cmphex(irb.dstat, ==,
                        CCW_DSTAT_CE_DE | (i >= 35 ? DSTAT_UE : 0));
    }
    tape_expect_uc(&t, tape_write_block(&t, label, sizeof(label)),
                   0x10, 0x38, NULL);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_WRITE_MARK, 0, 1, 0),
                   0x10, 0x38, NULL);
    g_assert_cmphex(tape_block_id(&t), ==, 40);
    /* Going back below the warning point clears it */
    for (i = 0; i < 7; i++) {
        tape_ok(&t, TAPE_BSB, 1, 0);
    }
    tape_mark(&t);
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(aws, NULL), ==, 34);
    tape_io_free(&t);
    unlink(aws);
}

/*
 * The block-ID limit of the 3480/3490 identities: 22 bits.  A medium of
 * 4194290 tape marks, positioned at its end: logical end of tape at once
 * (the automatic margin is 8192 blocks), physical end at block 0x3fffff.
 */
static void test_io_eot_block_id_22(void)
{
    g_autofree char *aws = tape_new_image("bid22");
    const uint32_t marks = 4194290;
    g_autofree uint8_t *image = g_malloc0((size_t)marks * 6);
    TapeIo t = { 0 };
    uint8_t label[80];
    uint32_t i;

    for (i = 0; i < marks; i++) {
        image[i * 6 + 4] = 0x40;
    }
    g_assert(g_file_set_contents(aws, (char *)image, (gssize)marks * 6,
                                 NULL));
    if (!tape_io_start_file(&t, aws, true, "ident=3490")) {
        unlink(aws);
        return;
    }
    qtest_writel(t.qts, TAPE_SENSE_BUF, cpu_to_be32(marks));
    tape_ok(&t, TAPE_LOCATE, 4, TAPE_SENSE_BUF);
    g_assert_cmphex(tape_block_id(&t), ==, marks);
    memset(label, 'x', sizeof(label));
    for (i = marks; i < 0x3fffff; i++) {
        CcwProgIrb irb = tape_write_block(&t, label, sizeof(label));

        g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    }
    g_assert_cmphex(tape_block_id(&t), ==, 0x3fffff);
    tape_expect_uc(&t, tape_write_block(&t, label, sizeof(label)),
                   0x10, 0x38, NULL);
    tape_io_stop(&t);
    tape_io_free(&t);
    unlink(aws);
}

/*
 * The host runs out of room (ENOSPC, EFBIG): physical end of tape, the
 * block is not on the medium, which stays usable for what still fits.
 * blkdebug fails every write that touches byte 8192 and on.
 */
static void do_test_io_host_full(int err)
{
    g_autofree char *aws = tape_new_image("full");
    g_autofree char *drive = g_strdup_printf(
        "-blockdev '{\"driver\": \"raw\", \"node-name\": \"tape\", "
        "\"file\": {\"driver\": \"blkdebug\", \"inject-error\": ["
        "{\"event\": \"pwritev\", \"errno\": %d, \"sector\": 16, "
        "\"once\": false}], \"image\": {\"driver\": \"file\", "
        "\"filename\": \"%s\"}}}'", err, aws);
    g_autofree uint8_t *block = g_malloc(4096);
    TapeIo t = { 0 };
    size_t size;

    if (!tape_io_start(&t, drive, NULL, NULL)) {
        unlink(aws);
        return;
    }
    fill_block(block, 4096, 1);
    tape_write_ok(&t, block, 4096);
    tape_expect_uc(&t, tape_write_block(&t, block, 4096), 0x10, 0x38, NULL);
    g_assert_cmphex(tape_block_id(&t), ==, 1);
    /* Not fenced: what fits is still written */
    tape_mark(&t);
    tape_write_label(&t, "EOV1");
    tape_ok(&t, TAPE_REWIND, 1, 0);
    tape_read_expect(&t, block, 4096);
    tape_read_mark(&t);
    tape_read_label(&t, "EOV1");
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(aws, &size), ==, 3);
    g_assert_cmpuint(size, ==, 4102 + 6 + 86);
    tape_io_free(&t);
    unlink(aws);
}

static void test_io_host_enospc(void)
{
    do_test_io_host_full(ENOSPC);
}

static void test_io_host_efbig(void)
{
    do_test_io_host_full(EFBIG);
}

/*
 * Images whose length is not a multiple of 512 bytes, with fewer than six
 * bytes to the next boundary, mount and read.
 */
static void test_io_unaligned_image(void)
{
    static const size_t lengths[] = { 1015, 1016, 1017, 506, 505 };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(lengths); i++) {
        g_autofree char *aws = tape_new_image("unal");
        g_autofree uint8_t *image = g_malloc0(lengths[i] + 6);
        TapeIo t = { 0 };

        stw_le_p(image, lengths[i]);
        image[4] = 0xa0;
        fill_block(image + 6, lengths[i], 3);
        g_assert(g_file_set_contents(aws, (char *)image, lengths[i] + 6,
                                     NULL));
        if (!tape_io_start_file(&t, aws, false, NULL)) {
            unlink(aws);
            return;
        }
        tape_read_expect(&t, image + 6, lengths[i]);
        tape_expect_uc(&t, tape_ccw(&t, TAPE_READ, CCW_FLAG_SLI, 80,
                                    TAPE_BUF2), 0x08, 0x36, NULL);
        tape_io_stop(&t);
        tape_io_free(&t);
        unlink(aws);
    }
}

/* Properties that would raise a limit past what is encodable are refused */
static void test_limit_properties(void)
{
    static const char * const bad[] = {
        "ident=3490,max-blocks=4194304",
        "ident=3480,max-blocks=4194304",
        "eot-margin=1M",
        "capacity=1M,eot-margin=1M",
        "max-blocks=10,eot-margin-blocks=10",
    };
    static const char * const good[] = {
        "ident=3490,max-blocks=4194303",
        "max-blocks=4294967295",
        "capacity=1M,eot-margin=512K,max-blocks=100,eot-margin-blocks=10",
    };
    const char *qemu = g_getenv("QTEST_QEMU_BINARY");
    size_t i;

    for (i = 0; i < ARRAY_SIZE(bad); i++) {
        g_autofree char *dev = g_strdup_printf("3590-ccw,%s", bad[i]);
        const char *argv[] = {
            qemu, "-machine", "s390-ccw-virtio", "-accel", "qtest",
            "-nodefaults", "-display", "none", "-S", "-device", dev, NULL,
        };
        g_autofree char *out = NULL, *err = NULL;
        int status;

        g_assert(g_spawn_sync(NULL, (char **)argv, NULL, 0, NULL, NULL,
                              &out, &err, &status, NULL));
        g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) != 0);
        g_assert_nonnull(strstr(err, bad[i][0] == 'i' ? "max-blocks" :
                                     strstr(bad[i], "blocks=") ?
                                     "eot-margin-blocks" : "eot-margin"));
    }
    for (i = 0; i < ARRAY_SIZE(good); i++) {
        QTestState *qts = qtest_initf("-nodefaults -S -device 3590-ccw,%s",
                                      good[i]);

        qtest_quit(qts);
    }
}

/*
 * Migration with the medium positioned in the middle of the data (a large
 * multi-chunk block before the position), in the early-warning zone, and
 * then writing on the destination.
 */
static void test_io_migration_positioned(void)
{
    g_autofree char *aws = tape_new_image("mig");
    g_autofree char *state = tape_new_image("migstate");
    g_autofree char *drive = g_strdup_printf(
        "-drive if=none,id=tape,format=raw,file=%s,read-only=off", aws);
    g_autofree uint8_t *big = g_malloc(200000);
    g_autofree char *status = NULL;
    TapeIo t = { 0 };
    CcwProgIrb irb;

    if (!tape_io_start(&t, drive, "max-blocks=6,eot-margin-blocks=2",
                       NULL)) {
        unlink(aws);
        unlink(state);
        return;
    }
    fill_block(big, 200000, 9);
    tape_write_label(&t, "B0");
    tape_write_ok(&t, big, 200000);
    tape_write_label(&t, "B2");
    tape_ok(&t, TAPE_REWIND, 1, 0);
    tape_read_label(&t, "B0");
    tape_read_expect(&t, big, 200000);
    g_assert_cmphex(tape_block_id(&t), ==, 2);
    status = migrate_to_file(t.qts, state);
    g_assert_cmpstr(status, ==, "completed");
    tape_io_stop(&t);

    g_assert_true(tape_io_start(&t, drive, "max-blocks=6,eot-margin-blocks=2",
                                "-incoming defer"));
    g_free(status);
    status = migrate_from_file(t.qts, state);
    g_assert_cmpstr(status, ==, "completed");
    g_assert_cmphex(tape_block_id(&t), ==, 2);
    tape_ok(&t, TAPE_READ_PREVIOUS, UINT16_MAX, TAPE_BUF2);
    g_assert_cmphex(tape_block_id(&t), ==, 1);
    tape_read_expect(&t, big, 200000);
    tape_read_label(&t, "B2");
    /* Block 4 reaches the warning point (6 - 2) */
    irb = tape_write_block(&t, big, 100);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE | DSTAT_UE);
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(aws, NULL), ==, 4);
    tape_io_free(&t);
    unlink(aws);
    unlink(state);
}

/*
 * Timing of a large volume, only when QTEST_3590_PERF_BLOCKS is set: write
 * that many 65535-byte blocks, then time Backward Space Block at the end
 * of the data, Locate Block to the end and Forward Space File over all.
 */
static void test_io_perf(void)
{
    const char *env = g_getenv("QTEST_3590_PERF_BLOCKS");
    g_autofree char *aws = NULL;
    g_autofree uint8_t *block = g_malloc(UINT16_MAX);
    unsigned long n = env ? strtoul(env, NULL, 0) : 0, i;
    TapeIo t = { 0 };
    gint64 t0;

    if (!n) {
        g_test_skip("QTEST_3590_PERF_BLOCKS not set");
        return;
    }
    aws = tape_new_image("perf");
    if (!tape_io_start_file(&t, aws, true, NULL)) {
        unlink(aws);
        return;
    }
    fill_block(block, UINT16_MAX, 1);
    qtest_memwrite(t.qts, TAPE_BUF, block, UINT16_MAX);
    ccw_prog_set_ccw(t.qts, 0, TAPE_WRITE, 0, UINT16_MAX, TAPE_BUF);
    t0 = g_get_monotonic_time();
    for (i = 0; i < n; i++) {
        CcwProgIrb irb;

        g_assert_cmpint(ccw_prog_run(t.qts, CCW_ORB_FMT, &irb), ==, 0);
        g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    }
    g_test_message("write %lu x 64K: %.3f s", n,
                   (g_get_monotonic_time() - t0) / 1e6);
    tape_mark(&t);
    tape_mark(&t);
    t0 = g_get_monotonic_time();
    tape_ccw(&t, TAPE_BSB, 0, 1, 0);        /* over a tape mark: UE */
    g_test_message("BSB at end of data: %.3f s",
                   (g_get_monotonic_time() - t0) / 1e6);
    tape_ok(&t, TAPE_REWIND, 1, 0);
    qtest_writel(t.qts, TAPE_SENSE_BUF, cpu_to_be32(n));
    t0 = g_get_monotonic_time();
    tape_ok(&t, TAPE_LOCATE, 4, TAPE_SENSE_BUF);
    g_test_message("Locate to block %lu: %.3f s", n,
                   (g_get_monotonic_time() - t0) / 1e6);
    tape_ok(&t, TAPE_REWIND, 1, 0);
    t0 = g_get_monotonic_time();
    tape_ok(&t, TAPE_FSF, 1, 0);
    g_test_message("FSF over %lu blocks: %.3f s", n,
                   (g_get_monotonic_time() - t0) / 1e6);
    g_assert_cmphex(tape_block_id(&t), ==, n + 1);
    tape_io_stop(&t);
    t0 = g_get_monotonic_time();
    g_assert_true(tape_io_start_file(&t, aws, false, NULL));
    g_test_message("mount: %.3f s", (g_get_monotonic_time() - t0) / 1e6);
    tape_io_stop(&t);
    tape_io_free(&t);
    unlink(aws);
}

/*
 * A volume switch as at end of volume: logical end of tape on volume 1,
 * trailer labels, Rewind Unload, the operator mounts a new empty image for
 * writing (blockdev-change-medium), the drive presents attention, and
 * writing continues on volume 2.
 */
static void test_io_volume_switch(void)
{
    g_autofree char *vol1 = tape_new_image("vol1");
    g_autofree char *vol2 = tape_new_image("vol2");
    g_autofree char *drive = g_strdup_printf(
        "-drive if=none,id=tape,format=raw,file=%s,read-only=off", vol1);
    uint8_t block[1000];
    TapeIo t = { 0 };
    CcwProgIrb irb;
    int i;

    if (!tape_io_start(&t, drive, "capacity=8K,eot-margin=2K", NULL)) {
        unlink(vol1);
        unlink(vol2);
        return;
    }
    tape_write_label(&t, "VOL1V00001");
    for (i = 0; ; i++) {
        fill_block(block, sizeof(block), i);
        irb = tape_write_block(&t, block, sizeof(block));
        if (irb.dstat & DSTAT_UE) {
            break;
        }
        g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    }
    g_assert_cmpint(i, ==, 6);   /* 86 + 7 * 1006 >= 6 KiB */
    tape_ccw(&t, TAPE_WRITE_MARK, 0, 1, 0);
    tape_write_block(&t, (const uint8_t *)"EOV1", 4);
    tape_ccw(&t, TAPE_WRITE_MARK, 0, 1, 0);
    tape_ccw(&t, TAPE_WRITE_MARK, 0, 1, 0);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_REWIND_UNLOAD, 0, 1, 0), 0, 0x2b,
                   NULL);
    g_assert_true(tape_tray_open(t.qts));

    /* Not ready while unloaded */
    tape_expect_uc(&t, tape_ccw(&t, TAPE_READ, CCW_FLAG_SLI, 80, TAPE_BUF2),
                   0x40, 0x43, NULL);

    qtest_qmp_assert_success(t.qts,
        "{'execute': 'blockdev-change-medium', 'arguments': {"
        " 'id': 'tape0', 'filename': %s, 'format': 'raw',"
        " 'read-only-mode': 'read-write'}}", vol2);
    g_assert_false(tape_tray_open(t.qts));

    /* The mount is reported by attention: SSCH finds status pending */
    ccw_prog_set_ccw(t.qts, 0, 0x03, CCW_FLAG_SLI, 1, 0);
    g_assert_cmpint(ccw_prog_run(t.qts, CCW_ORB_FMT, &irb), ==, 1);
    g_assert_cmphex(irb.dstat, ==, 0x80 | 0x04);

    tape_write_label(&t, "VOL1V00002");
    fill_block(block, sizeof(block), i);
    tape_write_ok(&t, block, sizeof(block));
    tape_ok(&t, TAPE_REWIND, 1, 0);
    tape_read_label(&t, "VOL1V00002");
    tape_read_expect(&t, block, sizeof(block));
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(vol1, NULL), ==, 12);
    g_assert_cmpuint(aws_check_file(vol2, NULL), ==, 2);
    tape_io_free(&t);
    unlink(vol1);
    unlink(vol2);
}

/*
 * Rewind Unload as Linux issues it (Mode Set, Rewind Unload, No-Operation,
 * command-chained): unit check with "unload complete" ends the chain, so
 * the No-Operation never meets the unloaded drive.  3590 sense: reason
 * code 3123, basic recovery action "continue"; 3490 sense: ERA 2B and the
 * block number in bytes 4-6.
 */
static void do_test_io_rewind_unload(const char *ident)
{
    g_autofree char *aws = create_aws_image();
    bool native = !strcmp(ident, "ident=3590");
    uint8_t sense[32];
    TapeIo t = { 0 };
    CcwProgIrb irb;

    if (!tape_io_start_file(&t, aws, false, ident)) {
        unlink(aws);
        return;
    }
    tape_ok(&t, TAPE_FSB, 1, 0);
    qtest_writeb(t.qts, TAPE_SENSE_BUF, 0);
    ccw_prog_set_ccw(t.qts, 0, 0xdb, CCW_FLAG_CC, 1, TAPE_SENSE_BUF);
    ccw_prog_set_ccw(t.qts, 1, TAPE_REWIND_UNLOAD, CCW_FLAG_CC, 1, 0);
    ccw_prog_set_ccw(t.qts, 2, 0x03, 0, 1, 0);
    g_assert_cmpint(ccw_prog_run(t.qts, CCW_ORB_FMT, &irb), ==, 0);
    g_assert_cmphex(irb.cpa, ==, CCW_PROG_CCWS + 16);
    tape_expect_uc(&t, irb, 0, 0x2b, sense);
    if (native) {
        g_assert_cmphex(lduw_be_p(sense + 4), ==, 0x3123);
        g_assert_cmphex(sense[2] >> 6, ==, 1);
    }
    g_assert_true(tape_tray_open(t.qts));

    /* Not ready now: intervention required, ERA 43 */
    tape_expect_uc(&t, tape_ccw(&t, 0x03, 0, 1, 0), 0x40, 0x43, sense);
    if (native) {
        g_assert_cmphex(lduw_be_p(sense + 4), ==, 0x4010);
    }
    tape_io_stop(&t);
    tape_io_free(&t);
    unlink(aws);
}

static void test_io_rewind_unload_3590(void)
{
    do_test_io_rewind_unload("ident=3590");
}

static void test_io_rewind_unload_3490(void)
{
    do_test_io_rewind_unload("ident=3490");
}

/* 3490 sense: channel block number in bytes 4-6 */
static void test_io_sense_3490(void)
{
    g_autofree char *aws = create_other_aws_image();   /* block, mark */
    uint8_t sense[32];
    TapeIo t = { 0 };

    if (!tape_io_start_file(&t, aws, false, "ident=3490")) {
        unlink(aws);
        return;
    }
    tape_ok(&t, TAPE_FSF, 1, 0);
    g_assert_cmphex(tape_block_id(&t), ==, 2);
    tape_expect_uc(&t, tape_ccw(&t, TAPE_FSB, 0, 1, 0), 0x08, 0x36, sense);
    g_assert_cmphex(sense[4] << 16 | sense[5] << 8 | sense[6], ==,
                    tape_block_id(&t));
    tape_io_stop(&t);
    tape_io_free(&t);
    unlink(aws);
}

/* Change media between read-only and writable images (QMP), both ways */
static void change_medium(TapeIo *t, const char *path, bool writable)
{
    CcwProgIrb irb;

    qtest_qmp_assert_success(t->qts,
        "{'execute': 'blockdev-change-medium', 'arguments': {"
        " 'id': 'tape0', 'filename': %s, 'format': 'raw',"
        " 'read-only-mode': %s}}", path,
        writable ? "read-write" : "read-only");
    g_assert_false(tape_tray_open(t->qts));
    /* attention from the mount */
    ccw_prog_set_ccw(t->qts, 0, 0x03, CCW_FLAG_SLI, 1, 0);
    g_assert_cmpint(ccw_prog_run(t->qts, CCW_ORB_FMT, &irb), ==, 1);
    g_assert_cmphex(irb.dstat, ==, 0x80 | 0x04);
}

static void expect_protected(TapeIo *t, bool protected)
{
    uint8_t sense[32];

    tape_sense(t, sense);
    g_assert_cmphex(sense[1] & 0x02, ==, protected ? 0x02 : 0);
    if (protected) {
        tape_expect_uc(t, tape_ccw(t, TAPE_WRITE_MARK, 0, 1, 0),
                       0x80, 0x30, NULL);
    } else {
        tape_mark(t);
    }
}

static void do_test_io_media_perm(bool start_writable)
{
    g_autofree char *ro = create_aws_image();
    g_autofree char *rw = tape_new_image("rw");
    g_autofree char *rw2 = tape_new_image("rw2");
    TapeIo t = { 0 };

    if (!tape_io_start_file(&t, start_writable ? rw : ro, start_writable,
                            NULL)) {
        unlink(ro);
        unlink(rw);
        unlink(rw2);
        return;
    }
    expect_protected(&t, !start_writable);
    change_medium(&t, start_writable ? ro : rw2, !start_writable);
    expect_protected(&t, start_writable);
    change_medium(&t, start_writable ? rw2 : ro, start_writable);
    expect_protected(&t, !start_writable);

    /* After the guest unloads, any medium can be inserted */
    tape_expect_uc(&t, tape_ccw(&t, TAPE_REWIND_UNLOAD, 0, 1, 0), 0, 0x2b,
                   NULL);
    change_medium(&t, start_writable ? ro : rw, !start_writable);
    expect_protected(&t, start_writable);
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(rw, NULL), ==, 1);
    g_assert_cmpuint(aws_check_file(rw2, NULL), ==, 1);
    tape_io_free(&t);
    unlink(ro);
    unlink(rw);
    unlink(rw2);
}

static void test_io_media_perm_from_ro(void)
{
    do_test_io_media_perm(false);
}

static void test_io_media_perm_from_rw(void)
{
    do_test_io_media_perm(true);
}

/* An empty drive gets a writable medium */
static void test_io_media_perm_empty(void)
{
    g_autofree char *rw = tape_new_image("rwe");
    TapeIo t = { 0 };

    if (!tape_io_start(&t, NULL, NULL, NULL)) {
        unlink(rw);
        return;
    }
    change_medium(&t, rw, true);
    expect_protected(&t, false);
    tape_io_stop(&t);
    g_assert_cmpuint(aws_check_file(rw, NULL), ==, 1);
    tape_io_free(&t);
    unlink(rw);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/3590-ccw/dev3590-shortcut", test_dev3590_shortcut);
    qtest_add_func("/3590-ccw/auto-devnos", test_auto_devnos);
    qtest_add_func("/3590-ccw/runtime-media-change",
                   test_runtime_media_change);
    qtest_add_func("/3590-ccw/identity-whitelist",
                   test_identity_whitelist);
    qtest_add_func("/3590-ccw/migration", test_migration);
    qtest_add_func("/3590-ccw/migration-tray-open",
                   test_migration_tray_open);
    qtest_add_func("/3590-ccw/migration-no-medium",
                   test_migration_no_medium);
    qtest_add_func("/3590-ccw/migration-other-medium",
                   test_migration_other_medium);
    qtest_add_func("/3590-ccw/snapshot", test_snapshot);
    qtest_add_func("/3590-ccw/migration-writable-growing",
                   test_migration_writable_growing);
    qtest_add_func("/3590-ccw/limit-properties", test_limit_properties);
    qtest_add_func("/3590-ccw/io/multifile", test_io_multifile);
    qtest_add_func("/3590-ccw/io/space-status", test_io_space_status);
    qtest_add_func("/3590-ccw/io/data-chaining", test_io_data_chaining);
    qtest_add_func("/3590-ccw/io/write-truncates", test_io_write_truncates);
    qtest_add_func("/3590-ccw/io/read-only", test_io_read_only);
    qtest_add_func("/3590-ccw/io/eot-capacity", test_io_eot_capacity);
    qtest_add_func("/3590-ccw/io/eot-blocks", test_io_eot_blocks);
    qtest_add_func("/3590-ccw/io/eot-block-id-22", test_io_eot_block_id_22);
    qtest_add_func("/3590-ccw/io/host-enospc", test_io_host_enospc);
    qtest_add_func("/3590-ccw/io/host-efbig", test_io_host_efbig);
    qtest_add_func("/3590-ccw/io/unaligned-image", test_io_unaligned_image);
    qtest_add_func("/3590-ccw/io/migration-positioned",
                   test_io_migration_positioned);
    qtest_add_func("/3590-ccw/io/volume-switch", test_io_volume_switch);
    qtest_add_func("/3590-ccw/io/rewind-unload-3590",
                   test_io_rewind_unload_3590);
    qtest_add_func("/3590-ccw/io/rewind-unload-3490",
                   test_io_rewind_unload_3490);
    qtest_add_func("/3590-ccw/io/sense-3490", test_io_sense_3490);
    qtest_add_func("/3590-ccw/io/media-perm-from-ro",
                   test_io_media_perm_from_ro);
    qtest_add_func("/3590-ccw/io/media-perm-from-rw",
                   test_io_media_perm_from_rw);
    qtest_add_func("/3590-ccw/io/media-perm-empty",
                   test_io_media_perm_empty);
    qtest_add_func("/3590-ccw/io/perf", test_io_perf);
    return g_test_run();
}
