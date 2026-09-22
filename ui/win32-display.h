/*
 * QEMU native Win32 display driver -- internal interface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The backend is split in two: ui/win32-display.c owns the per-console
 * rendering (the DisplayChangeListener ops, the 2D blit, the EGL path and
 * all guest input), while ui/win32-tabs.c owns the single frame window that
 * contains them -- its tab strip, its menu bar and everything that follows
 * from there being exactly one top-level window.
 *
 * The split is along that line and not somewhere else because the frame is
 * the only part that has to know about all the consoles at once, and the
 * render windows are the only part that has to know about EGL.  A render
 * window is created once, as a child of the frame, and is never reparented,
 * so the EGLSurface bound to its HWND outlives every tab switch, resize and
 * fullscreen transition.
 */

#ifndef UI_WIN32_DISPLAY_H
#define UI_WIN32_DISPLAY_H

#include <windows.h>

#include "ui/console.h"
#include "ui/kbd-state.h"

#ifdef CONFIG_OPENGL
#include "ui/egl-helpers.h"
#include "ui/shader.h"
#endif

#define WIN32_FRAME_CLASS   "QemuWin32Frame"
#define WIN32_WINDOW_CLASS  "QemuWin32Display"

/* zoom limits, matching ui/gtk.c's VC_SCALE_* so the two feel the same */
#define WIN32_SCALE_MIN   0.25
#define WIN32_SCALE_MAX   4.0
#define WIN32_SCALE_STEP  0.25

struct win32_console {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    HWND hwnd;              /* child render window, owned by the frame */
    QKbdState *kbd;
    int idx;
    int tab;                /* index in the tab control, -1 when absent */
    int idle_counter;

    /* zoom, applied by stretching the blit into the child window */
    double scale_x;
    double scale_y;

    bool hover_tracked;     /* a TrackMouseEvent() request is outstanding */

#ifdef CONFIG_OPENGL
    bool opengl;
    DisplayGLCtx dgc;
    EGLSurface esurface;
    EGLContext ectx;
    QemuGLShader *gls;
    int updates;
    bool scanout_mode;
    bool y0_top;
    egl_fb guest_fb;
    egl_fb win_fb;
#endif
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
void win32_frame_fit(void);
void win32_update_caption(void);
void win32_toggle_fullscreen(void);
void win32_zoom_step(double delta);
void win32_zoom_fixed(void);
void win32_toggle_free_scale(void);

#endif /* UI_WIN32_DISPLAY_H */
