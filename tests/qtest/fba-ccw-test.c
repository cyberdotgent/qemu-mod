/*
 * QTest testcase for emulated FBA CCW disks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "s390x-ccw-prog.h"

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

#define FBA_TEST_BLOCKS 2048
#define GIB2 0x80000000u

/* Define Extent over the whole test volume, then Locate @count blocks. */
static void fba_set_extent_locate(QTestState *qts, uint8_t op,
                                  uint16_t count, uint32_t first)
{
    uint8_t de[16] = { 0 };
    uint8_t lo[8] = { 0 };

    stl_be_p(de + 12, FBA_TEST_BLOCKS - 1);
    lo[0] = op;
    stw_be_p(lo + 2, count);
    stl_be_p(lo + 4, first);
    qtest_memwrite(qts, CCW_PROG_DATA, de, sizeof(de));
    qtest_memwrite(qts, CCW_PROG_DATA + 0x10, lo, sizeof(lo));
    ccw_prog_set_ccw(qts, 0, 0x63, CCW_FLAG_CC, 16, CCW_PROG_DATA);
    ccw_prog_set_ccw(qts, 1, 0x43, CCW_FLAG_CC, 8, CCW_PROG_DATA + 0x10);
}

static void fill(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = seed + i * 7;
    }
}

/*
 * Data-chained writes and reads with buffers at the 2 GiB line, as Linux
 * dasd_fba builds them with more than 2 GiB of guest memory: a format-1 CCW
 * may address a buffer whose last byte is at 0x7fffffff directly, anything
 * above needs format-2 IDAWs.  Before the fix the CCW whose buffer ended
 * exactly at 2 GiB failed with a channel program check.
 */
static void test_io_2g_boundary(void)
{
    g_autofree char *path = create_image();
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    g_autofree gchar *contents = NULL;
    uint8_t pat[3][512], back[512], idaws[3][8];
    gsize len;
    CcwProgIrb irb;
    g_autofree char *devices = g_strdup_printf(
        "-drive if=none,id=fba0,file=%s,format=raw "
        "-device fba-ccw,drive=fba0,devno=fe.0.0200", path);
    QTestState *qts = ccw_prog_start(image, "3G", devices);

    fill(pat[0], 512, 0x11);
    fill(pat[1], 512, 0x22);
    fill(pat[2], 512, 0x33);
    /* Block 0: direct, last byte at 2 GiB - 1. */
    qtest_memwrite(qts, GIB2 - 512, pat[0], 512);
    /* Block 1: format-2 IDAW above 2 GiB. */
    qtest_memwrite(qts, GIB2 + 0x100, pat[1], 512);
    stq_be_p(idaws[0], GIB2 + 0x100);
    /* Block 2: two IDAWs, from below 2 GiB to above. */
    qtest_memwrite(qts, GIB2 - 0x1100, pat[2], 256);
    qtest_memwrite(qts, GIB2 + 0x3000, pat[2] + 256, 256);
    stq_be_p(idaws[1], GIB2 - 0x1100);
    stq_be_p(idaws[2], GIB2 + 0x3000);
    qtest_memwrite(qts, CCW_PROG_DATA + 0x100, idaws, sizeof(idaws));

    fba_set_extent_locate(qts, 0x01, 3, 0);
    ccw_prog_set_ccw(qts, 2, 0x41, CCW_FLAG_DC, 512, GIB2 - 512);
    ccw_prog_set_ccw(qts, 3, 0x41, CCW_FLAG_DC | CCW_FLAG_IDA, 512,
                     CCW_PROG_DATA + 0x100);
    ccw_prog_set_ccw(qts, 4, 0x41, CCW_FLAG_IDA, 512, CCW_PROG_DATA + 0x108);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    g_assert_cmphex(irb.cpa, ==, CCW_PROG_CCWS + 5 * 8);

    /* Read block 0 back into the buffer that ends at 2 GiB. */
    qtest_memset(qts, GIB2 - 512, 0, 512);
    fba_set_extent_locate(qts, 0x06, 1, 0);
    ccw_prog_set_ccw(qts, 2, 0x42, 0, 512, GIB2 - 512);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(qts, GIB2 - 512, back, 512);
    g_assert_cmpmem(back, 512, pat[0], 512);

    /* One byte beyond 2 GiB without IDAWs: channel program check. */
    fba_set_extent_locate(qts, 0x06, 1, 0);
    ccw_prog_set_ccw(qts, 2, 0x42, 0, 512, GIB2 - 511);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, CCW_CSTAT_PROG_CHECK);
    g_assert_cmphex(irb.cpa, ==, CCW_PROG_CCWS + 3 * 8);

    qtest_quit(qts);
    g_assert_true(g_file_get_contents(path, &contents, &len, NULL));
    g_assert_cmpuint(len, ==, FBA_TEST_BLOCKS * 512);
    g_assert_cmpmem(contents, 512, pat[0], 512);
    g_assert_cmpmem(contents + 512, 512, pat[1], 512);
    g_assert_cmpmem(contents + 1024, 512, pat[2], 512);
    unlink(path);
    unlink(image);
}

static QTestState *fba_prog_start_opts(const char *image, const char *path,
                                       const char *extra)
{
    g_autofree char *devices = g_strdup_printf(
        "-drive if=none,id=fba0,file=%s,format=raw "
        "-device fba-ccw,drive=fba0,devno=fe.0.0200%s", path, extra);

    return ccw_prog_start(image, "128M", devices);
}

static void fba_check_sense_id(QTestState *qts, uint16_t cu_type,
                               uint8_t cu_model, uint16_t dev_type,
                               uint8_t dev_model)
{
    uint8_t buf[7];
    CcwProgIrb irb;

    ccw_prog_set_ccw(qts, 0, 0xe4, CCW_FLAG_SLI, sizeof(buf), CCW_PROG_DATA);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(qts, CCW_PROG_DATA, buf, sizeof(buf));
    g_assert_cmphex(buf[0], ==, 0xff);
    g_assert_cmphex(lduw_be_p(buf + 1), ==, cu_type);
    g_assert_cmphex(buf[3], ==, cu_model);
    g_assert_cmphex(lduw_be_p(buf + 4), ==, dev_type);
    g_assert_cmphex(buf[6], ==, dev_model);
}

static void test_identity(void)
{
    g_autofree char *path = create_image();
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts;

    qts = fba_prog_start_opts(image, path, "");
    fba_check_sense_id(qts, 0x6310, 0x01, 0x9336, 0x10);
    qtest_quit(qts);

    qts = fba_prog_start_opts(image, path,
                              ",control-unit-type=0x3880,"
                              "control-unit-model=1,device-type=0x3370,"
                              "device-model=0x02");
    fba_check_sense_id(qts, 0x3880, 0x01, 0x3370, 0x02);
    qtest_quit(qts);

    unlink(path);
    unlink(image);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/fba-ccw/full-configuration", test_full_configuration);
    qtest_add_func("/fba-ccw/dev9336-shortcut", test_dev9336_shortcut);
    qtest_add_func("/fba-ccw/auto-devnos", test_auto_devnos);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/fba-ccw/io/2g-boundary", test_io_2g_boundary);
        qtest_add_func("/fba-ccw/io/identity", test_identity);
    }
    return g_test_run();
}
