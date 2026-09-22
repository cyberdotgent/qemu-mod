/*
 * QEMU-mod: the PuTTY-terminal-backed text console -- internal interface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This header is for the files that live on the *PuTTY* side of the
 * boundary: ui/win32-term-shim.c, ui/win32-term.c and the vendored code
 * under ui/putty/.  It pulls in putty.h, which is not co-installable with
 * qemu/osdep.h (both define a large number of the same generic names), so
 * nothing that includes this may include any QEMU header.
 *
 * The QEMU side talks to this code only through ui/win32-term.h, which is
 * written in plain C plus <windows.h> and includes neither tree.
 */

#ifndef UI_WIN32_TERM_INTERNAL_H
#define UI_WIN32_TERM_INTERNAL_H

#include "putty.h"
#include "terminal/terminal.h"

#include "win32-term.h"

/*
 * PuTTY's Ldisc is its local line discipline: the thing a terminal hands
 * keystrokes to, which decides whether to echo them locally and when to
 * pass them to the backend.  A QEMU serial port or HMP monitor always wants
 * the raw, unechoed, unbuffered path -- the guest (or the monitor's own
 * readline) does all the editing -- so ours is just a forwarding shim.
 *
 * struct Ldisc_tag is only ever declared, never defined, by PuTTY's own
 * headers, so defining it here replaces ldisc.c wholesale.
 */
struct Ldisc_tag {
    Terminal *term;
    void (*send)(void *opaque, const char *buf, int len);
    void *opaque;
};

Ldisc *win32_term_ldisc_new(Terminal *term,
                            void (*send)(void *opaque,
                                         const char *buf, int len),
                            void *opaque);
void win32_term_ldisc_free(Ldisc *ldisc);

/*
 * Build a Conf carrying PuTTY's own compiled-in defaults, overridden with
 * the settings that make sense for a QEMU serial/monitor console.  See
 * ui/win32-term-shim.c for the list of deliberate overrides.
 */
Conf *win32_term_conf_new(int cols, int rows, const char *line_codepage);

/*
 * PuTTY expects its front end to run a main loop that services toplevel
 * callbacks and timers.  We are a guest inside QEMU's main loop instead,
 * so the shim exports a single pump that ui/win32-term.c calls from the
 * frame window's timer and after every batch of terminal input.
 *
 * Returns true if a timer is pending, and if so stores the tick count it
 * is due at in *next.
 */
bool win32_term_pump(unsigned long *next);

/*
 * The terminal itself.  The field names that ui/win32-term-keys.c touches
 * are spelled exactly as PuTTY's WinGuiSeat spells them, so that the
 * TranslateKey() body copied from window.c needs no edits at all.
 */
struct Win32Term {
    TermWin termwin;

    Terminal *term;
    Conf *conf;
    Ldisc *ldisc;
    struct unicode_data ucsdata;

    HWND parent;
    HWND term_hwnd;

    Win32TermCallbacks cb;
    void *opaque;

    /*
     * Set when the last WM_KEYDOWN was fully dealt with by the key
     * translation, so that the WM_CHAR the pump's TranslateMessage()
     * synthesises for it can be ignored instead of sending the character
     * twice.  PuTTY itself sidesteps this by never calling
     * TranslateMessage(); QEMU's shared message pump does call it, because
     * the graphics consoles need it.
     */
    bool key_handled;

    /* Keyboard state, owned by ui/win32-term-keys.c. */
    int compose_state;
    int compose_keycode;
    int compose_char;
    int alt_numberpad_accumulator;

    /* Font, and the cell metrics that follow from it. */
    HFONT fonts[4];                    /* [bold][underline] */
    int font_width, font_height, font_descent;
    unsigned dpi;                      /* the monitor DPI it was sized for */

    /* Character grid, and the padding left over in the window. */
    int cols, rows;
    int offset_width, offset_height;

    /* OSC 4 palette: 256 xterm colours plus PuTTY's six special ones. */
    COLORREF colours[OSC4_NCOLOURS];

    /*
     * The drawing context, valid only between setup_draw_ctx() and
     * free_draw_ctx().  During WM_PAINT it is BeginPaint()'s HDC, which we
     * must not release; paint_hdc says so.
     */
    HDC hdc;
    bool paint_hdc;

    bool has_focus;
    bool raw_mouse;                    /* the guest asked for mouse events */
    bool mouseptr_visible;
    bool caret_created;
    bool flashing;                     /* a visual bell is on screen */
    int caret_x, caret_y;
    int cursor_type;                   /* CURSOR_BLOCK/UNDERLINE/VERTICAL */

    char *title;                       /* last OSC 0/2 title, UTF-8 */

    /* Mouse selection state. */
    Mouse_Button last_mouse_button;
    int last_mouse_x, last_mouse_y;
};

#define WIN32_TERM_SAVELINES 10000

/* ui/win32-term-keys.c */
int win32_term_translate_key(Win32Term *wt, UINT message, WPARAM wParam,
                             LPARAM lParam, unsigned char *output);

/* ui/win32-term.c, called from the above */
void win32_term_show_mouseptr(Win32Term *wt, bool show);
void win32_term_send_break(Win32Term *wt);

#endif /* UI_WIN32_TERM_INTERNAL_H */
