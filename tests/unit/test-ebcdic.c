/*
 * EBCDIC code page tables and console character set helpers
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/s390x/ebcdic.h"

static void test_round_trip(gconstpointer opaque)
{
    uint32_t codepage = GPOINTER_TO_UINT(opaque);
    const uint8_t *to_latin1 = ebcdic_to_latin1_table(codepage);
    const uint8_t *to_ebcdic = latin1_to_ebcdic_table(codepage);
    unsigned i;

    for (i = 0; i < 256; i++) {
        g_assert_cmphex(to_ebcdic[to_latin1[i]], ==, i);
        g_assert_cmphex(to_latin1[to_ebcdic[i]], ==, i);
    }
}

typedef struct {
    char latin1;
    uint8_t cp037;
    uint8_t cp500;
} CodePoint;

/* The characters in which the two code pages differ, and a few others. */
static const CodePoint code_points[] = {
    { ' ', 0x40, 0x40 },
    { 'A', 0xc1, 0xc1 },
    { 'z', 0xa9, 0xa9 },
    { '0', 0xf0, 0xf0 },
    { '.', 0x4b, 0x4b },
    { '\n', 0x15, 0x15 },
    { '|', 0x4f, 0xbb },
    { '!', 0x5a, 0x4f },
    { '[', 0xba, 0x4a },
    { ']', 0xbb, 0x5a },
    { '^', 0xb0, 0x5f },
    { '\xac', 0x5f, 0xba },     /* not sign */
    { '\xa2', 0x4a, 0xb0 },     /* cent sign */
    { '$', 0x5b, 0x5b },
    { '#', 0x7b, 0x7b },
    { '@', 0x7c, 0x7c },
};

static void test_code_points(void)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(code_points); i++) {
        uint8_t c = code_points[i].latin1;

        g_assert_cmphex(latin12ebcdic_037[c], ==, code_points[i].cp037);
        g_assert_cmphex(latin12ebcdic_500[c], ==, code_points[i].cp500);
        g_assert_cmphex(ebcdic2latin1_037[code_points[i].cp037], ==, c);
        g_assert_cmphex(ebcdic2latin1_500[code_points[i].cp500], ==, c);
    }
    /* Fixed text uses code page 037. */
    g_assert_cmphex(ascii2ebcdic['|'], ==, 0x4f);
    g_assert_cmphex(ebcdic2ascii[0x4f], ==, '|');
    g_assert(ebcdic_codepage_valid(37));
    g_assert(ebcdic_codepage_valid(500));
    g_assert(!ebcdic_codepage_valid(0));
    g_assert(!ebcdic_codepage_valid(1047));
}

static void test_utf8(void)
{
    const uint8_t latin1[] = { 'a', 0xa2, 0xac, 0xff, '|' };
    const uint8_t expected[] = {
        'a', 0xc2, 0xa2, 0xc2, 0xac, 0xc3, 0xbf, '|',
    };
    /* a, not sign, U+20AC (euro, not in ISO 8859-1), stray byte, b */
    const uint8_t input[] = {
        'a', 0xc2, 0xac, 0xe2, 0x82, 0xac, 0x80, 'b', 0xc3, 'c',
    };
    const int decoded[] = { 'a', 0xac, '?', '?', 'b', 'c' };
    uint8_t out[sizeof(latin1) * 2];
    Utf8Decoder d = { 0 };
    int n, i, c;

    n = latin1_to_utf8(out, latin1, sizeof(latin1));
    g_assert_cmpint(n, ==, sizeof(expected));
    g_assert(memcmp(out, expected, n) == 0);

    n = 0;
    for (i = 0; i < sizeof(input); i++) {
        c = utf8_decode_latin1(&d, input[i]);
        if (c >= 0) {
            g_assert_cmpint(n, <, ARRAY_SIZE(decoded));
            g_assert_cmphex(c, ==, decoded[n++]);
        }
    }
    g_assert_cmpint(n, ==, ARRAY_SIZE(decoded));

    /* Every ISO 8859-1 character survives the UTF-8 round trip. */
    for (i = 0; i < 256; i++) {
        uint8_t ch = i, utf8[2];
        int j, len = latin1_to_utf8(utf8, &ch, 1);

        c = -1;
        for (j = 0; j < len; j++) {
            c = utf8_decode_latin1(&d, utf8[j]);
        }
        g_assert_cmphex(c, ==, i);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_data_func("/ebcdic/round-trip/037",
                         GUINT_TO_POINTER(EBCDIC_CP037), test_round_trip);
    g_test_add_data_func("/ebcdic/round-trip/500",
                         GUINT_TO_POINTER(EBCDIC_CP500), test_round_trip);
    g_test_add_func("/ebcdic/code-points", test_code_points);
    g_test_add_func("/ebcdic/utf8", test_utf8);
    return g_test_run();
}
