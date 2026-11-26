/*
 * Tests for s390x operator-style IPL device selection
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qbool.h"
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
        "-blockdev driver=null-co,node-name=d,size=1048576 "
        "-device fba-ccw,drive=d,devno=fe.0.1000 "
        "-ipl 1000 -loadparm ab.1");

    assert_machine_string(qts, "ipl", "1000");
    assert_machine_string(qts, "loadparm", "AB.1    ");
    qtest_quit(qts);
}

static void test_ipl_full_address(void)
{
    QTestState *qts = qtest_init(
        "-nodefaults "
        "-blockdev driver=null-co,node-name=d0,size=1048576 "
        "-blockdev driver=null-co,node-name=d1,size=1048576 "
        "-device fba-ccw,drive=d0,devno=fe.0.1000 "
        "-device fba-ccw,drive=d1,devno=fe.1.1000 "
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

static void assert_device_string(QTestState *qts, const char *path,
                                 const char *property, const char *expected)
{
    QDict *response = qtest_qmp(
        qts, "{'execute': 'qom-get', 'arguments': "
             " {'path': %s, 'property': %s}}", path, property);

    g_assert(qdict_haskey(response, "return"));
    g_assert_cmpstr(qdict_get_str(response, "return"), ==, expected);
    qobject_unref(response);
}

/*
 * A device loadparm on the -ipl device takes precedence over the machine
 * loadparm and becomes the machine loadparm (reported through SCLP).
 */
static void test_ipl_device_loadparm(void)
{
    QTestState *qts = qtest_init(
        "-nodefaults "
        "-blockdev driver=null-co,node-name=d0,size=1048576 "
        "-blockdev driver=null-co,node-name=d1,size=1048576 "
        "-device fba-ccw,drive=d0,devno=fe.0.1000,id=ipl0,loadparm=sys1 "
        "-device fba-ccw,drive=d1,devno=fe.0.1001 "
        "-ipl 1000 -loadparm other");

    assert_device_string(qts, "/machine/peripheral/ipl0", "loadparm",
                         "SYS1    ");
    assert_machine_string(qts, "loadparm", "SYS1    ");
    qtest_quit(qts);

    /* Without a device loadparm, the machine loadparm applies. */
    qts = qtest_init(
        "-nodefaults "
        "-blockdev driver=null-co,node-name=d0,size=1048576 "
        "-device fba-ccw,drive=d0,devno=fe.0.1000 "
        "-ipl 1000 -loadparm other");
    assert_machine_string(qts, "loadparm", "OTHER   ");
    qtest_quit(qts);
}

/* The order of loadparm and bootindex on -device does not matter. */
static void test_bootindex_loadparm_order(void)
{
    QTestState *qts = qtest_init(
        "-nodefaults "
        "-drive if=none,id=d0,file=null-co://,format=raw "
        "-device virtio-blk-ccw,drive=d0,devno=fe.0.0100,id=vd0,"
        "loadparm=2,bootindex=1");

    assert_device_string(qts, "/machine/peripheral/vd0", "loadparm",
                         "2       ");
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
    const char *const virtio[] = {
        "-drive", "if=none,id=d,file=null-co://,format=raw",
        "-device", "virtio-blk-ccw,drive=d,devno=fe.0.1000",
        "-ipl", "1000", NULL
    };
    const char *const virtio_unsupported[] = {
        "-device", "virtio-rng-ccw,devno=fe.0.1000",
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
    const char *const loadparm_not_ipl[] = {
        "-blockdev", "driver=null-co,node-name=d0,size=1048576",
        "-blockdev", "driver=null-co,node-name=d1,size=1048576",
        "-device", "fba-ccw,drive=d0,devno=fe.0.1000",
        "-device", "fba-ccw,drive=d1,devno=fe.0.1001,loadparm=x",
        "-ipl", "1000", NULL
    };
    const char *const loadparm_no_bootindex[] = {
        "-drive", "if=none,id=d,file=null-co://,format=raw",
        "-device", "virtio-blk-ccw,drive=d,devno=fe.0.1000,loadparm=x",
        NULL
    };

    assert_qemu_fails(invalid, "Invalid IPL device address");
    assert_qemu_fails(missing, "IPL device 1000 was not found");
    assert_qemu_fails(unsupported, "does not support IPL");
    assert_qemu_fails(bootindex, "cannot be combined with a bootindex");
    assert_qemu_fails(virtio, "virtio device 1000 does not support; "
                      "use bootindex instead");
    assert_qemu_fails(virtio_unsupported, "does not support IPL");
    assert_qemu_fails(ambiguous, "is ambiguous");
    assert_qemu_fails(kernel, "-ipl and -kernel cannot be used together");
    assert_qemu_fails(loadparm_not_ipl,
                      "loadparm is only valid for boot devices (a bootindex, "
                      "or the -ipl device); CCW device fe.0.1001 has neither");
    assert_qemu_fails(loadparm_no_bootindex,
                      "loadparm is only valid for boot devices");
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

/* Find a property of a device type, as device-list-properties reports it */
static QDict *find_device_property(QTestState *qts, const char *type,
                                   const char *name)
{
    QDict *response = qtest_qmp(
        qts, "{'execute': 'device-list-properties', "
             " 'arguments': {'typename': %s}}", type);
    QDict *found = NULL;
    const QListEntry *entry;

    g_assert(qdict_haskey(response, "return"));
    QLIST_FOREACH_ENTRY(qdict_get_qlist(response, "return"), entry) {
        QDict *prop = qobject_to(QDict, qlist_entry_obj(entry));

        if (g_str_equal(qdict_get_str(prop, "name"), name)) {
            found = qobject_ref(prop);
            break;
        }
    }
    qobject_unref(response);
    return found;
}

static void test_ipl_properties(void)
{
    static const struct {
        const char *type;
        bool iplable;
        const char *method;
    } cases[] = {
        { "eckd-ccw", true, "ccw" },
        { "fba-ccw", true, "ccw" },
        { "3590-ccw", true, "ccw" },
        { "vfio-ccw", true, "ccw" },
        { "virtio-blk-ccw", true, "firmware" },
        { "virtio-net-ccw", true, "firmware" },
        { "scsi-hd", true, "firmware" },
        { "scsi-cd", true, "firmware" },
        { "virtio-scsi-ccw", false, "none" },
        { "virtio-rng-ccw", false, "none" },
        { "qeth-ccw", false, "none" },
        { "x-terminal3270", false, "none" },
    };
    /* The device types are introspected without a machine or any device */
    QTestState *qts = qtest_init("-machine none");

    for (int i = 0; i < ARRAY_SIZE(cases); i++) {
        QDict *prop;

        if (!qtest_has_device(cases[i].type)) {
            continue;
        }

        prop = find_device_property(qts, cases[i].type, "iplable");
        g_assert_nonnull(prop);
        g_assert_cmpstr(qdict_get_str(prop, "type"), ==, "bool");
        g_assert_cmpint(qdict_get_bool(prop, "default-value"), ==,
                        cases[i].iplable);
        qobject_unref(prop);

        prop = find_device_property(qts, cases[i].type, "ipl-method");
        g_assert_nonnull(prop);
        g_assert_cmpstr(qdict_get_str(prop, "type"), ==, "string");
        g_assert_cmpstr(qdict_get_str(prop, "default-value"), ==,
                        cases[i].method);
        qobject_unref(prop);
    }
    qtest_quit(qts);
}

/* The PSW address of CPU 0, from "info registers". */
static uint64_t cpu0_psw_addr(QTestState *qts)
{
    g_autofree char *regs = qtest_hmp(qts, "info registers");
    const char *addr = strstr(regs, " addr ");

    g_assert_nonnull(addr);
    return g_ascii_strtoull(addr + 6, NULL, 16);
}

static void wait_psw_addr(QTestState *qts, uint64_t expected)
{
    for (int i = 0; i < 500 && cpu0_psw_addr(qts) != expected; i++) {
        g_usleep(10000);
    }
    g_assert_cmphex(cpu0_psw_addr(qts), ==, expected);
}

/*
 * IPL record of the channel-program IPL tests: the PSW (ESA/390 format,
 * 31-bit, starting at 0x18), a Read CCW at 8 that reads the code to 0x18,
 * and the code itself, "j .", which the IPL then executes.
 */
static const uint8_t ipl_psw_ccw[16] = {
    0x00, 0x08, 0x00, 0x00, 0x80, 0x00, 0x00, 0x18,
    0x00, 0x00, 0x00, 0x18, 0x20, 0x00, 0x00, 0x04,
};
static const uint8_t ipl_loop[4] = { 0xa7, 0xf4, 0x00, 0x00 };

/*
 * fba-ccw reads asynchronously: the IPL must wait for the end of its
 * channel program.  After Read IPL, the Read continues in block 0 at the
 * byte after the 24 bytes Read IPL transferred.
 */
static void test_ipl_fba_program(void)
{
    uint8_t block[512] = { 0 };
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("qemu-ipl-fba-XXXXXX.raw", &path, NULL);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    memcpy(block, ipl_psw_ccw, sizeof(ipl_psw_ccw));
    block[8] = 0x42;                    /* Read */
    memcpy(block + 0x18, ipl_loop, sizeof(ipl_loop));
    g_assert_cmpint(write(fd, block, sizeof(block)), ==, sizeof(block));
    g_assert_cmpint(ftruncate(fd, 1024 * 1024), ==, 0);
    close(fd);

    qts = qtest_initf("-nodefaults -accel kvm "
                      "-drive if=none,id=fba,file=%s,format=raw "
                      "-device fba-ccw,drive=fba,devno=fe.0.0200 -ipl 200",
                      path);
    wait_psw_addr(qts, 0x18);
    qtest_quit(qts);
    unlink(path);
}

/*
 * eckd-ccw: Read IPL reads R1 of track 0, the Read Data at 8 the next
 * record (R2), which holds the code.
 */
static void test_ipl_eckd_program(void)
{
    uint8_t header[512] = { 0 };
    uint8_t track[5 + 16 + 8 + 24 + 8 + 4 + 8] = { 0 };
    uint8_t *r1 = track + 5 + 16, *r2 = r1 + 8 + 24;
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("qemu-ipl-eckd-XXXXXX.ckd", &path, NULL);
    QTestState *qts;

    g_assert_cmpint(fd, >=, 0);
    memcpy(header, "CKD_P064", 8);
    stl_le_p(header + 8, 15);
    stl_le_p(header + 12, 56832);
    header[16] = 0x90;
    stw_le_p(header + 18, 1);
    stw_be_p(track + 5 + 6, 8);         /* R0 */
    r1[4] = 1;
    stw_be_p(r1 + 6, 24);
    memcpy(r1 + 8, ipl_psw_ccw, sizeof(ipl_psw_ccw));
    r1[8 + 8] = 0x06;                   /* Read Data */
    r2[4] = 2;
    stw_be_p(r2 + 6, 4);
    memcpy(r2 + 8, ipl_loop, sizeof(ipl_loop));
    memset(r2 + 8 + 4, 0xff, 8);
    g_assert_cmpint(write(fd, header, sizeof(header)), ==, sizeof(header));
    g_assert_cmpint(write(fd, track, sizeof(track)), ==, sizeof(track));
    g_assert_cmpint(ftruncate(fd, 512 + 15 * 56832), ==, 0);
    close(fd);

    qts = qtest_initf("-nodefaults -accel kvm "
                      "-drive if=none,id=ckd,file=%s,format=raw "
                      "-device eckd-ccw,drive=ckd,devno=fe.0.0300 -ipl 300",
                      path);
    wait_psw_addr(qts, 0x18);
    qtest_quit(qts);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/s390x/ipl/short-address", test_ipl_short_address);
    qtest_add_func("/s390x/ipl/full-address", test_ipl_full_address);
    qtest_add_func("/s390x/ipl/fba", test_ipl_fba);
    qtest_add_func("/s390x/ipl/device-loadparm", test_ipl_device_loadparm);
    qtest_add_func("/s390x/ipl/bootindex-loadparm-order",
                   test_bootindex_loadparm_order);
    qtest_add_func("/s390x/ipl/errors", test_ipl_errors);
    qtest_add_func("/s390x/ipl/ins-list-load", test_ins_list_load);
    qtest_add_func("/s390x/ipl/properties", test_ipl_properties);
    /* The IPL programs run under KVM only (this tree does not use TCG). */
    if (qtest_has_accel("kvm") && !access("/dev/kvm", R_OK | W_OK)) {
        qtest_add_func("/s390x/ipl/fba-program", test_ipl_fba_program);
        qtest_add_func("/s390x/ipl/eckd-program", test_ipl_eckd_program);
    }
    return g_test_run();
}
