/*
 * QTest testcase for the s390x storage keys across snapshots
 *
 * A tiny guest program, IPLed as a raw kernel image under TCG, waits for the
 * test.  Each time the test sets the GO byte, it sets the storage key KEY on
 * the pages from START to END (SET STORAGE KEY EXTENDED) and sets DONE.  The
 * keys are read with the dump-skeys monitor command.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define PROG_CODE   0x200
#define PROG_KEY    0xc00
#define PROG_START  0xc04
#define PROG_END    0xc08
#define PROG_GO     0xc10
#define PROG_DONE   0xc11
#define PROG_READY  0xc12
#define PROG_IMAGE  0xc20

#define FIRST_PAGE  0x100       /* the keyed range: 1 MiB to 2 MiB */
#define LAST_PAGE   0x1ff

#define KEY_ACC_F   0xf8
#define KEY_R       0x04
#define KEY_C       0x02

static char *create_image(void)
{
    static const uint8_t code[] = {
        0x92, 0x01, 0x0c, 0x12,     /* 200 mvi   READY,1         */
        0x95, 0x00, 0x0c, 0x10,     /* 204 cli   GO,0            */
        0xa7, 0x84, 0xff, 0xfe,     /* 208 je    204             */
        0x92, 0x00, 0x0c, 0x10,     /* 20c mvi   GO,0            */
        0x58, 0x20, 0x0c, 0x00,     /* 210 l     %r2,KEY         */
        0x58, 0x30, 0x0c, 0x04,     /* 214 l     %r3,START       */
        0x58, 0x40, 0x0c, 0x08,     /* 218 l     %r4,END         */
        0xb2, 0x2b, 0x00, 0x23,     /* 21c sske  %r2,%r3         */
        0xa7, 0x3a, 0x10, 0x00,     /* 220 ahi   %r3,4096        */
        0x15, 0x34,                 /* 224 clr   %r3,%r4         */
        0xa7, 0x44, 0xff, 0xfb,     /* 226 jl    21c             */
        0x92, 0x01, 0x0c, 0x11,     /* 22a mvi   DONE,1          */
        0xa7, 0xf4, 0xff, 0xeb,     /* 22e j     204             */
    };
    g_autofree uint8_t *image = g_malloc0(PROG_IMAGE);
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-skeys-XXXXXX.img", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    /* Short IPL PSW: 31-bit addressing, start at PROG_CODE. */
    stl_be_p(image, 0x00080000);
    stl_be_p(image + 4, 0x80000000 | PROG_CODE);
    memcpy(image + PROG_CODE, code, sizeof(code));
    g_assert_cmpint(write(fd, image, PROG_IMAGE), ==, PROG_IMAGE);
    close(fd);
    return path;
}

static char *create_snapshot_image(void)
{
    const char *qemu_img = g_getenv("QTEST_QEMU_IMG");
    g_autofree char *cmd = NULL;
    char *path = NULL;
    int fd;

    if (!qemu_img) {
        return NULL;
    }
    fd = g_file_open_tmp("qemu-skeys-XXXXXX.qcow2", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    cmd = g_strdup_printf("%s create -q -f qcow2 %s 16M", qemu_img, path);
    g_assert_cmpint(system(cmd), ==, 0);
    return path;
}

static void wait_byte(QTestState *qts, uint64_t addr)
{
    int i;

    for (i = 0; i < 200000 && !qtest_readb(qts, addr); i++) {
        g_usleep(50);
    }
    g_assert_cmpint(qtest_readb(qts, addr), ==, 1);
}

/* Let the guest set @key on the keyed range. */
static void guest_set_keys(QTestState *qts, uint8_t key)
{
    uint8_t parm[12];

    stl_be_p(parm, key);
    stl_be_p(parm + 4, FIRST_PAGE << 12);
    stl_be_p(parm + 8, (LAST_PAGE + 1) << 12);
    qtest_memwrite(qts, PROG_KEY, parm, sizeof(parm));
    qtest_writeb(qts, PROG_DONE, 0);
    qtest_writeb(qts, PROG_GO, 1);
    wait_byte(qts, PROG_DONE);
}

/*
 * The keys of the keyed range from dump-skeys into @keys; false if the
 * guest does not use storage keys.
 */
static bool dump_keys(QTestState *qts, uint8_t *keys)
{
    g_autofree char *path = NULL;
    g_autofree char *out = NULL;
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    int fd = g_file_open_tmp("qemu-skeys-XXXXXX.txt", &path, NULL);
    int i, found = 0;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    out = qtest_hmp(qts, "dump-skeys %s", path);
    if (strstr(out, "not using storage keys")) {
        unlink(path);
        return false;
    }
    g_assert_cmpstr(out, ==, "");
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    unlink(path);
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i]; i++) {
        unsigned int page, key;

        if (sscanf(lines[i], "page=%x: key(%u)", &page, &key) == 2 &&
            page >= FIRST_PAGE && page <= LAST_PAGE) {
            keys[page - FIRST_PAGE] = key;
            found++;
        }
    }
    g_assert_cmpint(found, ==, LAST_PAGE - FIRST_PAGE + 1);
    return true;
}

/* ACC and F of the keyed range are @acc_f; C is never under-indicated. */
static void assert_keys(QTestState *qts, uint8_t acc_f)
{
    uint8_t keys[LAST_PAGE - FIRST_PAGE + 1];
    int i;

    g_assert_true(dump_keys(qts, keys));
    for (i = 0; i < ARRAY_SIZE(keys); i++) {
        g_assert_cmphex(keys[i] & KEY_ACC_F, ==, acc_f);
        if (!acc_f) {
            /* reset keys: R and C over-indicated, as for unused keys */
            g_assert_cmphex(keys[i] & (KEY_R | KEY_C), ==, KEY_R | KEY_C);
        }
    }
}

static void hmp_ok(QTestState *qts, const char *cmd)
{
    g_autofree char *out = qtest_hmp(qts, "%s", cmd);

    g_assert_cmpstr(out, ==, "");
}

/*
 * loadvm restores the keys of the snapshot: the keys set after it are
 * gone, also when the guest did not use storage keys yet at savevm (the
 * stream then carries no keys at all).
 */
static void test_snapshot(void)
{
    g_autofree char *image = create_image();
    g_autofree char *disk = create_snapshot_image();
    uint8_t keys[LAST_PAGE - FIRST_PAGE + 1];
    QTestState *qts;

    if (!disk) {
        g_test_skip("QTEST_QEMU_IMG not set");
        unlink(image);
        return;
    }
    qts = qtest_initf("-nodefaults -accel tcg -m 32 -kernel %s "
                      "-drive if=none,id=snap,file=%s,format=qcow2",
                      image, disk);
    wait_byte(qts, PROG_READY);
    g_assert_false(dump_keys(qts, keys));
    hmp_ok(qts, "savevm unused");

    guest_set_keys(qts, 0x38);
    assert_keys(qts, 0x38);
    hmp_ok(qts, "savevm keyed");

    guest_set_keys(qts, 0x50);
    assert_keys(qts, 0x50);
    hmp_ok(qts, "loadvm keyed");
    assert_keys(qts, 0x38);

    hmp_ok(qts, "loadvm unused");
    assert_keys(qts, 0);

    /* the guest runs on and keys again */
    guest_set_keys(qts, 0x68);
    assert_keys(qts, 0x68);
    hmp_ok(qts, "loadvm keyed");
    assert_keys(qts, 0x38);

    qtest_quit(qts);
    unlink(disk);
    unlink(image);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (qtest_has_accel("tcg")) {
        qtest_add_func("/s390x/skeys/snapshot", test_snapshot);
    }

    return g_test_run();
}
