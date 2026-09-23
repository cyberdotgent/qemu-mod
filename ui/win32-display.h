/*
 * QEMU native Win32 display driver -- internal interface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The backend is split in two: ui/win32-display.c owns the per-console
 * rendering (the DisplayChangeListener ops, the 2D blit and all guest
 * input), while ui/win32-tabs.c owns the single frame window that contains
 * them -- its tab strip, its menu bar and everything that follows from
 * there being exactly one top-level window.
 *
 * The split is along that line and not somewhere else because the frame is
 * the only part that has to know about all the consoles at once.  A render
 * window is created once, as a child of the frame, and is never reparented,
 * so it survives every tab switch, resize and fullscreen transition.
 *
 * Threading
 * ---------
 * The backend takes over the process's main thread and runs QEMU's main
 * loop on a thread of its own, the way ui/cocoa.m does -- see the
 * "threading" section at the top of ui/win32-display.c.  Everything in
 * ui/win32-tabs.c, ui/win32-term*.c and every window procedure therefore
 * runs on the *UI* thread, while the DisplayChangeListener ops and the
 * chardev writes arrive on the *QEMU* thread and have to be marshalled
 * across with win32_ui_post().
 *
 * Any call from the UI thread into emulator state must hold the BQL.  Use
 * BQL_LOCK_GUARD() from "qemu/main-loop.h": it is the same conditional
 * lock ui/cocoa.m's with_bql() is, re-entrancy check included, so it is
 * also correct on the paths that already hold it.
 */

#ifndef UI_WIN32_DISPLAY_H
#define UI_WIN32_DISPLAY_H

#include <windows.h>

#include "qemu/thread.h"
#include "ui/console.h"
#include "ui/kbd-state.h"

#include "win32-term.h"

/*
 * A tab is one of two things: a QemuConsole-backed graphics console, or a
 * chardev-backed terminal (a serial port or the HMP monitor) running the
 * vendored PuTTY terminal emulator.  struct win32_console covers both; a
 * terminal tab has tcon set and dcl.con NULL, a graphics tab the reverse.
 * See ui/win32-term-chardev.c for why serial and monitor consoles stop
 * being QemuConsoles at all when this backend is in use.
 */
struct win32_term_console;

#define WIN32_FRAME_CLASS   "QemuWin32Frame"
#define WIN32_WINDOW_CLASS  "QemuWin32Display"

/* zoom limits, matching ui/gtk.c's VC_SCALE_* so the two feel the same */
#define WIN32_SCALE_MIN   0.25
#define WIN32_SCALE_MAX   4.0
#define WIN32_SCALE_STEP  0.25

struct win32_console {
    DisplayChangeListener dcl;
    /*
     * The surface the UI thread paints from.  It is *not* the surface
     * ui/console.c handed to dpy_gfx_switch: that one is freed the moment
     * the callback returns.  This is a private DisplaySurface holding a
     * reference of its own on the same pixman image -- see
     * win32_2d_switch().  Touched only by the UI thread.
     */
    DisplaySurface *surface;
    HWND hwnd;              /* child render window, owned by the frame */
    QKbdState *kbd;
    int idx;
    int con_index;          /* qemu_console_get_index(), cached at init */
    int tab;                /* index in the tab control, -1 when absent */

    /*
     * Handover slots, written on the QEMU thread and drained on the UI
     * thread; all of them are covered by win32_ui_mutex.  The *_posted
     * flags coalesce: while one message is still in flight another is not
     * sent, so a busy guest cannot outrun the UI thread's message queue.
     */
    DisplaySurface *pending_surface;
    bool pending_switch;
    bool switch_posted;
    bool damage_posted;
    bool damage_valid;
    RECT damage;            /* accumulated dirty rectangle, guest pixels */

    /* non-NULL for a terminal tab; then dcl is never registered */
    struct win32_term_console *tcon;

    /* zoom, applied by stretching the blit into the child window */
    double scale_x;
    double scale_y;

    bool hover_tracked;     /* a TrackMouseEvent() request is outstanding */
};

/* the consoles, in console-index order */
extern int win32_num_outputs;
extern struct win32_console *win32_consoles;
/* the console whose tab is selected; NULL until the first one appears */
extern struct win32_console *win32_active;

extern HWND win32_frame;
extern HWND win32_tabctl;
extern DisplayOptions *win32_opts;

extern bool gui_grab;
extern bool gui_fullscreen;
extern bool gui_free_scale;
extern bool gui_grab_on_hover;
extern bool gui_show_tabs;
extern bool alt_grab;
extern bool ctrl_grab;

extern bool absolute_enabled;
extern bool guest_cursor;
extern HCURSOR guest_sprite;
extern HCURSOR cursor_arrow;

static inline struct win32_console *win32_console_from_hwnd(HWND hwnd)
{
    return (struct win32_console *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
}

/*
 * Covers the handover slots in struct win32_console, and the terminal
 * consoles' output queues.  Never held across a call into QEMU, and never
 * held while waiting for anything.
 */
extern QemuMutex win32_ui_mutex;

/*
 * Messages the QEMU thread sends the UI thread.  They are posted, never
 * sent: the QEMU thread must never wait for the UI thread, because the UI
 * thread is allowed to wait for the BQL and a cycle between the two would
 * deadlock the moment a Windows modal loop was on the stack.
 */
enum {
    WIN32_UI_DAMAGE = WM_APP,   /* wparam: console index */
    WIN32_UI_SWITCH,            /* wparam: console index */
    WIN32_UI_CAPTION,
    WIN32_UI_MOUSE_SET,         /* wparam: on, lparam: MAKELPARAM(x, y) */
    WIN32_UI_CURSOR_DEFINE,     /* lparam: QEMUCursor *, one reference */
    WIN32_UI_MOUSE_MODE,
    WIN32_UI_TERM_OUTPUT,       /* wparam: console index */
    WIN32_UI_SHUTDOWN,
};

void win32_ui_post(UINT msg, WPARAM wparam, LPARAM lparam);

/* ui/win32-display.c */
void win32_show_cursor(bool show);
void win32_clip_cursor(bool clip);
void win32_grab_start(void);
void win32_grab_end(void);
void win32_release_modifiers(struct win32_console *wcon);
void win32_console_redraw(struct win32_console *wcon);
void win32_console_size(struct win32_console *wcon, int *w, int *h);

/* ui/win32-tabs.c */
void win32_frame_init(void);
void win32_frame_fini(void);
void win32_frame_add_console(struct win32_console *wcon);
void win32_frame_del_console(struct win32_console *wcon);
void win32_frame_activate(struct win32_console *wcon);
void win32_frame_layout(void);
double win32_dpi_scale(void);
void win32_frame_fit(void);
void win32_update_caption(void);
void win32_toggle_fullscreen(void);
void win32_zoom_step(double delta);
void win32_zoom_fixed(void);
void win32_toggle_free_scale(void);
void win32_frame_relabel(void);
void win32_frame_note_user_selection(void);

/* ui/win32-term-chardev.c */
void win32_term_chardev_register(void);
int win32_term_nb_vcs(void);
void win32_term_console_init(struct win32_console *wcon, int vc_index);
void win32_term_console_fini(struct win32_console *wcon);
const char *win32_term_console_label(struct win32_console *wcon);
void win32_term_consoles_set_dpi(unsigned dpi);
void win32_term_console_drain(struct win32_console *wcon);
Win32Term *win32_term_console_term(struct win32_console *wcon);

static inline bool win32_console_is_term(const struct win32_console *wcon)
{
    return wcon && wcon->tcon != NULL;
}
bool win32_dialog_filter(MSG *msg);

#endif /* UI_WIN32_DISPLAY_H */
