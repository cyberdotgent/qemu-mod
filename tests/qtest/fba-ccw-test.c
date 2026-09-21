/*
 * QTest testcase for emulated FBA CCW disks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

static char *create_image(void)
{
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-fba-XXXXXX.raw", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1024 * 1024), ==, 0);
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

static void test_full_configuration(void)
{
    g_autofree char *path = create_image();
    g_autofree char *dev_id = NULL;
    QTestState *qts;

    qts = qtest_initf(
        "-nodefaults -drive if=none,id=fba0,file=%s,format=raw "
        "-device fba-ccw,id=disk0,drive=fba0,devno=fe.0.0200",
        path);
    dev_id = qom_get_string(qts, "/machine/peripheral/disk0", "dev_id");
    g_assert_cmpstr(dev_id, ==, "fe.0.0200");
    qtest_quit(qts);
    unlink(path);
}

static void test_dev9336_shortcut(void)
{
    g_autofree char *path0 = create_image();
    g_autofree char *path1 = create_image();
    g_autofree char *dev_id0 = NULL;
    g_autofree char *dev_id1 = NULL;
    QTestState *qts;

    qts = qtest_initf(
        "-nodefaults "
        "-dev9336 file=%s,devno=0200,id=sysres "
        "-dev9336 file=%s,devno=fe.0.0201,id=work01,readonly=on",
        path0, path1);
    dev_id0 = qom_get_string(qts, "/machine/peripheral/sysres", "dev_id");
    dev_id1 = qom_get_string(qts, "/machine/peripheral/work01", "dev_id");
    g_assert_cmpstr(dev_id0, ==, "fe.0.0200");
    g_assert_cmpstr(dev_id1, ==, "fe.0.0201");
    qtest_quit(qts);
    unlink(path0);
    unlink(path1);
}

static void test_auto_devnos(void)
{
    g_autofree char *path0 = create_image();
    g_autofree char *path1 = create_image();
    g_autofree char *dev_id0 = NULL;
    g_autofree char *dev_id1 = NULL;
    QTestState *qts;

    qts = qtest_initf(
        "-nodefaults -dev9336 file=%s,id=disk0 -dev9336 file=%s,id=disk1",
        path0, path1);
    dev_id0 = qom_get_string(qts, "/machine/peripheral/disk0", "dev_id");
    dev_id1 = qom_get_string(qts, "/machine/peripheral/disk1", "dev_id");
    g_assert_cmpstr(dev_id0, ==, "fe.0.0200");
    g_assert_cmpstr(dev_id1, ==, "fe.0.0201");
    qtest_quit(qts);
    unlink(path0);
    unlink(path1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/fba-ccw/full-configuration", test_full_configuration);
    qtest_add_func("/fba-ccw/dev9336-shortcut", test_dev9336_shortcut);
    qtest_add_func("/fba-ccw/auto-devnos", test_auto_devnos);
    return g_test_run();
}
