/*
 * Tests for s390x operator-style IPL device selection
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static void assert_machine_string(QTestState *qts, const char *property,
                                  const char *expected)
{
    QDict *response = qtest_qmp(
        qts, "{'execute': 'qom-get', 'arguments': "
             " {'path': '/machine', 'property': %s}}", property);

    g_assert(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, expected);
    qobject_unref(response);
}

static void test_ipl_short_address(void)
{
    QTestState *qts = qtest_init(
        "-nodefaults "
        "-drive if=none,id=d,file=null-co://,format=raw "
        "-device virtio-blk-ccw,drive=d,devno=fe.0.1000 "
        "-ipl 1000 -loadparm ab.1");

    assert_machine_string(qts, "ipl", "1000");
    assert_machine_string(qts, "loadparm", "AB.1    ");
    qtest_quit(qts);
}

static void test_ipl_full_address(void)
{
    QTestState *qts = qtest_init(
        "-nodefaults "
        "-drive if=none,id=d0,file=null-co://,format=raw "
        "-drive if=none,id=d1,file=null-co://,format=raw "
        "-device virtio-blk-ccw,drive=d0,devno=fe.0.1000 "
        "-device virtio-blk-ccw,drive=d1,devno=fe.1.1000 "
        "-ipl fe.0.1000");

    assert_machine_string(qts, "ipl", "fe.0.1000");
    qtest_quit(qts);
}

static void assert_qemu_fails(const char *const extra_argv[],
                              const char *message)
{
    const char *binary = qtest_qemu_binary(NULL);
    g_autofree char *stderr_data = NULL;
    g_autoptr(GPtrArray) argv = g_ptr_array_new();
    int status;
    bool spawned;

    g_ptr_array_add(argv, (void *)binary);
    g_ptr_array_add(argv, (void *)"-nodefaults");
    g_ptr_array_add(argv, (void *)"-display");
    g_ptr_array_add(argv, (void *)"none");
    for (const char *const *arg = extra_argv; *arg; arg++) {
        g_ptr_array_add(argv, (void *)*arg);
    }
    g_ptr_array_add(argv, NULL);

    spawned = g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                           G_SPAWN_STDOUT_TO_DEV_NULL,
                           NULL, NULL, NULL, &stderr_data, &status, NULL);
    g_assert_true(spawned);
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    g_assert_nonnull(strstr(stderr_data, message));
}

static void test_ipl_errors(void)
{
    const char *const invalid[] = { "-ipl", "xyz", NULL };
    const char *const missing[] = { "-ipl", "1000", NULL };
    const char *const unsupported[] = {
        "-device", "x-terminal3270,devno=fe.0.1000",
        "-ipl", "1000", NULL
    };
    const char *const bootindex[] = {
        "-drive", "if=none,id=d,file=null-co://,format=raw",
        "-device", "virtio-blk-ccw,drive=d,devno=fe.0.1000,bootindex=1",
        "-ipl", "1000", NULL
    };
    const char *const ambiguous[] = {
        "-drive", "if=none,id=d0,file=null-co://,format=raw",
        "-drive", "if=none,id=d1,file=null-co://,format=raw",
        "-device", "virtio-blk-ccw,drive=d0,devno=fe.0.1000",
        "-device", "virtio-blk-ccw,drive=d1,devno=fe.1.1000",
        "-ipl", "1000", NULL
    };
    const char *const kernel[] = {
        "-ipl", "1000", "-kernel", "does-not-matter", NULL
    };

    assert_qemu_fails(invalid, "Invalid IPL device address");
    assert_qemu_fails(missing, "IPL device 1000 was not found");
    assert_qemu_fails(unsupported, "does not support IPL");
    assert_qemu_fails(bootindex, "cannot be combined with a bootindex");
    assert_qemu_fails(ambiguous, "is ambiguous");
    assert_qemu_fails(kernel, "-ipl and -kernel cannot be used together");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/s390x/ipl/short-address", test_ipl_short_address);
    qtest_add_func("/s390x/ipl/full-address", test_ipl_full_address);
    qtest_add_func("/s390x/ipl/errors", test_ipl_errors);
    return g_test_run();
}
