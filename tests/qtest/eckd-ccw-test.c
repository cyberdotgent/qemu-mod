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

/*
 * Write CKD takes the record identifier from the count area as given: a
 * real control unit orients by position and does not require the record
 * number to be the previous one plus one.  An end-of-file record is often
 * numbered X'FF', and data sets may skip numbers.  Format-write R3 behind
 * R1 (skipping 2), then an end-of-file R=X'FF', and check the track.
 */
static void test_write_ckd_record_number(void)
{
    uint8_t pattern[ECKD_R1_LENGTH], data3[32];
    uint8_t extent[16] = { 0 }, locate[16] = { 0 };
    uint8_t ckd3[8 + sizeof(data3)];
    const uint8_t eof[8] = { 0, 0, 0, 0, 0xff, 0, 0, 0 };
    uint8_t track[8 + sizeof(data3) + 8 + 8];
    g_autofree char *path = NULL;
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts;
    CcwProgIrb irb;
    int fd;

    for (int i = 0; i < ECKD_R1_LENGTH; i++) {
        pattern[i] = 0x30 + i;
    }
    memset(data3, 0x33, sizeof(data3));
    path = create_ckd_image_r1(pattern);
    qts = eckd_prog_start(image, path);

    /* Define Extent: tracks 0/0 to 0/14, file mask 0 (writes permitted). */
    stw_be_p(extent + 14, CKD_HEADS - 1);
    /* Locate Record: Format Write, 2 records, orient on 0000 0000 01. */
    locate[0] = 0x03;
    locate[3] = 2;
    locate[12] = 1;
    /* R3: key length 0, data length 32. */
    memset(ckd3, 0, 8);
    ckd3[4] = 3;
    stw_be_p(ckd3 + 6, sizeof(data3));
    memcpy(ckd3 + 8, data3, sizeof(data3));

    qtest_memwrite(qts, CCW_PROG_DATA, extent, sizeof(extent));
    qtest_memwrite(qts, CCW_PROG_DATA + 0x20, locate, sizeof(locate));
    qtest_memwrite(qts, CCW_PROG_DATA + 0x40, ckd3, sizeof(ckd3));
    qtest_memwrite(qts, CCW_PROG_DATA + 0x80, eof, sizeof(eof));
    ccw_prog_set_ccw(qts, 0, 0x63, CCW_FLAG_CC, sizeof(extent),
                     CCW_PROG_DATA);
    ccw_prog_set_ccw(qts, 1, 0x47, CCW_FLAG_CC, sizeof(locate),
                     CCW_PROG_DATA + 0x20);
    ccw_prog_set_ccw(qts, 2, 0x1d, CCW_FLAG_CC, sizeof(ckd3),
                     CCW_PROG_DATA + 0x40);
    ccw_prog_set_ccw(qts, 3, 0x1d, 0, sizeof(eof), CCW_PROG_DATA + 0x80);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_quit(qts);

    /* Track 0/0: HA, R0, R1 unchanged, then R3, the EOF record, the end. */
    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, track, sizeof(track),
                          ECKD_R1_OFFSET + ECKD_R1_LENGTH), ==, sizeof(track));
    close(fd);
    g_assert_cmpmem(track, sizeof(ckd3), ckd3, sizeof(ckd3));
    g_assert_cmpmem(track + sizeof(ckd3), 8, eof, 8);
    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(track[sizeof(ckd3) + 8 + i], ==, 0xff);
    }
    unlink(path);
    unlink(image);
}

/*
 * The SCSW describes the channel program of the current start function:
 * its F bit (CCW format) comes from that ORB.  Error recovery builds its
 * restart program in the format the SCSW reports, so a stale F bit from
 * an earlier format-1 program makes it restart a format-0 program in
 * format 1.  Read IPL in format 1, then in format 0: F must follow.
 */
static void test_scsw_ccw_format(void)
{
    uint8_t pattern[ECKD_R1_LENGTH];
    uint8_t ccw0[8] = { 0x02, 0, 0, 0, CCW_FLAG_SLI, 0, 0, ECKD_R1_LENGTH };
    g_autofree char *path = NULL;
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts;
    CcwProgIrb irb;

    for (int i = 0; i < ECKD_R1_LENGTH; i++) {
        pattern[i] = 0x50 + i;
    }
    path = create_ckd_image_r1(pattern);
    qts = eckd_prog_start(image, path);

    /* Format 1. */
    eckd_read_ipl_check(qts, CCW_PROG_DATA, pattern);
    g_assert_cmphex(qtest_readw(qts, CCW_PROG_IRB) & 0x0080, ==, 0x0080);

    /* Format 0: same Read IPL, data address in bytes 1-3. */
    ccw0[2] = CCW_PROG_DATA >> 8;
    ccw0[3] = CCW_PROG_DATA & 0xff;
    qtest_memset(qts, CCW_PROG_DATA, 0, ECKD_R1_LENGTH);
    qtest_memwrite(qts, CCW_PROG_CCWS, ccw0, sizeof(ccw0));
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    g_assert_cmphex(qtest_readw(qts, CCW_PROG_IRB) & 0x0080, ==, 0);

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

    dst = qtest_initf("-nodefaults -accel %s -m 3G -kernel %s %s "
                      "-incoming %s", ccw_prog_accel(), image, devices, uri);
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

#define ECKD_R2_LENGTH 32

/* Add a 32-byte R2 behind the R1 that ckd_write_r1() wrote on 0/@head. */
static void ckd_write_r2(const char *path, uint16_t head)
{
    uint8_t r2[8 + ECKD_R2_LENGTH + 8] = { 0 };
    int fd = open(path, O_WRONLY);

    g_assert_cmpint(fd, >=, 0);
    stw_be_p(r2 + 2, head);
    r2[4] = 2;
    stw_be_p(r2 + 6, ECKD_R2_LENGTH);
    memset(r2 + 8, 0x22, ECKD_R2_LENGTH);
    memset(r2 + 8 + ECKD_R2_LENGTH, 0xff, 8);
    g_assert_cmpint(pwrite(fd, r2, sizeof(r2),
                           CKD_HEADER_SIZE + (int64_t)head * CKD_TRACK_SIZE +
                           5 + 16 + 8 + ECKD_R1_LENGTH),
                    ==, sizeof(r2));
    close(fd);
}

/*
 * A Read Count that follows a Read CKD steps to the next count field, also
 * when the Read CKD ended the previous channel program.  That part of the
 * orientation must survive a migration: Read CKD of R1 on the source, then
 * Read Count on the destination returns the count field of R2, as it does
 * on a machine that was not migrated.
 */
static void read_ckd_then_count(bool migrate)
{
    uint8_t pattern[ECKD_R1_LENGTH];
    uint8_t ckd[8 + ECKD_R1_LENGTH], seek[6] = { 0, 0, 0, 0, 0, 1 };
    uint8_t count[8];
    const uint8_t count1[8] = { 0, 0, 0, 1, 1, 0, 0, ECKD_R1_LENGTH };
    const uint8_t count2[8] = { 0, 0, 0, 1, 2, 0, 0, ECKD_R2_LENGTH };
    g_autofree char *path = NULL;
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    g_autofree char *sock = g_strdup_printf("%s/qemu-eckd-mig-%d.sock",
                                            g_get_tmp_dir(), getpid());
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    g_autofree char *devices = NULL;
    QTestState *src, *dst = NULL, *qts;
    CcwProgIrb irb;

    for (int i = 0; i < ECKD_R1_LENGTH; i++) {
        pattern[i] = 0x40 + i;
    }
    path = create_ckd_image_r1(pattern);
    ckd_write_r1(path, 1, pattern);
    ckd_write_r2(path, 1);
    devices = g_strdup_printf(
        "-drive if=none,id=ckd0,file=%s,format=raw "
        "-device eckd-ccw,drive=ckd0,devno=fe.0.0200", path);

    /* Seek to track 0/1 and read R1 with Read CKD. */
    src = ccw_prog_start(image, "3G", devices);
    qtest_memwrite(src, CCW_PROG_DATA, seek, sizeof(seek));
    ccw_prog_set_ccw(src, 0, 0x07, CCW_FLAG_CC, sizeof(seek), CCW_PROG_DATA);
    ccw_prog_set_ccw(src, 1, 0x1e, CCW_FLAG_SLI, sizeof(ckd),
                     CCW_PROG_DATA + 0x100);
    g_assert_cmpint(ccw_prog_run(src, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(src, CCW_PROG_DATA + 0x100, ckd, sizeof(ckd));
    g_assert_cmpmem(ckd, 8, count1, 8);

    qts = src;
    if (migrate) {
        dst = qtest_initf("-nodefaults -accel %s -m 3G -kernel %s %s "
                          "-incoming %s", ccw_prog_accel(), image, devices,
                          uri);
        qtest_qmp_assert_success(src, "{'execute': 'migrate', "
                                 "'arguments': {'uri': %s}}", uri);
        wait_migration_completed(src);
        qtest_qmp_eventwait(dst, "RESUME");
        qts = dst;
    }

    /* A new channel program: Read Count. */
    ccw_prog_set_ccw(qts, 0, 0x12, CCW_FLAG_SLI, sizeof(count),
                     CCW_PROG_DATA + 0x200);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(qts, CCW_PROG_DATA + 0x200, count, sizeof(count));
    g_assert_cmpmem(count, 8, count2, 8);

    qtest_quit(src);
    if (dst) {
        qtest_quit(dst);
    }
    unlink(sock);
    unlink(path);
    unlink(image);
}

static void test_migration_read_orientation(void)
{
    read_ckd_then_count(false);
    read_ckd_then_count(true);
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
    dst = qtest_initf("-nodefaults -accel %s -m 3G -kernel %s %s "
                      "-incoming %s", ccw_prog_accel(), image, devices, uri);
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

/*
 * 3390 identity bytes, recorded before the 3380 identity was added: the
 * default device keeps presenting exactly these.
 */
static const uint8_t eckd_3390_sense_id[24] = {
    0xff, 0x21, 0x07, 0xe8, 0x33, 0x90, 0x02, 0x00,
    0x40, 0xfa, 0x01, 0x00, 0x41, 0x27, 0x00, 0x04,
    0x42, 0x3e, 0x01, 0xa0, 0x43, 0x3e, 0x00, 0x08,
};
static const uint8_t eckd_3390_rdc[64] = {
    0x21, 0x07, 0xe8, 0x33, 0x90, 0x02, 0x52, 0x80,
    0x10, 0xb7, 0x20, 0x26, 0x00, 0x01, 0x00, 0x0f,
    0xe0, 0x00, 0xe5, 0xa2, 0x05, 0x94, 0x02, 0x22,
    0x13, 0x09, 0x06, 0x74, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x26, 0x26, 0x1f, 0x02, 0xdf, 0xee, 0x00, 0x01,
    0x06, 0x77, 0x08, 0x00, 0x00, 0x00, 0x48, 0x00,
    0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

static void eckd_run_read(QTestState *qts, uint8_t cmd, uint16_t count,
                          uint8_t *buf)
{
    CcwProgIrb irb;

    qtest_memset(qts, CCW_PROG_DATA, 0, count);
    ccw_prog_set_ccw(qts, 0, cmd, CCW_FLAG_SLI, count, CCW_PROG_DATA);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    g_assert_cmphex(irb.cstat, ==, 0);
    g_assert_cmphex(irb.dstat, ==, CCW_DSTAT_CE_DE);
    qtest_memread(qts, CCW_PROG_DATA, buf, count);
}

static void eckd_print_bytes(const char *what, const uint8_t *buf,
                             size_t length)
{
    g_test_message("%s:", what);
    for (size_t i = 0; i < length; i += 8) {
        g_autoptr(GString) line = g_string_new(NULL);

        for (size_t j = i; j < length && j < i + 8; j++) {
            g_string_append_printf(line, "0x%02x, ", buf[j]);
        }
        g_test_message("%s", line->str);
    }
}

static void test_3390_identity_bytes(void)
{
    g_autofree char *path = create_ckd64_image();
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts = eckd_prog_start_opts(image, path, "");
    uint8_t buf[64];

    eckd_run_read(qts, 0xe4, 24, buf);
    eckd_print_bytes("Sense ID", buf, 24);
    g_assert_cmpmem(buf, 24, eckd_3390_sense_id, 24);
    eckd_run_read(qts, 0x64, 64, buf);
    eckd_print_bytes("RDC", buf, 64);
    g_assert_cmpmem(buf, 64, eckd_3390_rdc, 64);
    qtest_quit(qts);
    unlink(path);
    unlink(image);
}

#define CKD3380_TRACK_SIZE 47616

/*
 * A CKD_P370 image in the 3380 track format (header device type X'80',
 * 47616-byte track slots, as Hercules writes them) of @cylinders
 * cylinders; the tracks of cylinder 0 hold home address, R0 and the end.
 */
static char *create_3380_image(unsigned int cylinders)
{
    uint8_t header[CKD_HEADER_SIZE] = { 0 };
    uint8_t track[29] = { 0 };
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-eckd-3380-XXXXXX.ckd", &path, NULL);
    unsigned int head;

    g_assert_cmpint(fd, >=, 0);
    memcpy(header, "CKD_P370", 8);
    stl_le_p(header + 8, CKD_HEADS);
    stl_le_p(header + 12, CKD3380_TRACK_SIZE);
    header[16] = 0x80;
    memcpy(header + 20, "VOL380", 6);
    g_assert_cmpint(write(fd, header, sizeof(header)), ==, sizeof(header));
    for (head = 0; head < CKD_HEADS; head++) {
        memset(track, 0, sizeof(track));
        stw_be_p(track + 3, head);
        stw_be_p(track + 7, head);
        stw_be_p(track + 11, 8);
        memset(track + 21, 0xff, 8);
        g_assert_cmpint(pwrite(fd, track, sizeof(track),
                               CKD_HEADER_SIZE +
                               (int64_t)head * CKD3380_TRACK_SIZE), ==,
                        sizeof(track));
    }
    g_assert_cmpint(ftruncate(fd, CKD_HEADER_SIZE + (int64_t)cylinders *
                              CKD_HEADS * CKD3380_TRACK_SIZE), ==, 0);
    close(fd);
    return path;
}

/* The device bytes of RDC that tell a 3380 (IBM GA32-0274, appendix A). */
static void eckd_check_rdc_3380(const uint8_t *rdc, uint16_t primary,
                                uint16_t alternate_tracks)
{
    g_assert_cmphex(lduw_be_p(rdc + 3), ==, 0x3380);
    g_assert_cmphex(rdc[10], ==, 0x20);
    g_assert_cmphex(rdc[11], ==, 0x0e);
    g_assert_cmpuint(lduw_be_p(rdc + 12), ==, primary);
    g_assert_cmpuint(lduw_be_p(rdc + 14), ==, 15);
    g_assert_cmpuint(rdc[16], ==, 222);
    g_assert_cmpuint(ldl_be_p(rdc + 16) & 0xffffff, ==, 47968);
    g_assert_cmpuint(lduw_be_p(rdc + 20), ==, 1088);
    g_assert_cmpuint(rdc[22], ==, 1);
    g_assert_cmpuint(rdc[23], ==, 32);
    g_assert_cmpuint(lduw_be_p(rdc + 24), ==, 492);
    g_assert_cmpuint(lduw_be_p(rdc + 26), ==, 236);
    g_assert_cmpuint(lduw_be_p(rdc + 28), ==, alternate_tracks ? primary : 0);
    g_assert_cmpuint(lduw_be_p(rdc + 30), ==, alternate_tracks);
    g_assert_cmphex(rdc[40], ==, 0x0e);
    g_assert_cmphex(rdc[41], ==, 0x0e);
    g_assert_cmpuint(lduw_be_p(rdc + 44), ==, 47988);
    g_assert_cmpuint(rdc[48], ==, 0);
    g_assert_cmphex(lduw_be_p(rdc + 49), ==, 0x5007);
}

/*
 * A 3380 image makes a 3380 on a 3990 by default; a 3380-1 image with its
 * alternate cylinder (886 cylinders) reports 885 primary cylinders.
 */
static void test_3380_identity(void)
{
    g_autofree char *path = create_3380_image(1);
    g_autofree char *path886 = create_3380_image(886);
    g_autofree char *path3390 = create_ckd64_image();
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    static const uint8_t ciws[16] = {
        0x40, 0xfa, 0x01, 0x00, 0x41, 0x27, 0x00, 0x04,
        0x42, 0x3e, 0x01, 0xa0, 0x43, 0x3e, 0x00, 0x08,
    };
    QTestState *qts;
    uint8_t buf[64];

    qts = eckd_prog_start_opts(image, path, "");
    eckd_check_identity(qts, 0x3990, 0xe9, 0x3380, 0x02);
    eckd_run_read(qts, 0xe4, 24, buf);
    g_assert_cmpmem(buf + 8, 16, ciws, 16);
    eckd_run_read(qts, 0x64, 64, buf);
    eckd_check_rdc_3380(buf, 1, 0);
    qtest_quit(qts);

    qts = eckd_prog_start_opts(image, path886, "");
    eckd_run_read(qts, 0x64, 64, buf);
    g_assert_cmphex(buf[5], ==, 0x02);
    eckd_check_rdc_3380(buf, 885, 15);
    qtest_quit(qts);

    /* 3380 identity on a 3390 image: 3380 track format in RDC. */
    qts = eckd_prog_start_opts(image, path3390, ",device-type=0x3380");
    eckd_check_identity(qts, 0x3990, 0xe9, 0x3380, 0x02);
    eckd_run_read(qts, 0x64, 64, buf);
    eckd_check_rdc_3380(buf, 1, 0);
    qtest_quit(qts);

    /* A 3880 has no command information words: Sense ID is 7 bytes. */
    qts = eckd_prog_start_opts(image, path, ",control-unit-type=0x3880,"
                               "control-unit-model=0x05");
    eckd_run_read(qts, 0xe4, 7, buf);
    g_assert_cmphex(lduw_be_p(buf + 1), ==, 0x3880);
    g_assert_cmphex(buf[3], ==, 0x05);
    g_assert_cmphex(lduw_be_p(buf + 4), ==, 0x3380);
    {
        CcwProgIrb irb;

        /* 24 bytes asked for without SLI: incorrect length, 7 sent */
        ccw_prog_set_ccw(qts, 0, 0xe4, 0, 24, CCW_PROG_DATA);
        g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb),
                        ==, 0);
        g_assert_cmphex(irb.cstat, ==, 0x40);
        g_assert_cmpuint(irb.count, ==, 24 - 7);
    }
    qtest_quit(qts);

    unlink(path);
    unlink(path886);
    unlink(path3390);
    unlink(image);
}

static void test_3380_identity_invalid(void)
{
    g_autofree char *path = create_3380_image(1);

    assert_device_fails(path, ",device-type=0x3390",
                        "device-type 3390 needs an image with 3390 tracks");
    unlink(path);
}

/*
 * Format-write records behind R0 of track 0/@head in one channel program:
 * Define Extent, Locate Record (Format Write, orient on R@after), then one
 * Write CKD per record (key length @kl, data lengths @dl[]).  Returns the
 * IRB of the program.
 */
static void eckd_format_records(QTestState *qts, uint16_t head, uint8_t after,
                                uint8_t first, unsigned int count, uint8_t kl,
                                uint16_t dl, CcwProgIrb *irb)
{
    uint8_t extent[16] = { 0 }, locate[16] = { 0 }, count_area[8];
    uint64_t data = CCW_PROG_DATA + 0x100;
    unsigned int i;

    g_assert_cmpuint(count, <=, 30);
    stw_be_p(extent + 14, CKD_HEADS - 1);
    locate[0] = 0x03;
    locate[3] = count;
    stw_be_p(locate + 6, head);         /* seek address CCHH */
    stw_be_p(locate + 10, head);        /* search argument CCHHR */
    locate[12] = after;
    qtest_memwrite(qts, CCW_PROG_DATA, extent, sizeof(extent));
    qtest_memwrite(qts, CCW_PROG_DATA + 0x20, locate, sizeof(locate));
    ccw_prog_set_ccw(qts, 0, 0x63, CCW_FLAG_CC, sizeof(extent),
                     CCW_PROG_DATA);
    ccw_prog_set_ccw(qts, 1, 0x47, CCW_FLAG_CC, sizeof(locate),
                     CCW_PROG_DATA + 0x20);
    for (i = 0; i < count; i++) {
        memset(count_area, 0, sizeof(count_area));
        stw_be_p(count_area + 2, head);
        count_area[4] = first + i;
        count_area[5] = kl;
        stw_be_p(count_area + 6, dl);
        /* key and data stay zero: the device pads a short transfer */
        qtest_memwrite(qts, data + i * 8, count_area, 8);
        ccw_prog_set_ccw(qts, 2 + i, 0x1d,
                         i + 1 < count ? CCW_FLAG_CC : 0, 8, data + i * 8);
    }
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, irb), ==, 0);
}

static void eckd_expect_ok(QTestState *qts, const CcwProgIrb *irb)
{
    if (irb->dstat & CCW_DSTAT_UC) {
        uint8_t sense[32];

        eckd_run_read(qts, 0x04, sizeof(sense), sense);
        eckd_print_bytes("unexpected unit check, sense", sense, 32);
        g_test_message("CCW address %x", irb->cpa);
    }
    g_assert_cmphex(irb->cstat, ==, 0);
    g_assert_cmphex(irb->dstat, ==, CCW_DSTAT_CE_DE);
}

/* Unit check, sense byte 1 Invalid Track Format: the record does not fit. */
static void eckd_expect_track_full(QTestState *qts, const CcwProgIrb *irb)
{
    uint8_t sense[32];

    g_assert_cmphex(irb->dstat, ==, CCW_DSTAT_CE_DE | CCW_DSTAT_UC);
    eckd_run_read(qts, 0x04, sizeof(sense), sense);
    g_assert_cmphex(sense[0], ==, 0);
    g_assert_cmphex(sense[1], ==, 0x40);
}

/*
 * Write CKD keeps to the 3380 track capacity (formula 1, 47968 bytes):
 * one record of 47476 bytes, or 83 records of 80 bytes (576 bytes of
 * track each), or a keyed record of 8 + 47220 bytes fill a track.
 */
static void check_3380_track_capacity(const char *path, const char *extra,
                                      int64_t track_size)
{
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts = eckd_prog_start_opts(image, path, extra);
    uint8_t count_area[8];
    CcwProgIrb irb;
    int fd;

    /* Head 1: the largest record, then nothing more. */
    eckd_format_records(qts, 1, 0, 1, 1, 0, 47476, &irb);
    eckd_expect_ok(qts, &irb);
    eckd_format_records(qts, 1, 1, 2, 1, 0, 1, &irb);
    eckd_expect_track_full(qts, &irb);

    /* Head 2: one byte more than the largest record does not fit. */
    eckd_format_records(qts, 2, 0, 1, 1, 0, 47477, &irb);
    eckd_expect_track_full(qts, &irb);

    /* Head 3: 83 records of 80 bytes, not 84. */
    eckd_format_records(qts, 3, 0, 1, 30, 0, 80, &irb);
    eckd_expect_ok(qts, &irb);
    eckd_format_records(qts, 3, 30, 31, 30, 0, 80, &irb);
    eckd_expect_ok(qts, &irb);
    eckd_format_records(qts, 3, 60, 61, 23, 0, 80, &irb);
    eckd_expect_ok(qts, &irb);
    eckd_format_records(qts, 3, 83, 84, 1, 0, 80, &irb);
    eckd_expect_track_full(qts, &irb);

    /* Head 4: a keyed record. */
    eckd_format_records(qts, 4, 0, 1, 1, 8, 47221, &irb);
    eckd_expect_track_full(qts, &irb);
    eckd_format_records(qts, 4, 0, 1, 1, 8, 47220, &irb);
    eckd_expect_ok(qts, &irb);
    qtest_quit(qts);

    /* The image holds R83 of head 3 and the end of track behind it. */
    fd = open(path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, count_area, 8,
                          CKD_HEADER_SIZE + 3 * track_size + 5 + 16 +
                          82 * (8 + 80)), ==, 8);
    g_assert_cmphex(count_area[4], ==, 83);
    g_assert_cmpuint(lduw_be_p(count_area + 6), ==, 80);
    g_assert_cmpint(pread(fd, count_area, 8,
                          CKD_HEADER_SIZE + 3 * track_size + 5 + 16 +
                          83 * (8 + 80)), ==, 8);
    for (int i = 0; i < 8; i++) {
        g_assert_cmphex(count_area[i], ==, 0xff);
    }
    close(fd);
    unlink(image);
}

static void test_3380_track_capacity(void)
{
    g_autofree char *path = create_3380_image(1);
    g_autofree char *path3390 = create_ckd64_image();

    check_3380_track_capacity(path, "", CKD3380_TRACK_SIZE);
    /* The same on 3390 tracks with a 3380 identity. */
    check_3380_track_capacity(path3390, ",device-type=0x3380",
                              CKD_TRACK_SIZE);
    unlink(path);
    unlink(path3390);
}

/* A 3390 is not limited to 3380 tracks: two records beyond 47968 bytes. */
static void test_3390_beyond_3380_capacity(void)
{
    g_autofree char *path = create_ckd64_image();
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts = eckd_prog_start_opts(image, path, "");
    CcwProgIrb irb;

    eckd_format_records(qts, 1, 0, 1, 1, 0, 47477, &irb);
    eckd_expect_ok(qts, &irb);
    eckd_format_records(qts, 1, 1, 2, 1, 0, 4096, &irb);
    eckd_expect_ok(qts, &irb);
    qtest_quit(qts);
    unlink(path);
    unlink(image);
}

/* Read IPL and IPL from a 3380 image: the program loops at 0x18. */
static void test_3380_ipl(void)
{
    static const uint8_t ipl_psw_ccw[16] = {
        0x00, 0x08, 0x00, 0x00, 0x80, 0x00, 0x00, 0x18,
        0x06, 0x00, 0x00, 0x18, 0x20, 0x00, 0x00, 0x04,
    };
    static const uint8_t ipl_loop[4] = { 0xa7, 0xf4, 0x00, 0x00 };
    uint8_t track[5 + 16 + 8 + 24 + 8 + 4 + 8] = { 0 };
    uint8_t *r1 = track + 5 + 16, *r2 = r1 + 8 + 24;
    g_autofree char *path = create_3380_image(1);
    g_autofree char *regs = NULL;
    QTestState *qts;
    int fd, i;

    stw_be_p(track + 5 + 6, 8);         /* R0 */
    r1[4] = 1;
    stw_be_p(r1 + 6, 24);
    memcpy(r1 + 8, ipl_psw_ccw, sizeof(ipl_psw_ccw));
    r2[4] = 2;
    stw_be_p(r2 + 6, 4);
    memcpy(r2 + 8, ipl_loop, sizeof(ipl_loop));
    memset(r2 + 8 + 4, 0xff, 8);
    fd = open(path, O_WRONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, track, sizeof(track), CKD_HEADER_SIZE), ==,
                    sizeof(track));
    close(fd);

    qts = qtest_initf("-nodefaults -accel kvm -m 128M "
                      "-drive if=none,id=ckd,file=%s,format=raw "
                      "-device eckd-ccw,drive=ckd,devno=fe.0.0300 -ipl 300",
                      path);
    for (i = 0; i < 500; i++) {
        const char *addr;

        g_free(regs);
        regs = qtest_hmp(qts, "info registers");
        addr = strstr(regs, " addr ");
        g_assert_nonnull(addr);
        if (g_ascii_strtoull(addr + 6, NULL, 16) == 0x18) {
            break;
        }
        g_usleep(10000);
    }
    g_assert_cmpint(i, <, 500);
    qtest_quit(qts);
    unlink(path);
}

/*
 * A Locate Record "Read Tracks" domain of two tracks read with two Read
 * Track commands: the second Read Track advances to the next track
 * (3990/9390 Storage Control Reference, Read Track).  Tracks 0/1 and 0/2
 * hold R0 and an R1 with distinct contents; each buffer must receive R0's
 * count, its data, R1's count and data, then the X'FF' pseudo count.
 */
static void test_read_track_domain(void)
{
    uint8_t data1[ECKD_R1_LENGTH], data2[ECKD_R1_LENGTH];
    uint8_t extent[16] = { 0 }, locate[16] = { 0 };
    const unsigned int len = 8 + 8 + 8 + ECKD_R1_LENGTH + 8;
    uint8_t buf[2][8 + 8 + 8 + ECKD_R1_LENGTH + 8];
    g_autofree char *path = NULL;
    g_autofree char *image = ccw_prog_create_image(CCW_PROG_SID_OF(0));
    QTestState *qts;
    CcwProgIrb irb;

    memset(data1, 0x11, sizeof(data1));
    memset(data2, 0x22, sizeof(data2));
    path = create_ckd64_image();
    ckd_write_r1(path, 1, data1);
    ckd_write_r1(path, 2, data2);
    qts = eckd_prog_start(image, path);

    /* Define Extent: tracks 0/0 to 0/14. */
    stw_be_p(extent + 14, CKD_HEADS - 1);
    /* Locate Record: Read Tracks (0x0c), 2 tracks, seek/search 0/1, R0. */
    locate[0] = 0x0c;
    locate[3] = 2;
    stw_be_p(locate + 6, 1);            /* seek address: CC 0, HH 1 */
    stw_be_p(locate + 10, 1);           /* search argument: CC 0, HH 1 */
    locate[12] = 0;
    qtest_memwrite(qts, CCW_PROG_DATA, extent, sizeof(extent));
    qtest_memwrite(qts, CCW_PROG_DATA + 0x20, locate, sizeof(locate));
    qtest_memset(qts, CCW_PROG_DATA + 0x100, 0, 2 * 0x100);
    ccw_prog_set_ccw(qts, 0, 0x63, CCW_FLAG_CC, sizeof(extent),
                     CCW_PROG_DATA);
    ccw_prog_set_ccw(qts, 1, 0x47, CCW_FLAG_CC, sizeof(locate),
                     CCW_PROG_DATA + 0x20);
    ccw_prog_set_ccw(qts, 2, 0xde, CCW_FLAG_CC | CCW_FLAG_SLI, 0x100,
                     CCW_PROG_DATA + 0x100);
    ccw_prog_set_ccw(qts, 3, 0xde, CCW_FLAG_SLI, 0x100,
                     CCW_PROG_DATA + 0x200);
    g_assert_cmpint(ccw_prog_run(qts, CCW_ORB_FMT | CCW_ORB_C64, &irb), ==, 0);
    eckd_expect_ok(qts, &irb);
    qtest_memread(qts, CCW_PROG_DATA + 0x100, buf[0], len);
    qtest_memread(qts, CCW_PROG_DATA + 0x200, buf[1], len);
    for (int t = 0; t < 2; t++) {
        const uint8_t *b = buf[t];

        g_test_message("track %d: R0 %02x%02x%02x%02x%02x R1 data %02x", t,
                       b[0], b[1], b[2], b[3], b[4], b[8 + 8 + 8]);
        /* R0 count field: CC 0, HH 1 + t, R 0, KL 0, DL 8 */
        g_assert_cmphex(lduw_be_p(b + 2), ==, 1 + t);
        g_assert_cmphex(b[4], ==, 0);
        /* R1 count field and data */
        g_assert_cmphex(lduw_be_p(b + 16 + 2), ==, 1 + t);
        g_assert_cmphex(b[16 + 4], ==, 1);
        g_assert_cmpmem(b + 24, ECKD_R1_LENGTH, t ? data2 : data1,
                        ECKD_R1_LENGTH);
        for (int i = 0; i < 8; i++) {
            g_assert_cmphex(b[24 + ECKD_R1_LENGTH + i], ==, 0xff);
        }
    }
    /* The final status belongs to the second Read Track. */
    g_assert_cmphex(irb.cpa, ==, CCW_PROG_CCWS + 4 * 8);
    g_assert_cmphex(irb.count, ==, 0x100 - len);
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
    qtest_add_func("/eckd-ccw/header-size-any-file-sequence",
                   test_header_size_any_file_sequence);
    qtest_add_func("/eckd-ccw/cylinders-property", test_cylinders_property);
    qtest_add_func("/eckd-ccw/identity-invalid", test_identity_invalid);
    qtest_add_func("/eckd-ccw/3380-identity-invalid",
                   test_3380_identity_invalid);
    if (ccw_prog_accel()) {
        qtest_add_func("/eckd-ccw/io/identity", test_identity);
        qtest_add_func("/eckd-ccw/io/2g-boundary", test_io_2g_boundary);
        qtest_add_func("/eckd-ccw/io/track-cache-invalidation",
                       test_track_cache_invalidation);
        qtest_add_func("/eckd-ccw/io/write-ckd-record-number",
                       test_write_ckd_record_number);
        qtest_add_func("/eckd-ccw/io/read-track-domain",
                       test_read_track_domain);
        qtest_add_func("/eckd-ccw/io/scsw-ccw-format",
                       test_scsw_ccw_format);
        qtest_add_func("/eckd-ccw/io/migration-orientation",
                       test_migration_orientation);
        qtest_add_func("/eckd-ccw/io/migration-read-orientation",
                       test_migration_read_orientation);
        qtest_add_func("/eckd-ccw/io/migration-during-io",
                       test_migration_during_io);
        qtest_add_func("/eckd-ccw/io/3390-identity-bytes",
                       test_3390_identity_bytes);
        qtest_add_func("/eckd-ccw/io/3380-identity", test_3380_identity);
        qtest_add_func("/eckd-ccw/io/3380-track-capacity",
                       test_3380_track_capacity);
        qtest_add_func("/eckd-ccw/io/3390-beyond-3380-capacity",
                       test_3390_beyond_3380_capacity);
    }
    /* The IPL test runs the firmware: KVM only. */
    if (qtest_has_accel("kvm") && !access("/dev/kvm", R_OK | W_OK)) {
        qtest_add_func("/eckd-ccw/io/3380-ipl", test_3380_ipl);
    }
    return g_test_run();
}
