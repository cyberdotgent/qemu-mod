/*
 * Initialised 3390 CKD volumes: header, volume label and VTOC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/ckd-init.h"

typedef struct {
    uint16_t cylinder, head;
    uint8_t number, key_length;
    uint16_t data_length;
    const uint8_t *key, *data;
} Record;

/* Parse a track slot the way the eckd-ccw device does; returns the count. */
static unsigned parse_track(const uint8_t *t, uint32_t track, Record *r,
                            unsigned max)
{
    uint32_t off = 5;
    unsigned n = 0;
    static const uint8_t eot[8] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };

    g_assert_cmpuint(t[0], ==, 0);
    g_assert_cmpuint(lduw_be_p(t + 1), ==, track / CKD_INIT_HEADS);
    g_assert_cmpuint(lduw_be_p(t + 3), ==, track % CKD_INIT_HEADS);
    for (;;) {
        g_assert_cmpuint(off + 8, <=, CKD_INIT_TRACK_SIZE);
        if (!memcmp(t + off, eot, 8)) {
            break;
        }
        g_assert_cmpuint(n, <, max);
        r[n].cylinder = lduw_be_p(t + off);
        r[n].head = lduw_be_p(t + off + 2);
        r[n].number = t[off + 4];
        r[n].key_length = t[off + 5];
        r[n].data_length = lduw_be_p(t + off + 6);
        r[n].key = t + off + 8;
        r[n].data = r[n].key + r[n].key_length;
        g_assert_cmpuint(r[n].cylinder, ==, track / CKD_INIT_HEADS);
        g_assert_cmpuint(r[n].head, ==, track % CKD_INIT_HEADS);
        g_assert_cmpuint(r[n].number, ==, n);
        off += 8 + r[n].key_length + r[n].data_length;
        n++;
    }
    /* nothing after the end of track */
    for (off += 8; off < CKD_INIT_TRACK_SIZE; off++) {
        g_assert_cmpuint(t[off], ==, 0);
    }
    return n;
}

static void init_params(CkdInitParams *p, uint32_t cylinders)
{
    Error *err = NULL;

    memset(p, 0, sizeof(*p));
    p->ckd64 = true;
    p->cylinders = cylinders;
    g_assert_true(ckd_init_parse_volser("d9ecat", p->volser, &err));
    g_assert_true(ckd_init_check(p, &error_abort));
}

static void test_models(void)
{
    g_assert_cmpuint(ckd_init_model_cylinders("3390-1"), ==, 1113);
    g_assert_cmpuint(ckd_init_model_cylinders("3390-2"), ==, 2226);
    g_assert_cmpuint(ckd_init_model_cylinders("3390-3"), ==, 3339);
    g_assert_cmpuint(ckd_init_model_cylinders("3390-9"), ==, 10017);
    g_assert_cmpuint(ckd_init_model_cylinders("3390-27"), ==, 32760);
    g_assert_cmpuint(ckd_init_model_cylinders("3390-54"), ==, 65520);
    g_assert_cmpuint(ckd_init_model_cylinders("3390-A"), ==, 0);
    g_assert_cmpuint(ckd_init_image_size(3339), ==, 2846431232ULL);
}

static void test_volser(void)
{
    char v[7];
    static const char *const bad[] = {
        "", "SEVENCH", "A B", "A.B", "A%", "A_B", "\xc4",
    };
    size_t i;

    g_assert_true(ckd_init_parse_volser("sys@#$", v, &error_abort));
    g_assert_cmpstr(v, ==, "SYS@#$");
    g_assert_true(ckd_init_parse_volser("A-1", v, &error_abort));
    g_assert_cmpstr(v, ==, "A-1");
    for (i = 0; i < ARRAY_SIZE(bad); i++) {
        Error *err = NULL;

        g_assert_false(ckd_init_parse_volser(bad[i], v, &err));
        g_assert_nonnull(err);
        error_free(err);
    }
}

static void expect_check_error(CkdInitParams *p)
{
    Error *err = NULL;

    g_assert_false(ckd_init_check(p, &err));
    g_assert_nonnull(err);
    error_free(err);
}

static void test_check(void)
{
    CkdInitParams p;

    init_params(&p, 3339);
    g_assert_cmpuint(p.vtoc_cylinder, ==, 0);
    g_assert_cmpuint(p.vtoc_head, ==, 1);
    g_assert_cmpuint(p.vtoc_tracks, ==, 14);
    g_assert_cmpuint(strlen(p.serial), ==, 12);

    init_params(&p, 1);                 /* VTOC fills the only cylinder */
    g_assert_cmpuint(ckd_init_vtoc_last(&p), ==, 14);

    memset(&p, 0, sizeof(p));
    p.cylinders = 65521;                /* extended address volume */
    p.header_only = true;
    expect_check_error(&p);
    p.cylinders = 0;
    expect_check_error(&p);

    memset(&p, 0, sizeof(p));
    p.cylinders = 10;                   /* no volser */
    expect_check_error(&p);

    init_params(&p, 10);
    p.vtoc_cylinder = 0;                /* track 0 holds the label */
    p.vtoc_head = 0;
    p.vtoc_tracks = 1;
    expect_check_error(&p);
    p.vtoc_head = 15;
    expect_check_error(&p);
    p.vtoc_cylinder = 9;                /* past the end of the volume */
    p.vtoc_head = 14;
    p.vtoc_tracks = 2;
    expect_check_error(&p);

    init_params(&p, 10017);
    p.vtoc_cylinder = 4368;             /* ends on relative track 65534 */
    p.vtoc_head = 14;
    p.vtoc_tracks = 1;
    g_assert_true(ckd_init_check(&p, &error_abort));
    p.vtoc_tracks = 2;                  /* reaches track 65535 */
    expect_check_error(&p);
}

static void test_header(void)
{
    CkdInitParams p;
    uint8_t h[CKD_INIT_HEADER_SIZE];
    size_t i;

    init_params(&p, 10017);
    memcpy(p.serial, "000000012345", 13);
    ckd_init_build_header(&p, h);
    g_assert_cmpmem(h, 8, "CKD_P064", 8);
    g_assert_cmpuint(ldl_le_p(h + 8), ==, 15);
    g_assert_cmpuint(ldl_le_p(h + 12), ==, 56832);
    g_assert_cmpuint(h[16], ==, 0x90);
    g_assert_cmpuint(h[17], ==, 0);
    g_assert_cmpuint(lduw_le_p(h + 18), ==, 10017);
    g_assert_cmpmem(h + 20, 12, "000000012345", 12);
    for (i = 32; i < sizeof(h); i++) {
        g_assert_cmpuint(h[i], ==, 0);
    }
    p.ckd64 = false;
    ckd_init_build_header(&p, h);
    g_assert_cmpmem(h, 8, "CKD_P370", 8);
}

static void test_track0(void)
{
    static const uint8_t ipl1[24] = {
        0x00, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    };
    static const uint8_t ipl2_ccws[43] = {
        0x07, 0x00, 0x3a, 0xb8, 0x40, 0x00, 0x00, 0x06,
        0x31, 0x00, 0x3a, 0xbe, 0x40, 0x00, 0x00, 0x05,
        0x08, 0x00, 0x3a, 0xa0, 0x00, 0x00, 0x00, 0x00,
        0x06, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x04,
    };
    /* "VOL1D9ECAT" in EBCDIC */
    static const uint8_t vol1[10] = {
        0xe5, 0xd6, 0xd3, 0xf1, 0xc4, 0xf9, 0xc5, 0xc3, 0xc1, 0xe3,
    };
    CkdInitParams p;
    g_autofree uint8_t *t = g_malloc(CKD_INIT_TRACK_SIZE);
    Record r[8];
    unsigned i;

    init_params(&p, 3339);
    pstrcpy(p.owner, sizeof(p.owner), "OWNER");
    g_assert_true(ckd_init_track_is_formatted(&p, 0));
    ckd_init_build_track(&p, 0, t);
    g_assert_cmpuint(parse_track(t, 0, r, 8), ==, 4);

    /* record 0: no key, 8 zero bytes */
    g_assert_cmpuint(r[0].key_length, ==, 0);
    g_assert_cmpuint(r[0].data_length, ==, 8);
    g_assert_cmpmem(r[0].data, 8, "\0\0\0\0\0\0\0\0", 8);

    g_assert_cmpuint(r[1].key_length, ==, 4);
    g_assert_cmpmem(r[1].key, 4, "\xc9\xd7\xd3\xf1", 4);      /* IPL1 */
    g_assert_cmpmem(r[1].data, r[1].data_length, ipl1, sizeof(ipl1));

    g_assert_cmpmem(r[2].key, 4, "\xc9\xd7\xd3\xf2", 4);      /* IPL2 */
    g_assert_cmpuint(r[2].data_length, ==, 144);
    g_assert_cmpmem(r[2].data, 43, ipl2_ccws, 43);
    for (i = 43; i < 144; i++) {
        g_assert_cmpuint(r[2].data[i], ==, 0);
    }

    g_assert_cmpmem(r[3].key, 4, vol1, 4);
    g_assert_cmpuint(r[3].data_length, ==, 80);
    g_assert_cmpmem(r[3].data, 10, vol1, 10);
    g_assert_cmpuint(r[3].data[10], ==, 0xc0);              /* security */
    g_assert_cmpmem(r[3].data + 11, 5, "\0\0\0\1\1", 5);    /* VTOC 0,1,1 */
    g_assert_cmpmem(r[3].data + 37, 5, "\xd6\xe6\xd5\xc5\xd9", 5);
    for (i = 16; i < 80; i++) {
        if (i < 37 || i >= 42) {
            g_assert_cmpuint(r[3].data[i], ==, 0x40);
        }
    }
}

static void check_vtoc(uint32_t cylinders, uint32_t vc, uint32_t vh,
                       uint32_t vt, uint8_t vtoci, unsigned extents,
                       const uint8_t *f5_extents)
{
    CkdInitParams p;
    g_autofree uint8_t *t = g_malloc(CKD_INIT_TRACK_SIZE);
    Record r[CKD_INIT_DSCBS_PER_TRACK + 1];
    uint32_t first, track;
    unsigned i, j;

    init_params(&p, cylinders);
    if (vt) {
        p.vtoc_cylinder = vc;
        p.vtoc_head = vh;
        p.vtoc_tracks = vt;
        g_assert_true(ckd_init_check(&p, &error_abort));
    }
    first = ckd_init_vtoc_first(&p);
    g_assert_false(ckd_init_track_is_formatted(&p, first - 1) && first > 1);
    g_assert_false(ckd_init_track_is_formatted(&p,
                                               ckd_init_vtoc_last(&p) + 1));

    for (track = first; track <= ckd_init_vtoc_last(&p); track++) {
        g_assert_true(ckd_init_track_is_formatted(&p, track));
        ckd_init_build_track(&p, track, t);
        g_assert_cmpuint(parse_track(t, track, r, ARRAY_SIZE(r)), ==,
                         CKD_INIT_DSCBS_PER_TRACK + 1);
        for (i = 1; i <= CKD_INIT_DSCBS_PER_TRACK; i++) {
            g_assert_cmpuint(r[i].key_length, ==, 44);
            g_assert_cmpuint(r[i].data_length, ==, 96);
            if (track == first && i <= 2) {
                continue;
            }
            for (j = 0; j < 140; j++) {                    /* format 0 */
                g_assert_cmpuint(r[i].key[j], ==, 0);
            }
        }
        if (track != first) {
            continue;
        }
        /* format 4 */
        for (j = 0; j < 44; j++) {
            g_assert_cmpuint(r[1].key[j], ==, 0x04);
        }
        g_assert_cmpuint(r[1].data[0], ==, 0xf4);
        g_assert_cmpuint(lduw_be_p(r[1].data + 1), ==, first / 15);
        g_assert_cmpuint(lduw_be_p(r[1].data + 3), ==, first % 15);
        g_assert_cmpuint(r[1].data[5], ==, 2);
        g_assert_cmpuint(lduw_be_p(r[1].data + 6), ==,
                         p.vtoc_tracks * 50 - 2);
        g_assert_cmpuint(r[1].data[14], ==, vtoci);
        g_assert_cmpuint(r[1].data[15], ==, 1);
        g_assert_cmpuint(lduw_be_p(r[1].data + 18), ==, cylinders);
        g_assert_cmpuint(lduw_be_p(r[1].data + 20), ==, 15);
        g_assert_cmpuint(lduw_be_p(r[1].data + 22), ==, 58786);
        g_assert_cmpuint(r[1].data[27], ==, 0x30);
        g_assert_cmpuint(r[1].data[30], ==, 50);
        g_assert_cmpuint(r[1].data[31], ==, 45);
        g_assert_cmpuint(r[1].data[61], ==, 1);
        g_assert_cmpuint(lduw_be_p(r[1].data + 63), ==, first / 15);
        g_assert_cmpuint(lduw_be_p(r[1].data + 65), ==, first % 15);
        g_assert_cmpuint(lduw_be_p(r[1].data + 67), ==,
                         ckd_init_vtoc_last(&p) / 15);
        g_assert_cmpuint(lduw_be_p(r[1].data + 69), ==,
                         ckd_init_vtoc_last(&p) % 15);
        for (j = 81; j < 96; j++) {                        /* no format 7 */
            g_assert_cmpuint(r[1].data[j], ==, 0);
        }
        /* format 5 */
        g_assert_cmpmem(r[2].key, 4, "\x05\x05\x05\x05", 4);
        g_assert_cmpmem(r[2].key + 4, extents * 5, f5_extents, extents * 5);
        for (j = 4 + extents * 5; j < 44; j++) {
            g_assert_cmpuint(r[2].key[j], ==, 0);
        }
        g_assert_cmpuint(r[2].data[0], ==, 0xf5);
        for (j = 1; j < 96; j++) {
            g_assert_cmpuint(r[2].data[j], ==, 0);
        }
    }
}

static void test_vtoc_default(void)
{
    /* relative track 15, 3338 cylinders, 0 tracks */
    check_vtoc(3339, 0, 0, 0, 0, 1, (const uint8_t *)"\0\x0f\x0d\x0a\0");
    /* 3390-54: free space still fits format 5, flagged for rebuilding */
    check_vtoc(65520, 0, 0, 0, 0x80, 1,
               (const uint8_t *)"\0\x0f\xff\xef\0");
    /* one cylinder: no free space */
    check_vtoc(1, 0, 0, 0, 0, 0, NULL);
}

static void test_vtoc_placed(void)
{
    /* VTOC at 10,3 for 20 tracks: free 1..152 and 173..1499 */
    static const uint8_t ext[] = {
        0x00, 0x01, 0x00, 0x0a, 0x02,   /* 152 tracks = 10 cyl + 2 */
        0x00, 0xad, 0x00, 0x58, 0x07,   /* 1327 tracks = 88 cyl + 7 */
    };

    check_vtoc(100, 10, 3, 20, 0, 2, ext);
}

static void test_header_only(void)
{
    CkdInitParams p = { .cylinders = 3339, .header_only = true };

    g_assert_true(ckd_init_check(&p, &error_abort));
    g_assert_false(ckd_init_track_is_formatted(&p, 0));
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ckd-init/models", test_models);
    g_test_add_func("/ckd-init/volser", test_volser);
    g_test_add_func("/ckd-init/check", test_check);
    g_test_add_func("/ckd-init/header", test_header);
    g_test_add_func("/ckd-init/track0", test_track0);
    g_test_add_func("/ckd-init/vtoc-default", test_vtoc_default);
    g_test_add_func("/ckd-init/vtoc-placed", test_vtoc_placed);
    g_test_add_func("/ckd-init/header-only", test_header_only);
    return g_test_run();
}
