/*
 * QTest testcase for emulated 3590 CCW tape drives
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/3590-ccw/dev3590-shortcut", test_dev3590_shortcut);
    qtest_add_func("/3590-ccw/auto-devnos", test_auto_devnos);
    qtest_add_func("/3590-ccw/runtime-media-change",
                   test_runtime_media_change);
    qtest_add_func("/3590-ccw/identity-whitelist",
                   test_identity_whitelist);
    return g_test_run();
}
