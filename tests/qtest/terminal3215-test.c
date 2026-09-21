/*
 * QTest smoke tests for the IBM 3215 CCW console.
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

static void test_multiple_terminals(void)
{
    g_autofree char *dev_id0 = NULL;
    g_autofree char *dev_id1 = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults "
        "-chardev ringbuf,id=termchar0,size=4096 "
        "-chardev ringbuf,id=termchar1,size=4096 "
        "-device 3215-ccw,id=term0,chardev=termchar0,devno=fe.0.0009 "
        "-device 3215-ccw,id=term1,chardev=termchar1,devno=fe.0.000a");
    dev_id0 = qom_get_string(qts, "/machine/peripheral/term0", "dev_id");
    dev_id1 = qom_get_string(qts, "/machine/peripheral/term1", "dev_id");
    g_assert_cmpstr(dev_id0, ==, "fe.0.0009");
    g_assert_cmpstr(dev_id1, ==, "fe.0.000a");
    qtest_quit(qts);
}

static void test_dev3215_shortcut(void)
{
    g_autofree char *dev_id = NULL;
    QTestState *qts;

    qts = qtest_init(
        "-nodefaults -chardev ringbuf,id=operator,size=4096 "
        "-dev3215 chardev=operator,devno=0009,id=console");
    dev_id = qom_get_string(qts, "/machine/peripheral/console", "dev_id");
    g_assert_cmpstr(dev_id, ==, "fe.0.0009");
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/terminal3215/multiple", test_multiple_terminals);
    qtest_add_func("/terminal3215/dev3215-shortcut",
                   test_dev3215_shortcut);
    return g_test_run();
}
