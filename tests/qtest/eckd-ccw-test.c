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

/*
 * Write the home address, R0 and end of track of track 0 of @cylinder into
 * its slot, with @home_cylinder as the cylinder in the home address (a later
 * member of a multi-file volume starts with a cylinder other than 0).
 */
static void format_track(const char *path, unsigned int cylinder,
                         uint16_t home_cylinder)
{
    uint8_t track[29] = { 0 };
    int fd = open(path, O_WRONLY);
    int64_t offset = CKD_HEADER_SIZE + cylinder * CKD_CYLINDER_SIZE;

    g_assert_cmpint(fd, >=, 0);
    stw_be_p(track + 1, home_cylinder);
    stw_be_p(track + 5, home_cylinder);
    stw_be_p(track + 11, 8);
    memset(track + 21, 0xff, 8);
    g_assert_cmpint(pwrite(fd, track, sizeof(track), offset), ==,
                    sizeof(track));
    close(fd);
}

/* Bytes block storage adds to a volume of @cylinders: rounded up to @unit */
static int64_t padding(unsigned int cylinders, int64_t unit)
{
    int64_t size = CKD_HEADER_SIZE + cylinders * CKD_CYLINDER_SIZE;

    return ROUND_UP(size, unit) - size;
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

static void assert_open_fails_opts(const char *path, const char *opts,
                                   const char *message)
{
    const char *binary = qtest_qemu_binary(NULL);
    g_autofree char *drive = g_strdup_printf("file=%s,format=raw,devno=200%s",
                                             path, opts);
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

static void assert_open_fails(const char *path, const char *message)
{
    assert_open_fails_opts(path, "", message);
}

static void assert_open_succeeds_opts(const char *path, const char *opts)
{
    QTestState *qts = qtest_initf(
        "-nodefaults -dev3390 file=%s,format=raw,devno=200,id=dasd0%s -S",
        path, opts);

    qtest_quit(qts);
}

static void assert_open_succeeds(const char *path)
{
    assert_open_succeeds_opts(path, "");
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

    /* Hercules formats every track: the first member's cylinder 1 ... */
    format_track(first, 1, 1);
    /* ... and a later member starts with its first cylinder (here 2) */
    format_track(middle, 0, 2);

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

/* Format track 0/@head of @path with R0 and a 64-byte R1 holding @data. */
static void ckd_write_r1(const char *path, uint16_t head, const uint8_t *data)
{
    uint8_t track[5 + 16 + 8 + ECKD_R1_LENGTH + 8] = { 0 };
    uint8_t *r1 = track + 5 + 16;
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    stw_be_p(track + 3, head);          /* home address */
    stw_be_p(track + 5 + 2, head);      /* R0 count field */
    stw_be_p(track + 5 + 6, 8);
    stw_be_p(r1 + 2, head);             /* R1: record number 1, key 0 */
    r1[4] = 1;
    stw_be_p(r1 + 6, ECKD_R1_LENGTH);
    memcpy(r1 + 8, data, ECKD_R1_LENGTH);
    memset(r1 + 8 + ECKD_R1_LENGTH, 0xff, 8);
    g_assert_cmpint(pwrite(fd, track, sizeof(track),
                           CKD_HEADER_SIZE + (int64_t)head * CKD_TRACK_SIZE),
                    ==, sizeof(track));
    close(fd);
}

/* A one-cylinder image whose track 0 holds R0 and a 64-byte R1. */
static char *create_ckd_image_r1(const uint8_t *data)
{
    char *path = create_ckd64_image();

    ckd_write_r1(path, 0, data);
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

static void wait_migration_completed(QTestState *qts)
{
    for (int i = 0; i < 6000; i++) {
        QDict *rsp = qtest_qmp(qts, "{'execute': 'query-migrate'}");
        QDict *ret = qdict_get_qdict(rsp, "return");
        const char *status = qdict_haskey(ret, "status") ?
                             qdict_get_str(ret, "status") : "none";
        bool done = !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(rsp);
        if (done) {
            return;
        }
        g_usleep(10000);
    }
    g_assert_not_reached();
}

/*
 * The device's orientation survives a migration: a channel program seeks
 * to track 0/1 on the source, the next one reads the following record on
 * the destination without seeking again.
 */
static void test_migration_orientation(void)
{
    uint8_t pattern[2][ECKD_R1_LENGTH];
    uint8_t ckd[8 + ECKD_R1_LENGTH], seek[6] = { 0, 0, 0, 0, 0, 1 };
    const uint8_t count1[8] = { 0, 0, 0, 1, 1, 0, 0, ECKD_R1_LENGTH };
    g_autofree char *path = NULL;
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    g_autofree char *sock = g_strdup_printf("%s/qemu-eckd-mig-%d.sock",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *devices = NULL;
    QTestState *src, *dst;
    CcwProgIrb irb;

    for (int i = 0; i < ECKD_R1_LENGTH; i++) {
        pattern[0][i] = 0x40 + i;
        pattern[1][i] = 0x90 + i;
    }
    path = create_ckd_image_r1(pattern[0]);
    ckd_write_r1(path, 1, pattern[1]);
    devices = g_strdup_printf(
        "-drive if=none,id=ckd0,file=%s,format=raw "
        "-device eckd-ccw,drive=ckd0,devno=fe.0.0200", path);

    src = ccw_prog_start(image, "3G", devices);
    qtest_memwrite(src, CCW_PROG_DATA, seek, sizeof(seek));
    ccw_prog_set_ccw(src, 0, 0x07, 0, sizeof(seek), CCW_PROG_DATA);
    g_assert_cmpint(ccw_prog_run(src, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);

    dst = qtest_initf("-nodefaults -accel tcg -m 3G -kernel %s %s "
                      "-incoming %s", image, devices, uri);
    qtest_qmp_assert_success(src, "{'execute': 'migrate', "
                             "'arguments': {'uri': %s}}", uri);
    wait_migration_completed(src);
    qtest_qmp_eventwait(dst, "RESUME");

    /* Read Count, Key and Data of the next record (R1 of track 0/1). */
    ccw_prog_set_ccw(dst, 0, 0x1e, CCW_FLAG_SLI, sizeof(ckd),
                     CCW_PROG_DATA + 0x100);
    g_assert_cmpint(ccw_prog_run(dst, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(dst, CCW_PROG_DATA + 0x100, ckd, sizeof(ckd));
    g_assert_cmpmem(ckd, 8, count1, 8);
    g_assert_cmpmem(ckd + 8, ECKD_R1_LENGTH, pattern[1], ECKD_R1_LENGTH);

    qtest_quit(src);
    qtest_quit(dst);
    unlink(sock);
    unlink(path);
    unlink(image);
}

/*
 * Migration while a channel program runs: 14 seeks to tracks that are not
 * cached, on a throttled backend so that each takes a while.  The drain
 * before the device state is saved lets the program finish on the source;
 * the destination presents the final status and keeps the orientation.
 */
static void test_migration_during_io(void)
{
    const uint8_t r0_count[8] = { 0, 0, 0, 14, 0, 0, 0, 8 };
    uint8_t r0[16];
    g_autofree char *path = create_ckd64_image();
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    g_autofree char *sock = g_strdup_printf("%s/qemu-eckd-mig2-%d.sock",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *devices = g_strdup_printf(
        "-object throttle-group,id=tg0,x-iops-total=10 "
        "-blockdev '{\"driver\": \"throttle\", \"node-name\": \"ckd0\", "
        "\"throttle-group\": \"tg0\", \"file\": {\"driver\": \"raw\", "
        "\"file\": {\"driver\": \"file\", \"filename\": \"%s\"}}}' "
        "-device eckd-ccw,drive=ckd0,devno=fe.0.0200", path);
    QTestState *src, *dst;
    CcwProgIrb irb;

    src = ccw_prog_start(image, "3G", devices);
    for (int head = 1; head <= 14; head++) {
        uint8_t seek[6] = { 0, 0, 0, 0, 0, head };

        qtest_memwrite(src, CCW_PROG_DATA + head * 8, seek, sizeof(seek));
        ccw_prog_set_ccw(src, head - 1, 0x07, head < 14 ? CCW_FLAG_CC : 0,
                         sizeof(seek), CCW_PROG_DATA + head * 8);
    }
    dst = qtest_initf("-nodefaults -accel tcg -m 3G -kernel %s %s "
                      "-incoming %s", image, devices, uri);
    ccw_prog_go(src, CCW_ORB_FMT | CCW_ORB_C64);
    qtest_qmp_assert_success(src, "{'execute': 'migrate', "
                             "'arguments': {'uri': %s}}", uri);
    wait_migration_completed(src);
    qtest_qmp_eventwait(dst, "RESUME");
    g_assert_cmpint(ccw_prog_wait(dst, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    g_assert_cmphex(irb.cpa, ==, CCW_PROG_CCWS + 14 * 8);

    /* Read R0 of the track the program ended on. */
    ccw_prog_set_ccw(dst, 0, 0x16, CCW_FLAG_SLI, sizeof(r0),
                     CCW_PROG_DATA + 0x100);
    g_assert_cmpint(ccw_prog_run(dst, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(dst, CCW_PROG_DATA + 0x100, r0, sizeof(r0));
    g_assert_cmpmem(r0, 8, r0_count, 8);

    qtest_quit(src);
    qtest_quit(dst);
    unlink(sock);
    unlink(path);
    unlink(image);
}

/* Start the ccw_prog guest with one eckd-ccw device and @extra options. */
static QTestState *eckd_prog_start_opts(const char *image, const char *path,
                                        const char *extra)
{
    g_autofree char *devices = g_strdup_printf(
        "-drive if=none,id=ckd0,file=%s,format=raw "
        "-device eckd-ccw,drive=ckd0,devno=fe.0.0200%s", path, extra);

    return ccw_prog_start(image, "128M", devices);
}

/* Sense ID and RDC identity bytes: CU type/model, device type/model. */
static void eckd_check_identity(QTestState *qts, uint16_t cu_type,
                                uint8_t cu_model, uint16_t dev_type,
                                uint8_t dev_model)
{
    uint8_t buf[64];
    CcwProgIrb irb;

    ccw_prog_set_ccw(qts, 0, 0xe4, CCW_FLAG_SLI, 24, CCW_PROG_DATA);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(qts, CCW_PROG_DATA, buf, 7);
    g_assert_cmphex(buf[0], ==, 0xff);
    g_assert_cmphex(lduw_be_p(buf + 1), ==, cu_type);
    g_assert_cmphex(buf[3], ==, cu_model);
    g_assert_cmphex(lduw_be_p(buf + 4), ==, dev_type);
    g_assert_cmphex(buf[6], ==, dev_model);

    ccw_prog_set_ccw(qts, 0, 0x64, CCW_FLAG_SLI, 64, CCW_PROG_DATA);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(qts, CCW_PROG_DATA, buf, sizeof(buf));
    g_assert_cmphex(lduw_be_p(buf), ==, cu_type);
    g_assert_cmphex(buf[2], ==, cu_model);
    g_assert_cmphex(lduw_be_p(buf + 3), ==, dev_type);
    g_assert_cmphex(buf[5], ==, dev_model);
    /* The geometry stays the image's: one 3390 cylinder of 15 tracks. */
    g_assert_cmpuint(lduw_be_p(buf + 12), ==, 1);
    g_assert_cmpuint(lduw_be_p(buf + 14), ==, 15);
}

static void test_identity(void)
{
    g_autofree char *path = create_ckd64_image();
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts;

    /* Default: from the image (2107-E8, 3390 model by cylinders). */
    qts = eckd_prog_start_opts(image, path, "");
    eckd_check_identity(qts, 0x2107, 0xe8, 0x3390, 0x02);
    qtest_quit(qts);

    /* A 3380 identity on the 3390 image, model from the cylinder count. */
    qts = eckd_prog_start_opts(image, path,
                               ",control-unit-type=0x3990,device-type=0x3380");
    eckd_check_identity(qts, 0x3990, 0xe9, 0x3380, 0x02);
    qtest_quit(qts);

    /* Everything explicit, decimal as qemu-server passes it. */
    qts = eckd_prog_start_opts(image, path,
                               ",control-unit-type=8453,control-unit-model=232,"
                               "device-type=13200,device-model=10");
    eckd_check_identity(qts, 0x2105, 0xe8, 0x3390, 0x0a);
    qtest_quit(qts);

    unlink(path);
    unlink(image);
}

static void assert_device_fails(const char *path, const char *extra,
                                const char *message)
{
    const char *binary = qtest_qemu_binary(NULL);
    g_autofree char *drive = g_strdup_printf(
        "if=none,id=ckd0,file=%s,format=raw", path);
    g_autofree char *device = g_strdup_printf(
        "eckd-ccw,drive=ckd0,devno=fe.0.0200%s", extra);
    const char *argv[] = {
        binary, "-nodefaults", "-display", "none", "-accel", "qtest",
        "-drive", drive, "-device", device, NULL,
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

static void test_identity_invalid(void)
{
    g_autofree char *path = create_ckd64_image();

    assert_device_fails(path, ",device-type=0x9345",
                        "device-type 9345 needs device-model");
    assert_device_fails(path, ",control-unit-type=0x3880",
                        "control-unit-type 3880 needs control-unit-model");
    assert_device_fails(path, ",device-model=0x100", "device-model");
    unlink(path);
}

/*
 * zPDT-style images carry the exact cylinder count with a nonzero file
 * sequence byte (seen: 3 and 4).  On block storage that rounds sizes up
 * (ZFS volume block size, LVM extent) the rest is padding.
 */
static void test_header_size_any_file_sequence(void)
{
    const int64_t slacks[] = { 0, padding(2, 16 * 1024),
                               padding(2, 4 * 1024 * 1024), 12345,
                               CKD_CYLINDER_SIZE + 512 };
    g_autofree char *short_image =
        create_ckd_image_full("CKD_P370", 4, 3, 2, 0);
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(slacks); i++) {
        g_autofree char *path =
            create_ckd_image_full("CKD_P370", 4, 2, 2, slacks[i]);

        assert_open_succeeds(path);
        unlink(path);
    }
    assert_open_fails(short_image, "CKD image is too short for 3 cylinders");
    unlink(short_image);
}

/*
 * The cylinders property must match a count in the header, and gives the
 * count of an image whose header has none (high_cylinder 0).
 */
static void test_cylinders_property(void)
{
    g_autofree char *hercules = create_ckd_image_full(
        "CKD_P064", 0, 0, 2, padding(2, 4 * 1024 * 1024));
    g_autofree char *exact = create_ckd_image_full("CKD_P064", 0, 0, 2, 0);
    g_autofree char *declared = create_ckd_image_full(
        "CKD_P064", 0, 2, 2, padding(2, 16 * 1024));
    g_autofree char *zpdt = create_ckd_image_full(
        "CKD_P370", 4, 2, 2, padding(2, 16 * 1024));

    assert_open_succeeds_opts(hercules, ",cylinders=2");
    assert_open_succeeds_opts(exact, ",cylinders=2");
    assert_open_succeeds_opts(declared, ",cylinders=2");
    assert_open_succeeds_opts(zpdt, ",cylinders=2");
    assert_open_fails(hercules, "does not contain whole cylinders");
    assert_open_fails_opts(exact, ",cylinders=3",
                           "CKD image is too short for 3 cylinders");
    assert_open_fails_opts(declared, ",cylinders=3",
                           "cylinders=3 does not match the CKD image header "
                           "(2 cylinders)");
    assert_open_fails_opts(zpdt, ",cylinders=1",
                           "cylinders=1 does not match");
    /* a formatted cylinder follows: the volume is larger than claimed */
    format_track(hercules, 1, 1);
    assert_open_fails_opts(hercules, ",cylinders=1",
                           "CKD image holds more than 1 cylinders");
    unlink(hercules);
    unlink(exact);
    unlink(declared);
    unlink(zpdt);
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
    qtest_add_func("/eckd-ccw/header-size-any-file-sequence",
                   test_header_size_any_file_sequence);
    qtest_add_func("/eckd-ccw/cylinders-property", test_cylinders_property);
    qtest_add_func("/eckd-ccw/identity-invalid", test_identity_invalid);
    if (qtest_has_accel("tcg")) {
        qtest_add_func("/eckd-ccw/io/identity", test_identity);
        qtest_add_func("/eckd-ccw/io/2g-boundary", test_io_2g_boundary);
        qtest_add_func("/eckd-ccw/io/track-cache-invalidation",
                       test_track_cache_invalidation);
        qtest_add_func("/eckd-ccw/io/migration-orientation",
                       test_migration_orientation);
        qtest_add_func("/eckd-ccw/io/migration-during-io",
                       test_migration_during_io);
    }
    return g_test_run();
}
