/*
 * QTest testcase for emulated QETH CCW network adapters
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

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

static void test_auto_configuration(void)
{
    g_autofree char *dev_id = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults -netdev user,id=net0 "
        "-device qeth-ccw,id=qeth0,netdev=net0");
    dev_id = qom_get_string(qts, "/machine/peripheral/qeth0", "dev_id");
    g_assert_cmpstr(dev_id, ==, "fe.0.0000");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/qeth-ccw/configuration", test_configuration);
    qtest_add_func("/qeth-ccw/auto-configuration", test_auto_configuration);
    return g_test_run();
}
