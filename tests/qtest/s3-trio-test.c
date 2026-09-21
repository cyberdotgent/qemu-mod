/*
 * QTest testcase for the S3 Trio64 (s3-trio) display adapter
 *
 * Programs enhanced-mode linear framebuffer modes through the legacy VGA
 * and S3 extended CRTC registers, writes pixels into the linear
 * framebuffer (BAR 0) and checks the rendered output with a QMP
 * screendump.  Runs on the ppc 40p board (where the card is the default
 * display) and on the x86 pc machine with "-device s3-trio".
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

typedef struct S3Test {
    QTestState *qts;
    bool ppc;
    uint64_t io_base;     /* CPU address of legacy I/O port 0 */
    uint64_t pci_mem;     /* CPU address of PCI memory address 0 */
    uint32_t bar0_pci;    /* PCI address programmed into BAR 0 */
    uint64_t lfb;         /* CPU address of the linear framebuffer */
    int devfn;
} S3Test;

typedef struct Image {
    int width, height;
    uint8_t *rgb;
} Image;

/* ---- byte-order explicit access helpers ------------------------------ */

static void s3_outb(S3Test *t, uint16_t port, uint8_t val)
{
    if (t->ppc) {
        qtest_writeb(t->qts, t->io_base + port, val);
    } else {
        qtest_outb(t->qts, port, val);
    }
}

static uint8_t s3_inb(S3Test *t, uint16_t port)
{
    if (t->ppc) {
        return qtest_readb(t->qts, t->io_base + port);
    }
    return qtest_inb(t->qts, port);
}

static void G_GNUC_UNUSED s3_outw(S3Test *t, uint16_t port, uint16_t val)
{
    if (t->ppc) {
        uint8_t b[2] = { val & 0xff, val >> 8 };
        qtest_bufwrite(t->qts, t->io_base + port, b, 2);
    } else {
        qtest_outw(t->qts, port, val);
    }
}

static uint16_t G_GNUC_UNUSED s3_inw(S3Test *t, uint16_t port)
{
    if (t->ppc) {
        uint8_t b[2];
        qtest_memread(t->qts, t->io_base + port, b, 2);
        return b[0] | (b[1] << 8);
    }
    return qtest_inw(t->qts, port);
}

static void s3_outl(S3Test *t, uint16_t port, uint32_t val)
{
    if (t->ppc) {
        uint8_t b[4] = { val & 0xff, (val >> 8) & 0xff,
                         (val >> 16) & 0xff, val >> 24 };
        qtest_bufwrite(t->qts, t->io_base + port, b, 4);
    } else {
        qtest_outl(t->qts, port, val);
    }
}

static uint32_t s3_inl(S3Test *t, uint16_t port)
{
    if (t->ppc) {
        uint8_t b[4];
        qtest_memread(t->qts, t->io_base + port, b, 4);
        return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
    }
    return qtest_inl(t->qts, port);
}

static uint32_t cfg_read(S3Test *t, int devfn, int reg)
{
    s3_outl(t, 0xcf8, 0x80000000 | (devfn << 8) | (reg & 0xfc));
    return s3_inl(t, 0xcfc);
}

static void cfg_write(S3Test *t, int devfn, int reg, uint32_t val)
{
    s3_outl(t, 0xcf8, 0x80000000 | (devfn << 8) | (reg & 0xfc));
    s3_outl(t, 0xcfc, val);
}

static void crtc_w(S3Test *t, uint8_t index, uint8_t val)
{
    s3_outb(t, 0x3d4, index);
    s3_outb(t, 0x3d5, val);
}

static uint8_t crtc_r(S3Test *t, uint8_t index)
{
    s3_outb(t, 0x3d4, index);
    return s3_inb(t, 0x3d5);
}

static void seq_w(S3Test *t, uint8_t index, uint8_t val)
{
    s3_outb(t, 0x3c4, index);
    s3_outb(t, 0x3c5, val);
}

static void gfx_w(S3Test *t, uint8_t index, uint8_t val)
{
    s3_outb(t, 0x3ce, index);
    s3_outb(t, 0x3cf, val);
}

static void attr_w(S3Test *t, uint8_t index, uint8_t val)
{
    (void)s3_inb(t, 0x3da);   /* reset flip-flop */
    s3_outb(t, 0x3c0, index);
    s3_outb(t, 0x3c0, val);
}

static void dac_w(S3Test *t, uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    s3_outb(t, 0x3c8, index);
    s3_outb(t, 0x3c9, r >> 2);
    s3_outb(t, 0x3c9, g >> 2);
    s3_outb(t, 0x3c9, b >> 2);
}

/* ---- setup ------------------------------------------------------------ */

static void s3_test_init(S3Test *t)
{
    const char *arch = qtest_get_arch();
    int devfn;

    memset(t, 0, sizeof(*t));
    if (g_str_equal(arch, "ppc")) {
        t->ppc = true;
        t->io_base = 0x80000000;
        t->pci_mem = 0xc0000000;
        t->bar0_pci = 0x10000000;
        t->qts = qtest_init("-M 40p");
    } else {
        t->io_base = 0;
        t->pci_mem = 0;
        t->bar0_pci = 0xe0000000;
        t->qts = qtest_init("-M pc -vga none -device s3-trio");
    }

    for (devfn = 0; devfn < 256; devfn += 8) {
        if (cfg_read(t, devfn, 0) == 0x88115333) {
            break;
        }
    }
    g_assert_cmpint(devfn, <, 256);
    t->devfn = devfn;

    cfg_write(t, devfn, 0x10, t->bar0_pci);
    g_assert_cmphex(cfg_read(t, devfn, 0x10) & 0xfffffff0, ==, t->bar0_pci);
    cfg_write(t, devfn, 0x04, cfg_read(t, devfn, 0x04) | 0x3);
    t->lfb = t->pci_mem + t->bar0_pci;

    /* misc output: colour I/O at 0x3Dx, RAM enabled */
    s3_outb(t, 0x3c2, 0x23);
}

static void s3_test_fini(S3Test *t)
{
    qtest_quit(t->qts);
}

/*
 * Program an S3 enhanced (linear, packed pixel) mode.  bpp is 8, 15, 16,
 * 24 or 32.  pitch is in bytes.
 */
static void s3_set_mode(S3Test *t, int width, int height, int bpp, int pitch)
{
    int bytes_per_pixel = (bpp + 7) / 8;
    int hdisp, vdisp, i;
    uint8_t cr67, cr50;

    /* misc output: colour I/O at 0x3Dx, RAM enabled */
    s3_outb(t, 0x3c2, 0x23);

    /* unlock S3 extended registers */
    crtc_w(t, 0x38, 0x48);
    crtc_w(t, 0x39, 0xa5);
    seq_w(t, 0x08, 0x06);

    seq_w(t, 0x00, 0x03);
    seq_w(t, 0x01, 0x01);
    seq_w(t, 0x02, 0x0f);
    seq_w(t, 0x03, 0x00);
    seq_w(t, 0x04, 0x0e);

    for (i = 0; i < 9; i++) {
        gfx_w(t, i, 0);
    }
    gfx_w(t, 0x05, 0x40);
    gfx_w(t, 0x06, 0x05);
    gfx_w(t, 0x07, 0x0f);
    gfx_w(t, 0x08, 0xff);

    /*
     * CR01 counts character clocks of 8 dot clocks.  On the Trio64 the
     * CRTC runs at twice the pixel clock in 15/16bpp and at three dots per
     * pixel in 24bpp, so the horizontal display end is programmed in
     * bytes/8 - 1 for those depths and in pixels/8 - 1 for 8 and 32bpp.
     */
    switch (bpp) {
    case 15:
    case 16:
        hdisp = width * 2 / 8 - 1;
        break;
    case 24:
        hdisp = width * 3 / 8 - 1;
        break;
    default:
        hdisp = width / 8 - 1;
        break;
    }
    vdisp = height - 1;

    crtc_w(t, 0x11, 0x00);                 /* unlock CR0-7 */
    crtc_w(t, 0x00, (hdisp + 8) & 0xff);
    crtc_w(t, 0x01, hdisp & 0xff);
    crtc_w(t, 0x02, hdisp & 0xff);
    crtc_w(t, 0x03, 0x80);
    crtc_w(t, 0x04, (hdisp + 4) & 0xff);
    crtc_w(t, 0x05, 0x00);
    crtc_w(t, 0x06, (vdisp + 40) & 0xff);
    /* overflow: vdisp bits 8/9 -> CR07 bits 1/6, line compare bit 8 set */
    crtc_w(t, 0x07, 0x10 | ((vdisp >> 7) & 0x02) | ((vdisp >> 3) & 0x40));
    crtc_w(t, 0x08, 0x00);
    crtc_w(t, 0x09, 0x40);                 /* line compare bit 9, no double scan */
    crtc_w(t, 0x0a, 0x00);
    crtc_w(t, 0x0b, 0x00);
    crtc_w(t, 0x0c, 0x00);
    crtc_w(t, 0x0d, 0x00);
    crtc_w(t, 0x10, (vdisp + 10) & 0xff);
    crtc_w(t, 0x12, vdisp & 0xff);
    crtc_w(t, 0x13, (pitch / 8) & 0xff);
    crtc_w(t, 0x14, 0x40);
    crtc_w(t, 0x15, vdisp & 0xff);
    crtc_w(t, 0x16, (vdisp + 40) & 0xff);
    crtc_w(t, 0x17, 0xe3);
    crtc_w(t, 0x18, 0xff);

    /* S3 extended CRTC: horizontal/vertical overflow */
    crtc_w(t, 0x5d, (hdisp & 0x100) ? 0x02 : 0x00);
    crtc_w(t, 0x5e, (vdisp & 0x400) ? 0x02 : 0x00);
    /* logical line width bits 9-8 in CR51 bits 5-4 */
    crtc_w(t, 0x51, ((pitch / 8) >> 4) & 0x30);
    /* display start address bits 19-16 */
    crtc_w(t, 0x69, 0x00);

    switch (width) {
    case 640:
        cr50 = 0x40;
        break;
    case 800:
        cr50 = 0x80;
        break;
    case 1024:
        cr50 = 0x00;
        break;
    case 1152:
        cr50 = 0x01;
        break;
    case 1280:
        cr50 = 0xc0;
        break;
    case 1600:
        cr50 = 0x81;
        break;
    default:
        cr50 = 0x00;
        break;
    }
    switch (bytes_per_pixel) {
    case 2:
        cr50 |= 0x10;
        break;
    case 4:
        cr50 |= 0x30;
        break;
    default:
        break;
    }
    switch (bpp) {
    case 15:
        cr67 = 0x30;
        break;
    case 16:
        cr67 = 0x50;
        break;
    case 24:
        cr67 = 0x70;
        break;
    case 32:
        cr67 = 0xd0;
        break;
    default:
        cr67 = 0x00;
        break;
    }
    crtc_w(t, 0x31, 0x89);   /* enhanced memory mapping, bank enable */
    crtc_w(t, 0x3a, 0x15);   /* enhanced mode */
    crtc_w(t, 0x50, cr50);
    crtc_w(t, 0x53, 0x00);
    crtc_w(t, 0x58, 0x13);   /* linear addressing, 8MB window */
    crtc_w(t, 0x67, cr67);

    for (i = 0; i < 16; i++) {
        attr_w(t, i, i);
    }
    attr_w(t, 0x10, 0x41);
    attr_w(t, 0x11, 0x00);
    attr_w(t, 0x12, 0x0f);
    attr_w(t, 0x13, 0x00);
    attr_w(t, 0x14, 0x00);
    (void)s3_inb(t, 0x3da);
    s3_outb(t, 0x3c0, 0x20);  /* palette address source: display */
    s3_outb(t, 0x3c6, 0xff);
}

/* ---- screendump ------------------------------------------------------- */

static void image_free(Image *img)
{
    g_free(img->rgb);
    img->rgb = NULL;
}

static void screendump(S3Test *t, Image *img)
{
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    gsize len;
    int fd, maxval, n;
    char *p;

    fd = g_file_open_tmp("qtest-s3-XXXXXX.ppm", &path, NULL);
    g_assert(fd >= 0);
    close(fd);

    qtest_qmp_assert_success(t->qts,
        "{ 'execute': 'screendump', 'arguments': { 'filename': %s } }",
        path);

    g_assert(g_file_get_contents(path, &contents, &len, NULL));
    unlink(path);

    g_assert(len > 3 && contents[0] == 'P' && contents[1] == '6');
    n = sscanf(contents + 2, "%d %d %d", &img->width, &img->height, &maxval);
    g_assert_cmpint(n, ==, 3);
    g_assert_cmpint(maxval, ==, 255);
    /* skip the three header tokens and the single whitespace after maxval */
    p = contents + 2;
    for (n = 0; n < 3; n++) {
        while (g_ascii_isspace(*p)) {
            p++;
        }
        while (!g_ascii_isspace(*p)) {
            p++;
        }
    }
    p++;
    g_assert_cmpint(len - (p - contents), ==, img->width * img->height * 3);
    img->rgb = g_memdup2(p, img->width * img->height * 3);
}

static void check_pixel(Image *img, int x, int y, int r, int g, int b)
{
    const uint8_t *p;

    g_assert_cmpint(x, <, img->width);
    g_assert_cmpint(y, <, img->height);
    p = img->rgb + (y * img->width + x) * 3;
    /* allow for the loss of low bits in 15/16bpp modes */
    if (abs(p[0] - r) > 8 || abs(p[1] - g) > 8 || abs(p[2] - b) > 8) {
        g_test_message("pixel (%d,%d) = (%d,%d,%d), expected (%d,%d,%d)",
                       x, y, p[0], p[1], p[2], r, g, b);
        g_assert_not_reached();
    }
}

/* ---- framebuffer helpers --------------------------------------------- */

static void put_pixel(S3Test *t, int pitch, int bpp, int x, int y,
                      uint32_t val)
{
    uint8_t b[4] = { val & 0xff, (val >> 8) & 0xff, (val >> 16) & 0xff,
                     val >> 24 };
    int bytes = (bpp + 7) / 8;

    qtest_bufwrite(t->qts, t->lfb + y * pitch + x * bytes, b, bytes);
}

static void fill_rect(S3Test *t, int pitch, int bpp, int x, int y,
                      int w, int h, uint32_t val)
{
    int bytes = (bpp + 7) / 8;
    g_autofree uint8_t *line = g_malloc(w * bytes);
    int i;

    for (i = 0; i < w; i++) {
        line[i * bytes] = val & 0xff;
        if (bytes > 1) {
            line[i * bytes + 1] = (val >> 8) & 0xff;
        }
        if (bytes > 2) {
            line[i * bytes + 2] = (val >> 16) & 0xff;
        }
        if (bytes > 3) {
            line[i * bytes + 3] = val >> 24;
        }
    }
    for (i = 0; i < h; i++) {
        qtest_bufwrite(t->qts, t->lfb + (y + i) * pitch + x * bytes,
                       line, w * bytes);
    }
}

static void clear_fb(S3Test *t, int bytes)
{
    g_autofree uint8_t *zero = g_malloc0(65536);
    int off;

    for (off = 0; off < bytes; off += 65536) {
        qtest_bufwrite(t->qts, t->lfb + off, zero, MIN(65536, bytes - off));
    }
}

/* ---- tests ------------------------------------------------------------ */

static void test_ids(void)
{
    S3Test t;

    s3_test_init(&t);
    /* locked: chip ID reads back as 0xff */
    crtc_w(&t, 0x38, 0x00);
    crtc_w(&t, 0x39, 0x00);
    g_assert_cmphex(crtc_r(&t, 0x30), ==, 0xff);
    crtc_w(&t, 0x38, 0x48);
    g_assert_cmphex(crtc_r(&t, 0x30), ==, 0xe1);   /* Trio64 */
    g_assert_cmphex(crtc_r(&t, 0x2d), ==, 0x88);
    g_assert_cmphex(crtc_r(&t, 0x2e), ==, 0x11);
    /* CR4x is protected by CR39 */
    crtc_w(&t, 0x50, 0xc0);
    g_assert_cmphex(crtc_r(&t, 0x50), !=, 0xc0);
    crtc_w(&t, 0x39, 0xa5);
    crtc_w(&t, 0x50, 0xc0);
    g_assert_cmphex(crtc_r(&t, 0x50), ==, 0xc0);
    s3_test_fini(&t);
}

static void test_mode_8bpp(void)
{
    S3Test t;
    Image img;
    const int w = 640, h = 480, pitch = 640;

    s3_test_init(&t);
    s3_set_mode(&t, w, h, 8, pitch);
    dac_w(&t, 0, 0, 0, 0);
    dac_w(&t, 1, 255, 0, 0);
    dac_w(&t, 2, 0, 255, 0);
    dac_w(&t, 3, 0, 0, 255);
    clear_fb(&t, pitch * h);
    fill_rect(&t, pitch, 8, 0, 0, 16, 16, 1);
    fill_rect(&t, pitch, 8, 320, 240, 8, 8, 2);
    put_pixel(&t, pitch, 8, w - 1, h - 1, 3);

    screendump(&t, &img);
    g_assert_cmpint(img.width, ==, w);
    g_assert_cmpint(img.height, ==, h);
    check_pixel(&img, 0, 0, 255, 0, 0);
    check_pixel(&img, 15, 15, 255, 0, 0);
    check_pixel(&img, 16, 16, 0, 0, 0);
    check_pixel(&img, 323, 243, 0, 255, 0);
    check_pixel(&img, w - 1, h - 1, 0, 0, 255);
    check_pixel(&img, w - 2, h - 1, 0, 0, 0);
    image_free(&img);
    s3_test_fini(&t);
}

static void test_mode_16bpp(void)
{
    S3Test t;
    Image img;
    const int w = 640, h = 480, pitch = 1280;

    s3_test_init(&t);
    s3_set_mode(&t, w, h, 16, pitch);
    clear_fb(&t, pitch * h);
    fill_rect(&t, pitch, 16, 0, 0, 16, 16, 0xf800);          /* red */
    fill_rect(&t, pitch, 16, 320, 240, 8, 8, 0x07e0);        /* green */
    put_pixel(&t, pitch, 16, w - 1, h - 1, 0x001f);          /* blue */
    fill_rect(&t, pitch, 16, 100, 400, 4, 4, 0xffff);        /* white */

    screendump(&t, &img);
    g_assert_cmpint(img.width, ==, w);
    g_assert_cmpint(img.height, ==, h);
    check_pixel(&img, 0, 0, 255, 0, 0);
    check_pixel(&img, 15, 15, 255, 0, 0);
    check_pixel(&img, 16, 0, 0, 0, 0);
    check_pixel(&img, 323, 243, 0, 255, 0);
    check_pixel(&img, w - 1, h - 1, 0, 0, 255);
    check_pixel(&img, w - 2, h - 1, 0, 0, 0);
    check_pixel(&img, 102, 402, 255, 255, 255);
    image_free(&img);
    s3_test_fini(&t);
}

static void test_mode_15bpp(void)
{
    S3Test t;
    Image img;
    const int w = 640, h = 480, pitch = 1280;

    s3_test_init(&t);
    s3_set_mode(&t, w, h, 15, pitch);
    clear_fb(&t, pitch * h);
    fill_rect(&t, pitch, 15, 0, 0, 16, 16, 0x7c00);          /* red */
    fill_rect(&t, pitch, 15, 320, 240, 8, 8, 0x03e0);        /* green */
    put_pixel(&t, pitch, 15, w - 1, h - 1, 0x001f);          /* blue */

    screendump(&t, &img);
    g_assert_cmpint(img.width, ==, w);
    g_assert_cmpint(img.height, ==, h);
    check_pixel(&img, 0, 0, 255, 0, 0);
    check_pixel(&img, 323, 243, 0, 255, 0);
    check_pixel(&img, w - 1, h - 1, 0, 0, 255);
    image_free(&img);
    s3_test_fini(&t);
}

static void test_mode_24bpp(void)
{
    S3Test t;
    Image img;
    const int w = 640, h = 480, pitch = 1920;

    s3_test_init(&t);
    s3_set_mode(&t, w, h, 24, pitch);
    clear_fb(&t, pitch * h);
    fill_rect(&t, pitch, 24, 0, 0, 16, 16, 0xff0000);        /* red */
    fill_rect(&t, pitch, 24, 320, 240, 8, 8, 0x00ff00);      /* green */
    put_pixel(&t, pitch, 24, w - 1, h - 1, 0x0000ff);        /* blue */

    screendump(&t, &img);
    g_assert_cmpint(img.width, ==, w);
    g_assert_cmpint(img.height, ==, h);
    check_pixel(&img, 0, 0, 255, 0, 0);
    check_pixel(&img, 16, 0, 0, 0, 0);
    check_pixel(&img, 323, 243, 0, 255, 0);
    check_pixel(&img, w - 1, h - 1, 0, 0, 255);
    image_free(&img);
    s3_test_fini(&t);
}

static void test_mode_32bpp(void)
{
    S3Test t;
    Image img;
    const int w = 320, h = 240, pitch = 1280;

    s3_test_init(&t);
    s3_set_mode(&t, w, h, 32, pitch);
    clear_fb(&t, pitch * h);
    fill_rect(&t, pitch, 32, 0, 0, 16, 16, 0x00ff0000);      /* red */
    fill_rect(&t, pitch, 32, 160, 120, 8, 8, 0x0000ff00);    /* green */
    put_pixel(&t, pitch, 32, w - 1, h - 1, 0x000000ff);      /* blue */
    fill_rect(&t, pitch, 32, 100, 200, 4, 4, 0x00ffffff);    /* white */

    screendump(&t, &img);
    g_assert_cmpint(img.width, ==, w);
    g_assert_cmpint(img.height, ==, h);
    check_pixel(&img, 0, 0, 255, 0, 0);
    check_pixel(&img, 16, 0, 0, 0, 0);
    check_pixel(&img, 163, 123, 0, 255, 0);
    check_pixel(&img, w - 1, h - 1, 0, 0, 255);
    check_pixel(&img, 102, 202, 255, 255, 255);
    image_free(&img);
    s3_test_fini(&t);
}

int main(int argc, char **argv)
{
    const char *arch = qtest_get_arch();

    g_test_init(&argc, &argv, NULL);

    if (g_str_equal(arch, "ppc")) {
        if (!qtest_has_machine("40p")) {
            return 0;
        }
    } else if (!qtest_has_device("s3-trio")) {
        return 0;
    }

    qtest_add_func("/s3-trio/ids", test_ids);
    qtest_add_func("/s3-trio/mode/8bpp", test_mode_8bpp);
    qtest_add_func("/s3-trio/mode/15bpp", test_mode_15bpp);
    qtest_add_func("/s3-trio/mode/16bpp", test_mode_16bpp);
    qtest_add_func("/s3-trio/mode/24bpp", test_mode_24bpp);
    qtest_add_func("/s3-trio/mode/32bpp", test_mode_32bpp);

    return g_test_run();
}
