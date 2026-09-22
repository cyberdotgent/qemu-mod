/*
 * QEMU-mod: a Win32 GDI front end for PuTTY's terminal emulator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original code.  This is the TermWin half of the contract in putty.h: the
 * twenty-four function pointers that PuTTY's terminal.c calls when it wants
 * something drawn, copied, pasted, scrolled or beeped, implemented against
 * plain GDI and a child window of QEMU's Win32 frame.
 *
 * It is modelled on PuTTY's own windows/window.c, but deliberately much
 * smaller, because we get to fix the environment rather than discover it:
 *
 *   - the font is always a monospace Unicode TrueType face, so char_width()
 *     is always 1 and there is no DBCS screen font, no OEM font and no
 *     "poor man's" VT mode;
 *   - the display is always true colour, so there is no logical palette;
 *   - the terminal never owns a top-level window, so minimise, maximise,
 *     move, restack and resize requests have nowhere to go and are refused;
 *   - bold is a bold font and underline is an underlined font, the two
 *     modes PuTTY calls BOLD_FONT and UND_FONT.
 *
 * What is NOT implemented, and is a real (if small) gap: the DECDWL and
 * DECDHL double-width and double-height line attributes are drawn as
 * ordinary single-width lines.  Nothing a Unix serial session does depends
 * on them; banner pages from some VMS-era software do.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windowsx.h>

#include "win32-term-internal.h"

#define WIN32_TERM_CLASS "QemuWin32Term"

/* Indices into Win32Term::fonts. */
#define FONTF_BOLD  1
#define FONTF_UNDER 2

/*
 * The six colours PuTTY keeps above the 256 xterm ones, in OSC 4 indexing:
 * 256 default foreground, 257 bold foreground, 258 default background,
 * 259 bold background, 260 cursor text, 261 cursor.
 */
#define COLOUR_BG     258
#define COLOUR_CURSOR 261

static LRESULT CALLBACK win32_term_wndproc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp);
static void win32_term_kick_timer(Win32Term *wt);

static inline Win32Term *win32_term_from_hwnd(HWND hwnd)
{
    return (Win32Term *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
}

/*
 * ----------------------------------------------------------------
 * Fonts.
 *
 * One face, four variants.  Consolas has been present since Vista and has
 * a far better Unicode repertoire than Courier New; fall back to whatever
 * the system calls a fixed-pitch modern face if it is missing.
 */

static const char *const font_faces[] = { "Consolas", "Courier New", NULL };

static HFONT win32_term_make_font(const char *face, int height, bool bold,
                                  bool underline)
{
    return CreateFontA(height, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL,
                       FALSE, underline, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, face);
}

/*
 * Nine points, the same size the About box uses for its fixed-pitch block,
 * so that the two agree on screen.  A negative lfHeight asks GDI for
 * character height rather than cell height, which is what a point size
 * means.
 */
#define WIN32_TERM_FONT_POINTS 9

static void win32_term_init_fonts(Win32Term *wt)
{
    const int height = -MulDiv(WIN32_TERM_FONT_POINTS, wt->dpi, 72);
    const char *face = font_faces[0];
    HDC hdc;
    TEXTMETRIC tm;
    HFONT old;
    int i;

    for (i = 0; font_faces[i]; i++) {
        HFONT probe = win32_term_make_font(font_faces[i], height,
                                           false, false);
        char got[LF_FACESIZE];
        bool match;

        if (!probe) {
            continue;
        }
        hdc = GetDC(NULL);
        old = SelectObject(hdc, probe);
        got[0] = '\0';
        GetTextFaceA(hdc, sizeof(got), got);
        SelectObject(hdc, old);
        ReleaseDC(NULL, hdc);

        match = !strcmp(got, font_faces[i]);
        DeleteObject(probe);
        if (match) {
            face = font_faces[i];
            break;
        }
    }

    for (i = 0; i < 4; i++) {
        wt->fonts[i] = win32_term_make_font(face, height,
                                            !!(i & FONTF_BOLD),
                                            !!(i & FONTF_UNDER));
    }

    hdc = GetDC(NULL);
    old = SelectObject(hdc, wt->fonts[0]);
    GetTextMetrics(hdc, &tm);
    /*
     * tmAveCharWidth lies for some TrueType faces.  For a fixed-pitch font
     * the width of a digit is the width of every cell, so measure one.
     */
    {
        SIZE sz;
        if (GetTextExtentPoint32A(hdc, "0", 1, &sz) && sz.cx > 0) {
            wt->font_width = sz.cx;
        } else {
            wt->font_width = tm.tmAveCharWidth;
        }
    }
    wt->font_height = tm.tmHeight;
    wt->font_descent = tm.tmDescent;
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);

    if (wt->font_width < 1) {
        wt->font_width = 8;
    }
    if (wt->font_height < 1) {
        wt->font_height = 16;
    }
}

static void win32_term_free_fonts(Win32Term *wt)
{
    for (int i = 0; i < 4; i++) {
        if (wt->fonts[i]) {
            DeleteObject(wt->fonts[i]);
            wt->fonts[i] = NULL;
        }
    }
}

/*
 * ----------------------------------------------------------------
 * Drawing context.
 */

static bool wintw_setup_draw_ctx(TermWin *tw)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    if (!wt->term_hwnd) {
        return false;
    }
    if (wt->hdc) {
        /* Already inside WM_PAINT; reuse BeginPaint()'s DC. */
        assert(wt->paint_hdc);
        return true;
    }
    wt->hdc = GetDC(wt->term_hwnd);
    wt->paint_hdc = false;
    return wt->hdc != NULL;
}

static void wintw_free_draw_ctx(TermWin *tw)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    assert(wt->hdc);
    if (!wt->paint_hdc) {
        ReleaseDC(wt->term_hwnd, wt->hdc);
        wt->hdc = NULL;
    }
}

/*
 * ----------------------------------------------------------------
 * Text.
 */

static void wintw_draw_text(TermWin *tw, int x, int y, wchar_t *text,
                            int len, unsigned long attr, int lattr,
                            truecolour tc)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    int nfg, nbg, nfont;
    int char_width = wt->font_width;
    COLORREF fg, bg;
    RECT box;
    int *dx;
    bool is_cursor = false;

    if (!wt->hdc || len <= 0) {
        return;
    }

    if (attr & ATTR_WIDE) {
        char_width *= 2;
    }

    x = x * wt->font_width + wt->offset_width;
    y = y * wt->font_height + wt->offset_height;

    /*
     * A block cursor is drawn by redrawing the cell in the dedicated
     * cursor colours, exactly as PuTTY does it.
     */
    if ((attr & ATTR_ACTCURS) && wt->cursor_type == CURSOR_BLOCK) {
        tc.fg = tc.bg = optionalrgb_none;
        attr &= ~(ATTR_REVERSE | ATTR_BLINK | ATTR_COLOURS | ATTR_DIM);
        attr |= (260 << ATTR_FGSHIFT) | (COLOUR_CURSOR << ATTR_BGSHIFT);
        is_cursor = true;
    }

    /*
     * PuTTY's unicode tables map any character the host's ANSI or OEM
     * codepage can represent into a DIRECT_FONT tag plus a byte, because
     * PuTTY keeps a separate GDI font per charset and draws those bytes
     * through it.  We have one Unicode font, so undo that mapping instead:
     * the byte came from the codepage the tag names, so converting it back
     * is exact.
     */
    for (int i = 0; i < len; i++) {
        if (DIRECT_FONT(text[i])) {
            UINT cp = ((text[i] & CSET_MASK) == CSET_OEMCP) ?
                CP_OEMCP : CP_ACP;
            char c = text[i] & 0xFF;
            wchar_t w;

            if (MultiByteToWideChar(cp, 0, &c, 1, &w, 1) == 1) {
                text[i] = w;
            } else {
                text[i] = 0xFFFD;
            }
        }
    }

    /*
     * Anything still carrying an original-character-set tag got here
     * without being translated to Unicode, which means we cannot draw it.
     */
    if (DIRECT_CHAR(text[0]) &&
        (len < 2 || !IS_SURROGATE_PAIR(text[0], text[1]))) {
        for (int i = 0; i < len; i++) {
            text[i] = 0xFFFD;
        }
    }

    nfg = (attr & ATTR_FGMASK) >> ATTR_FGSHIFT;
    nbg = (attr & ATTR_BGMASK) >> ATTR_BGSHIFT;
    if (attr & ATTR_REVERSE) {
        optionalrgb trgb;
        int t = nfg;
        nfg = nbg;
        nbg = t;
        trgb = tc.fg;
        tc.fg = tc.bg;
        tc.bg = trgb;
    }
    /*
     * Bold and blink brighten the colour as well as changing the font, the
     * way every terminal emulator has done since the IBM PC.  Not for the
     * cursor, whose colours are chosen deliberately.
     */
    if ((attr & ATTR_BOLD) && !is_cursor) {
        if (nfg < 16) {
            nfg |= 8;
        } else if (nfg >= 256) {
            nfg |= 1;
        }
    }
    if (attr & ATTR_BLINK) {
        if (nbg < 16) {
            nbg |= 8;
        } else if (nbg >= 256) {
            nbg |= 1;
        }
    }

    fg = tc.fg.enabled ? RGB(tc.fg.r, tc.fg.g, tc.fg.b) : wt->colours[nfg];
    bg = tc.bg.enabled ? RGB(tc.bg.r, tc.bg.g, tc.bg.b) : wt->colours[nbg];
    if (attr & ATTR_DIM) {
        fg = RGB(GetRValue(fg) * 2 / 3, GetGValue(fg) * 2 / 3,
                 GetBValue(fg) * 2 / 3);
    }

    nfont = 0;
    if (attr & ATTR_BOLD) {
        nfont |= FONTF_BOLD;
    }
    if (attr & ATTR_UNDER) {
        nfont |= FONTF_UNDER;
    }

    SelectObject(wt->hdc, wt->fonts[nfont]);
    SetTextColor(wt->hdc, fg);
    SetBkColor(wt->hdc, bg);
    /*
     * Combining characters are drawn over the base character that has
     * already been painted, so they must not erase it.
     */
    SetBkMode(wt->hdc, (attr & TATTR_COMBINING) ? TRANSPARENT : OPAQUE);

    box.left = x;
    box.top = y;
    box.right = x + char_width * len;
    box.bottom = y + wt->font_height;

    /*
     * Give GDI an explicit advance for every character.  Without it, a
     * font whose glyph advances differ by a fraction of a pixel from our
     * cell width -- which is most of them -- drifts out of the grid across
     * a wide line.
     */
    dx = snewn(len, int);
    for (int i = 0; i < len; i++) {
        dx[i] = char_width;
    }

    /*
     * The default text alignment is TA_TOP | TA_LEFT, so (x, y) is the top
     * left of the cell.
     */
    ExtTextOutW(wt->hdc, x, y,
                ((attr & TATTR_COMBINING) ? 0 : ETO_OPAQUE) | ETO_CLIPPED,
                &box, text, len, dx);

    if (attr & ATTR_STRIKE) {
        HPEN pen = CreatePen(PS_SOLID, 0, fg);
        HPEN old = SelectObject(wt->hdc, pen);
        int sy = y + wt->font_height / 2;
        MoveToEx(wt->hdc, x, sy, NULL);
        LineTo(wt->hdc, box.right, sy);
        SelectObject(wt->hdc, old);
        DeleteObject(pen);
    }

    sfree(dx);
}

static void wintw_draw_cursor(TermWin *tw, int x, int y, wchar_t *text,
                              int len, unsigned long attr, int lattr,
                              truecolour tc)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    int char_width = wt->font_width;
    int ctype = wt->cursor_type;

    if (!wt->hdc) {
        return;
    }

    if ((attr & ATTR_ACTCURS) && ctype == CURSOR_BLOCK) {
        if (*text != UCSWIDE) {
            win_draw_text(tw, x, y, text, len, attr, lattr, tc);
            return;
        }
        /* Right-hand half of a double-width character: draw a bar. */
        ctype = CURSOR_VERTICAL_LINE;
        attr |= ATTR_RIGHTCURS;
    }

    if (attr & ATTR_WIDE) {
        char_width *= 2;
    }
    x = x * wt->font_width + wt->offset_width;
    y = y * wt->font_height + wt->offset_height;

    if ((attr & ATTR_PASCURS) && ctype == CURSOR_BLOCK) {
        /* Unfocused block cursor: a hollow box. */
        POINT pts[5];
        HPEN pen = CreatePen(PS_SOLID, 0, wt->colours[COLOUR_CURSOR]);
        HPEN old = SelectObject(wt->hdc, pen);

        pts[0].x = pts[1].x = pts[4].x = x;
        pts[2].x = pts[3].x = x + char_width - 1;
        pts[0].y = pts[3].y = pts[4].y = y;
        pts[1].y = pts[2].y = y + wt->font_height - 1;
        Polyline(wt->hdc, pts, 5);
        SelectObject(wt->hdc, old);
        DeleteObject(pen);
    } else if ((attr & (ATTR_ACTCURS | ATTR_PASCURS)) &&
               ctype != CURSOR_BLOCK) {
        int startx, starty, dx, dy, length;

        if (ctype == CURSOR_UNDERLINE) {
            startx = x;
            starty = y + wt->font_height - wt->font_descent;
            dx = 1;
            dy = 0;
            length = char_width;
        } else {
            startx = x + ((attr & ATTR_RIGHTCURS) ? char_width - 1 : 0);
            starty = y;
            dx = 0;
            dy = 1;
            length = wt->font_height;
        }

        if (attr & ATTR_ACTCURS) {
            HPEN pen = CreatePen(PS_SOLID, 0, wt->colours[COLOUR_CURSOR]);
            HPEN old = SelectObject(wt->hdc, pen);
            MoveToEx(wt->hdc, startx, starty, NULL);
            LineTo(wt->hdc, startx + dx * length, starty + dy * length);
            SelectObject(wt->hdc, old);
            DeleteObject(pen);
        } else {
            /* Unfocused: a dotted line. */
            for (int i = 0; i < length; i++) {
                if (i % 2 == 0) {
                    SetPixel(wt->hdc, startx, starty,
                             wt->colours[COLOUR_CURSOR]);
                }
                startx += dx;
                starty += dy;
            }
        }
    }
}

static void wintw_draw_trust_sigil(TermWin *tw, int x, int y)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    RECT r;

    /*
     * PuTTY draws a padlock here, to mark a line as having come from PuTTY
     * itself rather than from the far end.  Nothing in QEMU sets the trust
     * status, so this never fires; draw two blank cells rather than leave
     * stale pixels behind if it somehow does.
     */
    if (!wt->hdc) {
        return;
    }
    r.left = x * wt->font_width + wt->offset_width;
    r.top = y * wt->font_height + wt->offset_height;
    r.right = r.left + wt->font_width * 2;
    r.bottom = r.top + wt->font_height;
    SetBkColor(wt->hdc, wt->colours[COLOUR_BG]);
    ExtTextOutW(wt->hdc, r.left, r.top, ETO_OPAQUE, &r, L"", 0, NULL);
}

static int wintw_char_width(TermWin *tw, int uc)
{
    /*
     * Every cell of a fixed-pitch font is one cell wide.  PuTTY needs this
     * hook only for fonts whose maximum and average widths differ, which
     * ours never are.
     */
    return 1;
}

/*
 * ----------------------------------------------------------------
 * The caret, which is what Windows accessibility tools and IMEs follow.
 */

static void wintw_set_cursor_pos(TermWin *tw, int x, int y)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    wt->caret_x = x;
    wt->caret_y = y;
    if (wt->has_focus && wt->caret_created) {
        SetCaretPos(x * wt->font_width + wt->offset_width,
                    y * wt->font_height + wt->offset_height);
    }
}

/*
 * ----------------------------------------------------------------
 * Mouse.
 */

void win32_term_show_mouseptr(Win32Term *wt, bool show)
{
    if (wt->mouseptr_visible == show) {
        return;
    }
    wt->mouseptr_visible = show;
    ShowCursor(show);
}

void win32_term_send_break(Win32Term *wt)
{
    if (wt->cb.send_break) {
        wt->cb.send_break(wt->opaque);
    }
}

static void wintw_set_raw_mouse_mode(TermWin *tw, bool enable)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    wt->raw_mouse = enable;
}

static void wintw_set_raw_mouse_mode_pointer(TermWin *tw, bool enable)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    /*
     * PuTTY switches to an arrow to say "the application is reading the
     * mouse, selection needs Shift".  Same here.
     */
    SetClassLongPtr(wt->term_hwnd, GCLP_HCURSOR,
                    (LONG_PTR)LoadCursor(NULL, enable ? IDC_ARROW : IDC_IBEAM));
}

/*
 * ----------------------------------------------------------------
 * Scrollbar.
 */

static void wintw_set_scrollbar(TermWin *tw, int total, int start, int page)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    SCROLLINFO si;

    if (!wt->term_hwnd) {
        return;
    }
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = total - 1;
    si.nPage = page;
    si.nPos = start;
    si.nTrackPos = 0;
    SetScrollInfo(wt->term_hwnd, SB_VERT, &si, TRUE);
}

/*
 * ----------------------------------------------------------------
 * Bell.
 */

static void wintw_bell(TermWin *tw, int mode)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    if (mode == BELL_VISUAL) {
        /*
         * Invert the whole window briefly.  Done synchronously with two
         * repaints rather than with a timer, because a terminal that is
         * beeping is a terminal nobody is watching closely.
         */
        HDC hdc = GetDC(wt->term_hwnd);
        if (hdc) {
            RECT r;
            GetClientRect(wt->term_hwnd, &r);
            InvertRect(hdc, &r);
            Sleep(20);
            InvertRect(hdc, &r);
            ReleaseDC(wt->term_hwnd, hdc);
        }
    } else if (mode != BELL_DISABLED) {
        MessageBeep(MB_OK);
    }

    if (wt->cb.bell) {
        wt->cb.bell(wt->opaque);
    }
}

/*
 * ----------------------------------------------------------------
 * Clipboard.
 *
 * PuTTY distinguishes CLIP_LOCAL (what the mouse selection copies) from
 * CLIP_SYSTEM (what Ctrl-Ins copies).  On Windows, where there is only one
 * clipboard and no primary selection, we make them the same thing -- which
 * is also what PuTTY itself does on this platform.
 */

static void wintw_clip_write(TermWin *tw, int clipboard, wchar_t *text,
                             int *attrs, truecolour *colours, int len,
                             bool must_deselect)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    HGLOBAL mem;
    wchar_t *p;

    if (len <= 0 || !wt->term_hwnd) {
        return;
    }

    mem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
    if (!mem) {
        return;
    }
    p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return;
    }
    memcpy(p, text, len * sizeof(wchar_t));
    p[len] = L'\0';
    GlobalUnlock(mem);

    if (OpenClipboard(wt->term_hwnd)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, mem)) {
            GlobalFree(mem);
        }
        CloseClipboard();
    } else {
        GlobalFree(mem);
    }
}

static void wintw_clip_request_paste(TermWin *tw, int clipboard)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    HGLOBAL mem;
    const wchar_t *p;

    if (!OpenClipboard(wt->term_hwnd)) {
        return;
    }
    mem = GetClipboardData(CF_UNICODETEXT);
    p = mem ? GlobalLock(mem) : NULL;
    if (p) {
        size_t len = wcslen(p);
        /*
         * term_do_paste() keeps its own copy, and takes care of bracketed
         * paste and of stripping control characters if CONF_paste_controls
         * says to.
         */
        term_do_paste(wt->term, p, len);
        GlobalUnlock(mem);
    }
    CloseClipboard();
}

/*
 * ----------------------------------------------------------------
 * Window management: all of it refused, because we are a tab.
 */

static void wintw_refresh(TermWin *tw)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    if (wt->term_hwnd) {
        InvalidateRect(wt->term_hwnd, NULL, FALSE);
    }
}

static void wintw_request_resize(TermWin *tw, int w, int h)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    /*
     * CONF_no_remote_resize is set, so terminal.c should never ask.  If it
     * does anyway, the contract says we must reply, or terminal output
     * processing stalls for ever.
     */
    term_resize_request_completed(wt->term);
}

static void wintw_set_title(TermWin *tw, const char *title, int codepage)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);
    wchar_t *wide;
    char *utf8;

    /* Normalise to UTF-8, which is what the QEMU side deals in. */
    wide = dup_mb_to_wc(codepage, title);
    utf8 = dup_wc_to_mb(CP_UTF8, wide, "?");
    sfree(wide);

    if (wt->title && !strcmp(wt->title, utf8)) {
        sfree(utf8);
        return;
    }
    sfree(wt->title);
    wt->title = utf8;

    if (wt->cb.title) {
        wt->cb.title(wt->opaque, wt->title);
    }
}

static void wintw_set_icon_title(TermWin *tw, const char *title, int codepage)
{
    /* We have no icon of our own to retitle. */
}

static void wintw_set_minimised(TermWin *tw, bool minimised) { }
static void wintw_set_maximised(TermWin *tw, bool maximised) { }
static void wintw_move(TermWin *tw, int x, int y) { }
static void wintw_set_zorder(TermWin *tw, bool top) { }

/*
 * ----------------------------------------------------------------
 * Palette.
 */

static void wintw_palette_set(TermWin *tw, unsigned start, unsigned ncolours,
                              const rgb *colours)
{
    Win32Term *wt = container_of(tw, Win32Term, termwin);

    assert(start <= OSC4_NCOLOURS);
    assert(ncolours <= OSC4_NCOLOURS - start);

    for (unsigned i = 0; i < ncolours; i++) {
        wt->colours[start + i] = RGB(colours[i].r, colours[i].g,
                                     colours[i].b);
    }
    if (wt->term_hwnd) {
        InvalidateRect(wt->term_hwnd, NULL, FALSE);
    }
}

static void wintw_palette_get_overrides(TermWin *tw, Terminal *term)
{
    /*
     * On PuTTY this is where the Windows "system colours" option reaches
     * in.  We always use the configured palette, so there is nothing to
     * override.
     */
}

static void wintw_unthrottle(TermWin *tw, size_t bufsize)
{
    /*
     * The terminal has caught up with its backlog.  The chardev side does
     * its own flow control through qemu_chr_fe_accept_input(), driven by
     * the front end rather than by us, so there is nothing to do here.
     */
}

static const TermWinVtable win32_termwin_vt = {
    .setup_draw_ctx = wintw_setup_draw_ctx,
    .draw_text = wintw_draw_text,
    .draw_cursor = wintw_draw_cursor,
    .draw_trust_sigil = wintw_draw_trust_sigil,
    .char_width = wintw_char_width,
    .free_draw_ctx = wintw_free_draw_ctx,
    .set_cursor_pos = wintw_set_cursor_pos,
    .set_raw_mouse_mode = wintw_set_raw_mouse_mode,
    .set_raw_mouse_mode_pointer = wintw_set_raw_mouse_mode_pointer,
    .set_scrollbar = wintw_set_scrollbar,
    .bell = wintw_bell,
    .clip_write = wintw_clip_write,
    .clip_request_paste = wintw_clip_request_paste,
    .refresh = wintw_refresh,
    .request_resize = wintw_request_resize,
    .set_title = wintw_set_title,
    .set_icon_title = wintw_set_icon_title,
    .set_minimised = wintw_set_minimised,
    .set_maximised = wintw_set_maximised,
    .move = wintw_move,
    .set_zorder = wintw_set_zorder,
    .palette_set = wintw_palette_set,
    .palette_get_overrides = wintw_palette_get_overrides,
    .unthrottle = wintw_unthrottle,
};

/*
 * ----------------------------------------------------------------
 * Geometry.
 */

/*
 * Resizing policy: the character grid follows the window.
 *
 * The alternative would be to keep the chardev's configured size (80x24,
 * or 132x43 for the monitor) and letterbox it in a larger window.  That is
 * not what a terminal does -- not PuTTY, not xterm, not VTE -- and it would
 * make the monitor tab, which has no guest at all, permanently unable to
 * use the space it has been given.
 *
 * The objection to reflowing is that a serial line has no SIGWINCH, so a
 * guest goes on believing whatever its stty says.  That is true, but it is
 * equally true of a physical VT220 and it is not a corruption: a guest that
 * thinks it has 80 columns inside a 137-column terminal simply wraps early
 * and paints its full-screen apps into the left-hand 80 columns.  Only
 * shrinking below what the guest believes causes double wrapping, and the
 * user asked for that by dragging the window.  The loop is closed the
 * normal Unix way -- DSR/CPR is implemented, so resize(1) or `stty` picks
 * up the new size on request.
 *
 * The initial size is still the documented default, so nothing changes for
 * anyone who does not resize the window.
 */
static void win32_term_resized(Win32Term *wt)
{
    RECT r;
    int cols, rows;

    if (!wt->term_hwnd) {
        return;
    }
    GetClientRect(wt->term_hwnd, &r);

    cols = (r.right - r.left) / wt->font_width;
    rows = (r.bottom - r.top) / wt->font_height;
    if (cols < 1) {
        cols = 1;
    }
    if (rows < 1) {
        rows = 1;
    }

    /*
     * Whatever is left over -- always less than one cell in each direction,
     * because the grid is recomputed to fit -- is split either side of the
     * grid.  WM_ERASEBKGND paints it.
     */
    wt->offset_width = ((r.right - r.left) - cols * wt->font_width) / 2;
    wt->offset_height = ((r.bottom - r.top) - rows * wt->font_height) / 2;

    if (cols != wt->cols || rows != wt->rows) {
        wt->cols = cols;
        wt->rows = rows;
        term_size(wt->term, rows, cols, WIN32_TERM_SAVELINES);

        if (wt->cb.resized) {
            wt->cb.resized(wt->opaque, cols, rows);
        }
    }

    /*
     * term_size() does not redraw; it queues the work and expects the front
     * end's main loop to come back for it.  We are not a main loop -- our
     * only pump is the window procedure -- so if we left it queued here the
     * window would keep the pixels from before the drag until the user
     * happened to press a key.  That is exactly what a resize looked like
     * before this call was added.  Flush it now, and erase first so that no
     * part of the client area is left showing what used to be there.
     */
    InvalidateRect(wt->term_hwnd, NULL, TRUE);
    term_update(wt->term);
    win32_term_kick_timer(wt);
}

/*
 * ----------------------------------------------------------------
 * Timers.  One Windows timer drives both PuTTY's timer wheel (cursor blink,
 * bell overload, the terminal's own deferred updates) and its toplevel
 * callback queue.
 */

#define WIN32_TERM_TIMER_ID 1

static void win32_term_kick_timer(Win32Term *wt)
{
    unsigned long next;

    if (win32_term_pump(&next)) {
        unsigned long now = GETTICKCOUNT();
        long delay = (long)(next - now);
        if (delay < 1) {
            delay = 1;
        }
        if (delay > 1000) {
            delay = 1000;
        }
        SetTimer(wt->term_hwnd, WIN32_TERM_TIMER_ID, (UINT)delay, NULL);
    }
}

/*
 * ----------------------------------------------------------------
 * Mouse translation.
 */

/*
 * terminal.c wants both the physical button and what it means.  PuTTY calls
 * the second one "cooked": select, extend or paste.  Which physical button
 * means paste depends on CONF_mouse_is_xterm -- in PuTTY's own ("Windows")
 * mode the right button pastes and the middle one extends the selection,
 * which is what a Windows user expects and what we leave configured.
 */
static Mouse_Button win32_term_cook_button(Win32Term *wt, Mouse_Button b)
{
    bool xterm = conf_get_int(wt->conf, CONF_mouse_is_xterm) == 1;

    switch (b) {
    case MBT_LEFT:
        return MBT_SELECT;
    case MBT_MIDDLE:
        return xterm ? MBT_PASTE : MBT_EXTEND;
    case MBT_RIGHT:
        return xterm ? MBT_EXTEND : MBT_PASTE;
    default:
        return b;
    }
}

static void win32_term_mouse(Win32Term *wt, Mouse_Button b, Mouse_Action a,
                             WPARAM wp, LPARAM lp)
{
    int x = (GET_X_LPARAM(lp) - wt->offset_width) / wt->font_width;
    int y = (GET_Y_LPARAM(lp) - wt->offset_height) / wt->font_height;
    bool shift = (wp & MK_SHIFT) != 0;
    bool ctrl = (wp & MK_CONTROL) != 0;
    bool alt = GetKeyState(VK_MENU) < 0;

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= wt->cols) {
        x = wt->cols - 1;
    }
    if (y >= wt->rows) {
        y = wt->rows - 1;
    }

    win32_term_show_mouseptr(wt, true);
    term_mouse(wt->term, b, win32_term_cook_button(wt, b), a,
               x, y, shift, ctrl, alt);
}

/*
 * ----------------------------------------------------------------
 * Window procedure.
 */

static LRESULT CALLBACK win32_term_wndproc(HWND hwnd, UINT msg,
                                           WPARAM wp, LPARAM lp)
{
    Win32Term *wt = win32_term_from_hwnd(hwnd);

    if (msg == WM_NCCREATE) {
        CREATESTRUCT *cs = (CREATESTRUCT *)lp;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    if (!wt || !wt->term) {
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT p;
        HDC hdc = BeginPaint(hwnd, &p);

        assert(!wt->hdc);
        wt->hdc = hdc;
        wt->paint_hdc = true;
        /*
         * Do not paint cells that already have an update pending from
         * terminal output: term_paint() would mark them clean having only
         * redrawn the exposed part of them.  This is PuTTY's rule and the
         * comment in its window.c explains it at length.
         */
        term_paint(wt->term,
                   (p.rcPaint.left - wt->offset_width) / wt->font_width,
                   (p.rcPaint.top - wt->offset_height) / wt->font_height,
                   (p.rcPaint.right - wt->offset_width - 1) / wt->font_width,
                   (p.rcPaint.bottom - wt->offset_height - 1) /
                   wt->font_height,
                   !wt->term->window_update_pending);
        wt->hdc = NULL;
        wt->paint_hdc = false;

        /* Fill the padding around the grid. */
        if (wt->offset_width || wt->offset_height) {
            RECT r;
            HBRUSH brush = CreateSolidBrush(wt->colours[COLOUR_BG]);
            GetClientRect(hwnd, &r);
            ExcludeClipRect(hdc, wt->offset_width, wt->offset_height,
                            wt->offset_width + wt->cols * wt->font_width,
                            wt->offset_height + wt->rows * wt->font_height);
            FillRect(hdc, &r, brush);
            DeleteObject(brush);
        }

        EndPaint(hwnd, &p);
        return 0;
    }

    case WM_SIZE:
        win32_term_resized(wt);
        return 0;

    case WM_SETFOCUS:
        wt->has_focus = true;
        if (CreateCaret(hwnd, NULL, wt->font_width, wt->font_height)) {
            wt->caret_created = true;
            SetCaretPos(wt->caret_x * wt->font_width + wt->offset_width,
                        wt->caret_y * wt->font_height + wt->offset_height);
        }
        term_set_focus(wt->term, true);
        term_update(wt->term);
        return 0;

    case WM_KILLFOCUS:
        wt->has_focus = false;
        if (wt->caret_created) {
            DestroyCaret();
            wt->caret_created = false;
        }
        term_set_focus(wt->term, false);
        term_update(wt->term);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        unsigned char buf[20];
        int len;

        if (!wt->term_hwnd) {
            break;
        }
        len = win32_term_translate_key(wt, msg, wp, lp, buf);
        if (len == -1) {
            wt->key_handled = false;
            return DefWindowProc(hwnd, msg, wp, lp);
        }
        wt->key_handled = true;
        if (len != 0) {
            term_seen_key_event(wt->term);
            term_keyinput(wt->term, -1, buf, len);
            win32_term_show_mouseptr(wt, false);
        }
        win32_term_kick_timer(wt);
        return 0;
    }

    case WM_CHAR:
    case WM_SYSCHAR: {
        /*
         * The key translation above has already turned ordinary typing
         * into terminal input, so the WM_CHAR that TranslateMessage()
         * makes from the same keystroke must be dropped.  What is left
         * here is input with no WM_KEYDOWN of its own -- an IME commit, or
         * a character posted to us by some other program -- which we do
         * want.
         */
        wchar_t c = (wchar_t)wp;

        if (wt->key_handled) {
            wt->key_handled = false;
            return 0;
        }
        term_seen_key_event(wt->term);
        term_keyinputw(wt->term, &c, 1);
        win32_term_kick_timer(wt);
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
        Mouse_Button b;
        Mouse_Action a;
        bool dbl = (msg == WM_LBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK ||
                    msg == WM_RBUTTONDBLCLK);
        bool down = dbl || (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN ||
                            msg == WM_RBUTTONDOWN);

        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
            msg == WM_LBUTTONDBLCLK) {
            b = MBT_LEFT;
        } else if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ||
                   msg == WM_MBUTTONDBLCLK) {
            b = MBT_MIDDLE;
        } else {
            b = MBT_RIGHT;
        }
        /*
         * A double click selects a word and a triple click a line.  The
         * window class has CS_DBLCLKS, so Windows tells us about the
         * second click; the third arrives as another WM_xBUTTONDBLCLK,
         * which is how PuTTY's own front end detects it too.
         */
        a = down ? (dbl ? MA_2CLK : MA_CLICK) : MA_RELEASE;

        if (down) {
            SetFocus(hwnd);
            SetCapture(hwnd);
        } else {
            ReleaseCapture();
        }
        win32_term_mouse(wt, b, a, wp, lp);
        win32_term_kick_timer(wt);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (wp & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) {
            Mouse_Button b = (wp & MK_LBUTTON) ? MBT_LEFT :
                (wp & MK_MBUTTON) ? MBT_MIDDLE : MBT_RIGHT;
            win32_term_mouse(wt, b, MA_DRAG, wp, lp);
            win32_term_kick_timer(wt);
        } else {
            win32_term_show_mouseptr(wt, true);
        }
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int lines = delta / WHEEL_DELTA;

        if (wt->raw_mouse) {
            /*
             * The application is reading the mouse, so report the wheel as
             * buttons 4 and 5 the way xterm does.  term_mouse() wants
             * client coordinates, and WM_MOUSEWHEEL gives screen ones.
             */
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            for (int i = 0; i < (lines < 0 ? -lines : lines); i++) {
                win32_term_mouse(wt, lines > 0 ? MBT_WHEEL_UP :
                                 MBT_WHEEL_DOWN, MA_CLICK, wp,
                                 MAKELPARAM(pt.x, pt.y));
            }
        } else {
            term_scroll(wt->term, 0, -lines * 3);
        }
        win32_term_kick_timer(wt);
        return 0;
    }

    case WM_VSCROLL: {
        switch (LOWORD(wp)) {
        case SB_BOTTOM:
            term_scroll(wt->term, -1, 0);
            break;
        case SB_TOP:
            term_scroll(wt->term, 1, 0);
            break;
        case SB_LINEDOWN:
            term_scroll(wt->term, 0, 1);
            break;
        case SB_LINEUP:
            term_scroll(wt->term, 0, -1);
            break;
        case SB_PAGEDOWN:
            term_scroll(wt->term, 0, wt->rows / 2);
            break;
        case SB_PAGEUP:
            term_scroll(wt->term, 0, -wt->rows / 2);
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: {
            /*
             * HIWORD(wp) is only 16 bits, which is not enough for a long
             * scrollback, so ask for the real position.
             */
            SCROLLINFO si;
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(hwnd, SB_VERT, &si)) {
                term_scroll(wt->term, 1, si.nTrackPos);
            } else {
                term_scroll(wt->term, 1, HIWORD(wp));
            }
            break;
        }
        }
        win32_term_kick_timer(wt);
        return 0;
    }

    case WM_TIMER:
        if (wp == WIN32_TERM_TIMER_ID) {
            KillTimer(hwnd, WIN32_TERM_TIMER_ID);
            win32_term_kick_timer(wt);
            return 0;
        }
        break;

    case WM_ERASEBKGND: {
        /*
         * WM_PAINT paints the character grid and the sliver of margin left
         * over from dividing the window by the cell size -- but only cells
         * the terminal believes are invalid, and only the margin as it was
         * when the grid was last measured.  Neither is true of a window
         * that has just been dragged to a new size, so the background does
         * have to be erased.  It is cheap: WM_ERASEBKGND only arrives when
         * something invalidates with bErase, which here means a resize or a
         * DPI change, never ordinary terminal output.
         */
        HDC hdc = (HDC)wp;
        HBRUSH brush = CreateSolidBrush(wt->colours[COLOUR_BG]);
        RECT r;

        GetClientRect(hwnd, &r);
        FillRect(hdc, &r, brush);
        DeleteObject(brush);
        return 1;
    }

    case WM_GETDLGCODE:
        /* We want every key, including Tab and the arrows. */
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS |
            DLGC_WANTTAB;
    }

    return DefWindowProc(hwnd, msg, wp, lp);
}

/*
 * ----------------------------------------------------------------
 * Construction.
 */

void win32_term_global_init(void)
{
    static bool done;
    WNDCLASSEX wc;

    if (done) {
        return;
    }
    done = true;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = win32_term_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WIN32_TERM_CLASS;
    RegisterClassEx(&wc);
}

Win32Term *win32_term_new(HWND parent, const Win32TermCallbacks *cb,
                          void *opaque, const Win32TermOptions *opts)
{
    Win32Term *wt = snew(Win32Term);
    int cols = opts->cols;
    int rows = opts->rows;

    memset(wt, 0, sizeof(*wt));
    wt->termwin.vt = &win32_termwin_vt;
    wt->parent = parent;
    wt->cb = *cb;
    wt->opaque = opaque;
    wt->mouseptr_visible = true;
    wt->compose_keycode = 0x100;

    wt->dpi = opts->dpi ? opts->dpi : USER_DEFAULT_SCREEN_DPI;

    win32_term_global_init();
    win32_term_init_fonts(wt);

    /*
     * A size given in pixels -- "-serial vc:640x480" -- can only be turned
     * into a character grid once the font has been measured, which is why
     * this is here rather than in the caller.
     */
    if (cols <= 0 && opts->width > 0) {
        cols = opts->width / wt->font_width;
    }
    if (rows <= 0 && opts->height > 0) {
        rows = opts->height / wt->font_height;
    }

    wt->conf = win32_term_conf_new(cols, rows, opts->line_codepage);
    wt->cursor_type = conf_get_int(wt->conf, CONF_cursor_type);
    init_ucs(wt->conf, &wt->ucsdata);

    wt->cols = conf_get_int(wt->conf, CONF_width);
    wt->rows = conf_get_int(wt->conf, CONF_height);

    wt->term_hwnd = CreateWindowEx(
        0, WIN32_TERM_CLASS, "", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
        0, 0, wt->cols * wt->font_width, wt->rows * wt->font_height,
        parent, NULL, GetModuleHandle(NULL), wt);
    if (!wt->term_hwnd) {
        win32_term_free_fonts(wt);
        conf_free(wt->conf);
        sfree(wt);
        return NULL;
    }

    wt->term = term_init(wt->conf, &wt->ucsdata, &wt->termwin);

    /*
     * Which clipboard the mouse and the copy/paste keys use.  terminal.c
     * leaves this to the front end (PuTTY's window.c has the same function
     * under the same name); without it mouse_paste_clipboard stays
     * CLIP_NULL and right-click does nothing at all.
     */
    assert(wt->term->mouse_select_clipboards[0] == CLIP_LOCAL);
    wt->term->n_mouse_select_clipboards = 1;
    if (conf_get_bool(wt->conf, CONF_mouseautocopy)) {
        wt->term->mouse_select_clipboards[
            wt->term->n_mouse_select_clipboards++] = CLIP_SYSTEM;
    }
    switch (conf_get_int(wt->conf, CONF_mousepaste)) {
    case CLIPUI_IMPLICIT:
        wt->term->mouse_paste_clipboard = CLIP_LOCAL;
        break;
    case CLIPUI_EXPLICIT:
        wt->term->mouse_paste_clipboard = CLIP_SYSTEM;
        break;
    default:
        wt->term->mouse_paste_clipboard = CLIP_NULL;
        break;
    }
    wt->ldisc = win32_term_ldisc_new(wt->term, cb->send, opaque);
    wt->term->ldisc = wt->ldisc;
    term_size(wt->term, wt->rows, wt->cols, WIN32_TERM_SAVELINES);
    term_setup_window_titles(wt->term, NULL);

    win32_term_kick_timer(wt);
    return wt;
}

void win32_term_free(Win32Term *wt)
{
    if (!wt) {
        return;
    }
    if (wt->term_hwnd) {
        KillTimer(wt->term_hwnd, WIN32_TERM_TIMER_ID);
        SetWindowLongPtr(wt->term_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(wt->term_hwnd);
        wt->term_hwnd = NULL;
    }
    if (wt->term) {
        term_free(wt->term);
        wt->term = NULL;
    }
    win32_term_ldisc_free(wt->ldisc);
    win32_term_free_fonts(wt);
    if (wt->conf) {
        conf_free(wt->conf);
    }
    sfree(wt->title);
    sfree(wt);
}

void win32_term_set_dpi(Win32Term *wt, unsigned dpi)
{
    if (!dpi) {
        dpi = USER_DEFAULT_SCREEN_DPI;
    }
    if (dpi == wt->dpi) {
        return;
    }
    wt->dpi = dpi;

    win32_term_free_fonts(wt);
    win32_term_init_fonts(wt);

    /*
     * The cell size has changed, so the grid that fits the window has too.
     * Force the recount rather than letting win32_term_resized() short out
     * on an unchanged window size.
     */
    wt->cols = 0;
    wt->rows = 0;
    win32_term_resized(wt);
}

HWND win32_term_hwnd(Win32Term *wt)
{
    return wt->term_hwnd;
}

void win32_term_write(Win32Term *wt, const char *buf, int len)
{
    if (len <= 0) {
        return;
    }
    term_data(wt->term, buf, len);
    term_update(wt->term);
    win32_term_kick_timer(wt);
}

void win32_term_set_focus(Win32Term *wt, bool focus)
{
    if (focus) {
        SetFocus(wt->term_hwnd);
    }
}

void win32_term_size_hint(Win32Term *wt, int cols, int rows,
                          int *width, int *height)
{
    if (cols <= 0) {
        cols = wt->cols;
    }
    if (rows <= 0) {
        rows = wt->rows;
    }
    *width = cols * wt->font_width + GetSystemMetrics(SM_CXVSCROLL);
    *height = rows * wt->font_height;
}

void win32_term_copy(Win32Term *wt)
{
    static const int clips[] = { CLIP_SYSTEM };
    term_request_copy(wt->term, clips, 1);
}

void win32_term_paste(Win32Term *wt)
{
    term_request_paste(wt->term, CLIP_SYSTEM);
    win32_term_kick_timer(wt);
}

void win32_term_select_all(Win32Term *wt)
{
    static const int clips[] = { CLIP_SYSTEM };
    term_copyall(wt->term, clips, 1);
}

void win32_term_clear_scrollback(Win32Term *wt)
{
    term_clrsb(wt->term);
    win32_term_kick_timer(wt);
}

void win32_term_reset(Win32Term *wt)
{
    term_pwron(wt->term, true);
    win32_term_kick_timer(wt);
}
