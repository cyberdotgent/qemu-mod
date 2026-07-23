/*
 * QTest testcase for emulated 2107/3390 ECKD disks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"

#define CKD_HEADER_SIZE 512
#define CKD_TRACK_SIZE 56832
#define CKD_HEADS 15

static char *create_ckd64_image(void)
{
    uint8_t header[CKD_HEADER_SIZE] = { 0 };
    uint8_t track[13] = { 0 };
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-eckd-XXXXXX.ckd64", &path, NULL);
    unsigned int head;

    g_assert_cmpint(fd, >=, 0);
    memcpy(header, "CKD_P064", 8);
    stl_le_p(header + 8, CKD_HEADS);
    stl_le_p(header + 12, CKD_TRACK_SIZE);
    header[16] = 0x90;
    memcpy(header + 20, "123456789012", 12);
    g_assert_cmpint(write(fd, header, sizeof(header)), ==, sizeof(header));

    for (head = 0; head < CKD_HEADS; head++) {
        int64_t offset = CKD_HEADER_SIZE + (int64_t)head * CKD_TRACK_SIZE;

        memset(track, 0, sizeof(track));
        stw_be_p(track + 3, head);
        memset(track + 5, 0xff, 8);
        g_assert_cmpint(pwrite(fd, track, sizeof(track), offset), ==,
                        sizeof(track));
    }
    g_assert_cmpint(ftruncate(fd, CKD_HEADER_SIZE +
                             CKD_HEADS * CKD_TRACK_SIZE), ==, 0);
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

static void test_dev3390_shortcut(void)
{
    g_autofree char *path = create_ckd64_image();
    g_autofree char *dev_id = NULL;
    QTestState *qts = qtest_initf(
        "-nodefaults -dev3390 file=%s,devno=200,id=dasd0 -S", path);

    dev_id = qom_get_string(qts, "/machine/peripheral/dasd0", "dev_id");
    g_assert_cmpstr(dev_id, ==, "fe.0.0200");
    qtest_quit(qts);
    unlink(path);
}

static void test_automatic_addresses(void)
{
    g_autofree char *path0 = create_ckd64_image();
    g_autofree char *path1 = create_ckd64_image();
    g_autofree char *dev_id0 = NULL;
    g_autofree char *dev_id1 = NULL;
    QTestState *qts = qtest_initf(
        "-nodefaults "
        "-dev3390 file=%s,id=dasd0 "
        "-dev3390 file=%s,id=dasd1 -S", path0, path1);

    dev_id0 = qom_get_string(qts, "/machine/peripheral/dasd0", "dev_id");
    dev_id1 = qom_get_string(qts, "/machine/peripheral/dasd1", "dev_id");
    g_assert_cmpstr(dev_id0, ==, "fe.0.0200");
    g_assert_cmpstr(dev_id1, ==, "fe.0.0201");
    qtest_quit(qts);
    unlink(path0);
    unlink(path1);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/eckd-ccw/dev3390-shortcut", test_dev3390_shortcut);
    qtest_add_func("/eckd-ccw/automatic-addresses", test_automatic_addresses);
    return g_test_run();
}
