/*
 * QEMU PCI S3 Trio (VGA compatible)
 *
 * Copyright (c) 2017 Hervé Poussineau
 *
 * Parts of the S3 Trio64 register semantics, tables and 2D engine
 * algorithms are derived from 86Box's vid_s3.c:
 *   Copyright 2008-2019 Sarah Walker.
 *   Copyright 2016-2019 Miran Grca.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * S3 Trio64 (86C764): VGA core plus the S3 extended CRTC registers,
 * hardware cursor and the 8514-style 2D graphics engine with its I/O and
 * memory-mapped register interfaces.  The Trio64V+ streams processor is
 * not modelled.
 */

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "vga_int.h"
#include "hw/display/vga.h"
#include "hw/display/vga_regs.h"
#include "qom/object.h"
#include "trace.h"
#include "qemu/log.h"
#include "system/reset.h"
#include "qemu/range.h"
#include "ui/pixel_ops.h"

#define TYPE_S3_TRIO "s3-trio"

/*
 * 8514/A-style graphics engine registers.  Ports are xxE8 in I/O space; the
 * same offsets are used inside the memory-mapped I/O window (0xA0000 +
 * port) and, for the packed register file, at 0x8100-0x816F.
 */
enum {
    PORT_SUBSYS_STAT    = 0x42e8, /* read: status, write: control */
    PORT_SETUP_MD       = 0x46e8,
    PORT_ADVFUNC_CNTL   = 0x4ae8,
    PORT_CUR_Y          = 0x82e8,
    PORT_CUR_Y2         = 0x82ea,
    PORT_CUR_X          = 0x86e8,
    PORT_CUR_X2         = 0x86ea,
    PORT_DESTY_AXSTP    = 0x8ae8,
    PORT_DESTY_AXSTP2   = 0x8aea,
    PORT_DESTX_DIASTP   = 0x8ee8,
    PORT_X2             = 0x8eea,
    PORT_ERR_TERM       = 0x92e8,
    PORT_ERR_TERM2      = 0x92ea,
    PORT_MAJ_AXIS_PCNT  = 0x96e8,
    PORT_MAJ_AXIS_PCNT2 = 0x96ea,
    PORT_CMD            = 0x9ae8, /* read: GP_STAT */
    PORT_CMD2           = 0x9aea,
    PORT_SHORT_STROKE   = 0x9ee8,
    PORT_BKGD_COLOR     = 0xa2e8,
    PORT_FRGD_COLOR     = 0xa6e8,
    PORT_WRT_MASK       = 0xaae8,
    PORT_RD_MASK        = 0xaee8,
    PORT_COLOR_CMP      = 0xb2e8,
    PORT_BKGD_MIX       = 0xb6e8,
    PORT_FRGD_MIX       = 0xbae8,
    PORT_MULTIFUNC_CNTL = 0xbee8,
    PORT_PIX_TRANS      = 0xe2e8,
};

enum {
    DISP_STAT_SENSE = 0x0001,
};

/* GP_STAT (read of 0x9AE8) */
enum {
    GP_STAT_DATA_AVAIL = 0x0100,
    GP_STAT_BUSY       = 0x0200,
    GP_STAT_FIFO_EMPTY = 0x0400,
};

/* CMD register (0x9AE8) */
enum {
    CMD_WRTDATA  = 0x0001, /* write data (pixel transfer direction) */
    CMD_PLANAR   = 0x0002, /* "across the plane": CPU data is a mono mask */
    CMD_LASTPIX  = 0x0004, /* last pixel off */
    CMD_LINETYPE = 0x0008, /* 1 = radial (vector) line, 0 = Bresenham */
    CMD_DRAW     = 0x0010, /* write to video memory */
    CMD_INC_X    = 0x0020,
    CMD_YMAJAXIS = 0x0040,
    CMD_INC_Y    = 0x0080,
    CMD_PCDATA   = 0x0100, /* wait for CPU data through PIX_TRANS */
    CMD_BUS_MASK = 0x0600, /* 0 = 8 bit, 0x200 = 16 bit, 0x400 = 32 bit */
    CMD_BUS_16   = 0x0200,
    CMD_BUS_32   = 0x0400,
    CMD_EXT      = 0x0800, /* Trio64: extended command set (bit 3 of the op) */
    CMD_BYTSEQ   = 0x1000, /* swap bytes of 16-bit mono transfers */
};

/* command opcode: bits 15-13, extended by bit 11 on the Trio64 */
enum {
    OP_NOP        = 0,  /* short stroke vectors */
    OP_LINE       = 1,
    OP_RECT       = 2,
    OP_POLY_SOLID = 3,
    OP_BITBLT     = 6,
    OP_PATBLT     = 7,
    OP_LINE_2PT   = 9,
    OP_POLY_PAT   = 11,
};

/* MULTIFUNC_CNTL indices */
enum {
    MF_MIN_AXIS_PCNT = 0x0,
    MF_SCISSORS_T    = 0x1,
    MF_SCISSORS_L    = 0x2,
    MF_SCISSORS_B    = 0x3,
    MF_SCISSORS_R    = 0x4,
    MF_PIX_CNTL      = 0xa,
    MF_MULT_MISC2    = 0xd,
    MF_MULT_MISC     = 0xe,
    MF_READ_SEL      = 0xf,
};

/* PIX_CNTL (MULTIFUNC index 0xA) bits 7-6: mix select */
#define PIX_CNTL_MIXSEL_MASK 0x00c0
enum {
    PIX_CNTL_MIXSEL_FOREMIX = 0x0000,
    PIX_CNTL_MIXSEL_CPU     = 0x0080, /* CPU data is a mono mask */
    PIX_CNTL_MIXSEL_VRAM    = 0x00c0, /* video memory is a mono mask */
};

/* MULT_MISC (index 0xE) */
enum {
    MULT_MISC_HIGH_WORD  = 0x010, /* next colour register byte pair is 31-16 */
    MULT_MISC_CLIP_OUT   = 0x020, /* draw outside the scissors instead */
    MULT_MISC_CMP_SENSE  = 0x080, /* 1 = update when equal */
    MULT_MISC_CMP_ENABLE = 0x100,
    MULT_MISC_32BIT_REGS = 0x200, /* colour registers written as 32 bits */
};

/* mix registers: bits 6-5 colour source, bits 3-0 mix (ROP) */
#define MIX_SRC_MASK 0x60
enum {
    MIX_SRC_BKGD   = 0x00,
    MIX_SRC_FRGD   = 0x20,
    MIX_SRC_CPU    = 0x40,
    MIX_SRC_BITMAP = 0x60,
};

typedef struct S3TrioState {
    PCIDevice dev;
    VGACommonState vga;
    PortioList portio;

    uint32_t dclk;
    uint32_t mclk;

    uint16_t disp_stat;    /* 02e8 (R) */
    uint16_t subsys_cntl;  /* 42e8 (W) */
    uint16_t subsys_stat;  /* 42e8 (R) */
    uint16_t setup_md;     /* 46e8 */
    uint16_t advfunc_cntl; /* 4ae8 */

    /* graphics engine registers */
    uint16_t cur_x, cur_y, cur_x2, cur_y2;
    int16_t desty_axstp, desty_axstp2, destx_diastp, x2;
    int16_t err_term, err_term2;
    uint16_t maj_axis_pcnt, maj_axis_pcnt2;
    uint16_t cmd, cmd2;
    uint16_t short_stroke;
    uint32_t bkgd_color, frgd_color;
    uint32_t wrt_mask, rd_mask;
    uint32_t color_cmp;
    uint8_t bkgd_mix, frgd_mix;
    uint16_t multifunc_cntl;
    uint16_t mfc[16];
    uint8_t read_sel;
    uint8_t pix_trans[4];

    /* graphics engine working state */
    int32_t cx, cy, dx, dy, sx, sy;
    uint32_t src, dest, pattern;
    int32_t poly_cx, poly_cy, poly_cx2, poly_cy2, poly_dx1, poly_dx2, poly_x;
    uint8_t point_1_updated, point_2_updated;
    uint8_t ssv_state, ssv_len, ssv_dir, ssv_draw;
    uint32_t dat_buf;
    uint8_t dat_count;
    uint8_t busy; /* a CMD_PCDATA command is waiting for CPU data */

    uint8_t unlock_pll;

    /* hardware cursor (CR45-CR4F, CR55) */
    uint32_t hwc_fg_col;
    uint32_t hwc_bg_col;
    uint8_t hwc_col_stack_pos;
    int last_hwc_x;
    int last_hwc_y;
    int last_hwc_ysize;

    /* extended display start address bits 20-16 (CR31/CR51/CR69) */
    uint8_t ma_ext;
    /* 64K/16K CPU bank (CR35/CR51/CR6A) */
    uint8_t bank;
    MemoryRegion vga_mem;
} S3TrioState;

OBJECT_DECLARE_SIMPLE_TYPE(S3TrioState, S3_TRIO)

static bool s3_enhanced_mode(S3TrioState *s);
static void s3_trio_vga_ioport_write(void *opaque, uint32_t addr, uint32_t val);
static uint32_t s3_trio_vga_ioport_read(void *opaque, uint32_t addr);

/*
 * ---- Graphics engine ---------------------------------------------------
 *
 * The algorithms below follow 86Box's s3_accel_start() and
 * s3_accel_out_fifo() for the Trio64: the engine draws with a pitch taken
 * from CR50 (screen width) and CR31 bit 1, a pixel size from CR50 bits 5-4,
 * a 16-entry mix (ROP) table selected per pixel by a mono mask, a write
 * mask, an optional colour compare and a scissors rectangle.
 */

/* pixel size code from CR50 bits 5-4: 0 = 8bpp, 1 = 16bpp, 2 = 24, 3 = 32 */
static int s3_accel_bpp(S3TrioState *s)
{
    return (s->vga.cr[0x50] >> 4) & 3;
}

static int s3_accel_pixel_bytes(S3TrioState *s)
{
    return s3_accel_bpp(s) + 1;
}

/* engine pitch in pixels from CR50 bits 7-6 and 0 (86Box s3_recalctimings) */
static int s3_accel_width(S3TrioState *s)
{
    uint8_t *cr = s->vga.cr;

    switch (cr[0x50] & 0xc1) {
    case 0x00:
        return (cr[0x31] & 0x02) ? 2048 : 1024;
    case 0x01:
        return 1152;
    case 0x40:
        return 640;
    case 0x80:
        return ((s->advfunc_cntl & 0x04) && !s3_enhanced_mode(s)) ? 1600 : 800;
    case 0x81:
        return 1600;
    case 0xc0:
        return 1280;
    default:
        return 1024;
    }
}

static uint32_t s3_accel_read_pixel(S3TrioState *s, uint32_t addr)
{
    VGACommonState *vga = &s->vga;
    uint32_t mask = vga->vbe_size_mask;

    addr &= mask;
    switch (s3_accel_bpp(s)) {
    case 0:
        return vga->vram_ptr[addr];
    case 1:
        if (addr + 1 > mask) {
            return 0;
        }
        return lduw_le_p(vga->vram_ptr + addr);
    case 2:
        if (addr + 3 > mask) {
            return 0;
        }
        return ldl_le_p(vga->vram_ptr + addr) & 0xffffff;
    default:
        if (addr + 3 > mask) {
            return 0;
        }
        return ldl_le_p(vga->vram_ptr + addr);
    }
}

static void s3_accel_write_pixel(S3TrioState *s, uint32_t addr, uint32_t val)
{
    VGACommonState *vga = &s->vga;
    uint32_t mask = vga->vbe_size_mask;

    addr &= mask;
    switch (s3_accel_bpp(s)) {
    case 0:
        vga->vram_ptr[addr] = val;
        memory_region_set_dirty(&vga->vram, addr, 1);
        break;
    case 1:
        if (addr + 1 > mask) {
            return;
        }
        stw_le_p(vga->vram_ptr + addr, val);
        memory_region_set_dirty(&vga->vram, addr, 2);
        break;
    case 2:
        if (addr + 2 > mask) {
            return;
        }
        vga->vram_ptr[addr] = val;
        vga->vram_ptr[addr + 1] = val >> 8;
        vga->vram_ptr[addr + 2] = val >> 16;
        memory_region_set_dirty(&vga->vram, addr, 3);
        break;
    default:
        if (addr + 3 > mask) {
            return;
        }
        stl_le_p(vga->vram_ptr + addr, val);
        memory_region_set_dirty(&vga->vram, addr, 4);
        break;
    }
}

/* the 16 mixes (raster operations) of the S3/8514 mix registers */
static uint32_t s3_mix(int mix, uint32_t src, uint32_t dst)
{
    switch (mix & 0xf) {
    case 0x0: return ~dst;
    case 0x1: return 0;
    case 0x2: return ~0;
    case 0x3: return dst;
    case 0x4: return ~src;
    case 0x5: return src ^ dst;
    case 0x6: return ~(src ^ dst);
    case 0x7: return src;
    case 0x8: return ~(src & dst);
    case 0x9: return ~src | dst;
    case 0xa: return src | ~dst;
    case 0xb: return src | dst;
    case 0xc: return src & dst;
    case 0xd: return src & ~dst;
    case 0xe: return ~src & dst;
    default:  return ~(src | dst);
    }
}

/* engine parameters that are constant for one s3_accel_start() call */
typedef struct S3AccelCtx {
    int clip_t, clip_l, clip_b, clip_r;
    bool vram_mask;         /* PIX_CNTL: video memory supplies the mask */
    uint32_t mix_mask;      /* MSB of the mono mask for the bus width */
    uint32_t compare, rd_mask, wrt_mask, frgd_color, bkgd_color;
    int frgd_sel, bkgd_sel; /* mix register bits 6-5 */
    int frgd_mix, bkgd_mix; /* mix register bits 3-0 */
    int x_mul;              /* bytes per pixel */
    int width;              /* pitch in pixels */
    uint32_t srcbase, dstbase;
} S3AccelCtx;

static bool s3_accel_clipped(S3TrioState *s, const S3AccelCtx *c, int x, int y)
{
    bool inside = x >= c->clip_l && x <= c->clip_r &&
                  y >= c->clip_t && y <= c->clip_b;

    if (s->mfc[MF_MULT_MISC] & MULT_MISC_CLIP_OUT) {
        return inside;
    }
    return !inside;
}

/* colour compare: returns false when the pixel must not be updated */
static bool s3_accel_compare(S3TrioState *s, const S3AccelCtx *c, uint32_t src)
{
    if (!(s->mfc[MF_MULT_MISC] & MULT_MISC_CMP_ENABLE)) {
        return true;
    }
    if (s->mfc[MF_MULT_MISC] & MULT_MISC_CMP_SENSE) {
        return src == c->compare;
    }
    return src != c->compare;
}

/* source operand for one pixel, from the selected mix register */
static uint32_t s3_accel_source(S3TrioState *s, const S3AccelCtx *c,
                                bool fg, uint32_t cpu_dat, uint32_t bmp_addr)
{
    switch (fg ? c->frgd_sel : c->bkgd_sel) {
    case 0:
        return c->bkgd_color;
    case 1:
        return c->frgd_color;
    case 2:
        /* only the current pixel takes part in the mix and compare */
        switch (s3_accel_bpp(s)) {
        case 0:
            return cpu_dat & 0xff;
        case 1:
            return cpu_dat & 0xffff;
        case 2:
            return cpu_dat & 0xffffff;
        default:
            return cpu_dat;
        }
    default:
        return s3_accel_read_pixel(s, bmp_addr);
    }
}

/* mix one pixel into the destination at addr and write it back */
static void s3_accel_plot(S3TrioState *s, const S3AccelCtx *c, uint32_t addr,
                          bool fg, uint32_t src, bool write)
{
    uint32_t dst, out;

    dst = s3_accel_read_pixel(s, addr);
    out = s3_mix(fg ? c->frgd_mix : c->bkgd_mix, src, dst);
    out = (out & c->wrt_mask) | (dst & ~c->wrt_mask);
    if (write) {
        s3_accel_write_pixel(s, addr, out);
    }
}

static void s3_accel_setup_ctx(S3TrioState *s, S3AccelCtx *c)
{
    int bpp = s3_accel_bpp(s);

    c->clip_t = s->mfc[MF_SCISSORS_T] & 0xfff;
    c->clip_l = s->mfc[MF_SCISSORS_L] & 0xfff;
    c->clip_b = s->mfc[MF_SCISSORS_B] & 0xfff;
    c->clip_r = s->mfc[MF_SCISSORS_R] & 0xfff;
    c->vram_mask = (s->mfc[MF_PIX_CNTL] & PIX_CNTL_MIXSEL_MASK) ==
                   PIX_CNTL_MIXSEL_VRAM;
    c->compare = s->color_cmp;
    c->rd_mask = s->rd_mask;
    c->wrt_mask = s->wrt_mask;
    c->frgd_color = s->frgd_color;
    c->bkgd_color = s->bkgd_color;
    c->frgd_sel = (s->frgd_mix >> 5) & 3;
    c->bkgd_sel = (s->bkgd_mix >> 5) & 3;
    c->frgd_mix = s->frgd_mix & 0xf;
    c->bkgd_mix = s->bkgd_mix & 0xf;
    c->x_mul = s3_accel_pixel_bytes(s);
    c->width = s3_accel_width(s);

    /* source/destination base (MULT_MISC2 / MULT_MISC), in 1MB units */
    if ((s->mfc[MF_MULT_MISC2] >> 4) & 7) {
        c->srcbase = 0x100000 * ((s->mfc[MF_MULT_MISC2] >> 4) & 7);
    } else {
        c->srcbase = 0x100000 * ((s->mfc[MF_MULT_MISC] >> 2) & 3);
    }
    if (s->mfc[MF_MULT_MISC2] & 7) {
        c->dstbase = 0x100000 * (s->mfc[MF_MULT_MISC2] & 7);
    } else {
        c->dstbase = 0x100000 * (s->mfc[MF_MULT_MISC] & 3);
    }
    switch (bpp) {
    case 1:
        c->srcbase >>= 1;
        c->dstbase >>= 1;
        break;
    case 2:
        c->srcbase /= 3;
        c->dstbase /= 3;
        break;
    case 3:
        c->srcbase >>= 2;
        c->dstbase >>= 2;
        break;
    default:
        break;
    }

    switch (bpp) {
    case 0:
        c->rd_mask &= 0xff;
        c->compare &= 0xff;
        break;
    case 1:
        c->rd_mask &= 0xffff;
        c->compare &= 0xffff;
        break;
    case 2:
        if (c->wrt_mask == 0xffff) {
            c->wrt_mask = 0xffffff;
        }
        if (c->rd_mask == 0xffff) {
            c->rd_mask = 0xffffff;
        }
        break;
    default:
        break;
    }

    switch (s->cmd & CMD_BUS_MASK) {
    case 0x000:
        c->mix_mask = 0x80;
        break;
    case CMD_BUS_16:
        c->mix_mask = 0x8000;
        break;
    default:
        c->mix_mask = 0x80000000;
        break;
    }
}

static inline uint32_t s3_pix_addr(const S3AccelCtx *c, uint32_t base,
                                   int x, int y)
{
    return base + (y * c->width + x) * c->x_mul;
}

/* the mono mask is consumed MSB first; a 1 is shifted in from the right */
static inline void s3_next_mix(uint32_t *mix_dat)
{
    *mix_dat = (*mix_dat << 1) | 1;
}

static inline void s3_next_cpu_dat(S3TrioState *s, uint32_t *cpu_dat)
{
    if (s3_accel_bpp(s) == 0) {
        *cpu_dat >>= 8;
    } else {
        *cpu_dat >>= 16;
    }
}

/* step one pixel in one of the eight radial directions (cmd/ssv bits 7-5) */
static void s3_step_dir(int dir, int32_t *x, int32_t *y)
{
    switch (dir & 0xe0) {
    case 0x00: (*x)++; break;
    case 0x20: (*x)++; (*y)--; break;
    case 0x40: (*y)--; break;
    case 0x60: (*x)--; (*y)--; break;
    case 0x80: (*x)--; break;
    case 0xa0: (*x)--; (*y)++; break;
    case 0xc0: (*y)++; break;
    default:   (*x)++; (*y)++; break;
    }
}

static void s3_polygon_setup(S3TrioState *s)
{
    if (s->point_1_updated) {
        int start_x = s->poly_cx;
        int start_y = s->poly_cy;
        int end_x = s->destx_diastp << 20;
        int end_y = s->desty_axstp;

        s->poly_dx1 = (end_y - start_y) ? (end_x - start_x) / (end_y - start_y)
                                        : 0;
        s->point_1_updated = 0;
        if (end_y == s->poly_cy) {
            s->poly_cx = end_x;
            s->poly_x = end_x >> 20;
        }
    }
    if (s->point_2_updated) {
        int start_x = s->poly_cx2;
        int start_y = s->poly_cy2;
        int end_x = s->x2 << 20;
        int end_y = s->desty_axstp2;

        s->poly_dx2 = (end_y - start_y) ? (end_x - start_x) / (end_y - start_y)
                                        : 0;
        s->point_2_updated = 0;
        if (end_y == s->poly_cy) {
            s->poly_cx2 = end_x;
        }
    }
}

static bool s3_cpu_src(S3TrioState *s)
{
    /* on the Trio64 CPU data can only be a source */
    return s->cmd & CMD_PCDATA;
}

static void s3_accel_done(S3TrioState *s)
{
    s->busy = 0;
}

/*
 * Run (part of) the current command.  count is the number of pixels or
 * mask bits provided by this call (-1: run to completion), cpu_input tells
 * whether this is a PIX_TRANS transfer, mix_dat is the mono mask
 * (0xffffffff = all foreground) and cpu_dat the CPU colour data.
 */
static void s3_accel_start(S3TrioState *s, int count, bool cpu_input,
                           uint32_t mix_dat, uint32_t cpu_dat)
{
    S3AccelCtx ctx, *c = &ctx;
    int op = s->cmd >> 13;
    uint32_t src, addr;
    bool fg;

    if (s->cmd & CMD_EXT) {
        op |= 0x08;
    }
    s3_accel_setup_ctx(s, c);

    if (!cpu_input) {
        s->dat_count = 0;
        s->busy = 0;
        trace_s3_vga_accel_start(op, s->cmd, s->cur_x, s->cur_y,
                                 s->destx_diastp, s->desty_axstp,
                                 s->maj_axis_pcnt, s->mfc[MF_MIN_AXIS_PCNT]);
        trace_s3_vga_accel_regs(s->frgd_mix, s->bkgd_mix, s->mfc[MF_PIX_CNTL],
                                s->mfc[MF_MULT_MISC], s->color_cmp,
                                s->wrt_mask, s->frgd_color, s->bkgd_color);
    } else if (cpu_input && ((s->mfc[MF_PIX_CNTL] & PIX_CNTL_MIXSEL_MASK) !=
                             PIX_CNTL_MIXSEL_CPU) && !(s->cmd & CMD_PLANAR)) {
        /* colour data: count was in bytes, convert to pixels */
        if (s3_accel_bpp(s) == 3 && count == 2) {
            /* 32bpp pixels arriving as 16-bit halves */
            if (s->dat_count) {
                cpu_dat = ((cpu_dat & 0xffff) << 16) | s->dat_buf;
                count = 4;
                s->dat_count = 0;
            } else {
                s->dat_buf = cpu_dat & 0xffff;
                s->dat_count = 1;
                return;
            }
        }
        if (s3_accel_bpp(s) == 1) {
            count >>= 1;
        } else if (s3_accel_bpp(s) >= 2) {
            count >>= 2;
        }
    }

#define LOOP_COUNT()  (count != 0 ? (count > 0 ? count-- : 1) : 0)

    switch (op) {
    case OP_NOP: /* short stroke vectors */
        if (!s->ssv_state) {
            break;
        }
        if (!(s->cmd & CMD_LINETYPE)) {
            break;
        }
        while (LOOP_COUNT()) {
            if (!s3_accel_clipped(s, c, s->cx & 0xfff, s->cy & 0xfff)) {
                fg = mix_dat & c->mix_mask;
                addr = s3_pix_addr(c, 0, s->cx, s->cy);
                src = s3_accel_source(s, c, fg, cpu_dat, addr);
                if (s3_accel_compare(s, c, src)) {
                    s3_accel_plot(s, c, addr, fg, src, s->ssv_draw);
                }
            }
            s3_next_mix(&mix_dat);
            s3_next_cpu_dat(s, &cpu_dat);
            if (!s->ssv_len) {
                s->cur_x = s->cx & 0xfff;
                s->cur_y = s->cy & 0xfff;
                break;
            }
            s3_step_dir(s->ssv_dir, &s->cx, &s->cy);
            s->ssv_len--;
            s->cx &= 0xfff;
            s->cy &= 0xfff;
        }
        break;

    case OP_LINE:
        if (!cpu_input) {
            s->cx = s->cur_x & 0xfff;
            s->cy = s->cur_y & 0xfff;
            s->sy = s->maj_axis_pcnt;
            if (s3_cpu_src(s)) {
                s->busy = 1;
                return;
            }
        }
        while (LOOP_COUNT() && s->sy >= 0) {
            bool last = s->sy == 0;

            if (!s3_accel_clipped(s, c, s->cx & 0xfff, s->cy & 0xfff) &&
                !(last && (s->cmd & CMD_LASTPIX))) {
                fg = mix_dat & c->mix_mask;
                addr = s3_pix_addr(c, 0, s->cx, s->cy);
                src = s3_accel_source(s, c, fg, cpu_dat, addr);
                if (s3_accel_compare(s, c, src)) {
                    s3_accel_plot(s, c, addr, fg, src, true);
                }
            }
            s3_next_mix(&mix_dat);
            s3_next_cpu_dat(s, &cpu_dat);
            if (last) {
                s3_accel_done(s);
                break;
            }
            if (s->cmd & CMD_LINETYPE) {
                /* radial: fixed direction from bits 7-5 */
                s3_step_dir(s->cmd, &s->cx, &s->cy);
            } else if (s->cmd & CMD_YMAJAXIS) {
                /* Bresenham, Y major */
                s->cy += (s->cmd & CMD_INC_Y) ? 1 : -1;
                if (s->err_term >= s->maj_axis_pcnt) {
                    s->err_term = (int16_t)(s->err_term + s->destx_diastp);
                    s->cx += (s->cmd & CMD_INC_X) ? 1 : -1;
                } else {
                    s->err_term = (int16_t)(s->err_term + s->desty_axstp);
                }
            } else {
                /* Bresenham, X major */
                s->cx += (s->cmd & CMD_INC_X) ? 1 : -1;
                if (s->err_term >= s->maj_axis_pcnt) {
                    s->err_term = (int16_t)(s->err_term + s->destx_diastp);
                    s->cy += (s->cmd & CMD_INC_Y) ? 1 : -1;
                } else {
                    s->err_term = (int16_t)(s->err_term + s->desty_axstp);
                }
            }
            s->sy--;
            s->cx &= 0xfff;
            s->cy &= 0xfff;
        }
        s->cur_x = s->cx & 0xfff;
        s->cur_y = s->cy & 0xfff;
        break;

    case OP_RECT:
        if (!cpu_input) {
            s->sx = s->maj_axis_pcnt & 0xfff;
            s->sy = s->mfc[MF_MIN_AXIS_PCNT] & 0xfff;
            s->cx = s->cur_x & 0xfff;
            s->cy = s->cur_y & 0xfff;
            s->dest = s3_pix_addr(c, c->dstbase, 0, s->cy);
            if (s3_cpu_src(s)) {
                s->busy = 1;
                return;
            }
        }
        while (LOOP_COUNT() && s->sy >= 0) {
            if (!s3_accel_clipped(s, c, s->cx, s->cy)) {
                fg = mix_dat & c->mix_mask;
                addr = s->dest + s->cx * c->x_mul;
                src = s3_accel_source(s, c, fg, cpu_dat, addr);
                if (s3_accel_compare(s, c, src)) {
                    s3_accel_plot(s, c, addr, fg, src, s->cmd & CMD_DRAW);
                }
            }
            s3_next_mix(&mix_dat);
            s3_next_cpu_dat(s, &cpu_dat);
            s->cx += (s->cmd & CMD_INC_X) ? 1 : -1;
            s->cx &= 0xfff;
            s->sx--;
            if (s->sx < 0) {
                s->sx = s->maj_axis_pcnt & 0xfff;
                if (s->cmd & CMD_INC_X) {
                    s->cx -= s->sx + 1;
                } else {
                    s->cx += s->sx + 1;
                }
                s->cy += (s->cmd & CMD_INC_Y) ? 1 : -1;
                s->cy &= 0xfff;
                s->dest = s3_pix_addr(c, c->dstbase, 0, s->cy);
                s->sy--;
                if (s->sy < 0) {
                    s->cur_x = s->cx;
                    s->cur_y = s->cy;
                    s3_accel_done(s);
                    return;
                }
                if (cpu_input) {
                    /* the rest of the transfer is padding */
                    return;
                }
            }
        }
        break;

    case OP_POLY_SOLID:
    case OP_POLY_PAT:
    {
        int end_y1, end_y2;

        s3_polygon_setup(s);
        if ((s->cmd & CMD_PCDATA) && !cpu_input) {
            s->busy = 1;
            return;
        }
        end_y1 = s->desty_axstp;
        end_y2 = s->desty_axstp2;
        while (s->poly_cy < end_y1 && s->poly_cy2 < end_y2) {
            int y = s->poly_cy;
            int x_count = abs((s->poly_cx2 >> 20) - s->poly_x) + 1;
            uint32_t pat_line = c->srcbase + s->pattern +
                                (y & 7) * c->width * c->x_mul;

            s->dest = s3_pix_addr(c, c->dstbase, 0, y);
            while (x_count-- && LOOP_COUNT()) {
                if (!s3_accel_clipped(s, c, s->poly_x & 0xfff, y & 0xfff)) {
                    uint32_t pat_addr = pat_line + (s->poly_x & 7) * c->x_mul;

                    if (op == OP_POLY_PAT && c->vram_mask) {
                        mix_dat = (s3_accel_read_pixel(s, pat_addr) &
                                   c->rd_mask) == c->rd_mask ? c->mix_mask : 0;
                    }
                    fg = (op == OP_POLY_SOLID) || (mix_dat & c->mix_mask);
                    addr = s->dest + s->poly_x * c->x_mul;
                    src = s3_accel_source(s, c, fg, cpu_dat, pat_addr);
                    if (op == OP_POLY_PAT && c->vram_mask &&
                        (fg ? c->frgd_sel : c->bkgd_sel) == 3) {
                        src = (src & c->rd_mask) == c->rd_mask;
                    }
                    if (s3_accel_compare(s, c, src)) {
                        s3_accel_plot(s, c, addr, fg, src, s->cmd & CMD_DRAW);
                    }
                }
                s3_next_cpu_dat(s, &cpu_dat);
                if (op == OP_POLY_PAT) {
                    s3_next_mix(&mix_dat);
                }
                if (s->poly_x < (s->poly_cx2 >> 20)) {
                    s->poly_x++;
                } else {
                    s->poly_x--;
                }
            }
            s->poly_cx += s->poly_dx1;
            s->poly_cx2 += s->poly_dx2;
            s->poly_x = s->poly_cx >> 20;
            s->poly_cy++;
            s->poly_cy2++;
            if (!count) {
                break;
            }
        }
        s->cur_x = s->poly_cx & 0xfff;
        s->cur_y = s->poly_cy & 0xfff;
        s->cur_x2 = s->poly_cx2 & 0xfff;
        s->cur_y2 = s->poly_cy2 & 0xfff;
        if (s->poly_cy >= end_y1 || s->poly_cy2 >= end_y2) {
            s3_accel_done(s);
        }
        break;
    }

    case OP_BITBLT:
        if (!cpu_input) {
            s->sx = s->maj_axis_pcnt & 0xfff;
            s->sy = s->mfc[MF_MIN_AXIS_PCNT] & 0xfff;
            s->dx = s->destx_diastp & 0xfff;
            s->dy = s->desty_axstp & 0xfff;
            s->cx = s->cur_x & 0xfff;
            s->cy = s->cur_y & 0xfff;
            s->src = s3_pix_addr(c, c->srcbase, 0, s->cy);
            s->dest = s3_pix_addr(c, c->dstbase, 0, s->dy);
            if (s->cmd & CMD_PCDATA) {
                s->busy = 1;
                return;
            }
        }
        while (LOOP_COUNT() && s->sy >= 0) {
            if (!s3_accel_clipped(s, c, s->dx, s->dy)) {
                uint32_t src_addr = s->src + s->cx * c->x_mul;

                if (c->vram_mask && (s->cmd & CMD_DRAW)) {
                    mix_dat = (s3_accel_read_pixel(s, src_addr) & c->rd_mask)
                              == c->rd_mask ? c->mix_mask : 0;
                }
                fg = mix_dat & c->mix_mask;
                addr = s->dest + s->dx * c->x_mul;
                src = s3_accel_source(s, c, fg, cpu_dat, src_addr);
                if ((fg ? c->frgd_sel : c->bkgd_sel) == 3 && c->vram_mask &&
                    (s->cmd & CMD_DRAW)) {
                    src = (src & c->rd_mask) == c->rd_mask;
                }
                if (s3_accel_compare(s, c, src)) {
                    s3_accel_plot(s, c, addr, fg, src,
                                  (s->cmd & CMD_DRAW) || c->vram_mask);
                }
            }
            s3_next_mix(&mix_dat);
            s3_next_cpu_dat(s, &cpu_dat);
            if (s->cmd & CMD_INC_X) {
                s->cx++;
                s->dx++;
            } else {
                s->cx--;
                s->dx--;
            }
            s->dx &= 0xfff;
            s->sx--;
            if (s->sx < 0) {
                int w = (s->maj_axis_pcnt & 0xfff) + 1;

                s->sx = s->maj_axis_pcnt & 0xfff;
                if (s->cmd & CMD_INC_X) {
                    s->cx -= w;
                    s->dx -= w;
                } else {
                    s->cx += w;
                    s->dx += w;
                }
                if (s->cmd & CMD_INC_Y) {
                    s->cy++;
                    s->dy++;
                } else {
                    s->cy--;
                    s->dy--;
                }
                s->src = s3_pix_addr(c, c->srcbase, 0, s->cy);
                s->dest = s3_pix_addr(c, c->dstbase, 0, s->dy);
                s->sy--;
                if (s->sy < 0) {
                    s->destx_diastp = s->dx;
                    s->desty_axstp = s->dy;
                    s3_accel_done(s);
                    return;
                }
                if (cpu_input) {
                    return;
                }
            }
        }
        break;

    case OP_PATBLT: /* BitBlt with an 8x8 source pattern */
        if (!cpu_input) {
            s->sx = s->maj_axis_pcnt & 0xfff;
            s->sy = s->mfc[MF_MIN_AXIS_PCNT] & 0xfff;
            s->dx = s->destx_diastp & 0xfff;
            s->dy = s->desty_axstp & 0xfff;
            s->cx = s->cur_x & 0xfff;
            s->cy = s->cur_y & 0xfff;
            /* align the pattern with the destination */
            s->pattern = s3_pix_addr(c, 0, s->cx, s->cy);
            s->dest = s3_pix_addr(c, c->dstbase, 0, s->dy);
            s->cx = s->dx & 7;
            s->cy = s->dy & 7;
            s->src = c->srcbase + s->pattern + s->cy * c->width * c->x_mul;
            if (s->cmd & CMD_PCDATA) {
                s->busy = 1;
                return;
            }
        }
        while (LOOP_COUNT() && s->sy >= 0) {
            if (!s3_accel_clipped(s, c, s->dx, s->dy)) {
                uint32_t pat_addr = s->src + s->cx * c->x_mul;

                if (c->vram_mask) {
                    mix_dat = (s3_accel_read_pixel(s, pat_addr) & c->rd_mask)
                              == c->rd_mask ? c->mix_mask : 0;
                }
                fg = mix_dat & c->mix_mask;
                addr = s->dest + s->dx * c->x_mul;
                src = s3_accel_source(s, c, fg, cpu_dat, pat_addr);
                if ((fg ? c->frgd_sel : c->bkgd_sel) == 3 && c->vram_mask) {
                    src = (src & c->rd_mask) == c->rd_mask;
                }
                if (s3_accel_compare(s, c, src)) {
                    s3_accel_plot(s, c, addr, fg, src, s->cmd & CMD_DRAW);
                }
            }
            s3_next_mix(&mix_dat);
            s3_next_cpu_dat(s, &cpu_dat);
            if (s->cmd & CMD_INC_X) {
                s->cx = ((s->cx + 1) & 7) | (s->cx & ~7);
                s->dx++;
            } else {
                s->cx = ((s->cx - 1) & 7) | (s->cx & ~7);
                s->dx--;
            }
            s->dx &= 0xfff;
            s->sx--;
            if (s->sx < 0) {
                int w = (s->maj_axis_pcnt & 0xfff) + 1;

                if (s->cmd & CMD_INC_X) {
                    s->cx = ((s->cx - w) & 7) | (s->cx & ~7);
                    s->dx -= w;
                } else {
                    s->cx = ((s->cx + w) & 7) | (s->cx & ~7);
                    s->dx += w;
                }
                s->sx = s->maj_axis_pcnt & 0xfff;
                if (s->cmd & CMD_INC_Y) {
                    s->cy = ((s->cy + 1) & 7) | (s->cy & ~7);
                    s->dy++;
                } else {
                    s->cy = ((s->cy - 1) & 7) | (s->cy & ~7);
                    s->dy--;
                }
                s->src = c->srcbase + s->pattern +
                         s->cy * c->width * c->x_mul;
                s->dest = s3_pix_addr(c, c->dstbase, 0, s->dy);
                s->sy--;
                if (s->sy < 0) {
                    s->destx_diastp = s->dx;
                    s->desty_axstp = s->dy;
                    s3_accel_done(s);
                    return;
                }
                if (cpu_input) {
                    return;
                }
            }
        }
        break;

    case OP_LINE_2PT: /* line from CUR to DEST, foreground colour only */
    {
        int error;

        if (!cpu_input) {
            s->dx = abs(s->destx_diastp - s->cur_x);
            s->dy = abs(s->desty_axstp - s->cur_y);
            s->cx = s->cur_x & 0xfff;
            s->cy = s->cur_y & 0xfff;
        }
        if ((s->cmd & CMD_PCDATA) && !cpu_input) {
            s->busy = 1;
            return;
        }
        if (s->dx > s->dy) {
            error = s->dx / 2;
            while (s->cx != s->destx_diastp && LOOP_COUNT()) {
                if (!s3_accel_clipped(s, c, s->cx & 0xfff, s->cy & 0xfff)) {
                    addr = s3_pix_addr(c, 0, s->cx, s->cy);
                    if (s3_accel_compare(s, c, c->frgd_color)) {
                        s3_accel_plot(s, c, addr, true, c->frgd_color,
                                      s->cmd & CMD_DRAW);
                    }
                }
                error -= s->dy;
                if (error < 0) {
                    error += s->dx;
                    s->cy += (s->desty_axstp > s->cur_y) ? 1 : -1;
                    s->cy &= 0xfff;
                }
                s->cx += (s->destx_diastp > s->cur_x) ? 1 : -1;
                s->cx &= 0xfff;
            }
        } else {
            error = s->dy / 2;
            while (s->cy != s->desty_axstp && LOOP_COUNT()) {
                if (!s3_accel_clipped(s, c, s->cx & 0xfff, s->cy & 0xfff)) {
                    addr = s3_pix_addr(c, 0, s->cx, s->cy);
                    if (s3_accel_compare(s, c, c->frgd_color)) {
                        s3_accel_plot(s, c, addr, true, c->frgd_color,
                                      s->cmd & CMD_DRAW);
                    }
                }
                error -= s->dx;
                if (error < 0) {
                    error += s->dy;
                    s->cx += (s->destx_diastp > s->cur_x) ? 1 : -1;
                    s->cx &= 0xfff;
                }
                s->cy += (s->desty_axstp > s->cur_y) ? 1 : -1;
                s->cy &= 0xfff;
            }
        }
        s->cur_x = s->cx;
        s->cur_y = s->cy;
        s3_accel_done(s);
        break;
    }

    default:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented command %d (%04x)\n",
                      op, s->cmd);
        s3_accel_done(s);
        break;
    }
#undef LOOP_COUNT
}

static void s3_short_stroke_start(S3TrioState *s, uint8_t ssv)
{
    s->ssv_len = ssv & 0x0f;
    s->ssv_dir = ssv & 0xe0;
    s->ssv_draw = !!(ssv & 0x10);
    if (s3_cpu_src(s)) {
        s->busy = 1;
        return;
    }
    s3_accel_start(s, -1, false, 0xffffffff, 0);
}

/*
 * PIX_TRANS data (86Box s3_accel_out_pixtrans_w/_l and the E2E8 byte
 * cases).  With the mix select set to "CPU data" (or the "across the
 * plane" command bit) the data is a mono mask of 8/16/32 bits, the bus
 * width from CMD bits 10-9; otherwise it is colour data.  CMD bit 12
 * (BYTSEQ) selects the byte order of 16-bit units: clear means the high
 * byte is the first pixel (8514/A compatible), set means the low byte is.
 * 86Box only applies the bit to mono masks because x86 drivers always set
 * it; the IBM RS/6000 firmware draws colour data with it clear.
 */
static uint32_t s3_swap_halfwords(uint32_t val)
{
    return ((val & 0xff00ff00) >> 8) | ((val & 0x00ff00ff) << 8);
}

static void s3_accel_pix_trans(S3TrioState *s, uint32_t val, int bytes)
{
    bool mono, swap;

    if (!(s->cmd & CMD_PCDATA)) {
        return;
    }
    trace_s3_vga_accel_pix_trans(val, bytes, s->cmd);
    mono = ((s->mfc[MF_PIX_CNTL] & PIX_CNTL_MIXSEL_MASK) == PIX_CNTL_MIXSEL_CPU
            || (s->cmd & CMD_PLANAR)) &&
           ((s->frgd_mix & MIX_SRC_MASK) != MIX_SRC_CPU ||
            (s->bkgd_mix & MIX_SRC_MASK) != MIX_SRC_CPU);
    /* a mono mask is consumed MSB first, colour data low byte first */
    swap = mono ? !!(s->cmd & CMD_BYTSEQ) : !(s->cmd & CMD_BYTSEQ);

    switch (s->cmd & CMD_BUS_MASK) {
    case 0x000: /* 8-bit bus: one byte per write, whatever its size */
        if (bytes >= 2 && swap) {
            val = s3_swap_halfwords(val);
        }
        if (mono) {
            s3_accel_start(s, 8, true, val & 0xff, 0);
            if (bytes == 4) {
                s3_accel_start(s, 8, true, (val >> 16) & 0xff, 0);
            }
        } else {
            s3_accel_start(s, 1, true, 0xffffffff, val);
            if (bytes == 4) {
                s3_accel_start(s, 1, true, 0xffffffff, val >> 16);
            }
        }
        break;
    case CMD_BUS_16:
        if (bytes < 2) {
            return;
        }
        if (swap) {
            val = s3_swap_halfwords(val);
        }
        if (mono) {
            s3_accel_start(s, 16, true, val & 0xffff, 0);
            if (bytes == 4) {
                s3_accel_start(s, 16, true, val >> 16, 0);
            }
        } else {
            s3_accel_start(s, 2, true, 0xffffffff, val);
            if (bytes == 4) {
                s3_accel_start(s, 2, true, 0xffffffff, val >> 16);
            }
        }
        break;
    default: /* 32-bit bus */
        if (bytes < 2) {
            return;
        }
        if (bytes == 2) {
            if (swap) {
                val = s3_swap_halfwords(val);
            }
            if (mono) {
                s3_accel_start(s, 16, true, val & 0xffff, 0);
            } else {
                s3_accel_start(s, 4, true, 0xffffffff,
                               (val & 0xffff) | (val << 16));
            }
        } else {
            if (swap) {
                val = bswap32(val);
            }
            if (mono) {
                s3_accel_start(s, 32, true, val, 0);
            } else {
                s3_accel_start(s, 4, true, 0xffffffff, val);
            }
        }
        break;
    }
}

/*
 * Write one byte of a 32-bit colour/mask register.  Bytes 0/1 go to the
 * low word; on the Trio64 MULT_MISC bit 4 redirects them to the high word
 * and is toggled by every high-byte write unless MULT_MISC bit 9 (32-bit
 * register access) is set, in which case bytes 2/3 address the high word
 * directly (86Box s3_accel_out_fifo for chips >= Vision964).
 */
static void s3_write_color_byte(S3TrioState *s, uint32_t *reg, int byte,
                                uint8_t val)
{
    uint16_t misc = s->mfc[MF_MULT_MISC];
    int shift;

    switch (byte) {
    case 0:
    case 1:
        shift = byte * 8;
        if (s3_accel_bpp(s) == 3 && (misc & MULT_MISC_HIGH_WORD) &&
            !(misc & MULT_MISC_32BIT_REGS)) {
            shift += 16;
        }
        *reg = deposit32(*reg, shift, 8, val);
        if (byte == 1 && !(misc & MULT_MISC_32BIT_REGS)) {
            s->mfc[MF_MULT_MISC] ^= MULT_MISC_HIGH_WORD;
        }
        break;
    default:
        shift = byte * 8;
        if (misc & MULT_MISC_32BIT_REGS) {
            *reg = deposit32(*reg, shift, 8, val);
        } else if (s3_accel_bpp(s) == 3) {
            if (!(misc & MULT_MISC_HIGH_WORD)) {
                shift -= 16;
            }
            *reg = deposit32(*reg, shift, 8, val);
            if (byte == 3) {
                s->mfc[MF_MULT_MISC] ^= MULT_MISC_HIGH_WORD;
            }
        }
        break;
    }
}

static void s3_accel_cmd_written(S3TrioState *s)
{
    s->ssv_state = 0;
    if (s3_accel_bpp(s) == 3 && !(s->mfc[MF_MULT_MISC] & MULT_MISC_32BIT_REGS)) {
        s->mfc[MF_MULT_MISC] &= ~MULT_MISC_HIGH_WORD;
    }
    trace_s3_vga_cmd(s->cmd);
    s3_accel_start(s, -1, false, 0xffffffff, 0);
}

static bool s3_accel_enabled(S3TrioState *s)
{
    /* CR40 bit 0: enable the 8514-style graphics engine registers */
    return s->vga.cr[0x40] & 0x01;
}

/* byte write to a graphics engine register (port address, 86Box layout) */
static void s3_accel_out_byte(S3TrioState *s, uint16_t port, uint8_t val)
{
    int byte = port & 3;

    switch (port) {
    case PORT_SUBSYS_STAT:
        s->subsys_stat &= ~val;
        s->subsys_cntl = (s->subsys_cntl & 0xff00) | val;
        return;
    case PORT_SUBSYS_STAT + 1:
        s->subsys_cntl = (s->subsys_cntl & 0xff) | (val << 8);
        return;
    case PORT_SETUP_MD:
        s->setup_md = (s->setup_md & 0xff00) | val;
        return;
    case PORT_SETUP_MD + 1:
        s->setup_md = (s->setup_md & 0xff) | (val << 8);
        return;
    case PORT_ADVFUNC_CNTL:
        s->advfunc_cntl = val;
        return;
    case PORT_ADVFUNC_CNTL + 1:
        return;
    default:
        break;
    }

    if (!s3_accel_enabled(s)) {
        return;
    }

    switch (port) {
    case PORT_CUR_Y:
        s->cur_y = (s->cur_y & 0xf00) | val;
        s->poly_cy = s->cur_y;
        break;
    case PORT_CUR_Y + 1:
        s->cur_y = (s->cur_y & 0xff) | ((val & 0x0f) << 8);
        s->poly_cy = s->cur_y;
        break;
    case PORT_CUR_Y2:
        s->cur_y2 = (s->cur_y2 & 0xf00) | val;
        s->poly_cy2 = s->cur_y2;
        break;
    case PORT_CUR_Y2 + 1:
        s->cur_y2 = (s->cur_y2 & 0xff) | ((val & 0x0f) << 8);
        s->poly_cy2 = s->cur_y2;
        break;
    case PORT_CUR_X:
        s->cur_x = (s->cur_x & 0xf00) | val;
        s->poly_cx = s->cur_x << 20;
        s->poly_x = s->poly_cx >> 20;
        break;
    case PORT_CUR_X + 1:
        s->cur_x = (s->cur_x & 0xff) | ((val & 0x0f) << 8);
        s->poly_cx = s->cur_x << 20;
        s->poly_x = s->poly_cx >> 20;
        break;
    case PORT_CUR_X2:
        s->cur_x2 = (s->cur_x2 & 0xf00) | val;
        s->poly_cx2 = s->cur_x2 << 20;
        break;
    case PORT_CUR_X2 + 1:
        s->cur_x2 = (s->cur_x2 & 0xff) | ((val & 0x0f) << 8);
        s->poly_cx2 = s->cur_x2 << 20;
        break;
    case PORT_DESTY_AXSTP:
        s->desty_axstp = (s->desty_axstp & 0x3f00) | val;
        s->point_1_updated = 1;
        break;
    case PORT_DESTY_AXSTP + 1:
        s->desty_axstp = (s->desty_axstp & 0xff) | ((val & 0x3f) << 8);
        if (val & 0x20) {
            s->desty_axstp |= ~0x3fff;
        }
        s->point_1_updated = 1;
        break;
    case PORT_DESTY_AXSTP2:
        s->desty_axstp2 = (s->desty_axstp2 & 0x3f00) | val;
        s->point_2_updated = 1;
        break;
    case PORT_DESTY_AXSTP2 + 1:
        s->desty_axstp2 = (s->desty_axstp2 & 0xff) | ((val & 0x3f) << 8);
        if (val & 0x20) {
            s->desty_axstp2 |= ~0x3fff;
        }
        s->point_2_updated = 1;
        break;
    case PORT_DESTX_DIASTP:
        s->destx_diastp = (s->destx_diastp & 0x3f00) | val;
        s->point_1_updated = 1;
        break;
    case PORT_DESTX_DIASTP + 1:
        s->destx_diastp = (s->destx_diastp & 0xff) | ((val & 0x3f) << 8);
        if (val & 0x20) {
            s->destx_diastp |= ~0x3fff;
        }
        s->point_1_updated = 1;
        break;
    case PORT_X2:
        s->x2 = (s->x2 & 0xf00) | val;
        s->point_2_updated = 1;
        break;
    case PORT_X2 + 1:
        s->x2 = (s->x2 & 0xff) | ((val & 0x0f) << 8);
        s->point_2_updated = 1;
        break;
    case PORT_ERR_TERM:
        s->err_term = (s->err_term & 0x3f00) | val;
        break;
    case PORT_ERR_TERM + 1:
        s->err_term = (s->err_term & 0xff) | ((val & 0x3f) << 8);
        if (val & 0x20) {
            s->err_term |= ~0x1fff;
        }
        break;
    case PORT_ERR_TERM2:
        s->err_term2 = (s->err_term2 & 0x3f00) | val;
        break;
    case PORT_ERR_TERM2 + 1:
        s->err_term2 = (s->err_term2 & 0xff) | ((val & 0x3f) << 8);
        if (val & 0x20) {
            s->err_term2 |= ~0x1fff;
        }
        break;
    case PORT_MAJ_AXIS_PCNT:
        s->maj_axis_pcnt = (s->maj_axis_pcnt & 0xf00) | val;
        break;
    case PORT_MAJ_AXIS_PCNT + 1:
        s->maj_axis_pcnt = (s->maj_axis_pcnt & 0xff) | ((val & 0x0f) << 8);
        break;
    case PORT_MAJ_AXIS_PCNT2:
        s->maj_axis_pcnt2 = (s->maj_axis_pcnt2 & 0xf00) | val;
        break;
    case PORT_MAJ_AXIS_PCNT2 + 1:
        s->maj_axis_pcnt2 = (s->maj_axis_pcnt2 & 0xff) | ((val & 0x0f) << 8);
        break;
    case PORT_CMD:
        s->cmd = (s->cmd & 0xff00) | val;
        break;
    case PORT_CMD + 1:
        s->cmd = (s->cmd & 0xff) | (val << 8);
        s3_accel_cmd_written(s);
        break;
    case PORT_CMD2:
        s->cmd2 = (s->cmd2 & 0xff00) | val;
        break;
    case PORT_CMD2 + 1:
        s->cmd2 = (s->cmd2 & 0xff) | (val << 8);
        break;
    case PORT_SHORT_STROKE:
        s->short_stroke = (s->short_stroke & 0xff00) | val;
        break;
    case PORT_SHORT_STROKE + 1:
        s->short_stroke = (s->short_stroke & 0xff) | (val << 8);
        s->ssv_state = 1;
        s->cx = s->cur_x & 0xfff;
        s->cy = s->cur_y & 0xfff;
        if (s->cmd & CMD_BYTSEQ) {
            s3_short_stroke_start(s, s->short_stroke & 0xff);
            s3_short_stroke_start(s, s->short_stroke >> 8);
        } else {
            s3_short_stroke_start(s, s->short_stroke >> 8);
            s3_short_stroke_start(s, s->short_stroke & 0xff);
        }
        break;
    case PORT_BKGD_COLOR ... PORT_BKGD_COLOR + 3:
        s3_write_color_byte(s, &s->bkgd_color, byte, val);
        break;
    case PORT_FRGD_COLOR ... PORT_FRGD_COLOR + 3:
        s3_write_color_byte(s, &s->frgd_color, byte, val);
        break;
    case PORT_WRT_MASK ... PORT_WRT_MASK + 3:
        s3_write_color_byte(s, &s->wrt_mask, byte, val);
        break;
    case PORT_RD_MASK ... PORT_RD_MASK + 3:
        s3_write_color_byte(s, &s->rd_mask, byte, val);
        break;
    case PORT_COLOR_CMP ... PORT_COLOR_CMP + 3:
        s3_write_color_byte(s, &s->color_cmp, byte, val);
        break;
    case PORT_BKGD_MIX:
        s->bkgd_mix = val;
        break;
    case PORT_BKGD_MIX + 1:
        break;
    case PORT_FRGD_MIX:
        s->frgd_mix = val;
        break;
    case PORT_FRGD_MIX + 1:
        break;
    case PORT_MULTIFUNC_CNTL:
        s->multifunc_cntl = (s->multifunc_cntl & 0xff00) | val;
        break;
    case PORT_MULTIFUNC_CNTL + 1:
        s->multifunc_cntl = (s->multifunc_cntl & 0xff) | (val << 8);
        if ((val >> 4) == MF_READ_SEL) {
            s->read_sel = s->multifunc_cntl & 0xf;
        } else {
            s->mfc[val >> 4] = s->multifunc_cntl & 0xfff;
        }
        trace_s3_vga_accel_multifunc(val >> 4, s->multifunc_cntl & 0xfff);
        break;
    case PORT_PIX_TRANS ... PORT_PIX_TRANS + 3:
        /* byte-wise PIX_TRANS: the bytes of one bus-width unit are collected */
        s->pix_trans[byte] = val;
        switch (s->cmd & CMD_BUS_MASK) {
        case 0x000:
            if (byte == 0) {
                s3_accel_pix_trans(s, val, 1);
            }
            break;
        case CMD_BUS_16:
            if (byte == 1) {
                s3_accel_pix_trans(s, lduw_le_p(s->pix_trans), 2);
            }
            break;
        default:
            if (byte == 3) {
                s3_accel_pix_trans(s, ldl_le_p(s->pix_trans), 4);
            }
            break;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: write to unknown engine "
                      "register %04x\n", port);
        break;
    }
}

/* access of `size` bytes to the graphics engine registers */
static void s3_accel_out(S3TrioState *s, uint16_t port, uint32_t val,
                         unsigned size)
{
    int i;

    trace_s3_vga_accel_out(port, val, size);
    if (size > 1 && (port & ~3) == PORT_PIX_TRANS && s3_accel_enabled(s)) {
        s3_accel_pix_trans(s, val, size);
        return;
    }
    if (size > 1 && (port & ~1) == PORT_SHORT_STROKE && s3_accel_enabled(s)) {
        /* the whole register is written before the strokes start */
        s->short_stroke = val;
        s3_accel_out_byte(s, PORT_SHORT_STROKE + 1, val >> 8);
        return;
    }
    for (i = 0; i < size; i++) {
        s3_accel_out_byte(s, port + i, (val >> (8 * i)) & 0xff);
    }
}

static uint32_t s3_gp_stat(S3TrioState *s)
{
    return s->busy ? GP_STAT_BUSY : GP_STAT_FIFO_EMPTY;
}

/* MULTIFUNC_CNTL read-back through the read select index */
static uint16_t s3_multifunc_read(S3TrioState *s)
{
    switch (s->read_sel) {
    case 0x0 ... 0x4:
        return s->mfc[s->read_sel];
    case 0x5:
        return s->mfc[MF_PIX_CNTL];
    case 0x6:
        return s->mfc[MF_MULT_MISC];
    case 0x7:
        return s->cmd;
    case 0x8:
        return s->subsys_cntl;
    case 0x9:
        return s->setup_md;
    case 0xa:
        return s->mfc[MF_MULT_MISC2];
    default:
        return 0xffff;
    }
}

static uint32_t s3_accel_in_reg(S3TrioState *s, uint16_t port)
{
    switch (port & ~3) {
    case PORT_SUBSYS_STAT:
        return s->subsys_stat;
    case PORT_SETUP_MD:
        return s->setup_md;
    case PORT_ADVFUNC_CNTL:
        return s->advfunc_cntl;
    case PORT_CUR_Y & ~3:
        return s->cur_y | (s->cur_y2 << 16);
    case PORT_CUR_X & ~3:
        return s->cur_x | (s->cur_x2 << 16);
    case PORT_DESTY_AXSTP & ~3:
        return (s->desty_axstp & 0xffff) | (s->desty_axstp2 << 16);
    case PORT_DESTX_DIASTP & ~3:
        return (s->destx_diastp & 0xffff) | (s->x2 << 16);
    case PORT_ERR_TERM & ~3:
        return (s->err_term & 0xffff) | (s->err_term2 << 16);
    case PORT_MAJ_AXIS_PCNT & ~3:
        return s->maj_axis_pcnt | (s->maj_axis_pcnt2 << 16);
    case PORT_CMD & ~3:
        return s3_gp_stat(s) | (s->cmd2 << 16);
    case PORT_SHORT_STROKE:
        return s->short_stroke;
    case PORT_BKGD_COLOR:
        return s->bkgd_color;
    case PORT_FRGD_COLOR:
        return s->frgd_color;
    case PORT_WRT_MASK:
        return s->wrt_mask;
    case PORT_RD_MASK:
        return s->rd_mask;
    case PORT_COLOR_CMP:
        return s->color_cmp;
    case PORT_BKGD_MIX:
        return s->bkgd_mix;
    case PORT_FRGD_MIX:
        return s->frgd_mix;
    case PORT_MULTIFUNC_CNTL:
        return s3_multifunc_read(s);
    case PORT_PIX_TRANS:
        /* the Trio64 does not read video memory through PIX_TRANS */
        return 0xffffffff;
    default:
        return 0xffffffff;
    }
}

static uint32_t s3_accel_in(S3TrioState *s, uint16_t port, unsigned size)
{
    uint32_t val = s3_accel_in_reg(s, port);

    val >>= 8 * (port & 3);
    if (size < 4) {
        val &= (1u << (8 * size)) - 1;
    }
    if ((port & ~1) == PORT_MULTIFUNC_CNTL && (size > 1 || (port & 1))) {
        /* reading the high byte advances the read select index */
        s->read_sel = (s->read_sel + 1) & 0xf;
    }
    trace_s3_vga_accel_in(port, val, size);
    return val;
}

/* I/O port glue */
static uint32_t s3_trio_accel_readb(void *opaque, uint32_t addr)
{
    return s3_accel_in(opaque, addr, 1);
}

static uint32_t s3_trio_accel_readw(void *opaque, uint32_t addr)
{
    return s3_accel_in(opaque, addr, 2);
}

static uint32_t s3_trio_accel_readl(void *opaque, uint32_t addr)
{
    return s3_accel_in(opaque, addr, 4);
}

static void s3_trio_accel_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    s3_accel_out(opaque, addr, val, 1);
}

static void s3_trio_accel_writew(void *opaque, uint32_t addr, uint32_t val)
{
    s3_accel_out(opaque, addr, val, 2);
}

static void s3_trio_accel_writel(void *opaque, uint32_t addr, uint32_t val)
{
    s3_accel_out(opaque, addr, val, 4);
}

/* 8514-style display status / CRT parameter ports (02E8-26E8) */
static uint32_t s3_trio_status_readw(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val = 0;

    if (addr == 0x02e8) {
        val = s->disp_stat;
    }
    trace_s3_vga_io_readw(addr, val);
    return val;
}

static void s3_trio_status_writew(void *opaque, uint32_t addr, uint32_t val)
{
    trace_s3_vga_io_writew(addr, val);
}

static uint32_t s3_trio_status_readb(void *opaque, uint32_t addr)
{
    return (s3_trio_status_readw(opaque, addr & ~1) >> ((addr & 1) * 8)) & 0xff;
}

static void s3_trio_status_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    trace_s3_vga_io_writeb(addr, val);
}

/*
 * Memory-mapped I/O window.  Enabled by CR53 bit 4 (or ADVFUNC_CNTL bit 5)
 * it replaces the VGA window at 0xA0000: offsets below 0x8000 are the
 * pixel transfer area, offsets 0x8000-0xFFFF map the engine ports, the
 * packed register file at 0x8100-0x816F aliases them (86Box
 * s3_accel_write_fifo), 0x83B0-0x83DF are the VGA ports and 0x8504/0x8505/
 * 0x850C the subsystem and advanced function control registers.
 */
static bool s3_mmio_enabled(S3TrioState *s)
{
    return (s->vga.cr[0x53] & 0x10) || (s->advfunc_cntl & 0x20);
}

static int s3_mmio_packed_port(uint32_t addr)
{
    switch (addr & 0xfffe) {
    case 0x8100: return PORT_CUR_Y;
    case 0x8102: return PORT_CUR_X;
    case 0x8104: return PORT_CUR_Y2;
    case 0x8106: return PORT_CUR_X2;
    case 0x8108: return PORT_DESTY_AXSTP;
    case 0x810a: return PORT_DESTX_DIASTP;
    case 0x810c: return PORT_DESTY_AXSTP2;
    case 0x810e: return PORT_X2;
    case 0x8110: return PORT_ERR_TERM;
    case 0x8112: return PORT_ERR_TERM2;
    case 0x8118: return PORT_CMD;
    case 0x811a: return PORT_CMD2;
    case 0x811c: return PORT_SHORT_STROKE;
    case 0x8120: return PORT_BKGD_COLOR;
    case 0x8122: return PORT_BKGD_COLOR + 2;
    case 0x8124: return PORT_FRGD_COLOR;
    case 0x8126: return PORT_FRGD_COLOR + 2;
    case 0x8128: return PORT_WRT_MASK;
    case 0x812a: return PORT_WRT_MASK + 2;
    case 0x812c: return PORT_RD_MASK;
    case 0x812e: return PORT_RD_MASK + 2;
    case 0x8130: return PORT_COLOR_CMP;
    case 0x8132: return PORT_COLOR_CMP + 2;
    case 0x8134: return PORT_BKGD_MIX;
    case 0x8136: return PORT_FRGD_MIX;
    case 0x814a: return PORT_MAJ_AXIS_PCNT;
    case 0x814c: return PORT_MAJ_AXIS_PCNT2;
    case 0x8154: return PORT_DESTX_DIASTP;
    case 0x8156: return PORT_MAJ_AXIS_PCNT;
    default:     return -1;
    }
}

/* packed MULTIFUNC registers written directly, 0x8138-0x8148 */
static int s3_mmio_packed_mfc(uint32_t addr)
{
    switch (addr & 0xfffe) {
    case 0x8138: return MF_SCISSORS_T;
    case 0x813a: return MF_SCISSORS_L;
    case 0x813c: return MF_SCISSORS_B;
    case 0x813e: return MF_SCISSORS_R;
    case 0x8140: return MF_PIX_CNTL;
    case 0x8142: return MF_MULT_MISC2;
    case 0x8144: return MF_MULT_MISC;
    case 0x8146: return MF_READ_SEL;
    case 0x8148: return MF_MIN_AXIS_PCNT;
    default:     return -1;
    }
}

static void s3_mmio_write_byte(S3TrioState *s, uint32_t addr, uint8_t val)
{
    int port, mfc;

    if (addr >= 0x83b0 && addr <= 0x83df) {
        s3_trio_vga_ioport_write(s, addr & 0x3ff, val);
        return;
    }
    switch (addr) {
    case 0x8504:
        s3_accel_out_byte(s, PORT_SUBSYS_STAT, val);
        return;
    case 0x8505:
        s3_accel_out_byte(s, PORT_SUBSYS_STAT + 1, val);
        return;
    case 0x850c:
        s3_accel_out_byte(s, PORT_ADVFUNC_CNTL, val);
        return;
    default:
        break;
    }
    mfc = s3_mmio_packed_mfc(addr);
    if (mfc >= 0) {
        if (mfc == MF_READ_SEL) {
            if (!(addr & 1)) {
                s->read_sel = val & 0xf;
            }
        } else if (addr & 1) {
            s->mfc[mfc] = (s->mfc[mfc] & 0xff) | ((val & 0x0f) << 8);
        } else {
            s->mfc[mfc] = (s->mfc[mfc] & 0xf00) | val;
        }
        return;
    }
    port = s3_mmio_packed_port(addr);
    if (port >= 0) {
        s3_accel_out_byte(s, port | (addr & 1), val);
        return;
    }
    s3_accel_out_byte(s, addr, val);
}

static void s3_mmio_write(S3TrioState *s, uint32_t addr, uint64_t val,
                          unsigned size)
{
    trace_s3_vga_mmio_write(addr, val, size);
    if (!s3_accel_enabled(s)) {
        return;
    }
    if (addr < 0x8000) {
        s3_accel_pix_trans(s, val, size);
        return;
    }
    if (size > 1 && ((addr & 0xfffc) == PORT_PIX_TRANS ||
                     (addr & 0xfffe) == 0x811c ||
                     (addr & 0xfffe) == PORT_SHORT_STROKE)) {
        int port = s3_mmio_packed_port(addr);

        s3_accel_out(s, port >= 0 ? port : addr, val, size);
        return;
    }
    for (int i = 0; i < size; i++) {
        s3_mmio_write_byte(s, addr + i, (val >> (8 * i)) & 0xff);
    }
}

static uint64_t s3_mmio_read(S3TrioState *s, uint32_t addr, unsigned size)
{
    uint64_t val = 0;
    int port, mfc;

    if (addr < 0x8000) {
        return (uint64_t)-1;
    }
    if (addr >= 0x83b0 && addr <= 0x83df) {
        for (int i = 0; i < size; i++) {
            val |= (uint64_t)s3_trio_vga_ioport_read(s, (addr + i) & 0x3ff)
                   << (8 * i);
        }
        return val;
    }
    mfc = s3_mmio_packed_mfc(addr);
    if (mfc >= 0) {
        uint16_t v = mfc == MF_READ_SEL ? s->read_sel : s->mfc[mfc];
        return (v >> (8 * (addr & 1))) & ((1u << (8 * size)) - 1);
    }
    port = s3_mmio_packed_port(addr);
    if (port >= 0) {
        return s3_accel_in(s, port | (addr & 1), size);
    }
    return s3_accel_in(s, addr, size);
}

static uint32_t s3_trio_enable_readb(void *opaque, uint32_t addr)
{
    uint32_t val;
    val = 0;
    trace_s3_vga_enable_readb(addr, val);
    return val;
}

static void s3_trio_enable_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    trace_s3_vga_enable_writeb(addr, val);
}

static uint32_t s3_trio_dac_ioport_readb(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;

    val = vga_ioport_read(&s->vga, addr - 0x2ea + VGA_PEL_MSK);
    trace_s3_vga_dac_readb(addr, val);
    return val;
}

static void s3_trio_dac_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;
    trace_s3_vga_dac_writeb(addr, val);
    vga_ioport_write(&s->vga, addr - 0x2ea + VGA_PEL_MSK, val);
}

/*
 * S3 extended CRTC registers.  CR30-CR3F are protected by CR38 (register
 * lock 1, unlock value 0x48) and CR40-CR6D by CR39 (register lock 2,
 * unlock value 0xA5); CR36 additionally requires CR39 == 0xA5.  Lock
 * semantics follow 86Box's s3_out().
 */
static bool s3_crtc_locked(S3TrioState *s, uint8_t index)
{
    uint8_t *cr = s->vga.cr;

    if (index >= 0x20 && index < 0x40 &&
        index != 0x36 && index != 0x38 && index != 0x39 &&
        (cr[0x38] & 0xcc) != 0x48) {
        return true;
    }
    if (index >= 0x40 && (cr[0x39] & 0xe0) != 0xa0) {
        return true;
    }
    if (index == 0x36 && cr[0x39] != 0xa5) {
        return true;
    }
    return false;
}

static bool s3_enhanced_mode(S3TrioState *s)
{
    /* CR3A bit 4: enhanced mode (single byte per dot clock, packed) */
    return s->vga.cr[0x3a] & 0x10;
}

/*
 * Colour depth of the display.  In enhanced mode CR67 bits 7-4 select the
 * pixel format (Trio64 extended miscellaneous control 2, as decoded by
 * 86Box for chips >= Trio32).  Outside enhanced mode the CRTC is a plain
 * VGA; the model keeps treating 256-colour modes as one byte per dot, which
 * is what previous versions of this model always did and what the OpenBIOS
 * FCode for this card relies on.
 */
static int s3_trio_get_bpp(VGACommonState *vga)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);

    if (!s3_enhanced_mode(s)) {
        return 8;
    }
    switch (vga->cr[0x67] >> 4) {
    case 3:
        return 15;
    case 5:
        return 16;
    case 7:
        return 24;
    case 13:
        return 32;
    default:
        return 8;
    }
}

/*
 * Display resolution.  The horizontal display end (CR01) counts character
 * clocks of 8 dot clocks; in 15/16bpp modes the Trio64 runs the CRTC at
 * twice the pixel clock and in 24bpp at three dots per pixel, so the
 * value has to be divided accordingly (86Box s3_recalctimings, Trio32/64
 * cases).  CR5D bit 1 and CR5E bit 1 are the S3 extended horizontal and
 * vertical overflow bits.
 */
static void s3_trio_get_resolution(VGACommonState *vga, int *pwidth,
                                   int *pheight)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);
    uint8_t *cr = vga->cr;
    int width, height;

    width = (cr[VGA_CRTC_H_DISP] | ((cr[0x5d] & 0x02) << 7)) + 1;
    width *= 8;
    height = cr[VGA_CRTC_V_DISP_END] |
        ((cr[VGA_CRTC_OVERFLOW] & 0x02) << 7) |
        ((cr[VGA_CRTC_OVERFLOW] & 0x40) << 3) |
        ((cr[0x5e] & 0x02) << 9);
    height += 1;

    if (s3_enhanced_mode(s)) {
        switch (s3_trio_get_bpp(vga)) {
        case 15:
        case 16:
            width /= 2;
            break;
        case 24:
            width /= 3;
            break;
        default:
            break;
        }
    }
    *pwidth = width;
    *pheight = height;
}

/*
 * Display parameters.  The logical line width is CR13 extended by CR51
 * bits 5-4 (or CR43 bit 2 on older chips) and the display start address
 * CR0C/CR0D is extended by ma_ext, which collects CR31 bits 5-4, CR51
 * bits 1-0 and CR69 bits 4-0 (86Box s3_recalctimings / s3_out).  The line
 * compare register gains bit 10 from CR5E bit 6 and is disabled when the
 * 8514-style enhanced functions are enabled through ADVFUNC_CNTL bit 0.
 * The VGA core multiplies start_addr by 4 and uses line_offset in bytes,
 * matching the dword units of the S3 registers in enhanced mode.
 */
static void s3_trio_get_params(VGACommonState *vga, VGADisplayParams *params)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);
    uint8_t *cr = vga->cr;
    uint32_t line_offset;

    line_offset = cr[VGA_CRTC_OFFSET];
    if (cr[0x51] & 0x30) {
        line_offset |= (cr[0x51] & 0x30) << 4;
    } else if (cr[0x43] & 0x04) {
        line_offset |= 0x100;
    }
    if (!line_offset) {
        line_offset = 0x100;
    }
    line_offset <<= 3;
    if (!s3_enhanced_mode(s) && (cr[0x31] & 0x08)) {
        /* enhanced 4bpp mode, drawn like the 8bpp mode per the spec */
        line_offset <<= 1;
    }
    params->line_offset = line_offset;

    params->start_addr = cr[VGA_CRTC_START_LO] |
        (cr[VGA_CRTC_START_HI] << 8) | (s->ma_ext << 16);

    params->line_compare = cr[VGA_CRTC_LINE_COMPARE] |
        ((cr[VGA_CRTC_OVERFLOW] & 0x10) << 4) |
        ((cr[VGA_CRTC_MAX_SCAN] & 0x40) << 3) |
        ((cr[0x5e] & 0x40) << 4);
    if (s->advfunc_cntl & 0x01) {
        params->line_compare = 0xffff;
    }

    params->hpel = vga->ar[VGA_ATC_PEL];
    params->hpel_split = vga->ar[VGA_ATC_MODE] & 0x20;
}

/*
 * CPU bank for the 64K window at 0xA0000: 64K granularity in chain-4
 * mode, 16K (times four planes) otherwise, as in 86Box.  CR31 bit 0
 * enables the bank register in VGA modes.
 */
static void s3_update_bank(S3TrioState *s)
{
    VGACommonState *vga = &s->vga;
    int32_t offset;

    if (!(vga->cr[0x31] & 0x01) && !s3_enhanced_mode(s)) {
        offset = 0;
    } else if (vga->sr[VGA_SEQ_MEMORY_MODE] & VGA_SR04_CHN_4M) {
        offset = s->bank << 16;
    } else {
        offset = s->bank << 14;
    }
    if (offset != vga->bank_offset) {
        vga->bank_offset = offset;
        trace_s3_vga_bank(s->bank, offset);
    }
}

/*
 * Legacy VGA memory window (0xA0000-0xBFFFF).  CR31 bit 3 (enhanced memory
 * mapping) forces a 64K window at 0xA0000 whatever GR06 says; the bank
 * register then selects the 64K page (86Box s3_decode_addr).  Everything
 * else is the standard VGA core behaviour.
 */
static uint8_t s3_vga_mem_readb(S3TrioState *s, hwaddr addr)
{
    VGACommonState *vga = &s->vga;

    if ((vga->cr[0x31] & 0x08) &&
        ((vga->gr[VGA_GFX_MISC] >> 2) & 3) != 1) {
        uint32_t off;

        if (addr >= 0x10000) {
            return 0xff;
        }
        off = (vga->bank_offset + addr) & vga->vbe_size_mask;
        return vga->vram_ptr[off];
    }
    return vga_mem_readb(vga, addr);
}

static void s3_vga_mem_writeb(S3TrioState *s, hwaddr addr, uint8_t val)
{
    VGACommonState *vga = &s->vga;

    if ((vga->cr[0x31] & 0x08) &&
        ((vga->gr[VGA_GFX_MISC] >> 2) & 3) != 1) {
        uint32_t off;

        if (addr >= 0x10000) {
            return;
        }
        off = (vga->bank_offset + addr) & vga->vbe_size_mask;
        vga->vram_ptr[off] = val;
        memory_region_set_dirty(&vga->vram, off, 1);
        return;
    }
    vga_mem_writeb(vga, addr, val);
}

static uint64_t s3_vga_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    S3TrioState *s = opaque;
    uint64_t val = 0;
    int i;

    if (s3_mmio_enabled(s) && addr < 0x10000) {
        return s3_mmio_read(s, addr, size);
    }
    for (i = 0; i < size; i++) {
        val |= (uint64_t)s3_vga_mem_readb(s, addr + i) << (8 * i);
    }
    return val;
}

static void s3_vga_mem_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    S3TrioState *s = opaque;
    int i;

    if (s3_mmio_enabled(s) && addr < 0x10000) {
        s3_mmio_write(s, addr, val, size);
        return;
    }
    for (i = 0; i < size; i++) {
        s3_vga_mem_writeb(s, addr + i, (val >> (8 * i)) & 0xff);
    }
}

static const MemoryRegionOps s3_vga_mem_ops = {
    .read = s3_vga_mem_read,
    .write = s3_vga_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * Hardware cursor.  The 64x64 two-plane cursor image lives in video memory
 * at (CR4C:CR4D & 0xfff) * 1024; each row is 16 bytes made of four
 * 16-pixel groups of a big-endian AND word followed by a big-endian XOR
 * word.  CR46:CR47 and CR48:CR49 give the screen position, CR4E/CR4F the
 * offset of the first displayed column/row within the image, CR4A/CR4B
 * the foreground/background colours through 3-byte stacks and CR55 bit 4
 * selects X11 semantics instead of the Windows ones.  This follows 86Box's
 * s3_hwcursor_draw() for the Trio32/Trio64.
 */
static bool s3_cursor_enabled(S3TrioState *s)
{
    return s->vga.cr[0x45] & 0x01;
}

static int s3_cursor_x(S3TrioState *s)
{
    int x = ((s->vga.cr[0x46] << 8) | s->vga.cr[0x47]) & 0x7ff;

    if (s3_trio_get_bpp(&s->vga) == 32) {
        x &= ~1;
    }
    return x - (s->vga.cr[0x4e] & 0x3f);
}

static int s3_cursor_y(S3TrioState *s)
{
    return ((s->vga.cr[0x48] << 8) | s->vga.cr[0x49]) & 0x7ff;
}

static int s3_cursor_ysize(S3TrioState *s)
{
    return 64 - (s->vga.cr[0x4f] & 0x3f);
}

static uint32_t s3_cursor_color(S3TrioState *s, uint32_t col)
{
    VGACommonState *vga = &s->vga;
    unsigned r, g, b;

    switch (s3_trio_get_bpp(vga)) {
    case 8:
    {
        const uint8_t *pal = vga->palette + (col & 0xff) * 3;
        if (vga->dac_8bit) {
            r = pal[0];
            g = pal[1];
            b = pal[2];
        } else {
            r = c6_to_8(pal[0]);
            g = c6_to_8(pal[1]);
            b = c6_to_8(pal[2]);
        }
        break;
    }
    case 15:
        r = ((col >> 10) & 0x1f) * 255 / 31;
        g = ((col >> 5) & 0x1f) * 255 / 31;
        b = (col & 0x1f) * 255 / 31;
        break;
    case 16:
        r = ((col >> 11) & 0x1f) * 255 / 31;
        g = ((col >> 5) & 0x3f) * 255 / 63;
        b = (col & 0x1f) * 255 / 31;
        break;
    default:
        r = (col >> 16) & 0xff;
        g = (col >> 8) & 0xff;
        b = col & 0xff;
        break;
    }
    return rgb_to_pixel32(r, g, b);
}

static void s3_cursor_invalidate(VGACommonState *vga)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);
    int x, y, ysize;

    if (s3_cursor_enabled(s)) {
        x = s3_cursor_x(s);
        y = s3_cursor_y(s);
        ysize = s3_cursor_ysize(s);
    } else {
        x = y = ysize = 0;
    }
    if (x != s->last_hwc_x || y != s->last_hwc_y ||
        ysize != s->last_hwc_ysize) {
        if (s->last_hwc_ysize) {
            vga_invalidate_scanlines(vga, s->last_hwc_y,
                                     s->last_hwc_y + s->last_hwc_ysize);
        }
        s->last_hwc_x = x;
        s->last_hwc_y = y;
        s->last_hwc_ysize = ysize;
        if (ysize) {
            vga_invalidate_scanlines(vga, y, y + ysize);
        }
    }
}

static void s3_cursor_draw_line(VGACommonState *vga, uint8_t *d, int scr_y)
{
    S3TrioState *s = container_of(vga, S3TrioState, vga);
    uint32_t *dst = (uint32_t *)d;
    uint32_t fg, bg, addr;
    const uint8_t *src;
    int x0, y0, row, group, i, width;
    bool x11 = vga->cr[0x55] & 0x10;

    if (!s3_cursor_enabled(s)) {
        return;
    }
    y0 = s3_cursor_y(s);
    if (scr_y < y0 || scr_y >= y0 + s3_cursor_ysize(s)) {
        return;
    }
    row = scr_y - y0 + (vga->cr[0x4f] & 0x3f);
    addr = (((vga->cr[0x4c] << 8) | vga->cr[0x4d]) & 0xfff) * 1024 + row * 16;
    x0 = s3_cursor_x(s);
    width = vga->last_scr_width;
    fg = s3_cursor_color(s, s->hwc_fg_col);
    bg = s3_cursor_color(s, s->hwc_bg_col);

    for (group = 0; group < 4; group++) {
        uint16_t and_mask, xor_mask;

        src = vga->vram_ptr + ((addr + group * 4) & vga->vbe_size_mask);
        and_mask = (src[0] << 8) | src[1];
        xor_mask = (src[2] << 8) | src[3];
        for (i = 0; i < 16; i++) {
            int x = x0 + group * 16 + i;
            bool a = and_mask & 0x8000;
            bool b = xor_mask & 0x8000;

            and_mask <<= 1;
            xor_mask <<= 1;
            if (x < 0 || x >= width) {
                continue;
            }
            if (x11) {
                if (a) {
                    dst[x] = b ? fg : bg;
                }
            } else {
                if (!a) {
                    dst[x] = b ? fg : bg;
                } else if (b) {
                    dst[x] ^= 0xffffff;
                }
            }
        }
    }
}

static uint32_t s3_crtc_read(S3TrioState *s, uint8_t index)
{
    uint8_t *cr = s->vga.cr;

    switch (index) {
    case 0x2d: /* extended chip ID: 0x88 for Trio32/Trio64 */
        return 0x88;
    case 0x2e: /* new chip ID: 0x11 = Trio64 (86C764) */
        return 0x11;
    case 0x2f: /* revision level */
        return 0x00;
    case 0x30: /* chip ID: 0xE1 = Trio64, readable when unlocked */
        return (((cr[0x38] & 0xcc) == 0x48) || ((cr[0x39] & 0xe0) == 0xa0))
            ? 0xe1 : 0xff;
    case 0x36:
    {
        /* configuration 1: bits 7-5 encode the installed memory */
        static const uint8_t smem[] = { 7, 6, 4, 2, 0, 0, 5, 5, 3 };
        uint8_t val;
        if (s->vga.vram_size_mb < sizeof(smem)) {
            val = smem[s->vga.vram_size_mb];
        } else {
            val = smem[sizeof(smem) - 1];
        }
        return (val << 5) | (cr[0x36] & 0x1f);
    }
    case 0x31:
        return (cr[0x31] & 0xcf) | ((s->ma_ext & 3) << 4);
    case 0x35:
        return (cr[0x35] & 0xf0) | (s->bank & 0xf);
    case 0x45: /* reading resets the cursor colour stack index */
        s->hwc_col_stack_pos = 0;
        return cr[0x45];
    case 0x51:
        return (cr[0x51] & 0xf0) | ((s->bank >> 2) & 0xc) |
               ((s->ma_ext >> 2) & 3);
    case 0x69:
        return s->ma_ext;
    case 0x6a:
        return s->bank;
    case 0x6b: /* mirrors of CR59/CR5A, expected by S3 video BIOSes */
        return (cr[0x53] & 0x08) ? (cr[0x59] & 0xfe) : cr[0x59];
    case 0x6c:
        return (cr[0x53] & 0x08) ? 0x00 : (cr[0x5a] & 0x80);
    default:
        return vga_ioport_read(&s->vga, VGA_CRT_DC);
    }
}

static void s3_crtc_write(S3TrioState *s, uint32_t addr, uint8_t index,
                          uint32_t val)
{
    if (s3_crtc_locked(s, index)) {
        trace_s3_vga_crtc_locked(index, val);
        return;
    }
    if (index >= 0x30) {
        trace_s3_vga_crtc_ext_write(index, val);
    }

    switch (index) {
    case 0x08:
        s->unlock_pll = (val == 0x06);
        break;
    case 0x10: /* memory pll data */
    case 0x11: /* memory pll data */
    case 0x12: /* video pll data */
    case 0x13: /* video pll data */
    case 0x15:
    case 0x18:
        if (s->unlock_pll) {
            qemu_log_mask(LOG_UNIMP,
                          "s3_trio: unimplemented PLL change\n");
        } else {
            vga_ioport_write(&s->vga, addr, val);
        }
        break;
    case 0x31:
        vga_ioport_write(&s->vga, addr, val);
        s->ma_ext = (s->ma_ext & 0x1c) | ((val & 0x30) >> 4);
        s3_update_bank(s);
        break;
    case 0x35:
        vga_ioport_write(&s->vga, addr, val);
        s->bank = (s->bank & 0x70) | (val & 0x0f);
        s3_update_bank(s);
        break;
    case 0x3a:
        vga_ioport_write(&s->vga, addr, val);
        s3_update_bank(s);
        break;
    case 0x45:
        vga_ioport_write(&s->vga, addr, val);
        /* the cursor is drawn into a shadow surface by the VGA core */
        s->vga.force_shadow = !!(val & 0x01);
        break;
    case 0x4a: /* foreground colour stack: 3 bytes, low byte first */
        if (s->hwc_col_stack_pos < 3) {
            s->hwc_fg_col = deposit32(s->hwc_fg_col,
                                      8 * s->hwc_col_stack_pos, 8, val);
        }
        s->hwc_col_stack_pos = (s->hwc_col_stack_pos + 1) & 3;
        break;
    case 0x4b: /* background colour stack */
        if (s->hwc_col_stack_pos < 3) {
            s->hwc_bg_col = deposit32(s->hwc_bg_col,
                                      8 * s->hwc_col_stack_pos, 8, val);
        }
        s->hwc_col_stack_pos = (s->hwc_col_stack_pos + 1) & 3;
        break;
    case 0x51:
        vga_ioport_write(&s->vga, addr, val);
        s->bank = (s->bank & 0x4f) | ((val & 0x0c) << 2);
        s->ma_ext = (s->ma_ext & ~0x0c) | ((val & 0x03) << 2);
        s3_update_bank(s);
        break;
    case 0x69:
        vga_ioport_write(&s->vga, addr, val);
        s->ma_ext = val & 0x1f;
        break;
    case 0x6a:
        vga_ioport_write(&s->vga, addr, val);
        s->bank = val;
        s3_update_bank(s);
        break;
    default:
        vga_ioport_write(&s->vga, addr, val);
        break;
    }
}

/* Same rule as vga_ioport_read/write: CRTC ports depend on MISC bit 0 */
static bool s3_vga_port_ignored(S3TrioState *s, uint32_t addr)
{
    return (addr >= 0x3b0 && addr <= 0x3bf && (s->vga.msr & VGA_MIS_COLOR)) ||
           (addr >= 0x3d0 && addr <= 0x3df && !(s->vga.msr & VGA_MIS_COLOR));
}

static uint32_t s3_trio_vga_ioport_read(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;

    if (s3_vga_port_ignored(s, addr)) {
        return 0xff;
    }

    switch (addr) {
    case VGA_CRT_DM:
    case VGA_CRT_DC:
        val = s3_crtc_read(s, s->vga.cr_index);
        break;
    case VGA_SEQ_D:
        switch (s->vga.sr_index) {
        case 0x17: /* CLKSYN */
        {
            uint32_t *clk;
            if (!(s->vga.sr[0x14] & 0x01)) {
                s->dclk++;
            }
            if (!(s->vga.sr[0x14] & 0x03)) {
                s->mclk++;
            }
            if (s->vga.sr[0x14] & 0x04) {
                clk = &s->mclk;
            } else {
                clk = &s->dclk;
            }
            val = (*clk) & 0xff;
            break;
        }
        default:
            val = vga_ioport_read(&s->vga, addr);
            break;
        }
        break;
    default:
        val = vga_ioport_read(&s->vga, addr);
        break;
    }

    trace_s3_vga_io_readb(addr, val);
    return val;
}

static void s3_trio_vga_ioport_write(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;

    trace_s3_vga_io_writeb(addr, val);
    if (s3_vga_port_ignored(s, addr)) {
        return;
    }
    switch (addr) {
    case VGA_CRT_DM:
    case VGA_CRT_DC:
        s3_crtc_write(s, addr, s->vga.cr_index, val);
        break;
    case VGA_SEQ_I:
        s->vga.sr_index = val;
        break;
    case VGA_SEQ_D:
        switch (s->vga.sr_index) {
        case 0x00 ... 0x07:
            vga_ioport_write(&s->vga, addr, val);
            if (s->vga.sr_index == VGA_SEQ_MEMORY_MODE) {
                s3_update_bank(s);
            }
            break;
        case 0x08:
            s->vga.sr[s->vga.sr_index] = val;
            break;
        case 0x09 ... 0x1c:
            if ((s->vga.sr[0x08] & 0x0f) == 0x06) {
                s->vga.sr[s->vga.sr_index] = val;
            }
            break;
        default:
            break;
        }
        break;
    default:
        vga_ioport_write(&s->vga, addr, val);
        break;
    }
}

static const MemoryRegionPortio s3_trio_portio_list[] = {
    /* entries must be sorted by offset for portio_list_add() */
    { 0x0102, 1, 1, .read = s3_trio_enable_readb, .write = s3_trio_enable_writeb, },
    { 0x02e8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x02e8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x02ea, 4, 1, .read = s3_trio_dac_ioport_readb, .write = s3_trio_dac_ioport_writeb, },
    { 0x03b4,  2, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03ba,  1, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03c0, 16, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03d4,  2, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03da,  1, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x06e8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x06e8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x0ae8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x0ae8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x0ee8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x0ee8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x12e8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x12e8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x16e8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x16e8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x1ae8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x1ae8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x1ee8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x1ee8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x22e8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x22e8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x26e8, 2, 1, .read = s3_trio_status_readb, .write = s3_trio_status_writeb, },
    { 0x26e8, 1, 2, .read = s3_trio_status_readw, .write = s3_trio_status_writew, },
    { 0x42e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x42e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x42e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x46e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x46e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x46e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x4ae8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x4ae8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x4ae8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x82e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x82e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x82e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x86e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x86e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x86e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x8ae8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x8ae8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x8ae8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x8ee8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x8ee8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x8ee8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x92e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x92e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x92e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x96e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x96e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x96e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x9ae8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x9ae8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x9ae8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0x9ee8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0x9ee8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0x9ee8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xa2e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xa2e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xa2e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xa6e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xa6e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xa6e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xaae8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xaae8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xaae8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xaee8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xaee8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xaee8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xb2e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xb2e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xb2e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xb6e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xb6e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xb6e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xbae8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xbae8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xbae8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xbee8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xbee8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xbee8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    { 0xe2e8, 4, 1, .read = s3_trio_accel_readb, .write = s3_trio_accel_writeb, },
    { 0xe2e8, 4, 2, .read = s3_trio_accel_readw, .write = s3_trio_accel_writew, },
    { 0xe2e8, 4, 4, .read = s3_trio_accel_readl, .write = s3_trio_accel_writel, },
    PORTIO_END_OF_LIST()
};

static int s3_trio_post_load(void *opaque, int version_id)
{
    S3TrioState *s = opaque;

    s->vga.force_shadow = s3_cursor_enabled(s);
    s3_update_bank(s);
    return 0;
}

static VMStateDescription vmstate_s3_trio = {
    .name = TYPE_S3_TRIO,
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = s3_trio_post_load,
    .fields = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, S3TrioState),
        VMSTATE_STRUCT(vga, S3TrioState, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_UINT16(disp_stat, S3TrioState),
        VMSTATE_UINT16(subsys_cntl, S3TrioState),
        VMSTATE_UINT16(subsys_stat, S3TrioState),
        VMSTATE_UINT16(setup_md, S3TrioState),
        VMSTATE_UINT16(advfunc_cntl, S3TrioState),
        VMSTATE_UINT16(cur_x, S3TrioState),
        VMSTATE_UINT16(cur_y, S3TrioState),
        VMSTATE_UINT16(cur_x2, S3TrioState),
        VMSTATE_UINT16(cur_y2, S3TrioState),
        VMSTATE_INT16(desty_axstp, S3TrioState),
        VMSTATE_INT16(desty_axstp2, S3TrioState),
        VMSTATE_INT16(destx_diastp, S3TrioState),
        VMSTATE_INT16(x2, S3TrioState),
        VMSTATE_INT16(err_term, S3TrioState),
        VMSTATE_INT16(err_term2, S3TrioState),
        VMSTATE_UINT16(maj_axis_pcnt, S3TrioState),
        VMSTATE_UINT16(maj_axis_pcnt2, S3TrioState),
        VMSTATE_UINT16(cmd, S3TrioState),
        VMSTATE_UINT16(cmd2, S3TrioState),
        VMSTATE_UINT16(short_stroke, S3TrioState),
        VMSTATE_UINT32(bkgd_color, S3TrioState),
        VMSTATE_UINT32(frgd_color, S3TrioState),
        VMSTATE_UINT32(wrt_mask, S3TrioState),
        VMSTATE_UINT32(rd_mask, S3TrioState),
        VMSTATE_UINT32(color_cmp, S3TrioState),
        VMSTATE_UINT8(bkgd_mix, S3TrioState),
        VMSTATE_UINT8(frgd_mix, S3TrioState),
        VMSTATE_UINT16(multifunc_cntl, S3TrioState),
        VMSTATE_UINT16_ARRAY(mfc, S3TrioState, 16),
        VMSTATE_UINT8(read_sel, S3TrioState),
        VMSTATE_UINT8_ARRAY(pix_trans, S3TrioState, 4),
        VMSTATE_INT32(cx, S3TrioState),
        VMSTATE_INT32(cy, S3TrioState),
        VMSTATE_INT32(dx, S3TrioState),
        VMSTATE_INT32(dy, S3TrioState),
        VMSTATE_INT32(sx, S3TrioState),
        VMSTATE_INT32(sy, S3TrioState),
        VMSTATE_UINT32(src, S3TrioState),
        VMSTATE_UINT32(dest, S3TrioState),
        VMSTATE_UINT32(pattern, S3TrioState),
        VMSTATE_INT32(poly_cx, S3TrioState),
        VMSTATE_INT32(poly_cy, S3TrioState),
        VMSTATE_INT32(poly_cx2, S3TrioState),
        VMSTATE_INT32(poly_cy2, S3TrioState),
        VMSTATE_INT32(poly_dx1, S3TrioState),
        VMSTATE_INT32(poly_dx2, S3TrioState),
        VMSTATE_INT32(poly_x, S3TrioState),
        VMSTATE_UINT8(point_1_updated, S3TrioState),
        VMSTATE_UINT8(point_2_updated, S3TrioState),
        VMSTATE_UINT8(ssv_state, S3TrioState),
        VMSTATE_UINT8(ssv_len, S3TrioState),
        VMSTATE_UINT8(ssv_dir, S3TrioState),
        VMSTATE_UINT8(ssv_draw, S3TrioState),
        VMSTATE_UINT32(dat_buf, S3TrioState),
        VMSTATE_UINT8(dat_count, S3TrioState),
        VMSTATE_UINT8(busy, S3TrioState),
        VMSTATE_UINT8(ma_ext, S3TrioState),
        VMSTATE_UINT8(bank, S3TrioState),
        VMSTATE_UINT32(hwc_fg_col, S3TrioState),
        VMSTATE_UINT32(hwc_bg_col, S3TrioState),
        VMSTATE_UINT8(hwc_col_stack_pos, S3TrioState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s3_trio_properties[] = {
    DEFINE_PROP_UINT32("vram_size_mb", S3TrioState, vga.vram_size_mb, 8),
};

/*
 * On the PCI Trio64 the linear address window base (CR59/CR5A) follows
 * base address register 0: 86Box's s3_pci_write stores config bytes 0x12
 * and 0x13 into CR5A bit 7 and CR59.  QEMU maps the framebuffer through
 * the BAR, so the CRTC registers are mirrors for the guest's benefit.
 */
static void s3_trio_pci_write_config(PCIDevice *dev, uint32_t address,
                                     uint32_t val, int len)
{
    S3TrioState *s = S3_TRIO(dev);

    pci_default_write_config(dev, address, val, len);

    if (ranges_overlap(address, len, PCI_BASE_ADDRESS_0, 4)) {
        uint32_t bar = pci_get_long(dev->config + PCI_BASE_ADDRESS_0);

        s->vga.cr[0x59] = bar >> 24;
        s->vga.cr[0x5a] = (bar >> 16) & 0x80;
        trace_s3_vga_law_base(bar & 0xff800000);
    }
}

static void s3_trio_reset(DeviceState *d)
{
    S3TrioState *s = S3_TRIO(d);

    vga_common_reset(&s->vga);

    s->disp_stat |= DISP_STAT_SENSE;
    s->subsys_cntl = s->subsys_stat = s->setup_md = 0;
    s->cur_x = s->cur_y = s->cur_x2 = s->cur_y2 = 0;
    s->desty_axstp = s->desty_axstp2 = s->destx_diastp = s->x2 = 0;
    s->err_term = s->err_term2 = 0;
    s->maj_axis_pcnt = s->maj_axis_pcnt2 = 0;
    s->cmd = s->cmd2 = s->short_stroke = 0;
    s->bkgd_color = s->frgd_color = 0;
    s->wrt_mask = s->rd_mask = s->color_cmp = 0;
    s->bkgd_mix = s->frgd_mix = 0;
    s->multifunc_cntl = 0;
    memset(s->mfc, 0, sizeof(s->mfc));
    s->read_sel = 0;
    memset(s->pix_trans, 0, sizeof(s->pix_trans));
    s->cx = s->cy = s->dx = s->dy = s->sx = s->sy = 0;
    s->src = s->dest = s->pattern = 0;
    s->poly_cx = s->poly_cy = s->poly_cx2 = s->poly_cy2 = 0;
    s->poly_dx1 = s->poly_dx2 = s->poly_x = 0;
    s->point_1_updated = s->point_2_updated = 0;
    s->ssv_state = s->ssv_len = s->ssv_dir = s->ssv_draw = 0;
    s->dat_buf = s->dat_count = 0;
    s->busy = 0;
    s->ma_ext = 0;
    s->bank = 0;
    s->advfunc_cntl = 0;
    s->vga.bank_offset = 0;
    s->hwc_fg_col = 0;
    s->hwc_bg_col = 0;
    s->hwc_col_stack_pos = 0;
    s->last_hwc_x = s->last_hwc_y = s->last_hwc_ysize = 0;
    s->vga.force_shadow = false;
}

static void s3_trio_realize(PCIDevice *dev, Error **errp)

{
    S3TrioState *s = S3_TRIO(dev);
    Object *o = OBJECT(dev);

    /* setup VGA */
    if (!vga_common_init(&s->vga, OBJECT(dev), errp)) {
        return;
    }
    /* The Trio64 is a little-endian PCI chip whatever the host CPU is */
    s->vga.big_endian_fb = false;
    /*
     * legacy_address_space is left NULL on purpose: the VGA core would
     * otherwise overlay a RAM alias of the framebuffer at 0xA0000 in
     * chain-4 mode, computed from the bank offset current at GR/SR write
     * time, bypassing the S3 bank register (CR35/CR51/CR6A) and the CR31
     * enhanced mapping decoded by s3_vga_mem_ops below.
     */
    /*
     * Legacy memory window.  The generic vga_init_io() window is not used
     * because the S3 decodes CR31 (and, for MMIO, CR53) in front of it; the
     * VGA and VBE port lists it would return are not used either.
     */
    memory_region_init_io(&s->vga_mem, o, &s3_vga_mem_ops, s,
                          "s3-lowmem", 0x20000);
    memory_region_set_flush_coalesced(&s->vga_mem);
    memory_region_add_subregion_overlap(pci_address_space(dev),
                                        0x000a0000, &s->vga_mem, 1);
    memory_region_set_coalescing(&s->vga_mem);
    memory_region_set_coalescing(&s->vga.vram);

    s->vga.con = qemu_graphic_console_create(DEVICE(s), 0, s->vga.hw_ops,
                                             &s->vga);

    s->vga.get_bpp = s3_trio_get_bpp;

    /*
     * The Trio64 predates the PCI 2.1 subsystem ID registers and reads them
     * as zero.  AIX identifies PCI adapters by the subsystem vendor/ID when
     * they are non-zero, and only knows the S3 Trio by its 5333:8811 device
     * ID, so do not let the PCI core fill in QEMU's default subsystem ID.
     */
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, 0);
    s->vga.get_resolution = s3_trio_get_resolution;
    s->vga.get_params = s3_trio_get_params;
    s->vga.cursor_invalidate = s3_cursor_invalidate;
    s->vga.cursor_draw_line = s3_cursor_draw_line;

    /*
     * Legacy VGA ports.  Register them through the generic portio API so
     * the device does not depend on an ISA bus being present.
     */
    portio_list_init(&s->portio, OBJECT(s), s3_trio_portio_list, s, "s3_trio");
    portio_list_add(&s->portio, pci_address_space_io(dev), 0);

    /* setup PCI */
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &s->vga.vram);
}

static void s3_trio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = s3_trio_realize;
    k->config_write = s3_trio_pci_write_config;
    //k->romfile = "vgabios-s3.bin";
    k->vendor_id = PCI_VENDOR_ID_S3;
    k->device_id = PCI_DEVICE_ID_S3_TRIO;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    device_class_set_legacy_reset(dc, s3_trio_reset);
    dc->desc = "S3 Trio 32 VGA";
    dc->vmsd  = &vmstate_s3_trio;
    device_class_set_props(dc, s3_trio_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo s3_trio_info = {
    .name          = "s3-trio",
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(S3TrioState),
    .class_init    = s3_trio_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void s3_register_types(void)
{
    type_register_static(&s3_trio_info);
}

type_init(s3_register_types)
