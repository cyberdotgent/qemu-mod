/*
 * QTest testcase for emulated 2107/3390 ECKD disks
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "s390x-ccw-prog.h"

#define CKD_HEADER_SIZE 512
#define CKD_TRACK_SIZE 56832
#define CKD_HEADS 15

#define CKD_CYLINDER_SIZE ((int64_t)CKD_HEADS * CKD_TRACK_SIZE)

/*
 * Create a CKD image of @cylinders cylinders followed by @slack bytes.
 * Cylinder 0 is preformatted (home address, R0 and end of track); the other
 * track slots are left zero, which reads as unformatted empty tracks.
 */
static char *create_ckd_image_full(const char *identifier,
                                   uint8_t file_sequence,
                                   uint16_t high_cylinder,
                                   unsigned int cylinders, int64_t slack)
{
    uint8_t header[CKD_HEADER_SIZE] = { 0 };
    uint8_t track[29] = { 0 };
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-eckd-XXXXXX.ckd64", &path, NULL);
    unsigned int head;

    g_assert_cmpint(fd, >=, 0);
    memcpy(header, identifier, 8);
    stl_le_p(header + 8, CKD_HEADS);
    stl_le_p(header + 12, CKD_TRACK_SIZE);
    header[16] = 0x90;
    header[17] = file_sequence;
    stw_le_p(header + 18, high_cylinder);
    memcpy(header + 20, "123456789012", 12);
    g_assert_cmpint(write(fd, header, sizeof(header)), ==, sizeof(header));

    for (head = 0; head < CKD_HEADS; head++) {
        int64_t offset = CKD_HEADER_SIZE + (int64_t)head * CKD_TRACK_SIZE;

        memset(track, 0, sizeof(track));
        stw_be_p(track + 3, head);
        stw_be_p(track + 7, head);
        stw_be_p(track + 11, 8);
        memset(track + 21, 0xff, 8);
        g_assert_cmpint(pwrite(fd, track, sizeof(track), offset), ==,
                        sizeof(track));
    }
    g_assert_cmpint(ftruncate(fd, CKD_HEADER_SIZE +
                             cylinders * CKD_CYLINDER_SIZE + slack), ==, 0);
    close(fd);
    return path;
}

/* New images carry the cylinder count in the header. */
static char *create_ckd_image(const char *identifier, uint8_t file_sequence,
                              uint16_t high_cylinder)
{
    return create_ckd_image_full(identifier, file_sequence, high_cylinder,
                                 1, 0);
}

static char *create_ckd64_image(void)
{
    return create_ckd_image("CKD_P064", 0, 1);
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
    g_assert_cmpstr(dev_id0, ==, "fe.0.0300");
    g_assert_cmpstr(dev_id1, ==, "fe.0.0301");
    qtest_quit(qts);
    unlink(path0);
    unlink(path1);
}

static void test_legacy_single_file_header(void)
{
    g_autofree char *path = create_ckd_image("CKD_P370", 4, 1);
    QTestState *qts = qtest_initf(
        "-nodefaults -dev3390 file=%s,devno=200,id=dasd0 -S", path);

    qtest_quit(qts);
    unlink(path);
}

static void assert_open_fails(const char *path, const char *message)
{
    const char *binary = qtest_qemu_binary(NULL);
    g_autofree char *drive = g_strdup_printf("file=%s,format=raw,devno=200",
                                             path);
    const char *argv[] = {
        binary, "-nodefaults", "-display", "none", "-accel", "qtest",
        "-dev3390", drive, NULL,
    };
    g_autofree char *stderr_data = NULL;
    int status;

    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL,
                               G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
                               &stderr_data, &status, NULL));
    g_assert_true(WIFEXITED(status));
    g_assert_cmpint(WEXITSTATUS(status), !=, 0);
    if (!strstr(stderr_data, message)) {
        g_test_message("stderr: %s", stderr_data);
        g_assert_not_reached();
    }
}

static void assert_open_succeeds(const char *path)
{
    QTestState *qts = qtest_initf(
        "-nodefaults -dev3390 file=%s,format=raw,devno=200,id=dasd0 -S",
        path);

    qtest_quit(qts);
}

/*
 * A nonzero high_cylinder with file sequence 0 defines the cylinder count.
 * The backend may be longer (block storage rounds sizes up), but not
 * shorter.
 */
static void test_header_defined_size(void)
{
    static const int64_t slacks[] = { 0, 4 * 1024 * 1024, 12345,
                                      CKD_CYLINDER_SIZE + 512 };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(slacks); i++) {
        g_autofree char *path =
            create_ckd_image_full("CKD_P064", 0, 2, 2, slacks[i]);

        assert_open_succeeds(path);
        unlink(path);
    }
}

static void test_header_defined_size_too_short(void)
{
    g_autofree char *path = create_ckd_image_full("CKD_P064", 0, 3, 2, 0);
    g_autofree char *partial =
        create_ckd_image_full("CKD_P064", 0, 2, 1, CKD_CYLINDER_SIZE - 512);
    g_autofree char *header_only =
        create_ckd_image_full("CKD_P064", 0, 1, 0, 0);

    assert_open_fails(path, "CKD image is too short for 3 cylinders");
    assert_open_fails(partial, "CKD image is too short for 2 cylinders");
    assert_open_fails(header_only, "CKD image is too short for 1 cylinders");
    unlink(path);
    unlink(partial);
    unlink(header_only);
}

static void test_header_defined_size_limit(void)
{
    g_autofree char *path = create_ckd_image_full("CKD_P064", 0, 65521, 1, 0);

    assert_open_fails(path, "CKD cylinder count 65521 is outside 1..65520");
    unlink(path);
}

/* Without a cylinder count in the header, the length must be exact. */
static void test_legacy_exact_length(void)
{
    g_autofree char *exact = create_ckd_image_full("CKD_P064", 0, 0, 2, 0);
    g_autofree char *slack =
        create_ckd_image_full("CKD_P064", 0, 0, 2, 4 * 1024 * 1024);
    g_autofree char *partial = create_ckd_image_full("CKD_P064", 0, 0, 2, 512);

    assert_open_succeeds(exact);
    assert_open_fails(slack, "does not contain whole cylinders");
    assert_open_fails(partial, "does not contain whole cylinders");
    unlink(exact);
    unlink(slack);
    unlink(partial);
}

/*
 * The first member of a Hercules multi-file volume has file sequence 1 and
 * the zero-origin number of its last cylinder in high_cylinder, so it holds
 * high_cylinder + 1 cylinders.  It must not pass as a volume with slack.
 */
static void test_multi_file_rejected(void)
{
    g_autofree char *first = create_ckd_image_full("CKD_P064", 1, 1, 2, 0);
    g_autofree char *middle = create_ckd_image_full("CKD_P064", 2, 3, 2, 0);
    g_autofree char *last = create_ckd_image_full("CKD_P064", 2, 0, 2, 0);

    assert_open_fails(first, "multi-file CKD images are unsupported");
    assert_open_fails(middle, "multi-file CKD images are unsupported");
    assert_open_fails(last, "multi-file CKD images are unsupported");
    unlink(first);
    unlink(middle);
    unlink(last);
}

#define ECKD_R1_LENGTH 64
#define ECKD_R1_OFFSET (CKD_HEADER_SIZE + 5 + 16 + 8)
#define GIB2 0x80000000u

/* A one-cylinder image whose track 0 holds R0 and a 64-byte R1. */
static char *create_ckd_image_r1(const uint8_t *data)
{
    char *path = create_ckd64_image();
    uint8_t track[5 + 16 + 8 + ECKD_R1_LENGTH + 8] = { 0 };
    uint8_t *r1 = track + 5 + 16;
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    stw_be_p(track + 5 + 6, 8);         /* R0: data length 8 */
    r1[4] = 1;                          /* R1: record number 1, key 0 */
    stw_be_p(r1 + 6, ECKD_R1_LENGTH);
    memcpy(r1 + 8, data, ECKD_R1_LENGTH);
    memset(r1 + 8 + ECKD_R1_LENGTH, 0xff, 8);
    g_assert_cmpint(pwrite(fd, track, sizeof(track), CKD_HEADER_SIZE), ==,
                    sizeof(track));
    close(fd);
    return path;
}

static QTestState *eckd_prog_start(const char *image, const char *path)
{
    g_autofree char *devices = g_strdup_printf(
        "-drive if=none,id=ckd0,file=%s,format=raw "
        "-device eckd-ccw,drive=ckd0,devno=fe.0.0200", path);

    return ccw_prog_start(image, "3G", devices);
}

/* Read R1 of track 0 with Read IPL into @addr and compare it. */
static void eckd_read_ipl_check(QTestState *qts, uint64_t addr,
                                const uint8_t *expected)
{
    uint8_t data[ECKD_R1_LENGTH];
    CcwProgIrb irb;

    qtest_memset(qts, addr, 0, sizeof(data));
    ccw_prog_set_ccw(qts, 0, 0x02, CCW_FLAG_SLI, ECKD_R1_LENGTH, addr);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(qts, addr, data, sizeof(data));
    g_assert_cmpmem(data, sizeof(data), expected, ECKD_R1_LENGTH);
}

/* A record read into a buffer whose last byte is at 2 GiB - 1. */
static void test_io_2g_boundary(void)
{
    uint8_t pattern[ECKD_R1_LENGTH];
    g_autofree char *path = NULL;
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts;

    for (int i = 0; i < ECKD_R1_LENGTH; i++) {
        pattern[i] = 0xa0 + i;
    }
    path = create_ckd_image_r1(pattern);
    qts = eckd_prog_start(image, path);
    eckd_read_ipl_check(qts, GIB2 - ECKD_R1_LENGTH, pattern);
    qtest_quit(qts);
    unlink(path);
    unlink(image);
}

static void update_r1(const char *path, const uint8_t *data)
{
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, data, ECKD_R1_LENGTH, ECKD_R1_OFFSET), ==,
                    ECKD_R1_LENGTH);
    close(fd);
}

/*
 * The device caches the current track.  When the image changes behind it
 * (here: rewritten by another process, as after a snapshot revert or by a
 * second system sharing the volume), a reset and every drained section
 * (stop/cont, block graph changes, loadvm) must drop the cached copy.
 */
static void test_track_cache_invalidation(void)
{
    uint8_t pattern[3][ECKD_R1_LENGTH];
    g_autofree char *path = NULL;
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts;

    for (int i = 0; i < ECKD_R1_LENGTH; i++) {
        pattern[0][i] = 0x10 + i;
        pattern[1][i] = 0x60 + i;
        pattern[2][i] = 0xb0 + i;
    }
    path = create_ckd_image_r1(pattern[0]);
    qts = eckd_prog_start(image, path);
    eckd_read_ipl_check(qts, CCW_PROG_DATA, pattern[0]);

    /* System reset: the guest program is IPLed again. */
    update_r1(path, pattern[1]);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    qtest_qmp_eventwait(qts, "RESET");
    eckd_read_ipl_check(qts, CCW_PROG_DATA, pattern[1]);

    /* stop and cont drain the block backend. */
    update_r1(path, pattern[2]);
    qtest_qmp_assert_success(qts, "{'execute': 'stop'}");
    qtest_qmp_assert_success(qts, "{'execute': 'cont'}");
    eckd_read_ipl_check(qts, CCW_PROG_DATA, pattern[2]);

    qtest_quit(qts);
    unlink(path);
    unlink(image);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/eckd-ccw/dev3390-shortcut", test_dev3390_shortcut);
    qtest_add_func("/eckd-ccw/automatic-addresses", test_automatic_addresses);
    qtest_add_func("/eckd-ccw/legacy-single-file-header",
                   test_legacy_single_file_header);
    qtest_add_func("/eckd-ccw/header-defined-size", test_header_defined_size);
    qtest_add_func("/eckd-ccw/header-defined-size-too-short",
                   test_header_defined_size_too_short);
    qtest_add_func("/eckd-ccw/header-defined-size-limit",
                   test_header_defined_size_limit);
    qtest_add_func("/eckd-ccw/legacy-exact-length", test_legacy_exact_length);
    qtest_add_func("/eckd-ccw/multi-file-rejected", test_multi_file_rejected);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/eckd-ccw/io/2g-boundary", test_io_2g_boundary);
        qtest_add_func("/eckd-ccw/io/track-cache-invalidation",
                       test_track_cache_invalidation);
    }
    return g_test_run();
}
