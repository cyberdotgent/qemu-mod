/*
 * Tests for s390x operator-style IPL device selection
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include <glib/gstdio.h>

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

static void test_ipl_fba(void)
{
    QTestState *qts = qtest_init(
        "-nodefaults "
        "-blockdev driver=null-co,node-name=fba,size=1048576 "
        "-device fba-ccw,drive=fba,devno=fe.0.0200 "
        "-ipl 200");

    assert_machine_string(qts, "ipl", "0200");
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

static void test_ins_list_load(void)
{
    const uint8_t nucleus[] = {
        0x00, 0x08, 0x00, 0x00, 0x80, 0x00, 0x01, 0x00,
        0x11, 0x22, 0x33, 0x44,
    };
    const uint8_t ramdisk[] = { 0xaa, 0xbb, 0xcc, 0xdd };
    const char control[] =
        "* list-directed load test\n"
        "NUCLEUS 0x00000000\n"
        "RAMDISK 0x00001000\n";
    g_autofree char *directory = g_dir_make_tmp("qemu-s390-ins-XXXXXX", NULL);
    g_autofree char *nucleus_path =
        g_build_filename(directory, "NUCLEUS", NULL);
    g_autofree char *ramdisk_path =
        g_build_filename(directory, "RAMDISK", NULL);
    g_autofree char *ins_path = g_build_filename(directory, "BOOT.INS", NULL);
    uint8_t actual[sizeof(nucleus)];
    QTestState *qts;

    g_assert_true(g_file_set_contents(nucleus_path, (const char *)nucleus,
                                      sizeof(nucleus), NULL));
    g_assert_true(g_file_set_contents(ramdisk_path, (const char *)ramdisk,
                                      sizeof(ramdisk), NULL));
    g_assert_true(g_file_set_contents(ins_path, control, -1, NULL));

    qts = qtest_initf("-nodefaults -S -kernel %s", ins_path);
    qtest_memread(qts, 0, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), nucleus, sizeof(nucleus));
    qtest_memread(qts, 0x1000, actual, sizeof(ramdisk));
    g_assert_cmpmem(actual, sizeof(ramdisk), ramdisk, sizeof(ramdisk));
    qtest_quit(qts);

    g_assert_cmpint(g_remove(ins_path), ==, 0);
    g_assert_cmpint(g_remove(ramdisk_path), ==, 0);
    g_assert_cmpint(g_remove(nucleus_path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/s390x/ipl/short-address", test_ipl_short_address);
    qtest_add_func("/s390x/ipl/full-address", test_ipl_full_address);
    qtest_add_func("/s390x/ipl/fba", test_ipl_fba);
    qtest_add_func("/s390x/ipl/errors", test_ipl_errors);
    qtest_add_func("/s390x/ipl/ins-list-load", test_ins_list_load);
    return g_test_run();
}
