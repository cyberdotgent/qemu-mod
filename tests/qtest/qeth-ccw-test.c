/*
 * QTest testcase for emulated QETH CCW network adapters
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

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

static int64_t qom_get_int(QTestState *qts, const char *path,
                           const char *property)
{
    QDict *response = qtest_qmp(
        qts,
        "{'execute': 'qom-get', 'arguments': "
        " {'path': %s, 'property': %s}}",
        path, property);
    int64_t value;

    g_assert(qdict_haskey(response, "return"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);
    return value;
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

/*
 * An adapter migrates (it used to be unmigratable); the destination must
 * have the same number of data devices.
 */
static char *qeth_migrate(unsigned int src_datadevs,
                          unsigned int dst_datadevs)
{
    g_autofree char *state = NULL;
    g_autofree char *status = NULL;
    QTestState *src, *dst;
    char *result;
    int fd = g_file_open_tmp("qeth-ccw-state-XXXXXX", &state, NULL);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    src = qtest_initf("-nodefaults -S -netdev user,id=net0 "
                      "-device qeth-ccw,id=qeth0,netdev=net0,devno=fe.0.0600,"
                      "data-devices=%u", src_datadevs);
    status = migrate_to_file(src, state);
    g_assert_cmpstr(status, ==, "completed");
    qtest_quit(src);

    dst = qtest_initf("-nodefaults -S -netdev user,id=net0 "
                      "-device qeth-ccw,id=qeth0,netdev=net0,devno=fe.0.0600,"
                      "data-devices=%u -incoming defer", dst_datadevs);
    result = migrate_from_file(dst, state);
    qtest_quit(dst);
    unlink(state);
    return result;
}

static void test_migration(void)
{
    g_autofree char *status = qeth_migrate(2, 2);

    g_assert_cmpstr(status, ==, "completed");
}

static void test_migration_mismatch(void)
{
    g_autofree char *status = qeth_migrate(2, 1);

    g_assert_cmpstr(status, ==, "failed");
}

static void test_configuration(void)
{
    g_autofree char *dev_id = NULL;
    g_autofree char *mac = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults -netdev user,id=net0 "
        "-device qeth-ccw,id=qeth0,netdev=net0,devno=fe.0.0600,"
        "mac=52:54:00:12:34:60");
    dev_id = qom_get_string(qts, "/machine/peripheral/qeth0", "dev_id");
    mac = qom_get_string(qts, "/machine/peripheral/qeth0", "mac");
    g_assert_cmpstr(dev_id, ==, "fe.0.0600");
    g_assert_cmpstr(mac, ==, "52:54:00:12:34:60");
    qtest_quit(qts);
}

static void test_qeth_shortcut(void)
{
    g_autofree char *dev_id = NULL;
    g_autofree char *mac = NULL;
    g_autofree char *netdev = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults -netdev user,id=net0 "
        "-qeth devno=0400,netdev=net0,mac=52:54:00:12:34:61,chpid=0xf0");
    dev_id = qom_get_string(qts, "/machine/peripheral/qeth0", "dev_id");
    mac = qom_get_string(qts, "/machine/peripheral/qeth0", "mac");
    netdev = qom_get_string(qts, "/machine/peripheral/qeth0", "netdev");
    g_assert_cmpstr(dev_id, ==, "fe.0.0400");
    g_assert_cmpstr(mac, ==, "52:54:00:12:34:61");
    g_assert_cmpstr(netdev, ==, "net0");
    g_assert_cmpint(qom_get_int(qts, "/machine/peripheral/qeth0", "chpid"),
                    ==, 0xf0);
    qtest_quit(qts);
}

static void test_data_devices(void)
{
    g_autofree char *dev_id = NULL;
    QTestState *qts;

    /* Two data devices occupy 0400-0403; the next adapter starts at 0404 */
    qts = qtest_init(
        "-nodefaults -netdev user,id=net0 -netdev user,id=net1 "
        "-qeth netdev=net0,datadevs=2 "
        "-device qeth-ccw,id=qeth1,netdev=net1");
    g_assert_cmpint(qom_get_int(qts, "/machine/peripheral/qeth0",
                                "data-devices"), ==, 2);
    dev_id = qom_get_string(qts, "/machine/peripheral/qeth1", "dev_id");
    g_assert_cmpstr(dev_id, ==, "fe.0.0404");
    qtest_quit(qts);
}

static void test_readconfig(void)
{
    static const char config[] =
        "[netdev \"net0\"]\n"
        "  type = \"user\"\n"
        "[qeth]\n"
        "  devno = \"0500\"\n"
        "  netdev = \"net0\"\n"
        "  mac = \"52:54:00:12:34:62\"\n"
        "  datadevs = \"2\"\n";
    g_autofree char *path = NULL;
    g_autofree char *dev_id = NULL;
    g_autofree char *mac = NULL;
    g_autofree char *args = NULL;
    g_autoptr(GError) err = NULL;
    QTestState *qts;
    int fd;

    /* The -qeth shorthand as a config file section */
    fd = g_file_open_tmp("qeth-ccw-test-XXXXXX.conf", &path, &err);
    g_assert_no_error(err);
    g_assert_true(g_file_set_contents(path, config, -1, &err));
    g_assert_no_error(err);
    close(fd);

    args = g_strdup_printf("-nodefaults -readconfig %s", path);
    qts = qtest_init(args);
    dev_id = qom_get_string(qts, "/machine/peripheral/qeth0", "dev_id");
    mac = qom_get_string(qts, "/machine/peripheral/qeth0", "mac");
    g_assert_cmpstr(dev_id, ==, "fe.0.0500");
    g_assert_cmpstr(mac, ==, "52:54:00:12:34:62");
    g_assert_cmpint(qom_get_int(qts, "/machine/peripheral/qeth0",
                                "data-devices"), ==, 2);
    qtest_quit(qts);
    unlink(path);
}

static void test_auto_configuration(void)
{
    g_autofree char *dev_id0 = NULL;
    g_autofree char *dev_id1 = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults -netdev user,id=net0 -netdev user,id=net1 "
        "-device qeth-ccw,id=qeth0,netdev=net0 "
        "-device qeth-ccw,id=qeth1,netdev=net1");
    dev_id0 = qom_get_string(qts, "/machine/peripheral/qeth0", "dev_id");
    dev_id1 = qom_get_string(qts, "/machine/peripheral/qeth1", "dev_id");
    g_assert_cmpstr(dev_id0, ==, "fe.0.0400");
    g_assert_cmpstr(dev_id1, ==, "fe.0.0403");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/qeth-ccw/configuration", test_configuration);
    qtest_add_func("/qeth-ccw/auto-configuration", test_auto_configuration);
    qtest_add_func("/qeth-ccw/qeth-shortcut", test_qeth_shortcut);
    qtest_add_func("/qeth-ccw/data-devices", test_data_devices);
    qtest_add_func("/qeth-ccw/readconfig", test_readconfig);
    qtest_add_func("/qeth-ccw/migration", test_migration);
    qtest_add_func("/qeth-ccw/migration-mismatch", test_migration_mismatch);
    return g_test_run();
}
