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

/* S3 Trio is a very complex graphic card. Only some parts of them have
 * been implemented.
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

#define TYPE_S3_TRIO "s3-trio"

enum {
    REG_DISP_STAT      = 0x00,
    REG_H_DISP         = 0x01,
    REG_H_SYNC_START   = 0x02,
    REG_H_SYNC_WID     = 0x03,
    REG_V_TOTAL        = 0x04,
    REG_V_DISP         = 0x05,
    REG_V_SYNC_STRT    = 0x06,
    REG_V_SYNC_WID     = 0x07,
    REG_DISP_CNTL      = 0x08,
    REG_H_TOTAL        = 0x09,
    REG_SUBSYS_STAT    = 0x10, /* read-only */
    REG_SUBSYS_CNTL    = 0x10, /* write-only */
    REG_ROM_PAGE_SEL   = 0x11,
    REG_ADVFUNC_CNTL   = 0x12,
    REG_CUR_Y          = 0x20,
    REG_CUR_X          = 0x21,
    REG_DESTY_AXSTP    = 0x22,
    REG_DESTX_DIASTP   = 0x23,
    REG_ERR_TERM       = 0x24,
    REG_MAJ_AXIS_PCNT  = 0x25,
    REG_GP_STAT        = 0x26, /* read-only */
    REG_CMD            = 0x26, /* write-only */
    REG_SHORT_STROKE   = 0x27,
    REG_BKGD_COLOR     = 0x28,
    REG_FRGD_COLOR     = 0x29,
    REG_WRT_MASK       = 0x2A,
    REG_RD_MASK        = 0x2B,
    REG_COLOR_CMP      = 0x2C,
    REG_BKGD_MIX       = 0x2D,
    REG_FRGD_MIX       = 0x2E,
    REG_MULTIFUNC_CNTL = 0x2F,
    REG_PIX_TRANS      = 0x38,
};

enum {
    DISP_STAT_SENSE = 0x0001,
};

enum {
    GP_STAT_BUSY = 0x0200,
};

enum {
    CMD_WRTDATA  = 0x0001,
    CMD_PLANAR   = 0x0002,
    CMD_LASTPIX  = 0x0004,
    CMD_LINETYPE = 0x0008,
    CMD_DRAW     = 0x0010,
    CMD_INC_X    = 0x0020,
    CMD_YMAJAXIS = 0x0040,
    CMD_INC_Y    = 0x0080,
    CMD_PCDATA   = 0x0100,
    CMD_16BIT    = 0x0200,
    CMD_BYTSEQ   = 0x1000,
};

#define CMD_CMD_MASK 0xE000
enum {
    CMD_CMD_NOP    = 0x0000,
    CMD_CMD_LINE   = 0x2000,
    CMD_CMD_RECT   = 0x4000,
    CMD_CMD_RECTV1 = 0x6000,
    CMD_CMD_RECTV2 = 0x8000,
    CMD_CMD_LINEAF = 0xA000,
    CMD_CMD_BITBLT = 0xC000,
};

#define BKGD_MIX_BSS_MASK 0x0060
enum {
    BKGD_MIX_BSS_BKGD = 0x0000,
    BKGD_MIX_BSS_FRGD = 0x0020,
    BKGD_MIX_BSS_PIX  = 0x0040,
    BKGD_MIX_BSS_BMP  = 0x0060,
};

#define FRGD_MIX_FSS_MASK 0x0060
enum {
    FRGD_MIX_FSS_BKGD = 0x0000,
    FRGD_MIX_FSS_FRGD = 0x0020,
    FRGD_MIX_FSS_PIX  = 0x0040,
    FRGD_MIX_FSS_BMP  = 0x0060,
};

#define PIX_CNTL_MIXSEL_MASK 0x00C0
enum {
    PIX_CNTL_MIXSEL_FOREMIX = 0x0000,
    PIX_CNTL_MIXSEL_PATTERN = 0x0040,
    PIX_CNTL_MIXSEL_VAR     = 0x0080,
    PIX_CNTL_MIXSEL_TRANS   = 0x00C0,
};

typedef struct S3TrioState {
    PCIDevice dev;
    VGACommonState vga;
    uint16_t maj_axis, min_axis;
    PortioList portio;

    uint32_t dclk;
    uint32_t mclk;

    uint16_t disp_stat; /* 02e8 */
    uint16_t h_disp; /* 06e8 */
    uint16_t h_sync_strt; /* 0ae8 */
    uint16_t h_sync_wid; /* 0ee8 */
    uint16_t v_total; /* 12e8 */
    uint16_t v_disp; /* 16e8 */
    uint16_t v_sync_strt; /* 1ae8 */
    uint16_t v_sync_wid; /* 1ee8 */
    uint16_t disp_cntl; /* 22e8 */
    uint16_t h_total; /* 26e8 */
    uint16_t subsys_cntl; /* 42e8 (W) */
    uint16_t subsys_stat; /* 42e8 (R) */
    uint16_t rom_page_sel; /* 46e8 */
    uint16_t advfunc_cntl; /* 4ae8 */
    uint16_t cur_y; /* 82e8 */
    uint16_t cur_x; /* 86e8 */
    uint16_t desty_axstep; /* 8ae8 */
    uint16_t destx_diastp; /* 8ee8 */
    uint16_t err_term; /* 92e8 */
    uint16_t maj_axis_pcnt; /* 96e8 */
    uint16_t gp_stat; /* 9ae8 (R) */
    uint16_t cmd; /* 9ae8 (W) */
    uint16_t short_stroke; /* 9ee8 */
    uint16_t bkgd_color; /* a2e8 */
    uint16_t frgd_color; /* a6e8 */
    uint16_t wrt_mask; /* aae8 */
    uint16_t rd_mask; /* aee8 */
    uint16_t color_cmp; /* b2e8 */
    uint16_t bkgd_mix; /* b6e8 */
    uint16_t frgd_mix; /* bae8 */
    uint16_t mfc[16]; /* bee8 */
    uint16_t pix_trans; /* e2e8 */

    uint8_t origin_x;
    uint8_t origin_y;
    uint8_t unlock_pll;

    /* extended display start address bits 20-16 (CR31/CR51/CR69) */
    uint8_t ma_ext;
    /* 64K/16K CPU bank (CR35/CR51/CR6A) */
    uint8_t bank;
    MemoryRegion vga_mem;
} S3TrioState;

OBJECT_DECLARE_SIMPLE_TYPE(S3TrioState, S3_TRIO)

/* FIXME: remove forward declarations */
static uint16_t get_color_from_mix(S3TrioState *s, uint16_t mix);

#define min_axis_pcnt mfc[0]
#define scissors_t    mfc[1]
#define scissors_l    mfc[2]
#define scissors_b    mfc[3]
#define scissors_r    mfc[4]
#define mem_cntl      mfc[5]
#define pattern_l     mfc[8]
#define pattern_h     mfc[9]
#define pix_cntl      mfc[10]
#define color_compare mfc[14]

static inline int address_to_reg(uint32_t addr)
{
    assert((addr & 0x3ff) == 0x2e8);
    return addr >> 10;
}

static inline uint32_t reg_to_address(int reg)
{
    return (reg << 10) + 0x2e8;
}

static inline void do_cmd_done(S3TrioState *s)
{
    s->gp_stat &= ~GP_STAT_BUSY;
}

static void move_to_next_pixel(S3TrioState *s)
{
    uint16_t maj_axis_pcnt;
    int dx;
    int dy;

    switch (s->cmd & CMD_CMD_MASK) {
    case CMD_CMD_RECT:
        maj_axis_pcnt = s->maj_axis_pcnt + 1;
        dx = s->cmd & CMD_INC_X ? 1 : -1;
        dy = s->cmd & CMD_INC_Y ? 1 : -1;
        ++s->maj_axis;
        if (s->maj_axis < maj_axis_pcnt) {
            s->cur_x += dx;
        } else if (s->maj_axis == maj_axis_pcnt) {
            if ((maj_axis_pcnt % 2 == 0) || !(s->cmd & CMD_16BIT)) {
                s->maj_axis = 0;
            }
            s->cur_x -= (s->maj_axis_pcnt) * dx;
            s->cur_y += dy;
            s->min_axis++;
            if (s->min_axis == s->min_axis_pcnt + 1) {
                do_cmd_done(s);
            }
        } else {
            s->maj_axis = 0;
        }
        break;
    case CMD_CMD_LINE:
        if ((s->cmd & CMD_LINETYPE) == 0) {
            assert(0);
        } else {
            static const int xstep[] = { 1,  1,  0, -1, -1, -1, 0, 1 };
            static const int ystep[] = { 0, -1, -1, -1,  0,  1, 1, 1 };
            s->cur_x += xstep[(s->cmd >> 5) & 3];
            s->cur_y += ystep[(s->cmd >> 5) & 3];
            if (s->maj_axis_pcnt-- == 0) {
                do_cmd_done(s);
            }
        }
        break;
    default:
        assert(0);
        break;
    }
}

static void do_cmd_write_one_pixel(S3TrioState *s, uint8_t value)
{
    uint32_t offset;
    uint8_t* p8;
    int width, height;

    if (s->color_compare & 0x100) {
        if ((s->color_compare & 0x80) == 0x80 && s->color_cmp != value) {
            return;
        } else if ((s->color_compare & 0x80) == 0x00 && s->color_cmp == value) {
            return;
        }
    }

    s->vga.get_resolution(&s->vga, &width, &height);

    if ((s->maj_axis < s->maj_axis_pcnt) ||
        (s->maj_axis == s->maj_axis_pcnt && !(s->cmd & CMD_LASTPIX))) {
        offset = s->cur_y * width + s->cur_x;
        p8 = s->vga.vram_ptr + offset;
        p8[0] = value;
        memory_region_set_dirty(&s->vga.vram, offset, 1);
    }
}

static void do_cmd_write_pixel(S3TrioState *s, uint16_t value)
{
    int i, size;
    uint16_t color;

    if (!(s->gp_stat & GP_STAT_BUSY)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "s3_trio: %s called while GP_STAT_BUSY not set\n",
                      __func__);
        return;
    }

    if (s->cmd & CMD_PLANAR) {
        size = (s->cmd & CMD_16BIT) ? 16 : 8;
        for (i = 0; i < size; i++) {
            if (value & (1 << (size - i - 1))) {
                color = get_color_from_mix(s, s->frgd_mix);
            } else {
                color = get_color_from_mix(s, s->bkgd_mix);
            }
            do_cmd_write_one_pixel(s, color);
            move_to_next_pixel(s);
        }
    } else {
        if (s->cmd & CMD_16BIT) {
            do_cmd_write_one_pixel(s, value >> 8);
            move_to_next_pixel(s);
        }
        do_cmd_write_one_pixel(s, value & 0xff);
        move_to_next_pixel(s);
    }
}

static uint16_t get_current_source_bitmap(S3TrioState *s)
{
    qemu_log_mask(LOG_UNIMP,
                  "s3_trio: unimplemented source operand BMP\n");
    return 0;
}

static uint16_t get_current_destination_bitmap(S3TrioState *s)
{
    qemu_log_mask(LOG_UNIMP,
                  "s3_trio: unimplemented destination operand BMP\n");
    return 0;
}

static uint16_t get_color_from_mix(S3TrioState *s, uint16_t mix)
{
    switch (mix & FRGD_MIX_FSS_MASK) {
    case FRGD_MIX_FSS_BKGD:
        return s->bkgd_color;
    case FRGD_MIX_FSS_FRGD:
        return s->frgd_color;
    case FRGD_MIX_FSS_PIX:
        return s->pix_trans;
    case FRGD_MIX_FSS_BMP:
        return get_current_source_bitmap(s);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: invalid FSS 0x%x\n",
                      (s->frgd_mix & FRGD_MIX_FSS_MASK) >> 5);
        return 0;
    }
}

static uint16_t raster_op(S3TrioState *s, uint16_t mix)
{
    uint16_t src = get_color_from_mix(s, mix);
    uint16_t op = mix & 0x1f;

    switch (op) {
    case 0x00: return ~get_current_destination_bitmap(s);
    case 0x01: return 0;
    case 0x02: return 1;
    case 0x03: return ~get_current_destination_bitmap(s);
    case 0x04: return ~get_color_from_mix(s, mix);
    case 0x05: return get_color_from_mix(s, mix) ^ get_current_destination_bitmap(s);
    case 0x06: return ~(get_color_from_mix(s, mix) ^ get_current_destination_bitmap(s));
    case 0x07: return get_color_from_mix(s, mix);
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: invalid MIX operation 0x%x\n",
                      op);
        return src;
    }
}

static uint16_t get_foreground_color(S3TrioState *s)
{
    return raster_op(s, s->frgd_mix);
}

#if 0
static uint16_t get_background_color(S3TrioState *s)
{
    return raster_op(s, s->bkgd_mix);
}
#endif

static uint16_t get_color(S3TrioState *s)
{
    assert(!(s->cmd & CMD_PCDATA));

    if (s->cmd & CMD_PLANAR) {
        return 0xffff;
    }

    switch (s->pix_cntl & PIX_CNTL_MIXSEL_MASK) {
    case PIX_CNTL_MIXSEL_FOREMIX:
        return get_foreground_color(s);
    case PIX_CNTL_MIXSEL_PATTERN:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented mixel PATTERN\n");
        return 0;
    case PIX_CNTL_MIXSEL_VAR:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented mixel VAR\n");
        return 0;
    case PIX_CNTL_MIXSEL_TRANS:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented mixel TRANS\n");
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: invalid MIXSEL 0x%x\n",
                      (s->pix_cntl & PIX_CNTL_MIXSEL_MASK) >> 6);
        return 0;
    }
}

static void s3_do_cmd_bitblt(S3TrioState *s)
{
    int dx, dy, x, y, srcx, srcy, destx, desty;
    uint16_t x1 = s->cur_x;
    uint16_t y1 = s->cur_y;
    uint16_t x2 = s->destx_diastp;
    uint16_t y2 = s->desty_axstep;
    uint16_t width = s->maj_axis_pcnt;
    uint16_t height = s->min_axis_pcnt;
    uint8_t *p8_src, *p8_dest;
    int res_width, res_height;

    s->vga.get_resolution(&s->vga, &res_width, &res_height);

    if (x1 > x2) {
        dx = 1;
        srcx = x1;
        destx = x2;
    } else {
        dx = -1;
        srcx = x1 + width - 1;
        destx = x2 + width - 1;
    }
    if (y1 > y2) {
        dy = 1;
        srcy = y1;
        desty = y2;
    } else {
        dy = -1;
        srcy = y1 + height - 1;
        desty = y2 + height - 1;
    }
    for (y = 0; y < height; y++) {
        p8_src = s->vga.vram_ptr + (srcy + y * dy) * res_width + srcx;
        p8_dest = s->vga.vram_ptr + (desty + y * dy) * res_width + destx;
        for (x = 0; x < width; x++) {
            *p8_dest = *p8_src;
            memory_region_set_dirty(&s->vga.vram, p8_dest - s->vga.vram_ptr, 1);
            p8_src += dx;
            p8_dest += dx;
        }
    }
}

static void do_cmd_init(S3TrioState *s)
{
    s->gp_stat |= GP_STAT_BUSY;
    s->maj_axis = 0;
    s->min_axis = 0;
}

static void do_cmd(S3TrioState *s)
{
    trace_s3_vga_cmd(s->cmd);

    do_cmd_init(s);

    if ((s->cmd & CMD_WRTDATA) == 0) {
        qemu_log_mask(LOG_UNIMP,
                      "s3_trio: CMD_WRTDATA=0 not implemented (%04x)\n", s->cmd);
    }

    switch (s->cmd & CMD_CMD_MASK) {
    case CMD_CMD_NOP:
        qemu_log_mask(LOG_UNIMP, "s3_trio: CMD_NOP not implemented (%04x)\n",
                      s->cmd);
        break;
    case CMD_CMD_LINE:
        if ((s->cmd & CMD_LINETYPE) == 0) {
            trace_s3_vga_cmd_line_bresenham(s->cur_x, s->cur_y,
                                            s->cmd & CMD_INC_X ? 1 : -1,
                                            s->cmd & CMD_INC_Y ? 1 : -1,
                                            s->maj_axis_pcnt,
                                            s->cmd & CMD_YMAJAXIS ? 'Y' : 'X');
            qemu_log_mask(LOG_UNIMP,
                          "s3_trio: CMD_LINE (Bresenham) not implemented (%04x)\n",
                          s->cmd);
        } else {
            trace_s3_vga_cmd_line_vector(s->cur_x, s->cur_y, (s->cmd >> 5) & 3,
                                         s->maj_axis_pcnt);
            if (!(s->cmd & CMD_PCDATA)) {
                while (s->gp_stat & GP_STAT_BUSY) {
                    do_cmd_write_pixel(s, get_color(s));
                }
            }
        }
        break;
    case CMD_CMD_RECT:
        trace_s3_vga_cmd_rect(s->cur_x, s->cur_y, s->cmd & CMD_INC_X ? 1 : -1,
                              s->cmd & CMD_INC_Y ? 1 : -1, s->maj_axis_pcnt,
                              s->min_axis_pcnt);
        if (!(s->cmd & CMD_PCDATA)) {
            while (s->gp_stat & GP_STAT_BUSY) {
                do_cmd_write_pixel(s, get_color(s));
            }
        }
        break;
    case CMD_CMD_RECTV1:
        qemu_log_mask(LOG_UNIMP, "s3_trio: CMD_RECTV1 not implemented (%04x)\n",
                      s->cmd);
        break;
    case CMD_CMD_RECTV2:
        qemu_log_mask(LOG_UNIMP, "s3_trio: CMD_RECTV2 not implemented (%04x)\n",
                      s->cmd);
        break;
    case CMD_CMD_LINEAF:
        qemu_log_mask(LOG_UNIMP, "s3_trio: CMD_LINEAF not implemented (%04x)\n",
                      s->cmd);
        break;
    case CMD_CMD_BITBLT:
        trace_s3_vga_cmd_bitblt(s->cur_x, s->cur_y, s->destx_diastp, s->desty_axstep,
                                s->maj_axis_pcnt, s->min_axis_pcnt);
        s3_do_cmd_bitblt(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: illegal command %04x\n",
                      s->cmd);
        break;
    }
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

static uint16_t* s3_trio_get_register(S3TrioState *s, uint32_t addr, int is_write, uint32_t* val_if_write)
{
    uint16_t *p;

    switch (addr) {
    case REG_DISP_STAT:
        p = is_write ? &s->h_total : &s->disp_stat;
        break;
    case REG_H_DISP:
        p = is_write ? &s->h_disp : NULL;
        break;
    case REG_H_SYNC_START:
        p = is_write ? &s->h_sync_strt : NULL;
        break;
    case REG_H_SYNC_WID:
        p = is_write ? &s->h_sync_wid : NULL;
        break;
    case REG_V_TOTAL:
        p = is_write ? &s->v_total : NULL;
        break;
    case REG_V_DISP:
        p = is_write ? &s->v_disp : NULL;
        break;
    case REG_V_SYNC_STRT:
        p = is_write ? &s->v_sync_strt : NULL;
        break;
    case REG_V_SYNC_WID:
        p = is_write ? &s->v_sync_wid : NULL;
        break;
    case REG_DISP_CNTL:
        p = is_write ? &s->disp_cntl : NULL;
        break;
    case REG_H_TOTAL:
        p = is_write ? NULL: &s->h_total;
        break;
    case REG_SUBSYS_STAT: /* or REG_SUBSYS_CNTL */
        p = is_write ? &s->subsys_cntl : &s->subsys_stat;
        break;
    case REG_ROM_PAGE_SEL:
        p = is_write ? &s->rom_page_sel : NULL;
        break;
    case REG_ADVFUNC_CNTL:
        p = is_write ? &s->advfunc_cntl : NULL;
        break;
    case REG_CUR_Y:
        p = &s->cur_y;
        break;
    case REG_CUR_X:
        p = &s->cur_x;
        break;
    case REG_DESTY_AXSTP:
        p = is_write ? &s->desty_axstep : NULL;
        break;
    case REG_DESTX_DIASTP:
        p = is_write ? &s->destx_diastp : NULL;
        break;
    case REG_ERR_TERM:
        p = &s->err_term;
        break;
    case REG_MAJ_AXIS_PCNT:
        p = is_write ? &s->maj_axis_pcnt : NULL;
        break;
    case REG_GP_STAT: /* or REG_CMD */
        p = is_write ? &s->cmd : &s->gp_stat;
        break;
    case REG_SHORT_STROKE:
        p = is_write ? &s->short_stroke : NULL;
        break;
    case REG_BKGD_COLOR:
        p = is_write ? &s->bkgd_color : NULL;
        break;
    case REG_FRGD_COLOR:
        p = is_write ? &s->frgd_color : NULL;
        break;
    case REG_WRT_MASK:
        p = is_write ? &s->wrt_mask : NULL;
        break;
    case REG_RD_MASK:
        p = is_write ? &s->rd_mask : NULL;
        break;
    case REG_COLOR_CMP:
        p = is_write ? &s->color_cmp : NULL;
        break;
    case REG_BKGD_MIX:
        p = is_write ? &s->bkgd_mix : NULL;
        break;
    case REG_FRGD_MIX:
        p = is_write ? &s->frgd_mix : NULL;
        break;
    case REG_MULTIFUNC_CNTL:
        if (is_write) {
            p = &s->mfc[(*val_if_write >> 12) & 0xf];
            *val_if_write &= 0x0fff;
        } else {
            p = NULL;
        }
        break;
    case REG_PIX_TRANS:
        p = &s->pix_trans;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "s3_trio: invalid register 0x%04x\n",
                      addr);
        break;
    }

    return p;
}

static uint32_t s3_trio_ioport_readb(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;
    uint16_t *p;

    p = s3_trio_get_register(s, address_to_reg(addr & ~0x1), 0, NULL);

    if (p) {
        val = (*p >> ((~addr & 1) * 8)) & 0xff;
    } else {
        val = 0;
    }

    trace_s3_vga_io_readb(addr, val);
    return val;
}

static uint32_t s3_trio_ioport_readw(void *opaque, uint32_t addr)
{
    S3TrioState *s = opaque;
    uint32_t val;
    uint16_t *p;

    p = s3_trio_get_register(s, address_to_reg(addr), 0, NULL);

    if (p) {
        val = *p;
    } else {
        val = 0;
    }

    trace_s3_vga_io_readw(addr, val);
    return val;
}

static void s3_trio_post_write(S3TrioState* s, uint32_t addr)
{
    switch (address_to_reg(addr)) {
    case REG_H_DISP:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented write to H_DISP\n");
        break;
    case REG_V_DISP:
        qemu_log_mask(LOG_UNIMP, "s3_trio: unimplemented write to V_DISP\n");
        break;
    case REG_SUBSYS_CNTL:
        s->subsys_cntl &= ~(1 << 12); /* clear CHPTST */
        break;
    case REG_PIX_TRANS:
        do_cmd_write_pixel(s, s->pix_trans);
        break;
    case REG_CMD:
        do_cmd(s);
        break;
    default:
        break;
    }
}

static void s3_trio_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;
    uint16_t *p;
    uint8_t *c;

    trace_s3_vga_io_writeb(addr, val);
    p = s3_trio_get_register(s, address_to_reg(addr & ~0x1), 1, &val);

    if (p) {
        c = (uint8_t*)p;
        c[~addr & 1] = val;
    }

    s3_trio_post_write(s, addr & ~0x1);
}

static void s3_trio_ioport_writew(void *opaque, uint32_t addr, uint32_t val)
{
    S3TrioState *s = opaque;
    uint16_t *p;

    trace_s3_vga_io_writew(addr, val);
    p = s3_trio_get_register(s, address_to_reg(addr), 1, &val);

    if (p) {
        *p = val & 0xffff;
    }

    s3_trio_post_write(s, addr & ~0x1);
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
static uint64_t s3_vga_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    S3TrioState *s = opaque;
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

static void s3_vga_mem_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    S3TrioState *s = opaque;
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

static const MemoryRegionOps s3_vga_mem_ops = {
    .read = s3_vga_mem_read,
    .write = s3_vga_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

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
    case 0x47:
        return s->origin_x;
    case 0x49:
        return s->origin_y;
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
    case 0x47:
        s->origin_x = val;
        break;
    case 0x49:
        s->origin_y = val;
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
    { 0x0102, 1, 1, .read = s3_trio_enable_readb, .write = s3_trio_enable_writeb, },
    { 0x02e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x02e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x02ea, 4, 1, .read = s3_trio_dac_ioport_readb, .write = s3_trio_dac_ioport_writeb, },
    { 0x03b4,  2, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03ba,  1, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03c0, 16, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03d4,  2, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x03da,  1, 1, .read = s3_trio_vga_ioport_read, .write = s3_trio_vga_ioport_write },
    { 0x06e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x06e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x0ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x0ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x0ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x0ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x16e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x1ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x1ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x1ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x1ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x22e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x22e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x26e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x26e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x2ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x2ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x2ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x2ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x32e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x32e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x36e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x36e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x3ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x3ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x3ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x3ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x42e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x42e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x46e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x46e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x4ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x4ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x82e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x82e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x86e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x86e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x8ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x8ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x8ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x8ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x92e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x92e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x96e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x96e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x9ae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x9ae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0x9ee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0x9ee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xa2e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xa2e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xa6e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xa6e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xaae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xaae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xaee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xaee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xb2e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xb2e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xb6e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xb6e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xbae8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xbae8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xbee8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xbee8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    { 0xe2e8, 1, 2, .read = s3_trio_ioport_readw, .write = s3_trio_ioport_writew, },
    { 0xe2e8, 2, 1, .read = s3_trio_ioport_readb, .write = s3_trio_ioport_writeb, },
    PORTIO_END_OF_LIST()
};

static VMStateDescription vmstate_s3_trio = {
    .name = TYPE_S3_TRIO,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, S3TrioState),
        VMSTATE_STRUCT(vga, S3TrioState, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_UINT16(maj_axis, S3TrioState),
        VMSTATE_UINT16(min_axis, S3TrioState),
        VMSTATE_UINT16(disp_stat, S3TrioState),
        VMSTATE_UINT16(h_disp, S3TrioState),
        VMSTATE_UINT16(h_sync_strt, S3TrioState),
        VMSTATE_UINT16(h_sync_wid, S3TrioState),
        VMSTATE_UINT16(v_total, S3TrioState),
        VMSTATE_UINT16(v_disp, S3TrioState),
        VMSTATE_UINT16(v_sync_strt, S3TrioState),
        VMSTATE_UINT16(v_sync_wid, S3TrioState),
        VMSTATE_UINT16(disp_cntl, S3TrioState),
        VMSTATE_UINT16(h_total, S3TrioState),
        VMSTATE_UINT16(subsys_cntl, S3TrioState),
        VMSTATE_UINT16(subsys_stat, S3TrioState),
        VMSTATE_UINT16(rom_page_sel, S3TrioState),
        VMSTATE_UINT16(advfunc_cntl, S3TrioState),
        VMSTATE_UINT16(cur_y, S3TrioState),
        VMSTATE_UINT16(cur_x, S3TrioState),
        VMSTATE_UINT16(desty_axstep, S3TrioState),
        VMSTATE_UINT16(destx_diastp, S3TrioState),
        VMSTATE_UINT16(err_term, S3TrioState),
        VMSTATE_UINT16(maj_axis_pcnt, S3TrioState),
        VMSTATE_UINT16(gp_stat, S3TrioState),
        VMSTATE_UINT16(cmd, S3TrioState),
        VMSTATE_UINT16(short_stroke, S3TrioState),
        VMSTATE_UINT16(bkgd_color, S3TrioState),
        VMSTATE_UINT16(frgd_color, S3TrioState),
        VMSTATE_UINT16(wrt_mask, S3TrioState),
        VMSTATE_UINT16(rd_mask, S3TrioState),
        VMSTATE_UINT16(color_cmp, S3TrioState),
        VMSTATE_UINT16(bkgd_mix, S3TrioState),
        VMSTATE_UINT16(frgd_mix, S3TrioState),
        VMSTATE_UINT16_ARRAY(mfc, S3TrioState, 16),
        VMSTATE_UINT16(pix_trans, S3TrioState),
        VMSTATE_UINT8(ma_ext, S3TrioState),
        VMSTATE_UINT8(bank, S3TrioState),
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
    s->ma_ext = 0;
    s->bank = 0;
    s->advfunc_cntl = 0;
    s->vga.bank_offset = 0;
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
    s->vga.get_resolution = s3_trio_get_resolution;
    s->vga.get_params = s3_trio_get_params;

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
