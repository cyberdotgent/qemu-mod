/*
 * QEMU-mod: PuTTY-terminal-backed text consoles for the native Win32 UI
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This is the whole of the boundary between QEMU and the vendored PuTTY
 * terminal emulator in ui/putty/.  It deliberately mentions neither tree's
 * headers: putty.h and qemu/osdep.h cannot be included into the same
 * translation unit, so everything that crosses the line does so as plain C
 * plus <windows.h>.
 *
 *   QEMU side (includes qemu/osdep.h):  ui/win32-term-chardev.c,
 *                                       ui/win32-tabs.c
 *   PuTTY side (includes putty.h):      ui/win32-term.c,
 *                                       ui/win32-term-shim.c
 */

#ifndef UI_WIN32_TERM_H
#define UI_WIN32_TERM_H

#include <windows.h>
#include <stdbool.h>

typedef struct Win32Term Win32Term;

/*
 * Callbacks the QEMU side supplies.  All are invoked on the UI thread,
 * from inside a window procedure or a terminal-output call.
 *
 *   send        the user typed (or pasted) something; hand it to the
 *               chardev.  Bytes are already in the terminal's line
 *               codepage, which we always set to UTF-8.
 *   title       the guest changed the window title (OSC 0/2); used as the
 *               tab label.  May be NULL.
 *   bell        the guest rang the bell.  May be NULL.
 *   resized     the terminal's character grid changed size.  May be NULL.
 */
typedef struct Win32TermCallbacks {
    void (*send)(void *opaque, const char *buf, int len);
    void (*title)(void *opaque, const char *title);
    void (*bell)(void *opaque);
    void (*resized)(void *opaque, int cols, int rows);
    /*
     * The user asked for a serial BREAK (Ctrl-Break).  May be NULL, in
     * which case the keystroke is swallowed.
     */
    void (*send_break)(void *opaque);
} Win32TermCallbacks;

/*
 * How a terminal should come up.  Zero means "you choose".
 */
typedef struct Win32TermOptions {
    unsigned dpi;               /* monitor DPI; the font is sized in points */
    int cols, rows;             /* size in characters, if known */
    int width, height;          /* size in pixels, if that is all we have */
    const char *line_codepage;  /* PuTTY codepage name; NULL means UTF-8 */
} Win32TermOptions;

/*
 * Create a terminal as a child window of @parent.  The window is created
 * hidden; the caller positions and shows it.
 */
Win32Term *win32_term_new(HWND parent, const Win32TermCallbacks *cb,
                          void *opaque, const Win32TermOptions *opts);

/*
 * Adopt a new monitor DPI.  The font is specified in points, so the cell
 * size -- and therefore the whole character grid -- is re-derived.
 */
void win32_term_set_dpi(Win32Term *term, unsigned dpi);
void win32_term_free(Win32Term *term);

HWND win32_term_hwnd(Win32Term *term);

/* Feed output from the chardev to the terminal. */
void win32_term_write(Win32Term *term, const char *buf, int len);

/* Focus follows the tab strip, not Windows' idea of the focused window. */
void win32_term_set_focus(Win32Term *term, bool focus);

/*
 * The pixel size the terminal would like, for the given character grid.
 * Used by the frame to size itself around a freshly created console.
 */
void win32_term_size_hint(Win32Term *term, int cols, int rows,
                          int *width, int *height);

/* Menu actions. */
void win32_term_copy(Win32Term *term);
void win32_term_paste(Win32Term *term);
void win32_term_select_all(Win32Term *term);
void win32_term_clear_scrollback(Win32Term *term);
void win32_term_reset(Win32Term *term);

/*
 * Called once, before any terminal exists, to register the window class.
 * Safe to call more than once.
 */
void win32_term_global_init(void);

#endif /* UI_WIN32_TERM_H */
