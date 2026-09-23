/*
 * QEMU native Win32 display driver
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A display backend built directly on the Win32 API, as an alternative to
 * SDL on Windows hosts.  It implements nothing but QemuDisplay and
 * DisplayChangeListenerOps, with no hooks into the emulator core beyond
 * those two interfaces.  Keeping the surface that narrow is what lets
 * upstream changes to ui/console.h be absorbed mechanically.
 *
 * This file owns one render window per console.  They are children of the
 * single frame window in ui/win32-tabs.c, which owns the tab strip, the menu
 * bar and everything else that has to reason about all the consoles at once.
 *
 * Threading
 * ---------
 * The UI owns the process's main thread and QEMU's main loop runs on a
 * thread of its own, exactly as on macOS: system/main.c already creates
 * that thread and calls whatever ui/ puts in qemu_main(), and the comment
 * there ("main thread must be reserved for UI") is as true on Windows as it
 * is on Darwin.  It has to be, because Windows runs a modal message loop of
 * its own inside DefWindowProc for several perfectly ordinary things the
 * user does with a window -- holding the menu bar open, dragging the title
 * bar, dragging a border, holding a scrollbar thumb.  That loop does not
 * return until the user lets go.  While the pump rode on dpy_refresh, the
 * thread it stopped was QEMU's main loop, so the vCPUs, block I/O, the
 * monitor and QMP were all held up for as long as the mouse button was.
 * Now it stops only the display and input.
 *
 * The split is the same one ui/cocoa.m makes, and has the same two halves:
 *
 *   - QEMU -> UI.  DisplayChangeListenerOps callbacks, the mouse-mode
 *     notifier and the terminal chardev's writes all arrive on the QEMU
 *     thread, and none of them may touch a window: on Windows the thread
 *     that created a window is the only one that may pump its messages, and
 *     the PuTTY terminal state in ui/win32-term.c is the UI thread's alone.
 *     Where cocoa.m does dispatch_async(dispatch_get_main_queue(), ...),
 *     this file does win32_ui_post() -- PostMessage() to a message-only
 *     window created on the UI thread.  Posting, never sending: see below.
 *
 *   - UI -> QEMU.  Input, the menu actions and the caption all read or
 *     write emulator state and so need the BQL.  cocoa.m wraps those in
 *     with_bql(); C has no blocks, but QEMU's own BQL_LOCK_GUARD() is the
 *     same thing -- a scoped conditional lock with the same bql_locked()
 *     re-entrancy check, which matters because the very same functions are
 *     also reached from init and cleanup with the BQL already held.
 *
 * Deadlock is avoided by making the wait graph acyclic rather than by being
 * careful: *the QEMU thread never waits for the UI thread*.  Every
 * QEMU -> UI handover is a PostMessage(), so a UI thread parked inside a
 * menu's modal loop cannot hold the QEMU thread up, and a UI thread that
 * blocks on the BQL is therefore always waiting on someone who is running.
 * The one exception is the shutdown handshake in win32_display_cleanup(),
 * which drops the BQL for the duration of its bounded wait precisely so
 * that it is not an exception at all.
 *
 * Surface lifetime is the other thing the split breaks, and it breaks
 * silently.  The DisplaySurface passed to dpy_gfx_switch is freed as soon
 * as the callback returns, so once painting happens on another thread a
 * blit from it is a use-after-free.  cocoa.m answers this with
 * pixman_image_ref() before the dispatch_async, and so does this file:
 * win32_2d_switch() wraps the incoming image in a DisplaySurface of its
 * own via qemu_create_displaysurface_pixman(), which takes the reference,
 * and the UI thread frees the one it is replacing.  A reference rather than
 * a copy, for the same reason cocoa.m does: the guest writes into that
 * buffer continuously, so a copy would have to be retaken on every frame to
 * be worth anything, and the tearing a reference can show is exactly the
 * tearing every other backend shows.
 *
 * The 2D path blits the guest surface straight from pixman memory with
 * StretchDIBits(), so there is no intermediate copy and no texture to keep in
 * sync.  Because the render window's client area is always exactly the area
 * the image is stretched into, the zoom factor is expressed purely as a
 * window size and the blit itself never has to know about it.
 *
 * There is no GL path: -display win32,gl=on is rejected.  It used to exist,
 * built on EGL/ANGLE, and it is gone because of the threading split
 * described below.  EGL contexts are thread-affine, so the render window's
 * context would have to be current only on the UI thread -- while the
 * guest's own context (virtio-gpu-gl, virgl) is made current on the QEMU
 * thread and drew into the very same EGLSurface.  No QEMU display backend
 * has ever combined a threaded UI with GL; making this one the first is a
 * larger piece of work than the freeze it would have to pay for, so the
 * emulator running while a menu is open won out over accelerated guest
 * rendering that was never demonstrated end to end.
 */

#include "qemu/osdep.h"

#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM */
#include <math.h>

#include "qemu/error-report.h"
#include "qemu/help-texts.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "qemu-main.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "ui/win32-display.h"
#include "ui/win32-kbd-hook.h"
#include "standard-headers/linux/input-event-codes.h"

/*
 * Refresh pacing, mirroring ui/sdl2.c: refresh aggressively while the user
 * is doing something, then fall back to the default interval once things go
 * quiet.
 */
#define WIN32_REFRESH_INTERVAL_BUSY 10

int win32_num_outputs;
struct win32_console *win32_consoles;
DisplayOptions *win32_opts;

QemuMutex win32_ui_mutex;

bool gui_grab;
bool alt_grab;
bool ctrl_grab;

bool absolute_enabled;
bool guest_cursor;
HCURSOR guest_sprite;
HCURSOR cursor_arrow;

static ATOM win32_class_atom;
static ATOM win32_ui_class_atom;
static bool swallow_next_char;
static int guest_x, guest_y;
static bool cursor_visible = true;
static Notifier mouse_mode_notifier;

/* the message-only window win32_ui_post() posts to; UI thread */
static HWND win32_uiwnd;
/* signalled by the UI thread once it has torn its windows down */
static HANDLE win32_ui_done;

static void win32_ui_handle(UINT msg, WPARAM wparam, LPARAM lparam);
static void win32_ui_teardown(void);

/* ------------------------------------------------------------------ */
/* QEMU thread -> UI thread                                             */

/*
 * Hand a message to the UI thread.  Always asynchronous, so that the
 * calling thread -- which in every real case is the QEMU main loop, holding
 * the BQL -- cannot be held up by a UI thread that is inside one of
 * Windows' modal loops or waiting for the BQL itself.
 *
 * PostMessage() can fail: a thread's message queue holds 10000 messages by
 * default, and a UI thread held in a modal loop for long enough with a very
 * busy guest could in principle reach that.  Every message this backend
 * posts is coalesced -- at most one of each kind per console is ever in
 * flight -- so the real bound is a couple of dozen, and a failure here means
 * something is wrong rather than something is busy.  Say so, once.
 */
void win32_ui_post(UINT msg, WPARAM wparam, LPARAM lparam)
{
    static bool complained;

    if (!win32_uiwnd) {
        return;
    }
    if (!PostMessage(win32_uiwnd, msg, wparam, lparam)) {
        if (!complained) {
            complained = true;
            warn_report("win32: could not post message %u to the UI thread "
                        "(error %lu); the display may stop updating",
                        (unsigned)(msg - WM_APP), GetLastError());
        }
    }
}

/*
 * How recently the UI thread last saw something worth being quick about.
 * Read by the refresh callbacks on the QEMU thread to pace dcl.update_interval,
 * which is what the per-console idle counter used to do back when the pump
 * and the refresh were the same call.
 */
static int win32_ui_active_tick;

static void win32_ui_note_activity(void)
{
    g_atomic_int_set(&win32_ui_active_tick, (int)GetTickCount());
}

static void win32_ui_tick(void);

static LRESULT CALLBACK win32_uiproc(HWND hwnd, UINT msg,
                                     WPARAM wparam, LPARAM lparam)
{
    if (msg >= WM_APP && msg <= WIN32_UI_SHUTDOWN) {
        win32_ui_handle(msg, wparam, lparam);
        return 0;
    }
    if (msg == WM_TIMER) {
        win32_ui_tick();
        return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

/* ------------------------------------------------------------------ */
/* render windows                                                       */

/*
 * How many physical pixels the guest image wants, which is the only place the
 * two scale factors meet:
 *
 *   - wcon->scale_x/scale_y is the user's zoom, in guest pixels per guest
 *     pixel.  View->Zoom In/Out steps it, Zoom 100% resets it to 1.0.
 *   - win32_dpi_scale() is the monitor's DPI factor, in physical pixels per
 *     logical pixel.  The process is per-monitor-DPI-aware, so Windows does
 *     not apply this for us and nothing else in the backend applies it either.
 *
 * They multiply: zoom is relative to whatever "100%" means on this monitor, so
 * at 150% DPI a 1:1 guest pixel is 1.5 physical pixels and a 2x zoom of it is
 * 3.  Best Fit (win32_frame_fit()) and the minimum track size therefore get
 * correctly sized windows at any DPI, and the zoom steps stay relative.
 *
 * This is the *only* place the DPI factor is applied.  StretchDIBits()
 * stretches the guest surface into the render window's client rectangle, so
 * the blit already scales to whatever window size comes out of here, and
 * applying the factor again inside it would square it.
 */
void win32_console_size(struct win32_console *wcon, int *w, int *h)
{
    double dpi = win32_dpi_scale();

    if (win32_console_is_term(wcon)) {
        /*
         * A terminal sizes itself in whole character cells, and its zoom
         * is the font, not a stretch of a pixel buffer -- so no scale_x
         * and no DPI factor here.
         */
        win32_term_size_hint(win32_term_console_term(wcon), 0, 0, w, h);
        return;
    }

    if (!wcon || !wcon->surface) {
        *w = 0;
        *h = 0;
        return;
    }
    *w = lround(surface_width(wcon->surface) * wcon->scale_x * dpi);
    *h = lround(surface_height(wcon->surface) * wcon->scale_y * dpi);
}

void win32_console_redraw(struct win32_console *wcon)
{
    if (!wcon || !wcon->hwnd) {
        return;
    }
    InvalidateRect(wcon->hwnd, NULL, FALSE);
}

static void win32_window_create(struct win32_console *wcon)
{
    if (!wcon->surface) {
        return;
    }
    assert(!wcon->hwnd);
    assert(win32_frame);

    /*
     * A child window, never a top-level one: the frame decides where it goes
     * and whether it is visible, and its HWND -- and with it the EGL surface
     * bound to that HWND -- then stays put for as long as the console has a
     * surface.
     */
    wcon->hwnd = CreateWindowEx(0, WIN32_WINDOW_CLASS, NULL,
                                WS_CHILD | WS_CLIPSIBLINGS,
                                0, 0, 1, 1,
                                win32_frame, NULL,
                                GetModuleHandle(NULL), NULL);
    if (!wcon->hwnd) {
        error_report("win32: could not create window (error %lu)",
                     GetLastError());
        exit(1);
    }

    SetWindowLongPtr(wcon->hwnd, GWLP_USERDATA, (LONG_PTR)wcon);

    win32_frame_add_console(wcon);
}

static void win32_window_destroy(struct win32_console *wcon)
{
    if (!wcon->hwnd) {
        return;
    }
    SetWindowLongPtr(wcon->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(wcon->hwnd);
    wcon->hwnd = NULL;
    wcon->tab = -1;
    wcon->hover_tracked = false;

    if (win32_frame) {
        win32_frame_del_console(wcon);
    }
}

/* ------------------------------------------------------------------ */
/* cursor and input grab                                                */

void win32_show_cursor(bool show)
{
    if (win32_opts->has_show_cursor && win32_opts->show_cursor) {
        show = true;
    }
    if (show == cursor_visible) {
        return;
    }
    /*
     * ShowCursor() keeps an internal counter rather than a flag, so it must
     * be called exactly once per transition.
     */
    ShowCursor(show);
    cursor_visible = show;
}

void win32_clip_cursor(bool clip)
{
    HWND hwnd = win32_active ? win32_active->hwnd : NULL;
    RECT r;

    if (!clip || !hwnd) {
        ClipCursor(NULL);
        return;
    }
    if (GetClientRect(hwnd, &r)) {
        MapWindowPoints(hwnd, NULL, (POINT *)&r, 2);
        ClipCursor(&r);
    }
}

void win32_grab_start(void)
{
    struct win32_console *wcon = win32_active;
    QemuConsole *con = wcon ? wcon->dcl.con : NULL;
    bool graphic;

    if (!con || !wcon->hwnd) {
        return;
    }
    {
        BQL_LOCK_GUARD();
        graphic = qemu_console_is_graphic(con);
    }
    if (!graphic) {
        return;
    }
    if (GetForegroundWindow() != win32_frame) {
        return;
    }

    if (guest_cursor) {
        SetCursor(guest_sprite);
    } else {
        win32_show_cursor(false);
    }
    win32_clip_cursor(true);
    win32_kbd_set_grab(true);
    gui_grab = true;
    win32_update_caption();
}

void win32_grab_end(void)
{
    win32_clip_cursor(false);
    win32_kbd_set_grab(false);
    gui_grab = false;
    win32_show_cursor(true);
    win32_update_caption();
}

/*
 * The first QemuConsole-backed tab, or NULL if there is none.  Terminal
 * tabs share the array but have no QemuConsole, no surface, no QKbdState
 * and no GL context, so anything that wants those has to ask for a
 * graphics tab by name rather than assuming index 0 is one.
 */
static struct win32_console *win32_first_gfx_console(void)
{
    int i;

    for (i = 0; win32_consoles && i < win32_num_outputs; i++) {
        if (!win32_console_is_term(&win32_consoles[i])) {
            return &win32_consoles[i];
        }
    }
    return NULL;
}

/*
 * The notifier itself fires on the QEMU thread, from the input layer; the
 * work it wants done -- ending the grab -- is the UI thread's.
 */
static void win32_mouse_mode_change(Notifier *notify, void *data)
{
    win32_ui_post(WIN32_UI_MOUSE_MODE, 0, 0);
}

static void win32_mouse_mode_changed(void)
{
    struct win32_console *gfx = win32_first_gfx_console();
    bool absolute;

    if (!gfx) {
        return;
    }
    {
        BQL_LOCK_GUARD();
        absolute = qemu_input_is_absolute(gfx->dcl.con);
    }
    if (absolute) {
        absolute_enabled = true;
    } else if (absolute_enabled) {
        if (!gui_fullscreen) {
            win32_grab_end();
        }
        absolute_enabled = false;
    }
}

/* ------------------------------------------------------------------ */
/* pointer events                                                       */

static void win32_send_mouse_motion(struct win32_console *wcon,
                                    int x, int y, int dx, int dy, bool relative)
{
    BQL_LOCK_GUARD();

    if (!qemu_console_is_graphic(wcon->dcl.con) || !wcon->surface) {
        return;
    }

    if (qemu_input_is_absolute(wcon->dcl.con)) {
        RECT r;
        if (relative || !GetClientRect(wcon->hwnd, &r) ||
            r.right <= 0 || r.bottom <= 0) {
            return;
        }
        /*
         * The window is free to be any size; scale the pointer back into
         * surface coordinates so that the guest sees the position the user
         * is actually pointing at.  This is also what makes zooming work for
         * the pointer as well as the image.
         */
        qemu_input_queue_abs(wcon->dcl.con, INPUT_AXIS_X,
                             x * surface_width(wcon->surface) / r.right,
                             0, surface_width(wcon->surface));
        qemu_input_queue_abs(wcon->dcl.con, INPUT_AXIS_Y,
                             y * surface_height(wcon->surface) / r.bottom,
                             0, surface_height(wcon->surface));
    } else {
        if (!relative || !gui_grab) {
            return;
        }
        qemu_input_queue_rel(wcon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(wcon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void win32_send_mouse_buttons(struct win32_console *wcon, WPARAM wparam)
{
    static const uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]   = MK_LBUTTON,
        [INPUT_BUTTON_MIDDLE] = MK_MBUTTON,
        [INPUT_BUTTON_RIGHT]  = MK_RBUTTON,
        [INPUT_BUTTON_SIDE]   = MK_XBUTTON1,
        [INPUT_BUTTON_EXTRA]  = MK_XBUTTON2,
    };
    static uint32_t prev_state;
    uint32_t state = wparam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON |
                               MK_XBUTTON1 | MK_XBUTTON2);

    BQL_LOCK_GUARD();

    if (!qemu_console_is_graphic(wcon->dcl.con)) {
        return;
    }
    if (state != prev_state) {
        qemu_input_update_buttons(wcon->dcl.con, (uint32_t *)bmap,
                                  prev_state, state);
        prev_state = state;
        qemu_input_event_sync();
    }
}

static void win32_send_wheel(struct win32_console *wcon, InputButton btn)
{
    BQL_LOCK_GUARD();

    if (!qemu_console_is_graphic(wcon->dcl.con)) {
        return;
    }
    qemu_input_queue_btn(wcon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(wcon->dcl.con, btn, false);
    qemu_input_event_sync();
}

/*
 * "Grab on hover" needs to know when the pointer comes to rest inside the
 * guest image, which Win32 only reports if it is asked to: one request per
 * entry into the window, re-armed once the pointer leaves again.
 */
static void win32_track_hover(struct win32_console *wcon)
{
    TRACKMOUSEEVENT tme = {
        .cbSize      = sizeof(tme),
        .dwFlags     = TME_HOVER | TME_LEAVE,
        .hwndTrack   = wcon->hwnd,
        .dwHoverTime = HOVER_DEFAULT,
    };

    if (wcon->hover_tracked || !gui_grab_on_hover || gui_grab) {
        return;
    }
    if (TrackMouseEvent(&tme)) {
        wcon->hover_tracked = true;
    }
}

/* ------------------------------------------------------------------ */
/* keyboard                                                             */

/*
 * Windows hands us an AT set 1 scancode in bits 16..23 of lParam, plus an
 * extended flag in bit 24 -- exactly the encoding QEMU's atset1 keymap is
 * indexed by.  This mirrors gd_get_keycode() in ui/gtk.c.
 */
static int win32_get_atset1(WPARAM wparam, LPARAM lparam)
{
    int scancode = (lparam >> 16) & 0xff;
    bool extended = (lparam >> 24) & 1;

    if (!scancode) {
        scancode = MapVirtualKey(wparam, MAPVK_VK_TO_VSC);
    }
    /* NumLock arrives flagged as extended, but atset1 wants the plain code */
    if (extended && scancode == 0x45) {
        return 0x45;
    }
    return extended ? 0xe000 | scancode : scancode;
}

static unsigned int win32_map_keycode(int atset1)
{
    if (atset1 < 0 || atset1 >= qemu_input_map_atset1_to_linux_len) {
        return 0;
    }
    return qemu_input_map_atset1_to_linux[atset1];
}

static bool win32_grab_modifiers_down(void)
{
    bool lctrl  = GetKeyState(VK_LCONTROL) & 0x8000;
    bool lalt   = GetKeyState(VK_LMENU) & 0x8000;
    bool lshift = GetKeyState(VK_LSHIFT) & 0x8000;
    bool rctrl  = GetKeyState(VK_RCONTROL) & 0x8000;

    if (alt_grab) {
        return lctrl && lalt && lshift;
    } else if (ctrl_grab) {
        return rctrl;
    }
    return lctrl && lalt;
}

void win32_release_modifiers(struct win32_console *wcon)
{
    /*
     * A terminal tab has no QKbdState: it is a chardev, not a QemuConsole,
     * and nothing is tracking which of its keys the guest believes are
     * held.  The tab-switch path calls this for whichever console is being
     * left, so it must cope with that.
     */
    if (!wcon || !wcon->kbd) {
        return;
    }
    {
        BQL_LOCK_GUARD();
        /*
         * Re-checked with the lock held: win32_display_cleanup() clears
         * this pointer on the QEMU thread under the same lock.
         */
        if (wcon->kbd) {
            qkbd_state_lift_all_keys(wcon->kbd);
        }
    }
}

/* Returns true when the key was consumed as a UI hotkey. */
static bool win32_handle_hotkey(struct win32_console *wcon, WPARAM wparam)
{
    int win;

    if (!win32_grab_modifiers_down()) {
        return false;
    }

    switch (wparam) {
    case 'F':
        win32_toggle_fullscreen();
        win32_release_modifiers(wcon);
        return true;
    case 'G':
        if (gui_grab) {
            win32_grab_end();
        } else {
            win32_grab_start();
        }
        win32_release_modifiers(wcon);
        return true;
    case 'U':
        /* restore the guest's own resolution, i.e. the View/Best Fit item */
        win32_zoom_fixed();
        win32_release_modifiers(wcon);
        return true;
    case '1' ... '9':
        /*
         * The keyboard remains the primary way to reach a console: this now
         * selects the console's tab instead of toggling a window, but it is
         * still what the documented Ctrl-Alt-<n> does, and nothing here
         * depends on the menu being usable.
         */
        win = wparam - '1';
        if (win >= win32_num_outputs || !win32_consoles[win].hwnd) {
            return false;
        }
        win32_frame_note_user_selection();
        win32_frame_activate(&win32_consoles[win]);
        win32_release_modifiers(wcon);
        return true;
    default:
        return false;
    }
}

static void win32_handle_key(struct win32_console *wcon,
                             WPARAM wparam, LPARAM lparam, bool down)
{
    QemuConsole *con = wcon->dcl.con;
    unsigned int lnx;
    int atset1;

    /*
     * Terminal tabs have their own window and their own key translation;
     * they have neither a QemuConsole nor a QKbdState, so nothing below
     * applies to them.
     */
    if (!con || !wcon->kbd) {
        return;
    }

    /*
     * Everything below this point reaches into emulator state, and on the
     * UI thread that means the BQL.  The hotkey handling above it does not,
     * which is deliberate: Ctrl-Alt-F and friends have to keep working even
     * if the QEMU thread is wedged, since they are how the user gets the
     * pointer back.
     */
    if (down) {
        /*
         * TranslateMessage() has already queued the WM_CHAR belonging to
         * this WM_KEYDOWN, so a hotkey that is consumed here has to mark
         * that character for the character handler to drop -- otherwise a
         * layout where the combination also produces a character (AltGr is
         * Ctrl-Alt as far as the keyboard is concerned) would both switch
         * tab and type into the console.
         */
        swallow_next_char = win32_handle_hotkey(wcon, wparam);
        if (swallow_next_char) {
            return;
        }
    }

    BQL_LOCK_GUARD();
    if (!wcon->kbd) {
        return;                          /* cleanup got here first */
    }

    /*
     * VK_PAUSE does not carry a usable scancode; feed the key straight
     * through, as ui/gtk.c does for the same reason.
     */
    if (wparam == VK_PAUSE) {
        qkbd_state_key_event(wcon->kbd, KEY_PAUSE, down);
        return;
    }

    atset1 = win32_get_atset1(wparam, lparam);
    lnx = win32_map_keycode(atset1);
    if (!lnx) {
        return;
    }

    qkbd_state_key_event(wcon->kbd, lnx, down);

    if (QEMU_IS_TEXT_CONSOLE(con) && down) {
        QemuTextConsole *s = QEMU_TEXT_CONSOLE(con);
        bool ctrl = qkbd_state_modifier_get(wcon->kbd, QKBD_MOD_CTRL);

        if (lnx == KEY_ENTER) {
            qemu_text_console_put_keysym(s, '\n');
        } else {
            qemu_text_console_put_linux(s, lnx, ctrl);
        }
    }
}

/*
 * Text consoles need characters, not keycodes.
 *
 * qemu_text_console_put_linux() can only deliver what ui/console.c's
 * linux_to_keysym[] describes, and that table holds the eleven navigation
 * keys and nothing else -- it has no entry for a single printable
 * character, so letters, digits and punctuation routed through it are
 * dropped without a trace.  ui/sdl2.c and ui/gtk.c sidestep the table by
 * feeding the *translated* character to qemu_text_console_put_string()
 * (SDL_TEXTINPUT and GdkEventKey::string respectively); WM_CHAR, produced
 * by the TranslateMessage() in win32_poll_events(), is the same thing on
 * Windows, and this is the only way a monitor command can be typed.
 *
 * The keycode path above keeps ownership of every key it can already
 * handle -- Enter, Tab and Backspace, which do generate a WM_CHAR, as well
 * as the arrows, Home/End, PageUp/PageDown and Delete, which do not -- so
 * those control codes are discarded here rather than delivered twice.
 * Everything else, control characters from Ctrl combinations included
 * (Ctrl-C arrives as 0x03, and linux_to_keysym[] has no Ctrl-letter entry
 * to collide with), is passed on.
 */
static void win32_handle_char(struct win32_console *wcon, WPARAM wparam)
{
    /*
     * The window class is registered with the ANSI RegisterClassEx(), and
     * neither UNICODE nor _UNICODE is defined for this build, so the window
     * is an ANSI one and wparam is a byte in the host's ANSI code page --
     * not a UTF-16 unit, and hence never a surrogate.  It can still be the
     * lead byte of a double-byte sequence on a CJK code page, which arrives
     * as its own WM_CHAR and is held back until the trail byte follows.
     */
    static BYTE dbcs_lead;
    QemuConsole *con = wcon->dcl.con;
    char mb[2], utf8[8];
    WCHAR wide[2];
    int mblen, wlen, u8len;

    if (swallow_next_char) {
        swallow_next_char = false;
        return;
    }

    /* a graphics console gets its input from the scancode path only */
    if (!QEMU_IS_TEXT_CONSOLE(con)) {
        dbcs_lead = 0;
        return;
    }

    if (dbcs_lead) {
        mb[0] = (char)dbcs_lead;
        mb[1] = (char)wparam;
        mblen = 2;
        dbcs_lead = 0;
    } else if (wparam == '\b' || wparam == '\t' ||
               wparam == '\n' || wparam == '\r') {
        return;                          /* owned by win32_handle_key() */
    } else if (IsDBCSLeadByteEx(CP_ACP, (BYTE)wparam)) {
        dbcs_lead = (BYTE)wparam;
        return;
    } else {
        mb[0] = (char)wparam;
        mblen = 1;
    }

    /*
     * qemu_text_console_put_string() hands each byte to the terminal
     * emulator, which -- as with the UTF-8 strings ui/gtk.c passes it --
     * expects UTF-8 for anything outside ASCII.
     */
    wlen = MultiByteToWideChar(CP_ACP, 0, mb, mblen, wide, ARRAY_SIZE(wide));
    if (wlen <= 0) {
        return;
    }
    u8len = WideCharToMultiByte(CP_UTF8, 0, wide, wlen, utf8, sizeof(utf8),
                                NULL, NULL);
    if (u8len <= 0) {
        return;
    }

    {
        BQL_LOCK_GUARD();
        qemu_text_console_put_string(QEMU_TEXT_CONSOLE(con), utf8, u8len);
    }
}

/* ------------------------------------------------------------------ */
/* painting                                                             */

/*
 * Describe the guest surface as a top-down 32bpp DIB.  A negative height
 * means top-down; expressing the pixman stride as a wider bitmap lets
 * StretchDIBits() read the surface in place, with no intermediate copy.
 */
static void win32_fill_bitmapinfo(DisplaySurface *surf, BITMAPINFO *bmi)
{
    memset(bmi, 0, sizeof(*bmi));
    bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biWidth = surface_stride(surf) / 4;
    bmi->bmiHeader.biHeight = -surface_height(surf);
    bmi->bmiHeader.biPlanes = 1;
    bmi->bmiHeader.biBitCount = 32;
    bmi->bmiHeader.biCompression = BI_RGB;
}

/*
 * Blit the guest surface into the window.
 *
 * This runs on the UI thread, from WM_PAINT.  wcon->surface is the UI
 * thread's own DisplaySurface -- a private reference on the guest's pixman
 * image, taken in win32_2d_switch() -- and not the one ui/console.c is
 * about to free, so reading it here does not race the switch.  It does race
 * the *guest*, which keeps writing into that buffer; that shows up as the
 * same tearing every other backend can show and is why there is no copy.
 *
 * A slow blit is no longer a slow guest, but it is still a slow UI, and
 * every WM_PAINT covers whatever BeginPaint() says is dirty:
 *
 * HALFTONE is GDI's software resampler and by a wide margin its most
 * expensive StretchBlt mode.  It earns its cost when the image is being
 * *shrunk*, where simply dropping pixels visibly destroys text.  It does
 * not earn it anywhere else:
 *
 *   - At 1:1 there is nothing to resample.  COLORONCOLOR lets GDI take a
 *     straight copy, clipped by the DC's clip region (BeginPaint() has
 *     already set that to the update region), instead of running the
 *     resampler over the whole screen to produce the identical pixels.
 *     This is the ordinary case at 100% display scaling.
 *
 *   - When the image is being *enlarged* the resampler has no information
 *     to add; every destination pixel comes from one source pixel except
 *     at cell boundaries.  Enlargement is not a corner case: the frame is
 *     DPI-scaled (see win32_console_pixel_size()), so every machine whose
 *     display is set above 100% took the HALFTONE path for every frame.
 */
static void win32_paint(struct win32_console *wcon, HDC hdc)
{
    DisplaySurface *surf = wcon->surface;
    BITMAPINFO bmi;
    RECT client;
    int sw, sh;

    if (!surf || !GetClientRect(wcon->hwnd, &client)) {
        return;
    }
    if (client.right <= 0 || client.bottom <= 0) {
        return;
    }

    sw = surface_width(surf);
    sh = surface_height(surf);
    if (sw <= 0 || sh <= 0) {
        return;
    }

    win32_fill_bitmapinfo(surf, &bmi);
    SetStretchBltMode(hdc, (client.right < sw || client.bottom < sh)
                           ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchDIBits(hdc,
                  0, 0, client.right, client.bottom,
                  0, 0, sw, sh,
                  surface_data(surf), &bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* ------------------------------------------------------------------ */
/* render window procedure                                              */

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT msg,
                                      WPARAM wparam, LPARAM lparam)
{
    struct win32_console *wcon = win32_console_from_hwnd(hwnd);
    PAINTSTRUCT ps;
    HDC hdc;

    if (!wcon) {
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_PAINT:
        hdc = BeginPaint(hwnd, &ps);
        win32_paint(wcon, hdc);
        EndPaint(hwnd, &ps);
        return 0;

    case WM_ERASEBKGND:
        /* every pixel is repainted by WM_PAINT; erasing only causes flicker */
        return 1;

    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        if (gui_grab) {
            win32_clip_cursor(true);
        }
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        win32_handle_key(wcon, wparam, lparam, true);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        win32_handle_key(wcon, wparam, lparam, false);
        return 0;

    case WM_CHAR:
        win32_handle_char(wcon, wparam);
        return 0;

    case WM_MOUSEMOVE:
        win32_track_hover(wcon);
        win32_send_mouse_motion(wcon, GET_X_LPARAM(lparam),
                                GET_Y_LPARAM(lparam), 0, 0, false);
        win32_send_mouse_buttons(wcon, wparam);
        return 0;

    case WM_MOUSEHOVER:
        wcon->hover_tracked = false;
        if (gui_grab_on_hover && !gui_grab) {
            win32_grab_start();
        }
        return 0;

    case WM_MOUSELEAVE:
        wcon->hover_tracked = false;
        return 0;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_XBUTTONDOWN:
        /*
         * Clicking into an ungrabbed window with a relative-mode guest is
         * how the user asks for the pointer, matching the SDL behaviour.
         */
        SetFocus(hwnd);
        {
            bool absolute;

            {
                BQL_LOCK_GUARD();
                absolute = qemu_input_is_absolute(wcon->dcl.con);
            }
            if (!gui_grab && !absolute) {
                win32_grab_start();
            } else {
                SetCapture(hwnd);
                win32_send_mouse_buttons(wcon, wparam);
            }
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_XBUTTONUP:
        ReleaseCapture();
        win32_send_mouse_buttons(wcon, wparam);
        return 0;

    case WM_MOUSEWHEEL:
        win32_send_wheel(wcon, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ?
                         INPUT_BUTTON_WHEEL_UP : INPUT_BUTTON_WHEEL_DOWN);
        return 0;

    case WM_MOUSEHWHEEL:
        win32_send_wheel(wcon, GET_WHEEL_DELTA_WPARAM(wparam) > 0 ?
                         INPUT_BUTTON_WHEEL_RIGHT : INPUT_BUTTON_WHEEL_LEFT);
        return 0;

    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        bool absolute;

        if (!gui_grab) {
            break;
        }
        {
            BQL_LOCK_GUARD();
            absolute = qemu_input_is_absolute(wcon->dcl.con);
        }
        if (absolute) {
            break;
        }
        if (GetRawInputData((HRAWINPUT)lparam, RID_INPUT, &ri, &size,
                            sizeof(RAWINPUTHEADER)) == (UINT)-1) {
            break;
        }
        if (ri.header.dwType == RIM_TYPEMOUSE &&
            !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) &&
            (ri.data.mouse.lLastX || ri.data.mouse.lLastY)) {
            win32_send_mouse_motion(wcon, 0, 0, ri.data.mouse.lLastX,
                                    ri.data.mouse.lLastY, true);
        }
        break;
    }

    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            if (guest_cursor && (gui_grab || absolute_enabled)) {
                SetCursor(guest_sprite);
            } else if (cursor_visible) {
                SetCursor(cursor_arrow);
            } else {
                SetCursor(NULL);
            }
            return TRUE;
        }
        break;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

/* ------------------------------------------------------------------ */
/* DisplayChangeListener ops                                            */

/*
 * Turn an accumulated guest-side dirty rectangle into an InvalidateRect()
 * on the render window.  UI thread.
 */
static void win32_2d_invalidate(struct win32_console *wcon, const RECT *guest)
{
    RECT client, dirty;
    int sw, sh;
    int x = guest->left, y = guest->top;
    int w = guest->right - guest->left, h = guest->bottom - guest->top;

    if (!wcon->hwnd || !wcon->surface || wcon != win32_active) {
        return;
    }
    if (!GetClientRect(wcon->hwnd, &client) ||
        client.right <= 0 || client.bottom <= 0) {
        return;
    }

    /*
     * A zero-dimension surface would divide by zero below, and on Windows
     * that is a hardware exception rather than a signal.  It should not be
     * reachable; treat it as "nothing to invalidate" rather than trusting
     * that.
     */
    sw = surface_width(wcon->surface);
    sh = surface_height(wcon->surface);
    if (sw <= 0 || sh <= 0) {
        return;
    }

    /* map the guest-side dirty rectangle onto the (possibly scaled) window */
    dirty.left   = x * client.right / sw;
    dirty.top    = y * client.bottom / sh;
    dirty.right  = ((x + w) * client.right + sw - 1) / sw;
    dirty.bottom = ((y + h) * client.bottom + sh - 1) / sh;

    InvalidateRect(wcon->hwnd, &dirty, FALSE);
}

/*
 * dpy_gfx_update: QEMU thread.
 *
 * All this does is remember what changed and make sure the UI thread has
 * been told to look.  The rectangles are unioned rather than queued: a
 * guest that dirties a thousand small rectangles between two of the UI
 * thread's message-loop iterations should cost one repaint, not a thousand
 * posted messages.  Keeping the union in guest coordinates rather than
 * window ones is what lets the mapping stay on the UI thread, where the
 * window size and the UI-owned surface both live.
 */
static void win32_2d_update(DisplayChangeListener *dcl,
                            int x, int y, int w, int h)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    bool post;

    if (w <= 0 || h <= 0) {
        return;
    }

    qemu_mutex_lock(&win32_ui_mutex);
    if (!wcon->damage_valid) {
        wcon->damage_valid = true;
        wcon->damage.left = x;
        wcon->damage.top = y;
        wcon->damage.right = x + w;
        wcon->damage.bottom = y + h;
    } else {
        wcon->damage.left = MIN(wcon->damage.left, x);
        wcon->damage.top = MIN(wcon->damage.top, y);
        wcon->damage.right = MAX(wcon->damage.right, x + w);
        wcon->damage.bottom = MAX(wcon->damage.bottom, y + h);
    }
    post = !wcon->damage_posted;
    wcon->damage_posted = true;
    qemu_mutex_unlock(&win32_ui_mutex);

    if (post) {
        win32_ui_post(WIN32_UI_DAMAGE, wcon->idx, 0);
    }
}

/* UI thread: drain whatever win32_2d_update() has accumulated. */
static void win32_ui_damage(struct win32_console *wcon)
{
    RECT dirty;
    bool valid;

    qemu_mutex_lock(&win32_ui_mutex);
    dirty = wcon->damage;
    valid = wcon->damage_valid;
    wcon->damage_valid = false;
    wcon->damage_posted = false;
    qemu_mutex_unlock(&win32_ui_mutex);

    if (valid) {
        win32_2d_invalidate(wcon, &dirty);
    }
}

/*
 * Common to both paths: decide whether this console still deserves a window,
 * create or drop it, and re-fit the frame if it is the visible one.
 *
 * UI thread in the 2D case, QEMU thread under gl=on -- which is the same
 * thread in both cases, because gl=on does not take the main thread over.
 */
static void win32_switch_common(struct win32_console *wcon,
                                DisplaySurface *old_surface)
{
    DisplaySurface *new_surface = wcon->surface;

    if (!new_surface ||
        (surface_is_placeholder(new_surface) && wcon->con_index)) {
        win32_window_destroy(wcon);
        return;
    }

    if (!wcon->hwnd) {
        /* under gl=on this also brings up the EGL surface and shader */
        win32_window_create(wcon);
    } else if (old_surface &&
               (surface_width(old_surface) != surface_width(new_surface) ||
                surface_height(old_surface) != surface_height(new_surface)) &&
               wcon == win32_active) {
        win32_frame_fit();
    }

    if (wcon->hwnd && wcon == win32_active) {
        InvalidateRect(wcon->hwnd, NULL, FALSE);
    }
}

/*
 * dpy_gfx_switch: QEMU thread.
 *
 * The surface handed to us here is freed the moment this returns, so the UI
 * thread cannot be given it.  Take a reference on the pixman image behind
 * it instead -- which is exactly what ui/cocoa.m does at the same point --
 * and wrap that in a DisplaySurface the UI thread owns outright.  The
 * placeholder bit is carried over because win32_switch_common() decides
 * whether the console deserves a window at all from it; nothing else in the
 * original struct is needed, and QEMU_ALLOCATED_FLAG deliberately is not
 * copied, since the allocation is not ours to free.
 *
 * A switch that arrives while a previous one is still queued replaces it:
 * only the newest surface is ever of any interest, and the superseded
 * reference is dropped here rather than leaked.
 */
static void win32_2d_switch(DisplayChangeListener *dcl,
                            DisplaySurface *new_surface)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    DisplaySurface *proxy = NULL, *superseded;
    bool post;

    if (new_surface) {
        proxy = qemu_create_displaysurface_pixman(new_surface->image);
        proxy->flags = new_surface->flags & QEMU_PLACEHOLDER_FLAG;
    }

    qemu_mutex_lock(&win32_ui_mutex);
    superseded = wcon->pending_surface;
    wcon->pending_surface = proxy;
    wcon->pending_switch = true;
    post = !wcon->switch_posted;
    wcon->switch_posted = true;
    qemu_mutex_unlock(&win32_ui_mutex);

    qemu_free_displaysurface(superseded);

    if (post) {
        win32_ui_post(WIN32_UI_SWITCH, wcon->idx, 0);
    }
}

/* UI thread: adopt the surface win32_2d_switch() left for us. */
static void win32_ui_switch(struct win32_console *wcon)
{
    DisplaySurface *old, *new_surface;
    bool pending;

    qemu_mutex_lock(&win32_ui_mutex);
    new_surface = wcon->pending_surface;
    pending = wcon->pending_switch;
    wcon->pending_surface = NULL;
    wcon->pending_switch = false;
    wcon->switch_posted = false;
    qemu_mutex_unlock(&win32_ui_mutex);

    if (!pending) {
        return;
    }

    old = wcon->surface;
    wcon->surface = new_surface;
    win32_switch_common(wcon, old);
    qemu_free_displaysurface(old);
}

static bool win32_2d_check_format(DisplayChangeListener *dcl,
                                  pixman_format_code_t format)
{
    /*
     * Only the two formats that map directly onto a BI_RGB 32bpp DIB are
     * accepted; console.c converts anything else for us.  Accepting more
     * would mean carrying a format switch here for no gain.
     */
    return format == PIXMAN_x8r8g8b8 || format == PIXMAN_a8r8g8b8;
}

/*
 * What the pump is doing right now.  Set around DispatchMessage() and used
 * only by win32_fault_handler() below, to say which window message QEMU was
 * delivering when a window procedure blew up.  It is deliberately not used
 * as a re-entrancy guard: a flag that a non-local exit could leave set would
 * be one more way to kill the pump for good, which is the failure this file
 * is trying to make impossible.
 *
 * Thread-local, because with the UI on its own thread there are now two
 * places a fault can come from and the handler runs on whichever thread
 * faulted: a message being dispatched on the UI thread, and a refresh
 * running on the QEMU thread.  A process-wide flag would let one thread's
 * state describe the other thread's fault.
 */
static __thread bool win32_in_dispatch;
static __thread UINT win32_dispatch_msg;
static __thread HWND win32_dispatch_hwnd;

/* Translate and dispatch one message.  Common to both pumps. */
static void win32_dispatch_one(MSG *msg)
{
    /*
     * A dialog owned by the frame needs IsDialogMessage() to see its
     * input before it is translated and dispatched, or it has no Tab
     * navigation and neither Esc nor Enter reaches it.
     */
    if (win32_dialog_filter(msg)) {
        return;
    }
    if ((msg->message >= WM_KEYFIRST && msg->message <= WM_KEYLAST) ||
        (msg->message >= WM_MOUSEFIRST && msg->message <= WM_MOUSELAST)) {
        win32_ui_note_activity();
    }
    TranslateMessage(msg);

    win32_dispatch_msg = msg->message;
    win32_dispatch_hwnd = msg->hwnd;
    win32_in_dispatch = true;
    DispatchMessage(msg);
    win32_in_dispatch = false;
}

/*
 * How long after the last piece of UI activity the refresh rate drops back
 * to the idle one.  The old code counted idle polls because the pump and
 * the refresh were the same call; they are not any more, so the QEMU thread
 * asks the UI thread's clock instead.  The value is the same one the count
 * worked out to.
 */
#define WIN32_UI_BUSY_WINDOW_MS (2 * GUI_REFRESH_INTERVAL_DEFAULT)

/* QEMU thread: pick dcl.update_interval from how busy the UI thread is. */
static void win32_pace_refresh(struct win32_console *wcon)
{
    DWORD last = (DWORD)g_atomic_int_get(&win32_ui_active_tick);
    DWORD now = GetTickCount();

    if (last && now - last < WIN32_UI_BUSY_WINDOW_MS) {
        wcon->dcl.update_interval = WIN32_REFRESH_INTERVAL_BUSY;
    } else {
        wcon->dcl.update_interval = GUI_REFRESH_INTERVAL_DEFAULT;
    }
}

/*
 * The two liveness timers.
 *
 * win32_pump_timer runs on the *QEMU* thread.  It no longer pumps anything
 * -- the UI thread's own GetMessage() loop does that now -- but it is still
 * the only thing that can notice two failures that have no other symptom:
 *
 *   - ui/console.c's GUI timer dying.  gui_update() is
 *
 *         ds->refreshing = true;
 *         dpy_refresh(ds);                                  <- us
 *         ds->refreshing = false;
 *         ...
 *         timer_mod(ds->gui_timer, ds->last_update + interval);
 *
 *     i.e. a one-shot re-armed *after* the callback returns, so anything
 *     that leaves win32_2d_refresh() other than by returning takes the
 *     whole session's display with it while the guest carries on.  The
 *     window would still be alive and still repaint on WM_PAINT; it would
 *     simply never be told the guest had changed anything.
 *     win32_refresh_watchdog() says so.
 *
 *   - QEMU's own main loop stalling.  This used to mean "a window is being
 *     dragged", because the pump ran on this thread; it cannot mean that
 *     any more, which is exactly the point of the change.  If this timer is
 *     late now, the emulator itself is stuck and the report should say so.
 *
 * The UI thread has an opposite number, an ordinary WM_TIMER on the
 * message-only window.  It reports the same kind of thing for the thread it
 * runs on: the UI thread's message loop not getting round to it.
 *
 * It fires less often than one might expect, and that is worth knowing
 * rather than discovering.  Windows' modal loops are message loops -- they
 * call GetMessage()/PeekMessage() and dispatch what is not theirs -- so a
 * held menu or a grabbed scrollbar thumb does *not* stop this timer, as a
 * 45-second menu hold under wine confirmed.  What does stop it is the UI
 * thread being blocked rather than merely busy elsewhere: waiting for the
 * BQL behind a long-running QEMU operation, or a genuinely non-pumping
 * loop.  So the message says what was actually measured and what it does
 * and does not imply, rather than naming a cause it cannot know.
 */
static QEMUTimer *win32_pump_timer;

#define WIN32_UI_TIMER_ID     1
#define WIN32_UI_TIMER_MS     GUI_REFRESH_INTERVAL_DEFAULT

/*
 * Watchdog state.  A graphics console sets dcl.update_interval to at most
 * GUI_REFRESH_INTERVAL_DEFAULT, so dpy_refresh is due every 30ms at worst;
 * if several seconds go by without one while the pump timer is still
 * ticking, the GUI timer described above has died.
 */
#define WIN32_REFRESH_WATCHDOG_MS 5000

/* how late either timer has to be before it is worth saying so */
#define WIN32_MAINLOOP_STALL_MS 1000
#define WIN32_UI_STALL_MS 1000

static int64_t win32_last_refresh_ms;
static int64_t win32_last_tick_ms;
static bool win32_refresh_stalled;
static bool win32_have_gfx_console;

static void win32_refresh_watchdog(int64_t now)
{
    if (!win32_have_gfx_console) {
        return;
    }

    if (now - win32_last_refresh_ms >= WIN32_REFRESH_WATCHDOG_MS) {
        if (!win32_refresh_stalled) {
            win32_refresh_stalled = true;
            warn_report("win32: no display refresh for %" PRId64 "ms; "
                        "ui/console.c's GUI timer has stopped. The window "
                        "itself is still alive -- it is only no longer being "
                        "told that the guest has changed anything. Please "
                        "report this together with any 'win32: fault' line "
                        "above.",
                        now - win32_last_refresh_ms);
        }
    } else if (win32_refresh_stalled) {
        win32_refresh_stalled = false;
        warn_report("win32: display refresh resumed");
    }
}

/* UI thread; see the comment above for when this can and cannot fire. */
static void win32_ui_tick(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    DWORD late;

    win32_ui_note_activity();

    if (last) {
        late = now - last;
        if (late >= WIN32_UI_STALL_MS) {
            warn_report("win32: the UI thread did not process messages for "
                        "%lums, so the window will have stopped updating and "
                        "ignored input for that long. The emulator itself "
                        "was not affected: it runs on its own thread.",
                        (unsigned long)late);
        }
    }
    last = now;
}

static void win32_pump_tick(void *opaque)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    timer_mod(win32_pump_timer, now + GUI_REFRESH_INTERVAL_DEFAULT);

    if (win32_last_tick_ms &&
        now - win32_last_tick_ms >= WIN32_MAINLOOP_STALL_MS) {
        warn_report("win32: QEMU's main loop did not run for %" PRId64 "ms. "
                    "The display runs on its own thread, so this is a stall "
                    "inside the emulator itself and not something the window "
                    "did.",
                    now - win32_last_tick_ms);
    }
    win32_last_tick_ms = now;

    win32_refresh_watchdog(now);
}

/*
 * Fault reporting.
 *
 * A window procedure that raises a hardware exception is not a noisy
 * failure on Windows: on x86-64 the kernel-mode callback dispatcher
 * catches exceptions raised in a callback, so a dereference of a NULL
 * surface inside WM_PAINT does not produce a crash dump, an error message
 * or anything else -- the message is simply dropped and whatever that stack
 * was carrying is discarded.  Under wine the same thing shows up as
 * "err:seh:dispatch_callback ignoring exception".  QEMU carries on, one
 * frame or one keystroke poorer, and nobody ever finds out; repeated often
 * enough that is a display that "sometimes does nothing", reported as a
 * freeze, with not one line of evidence anywhere.
 *
 * A vectored handler runs before any of that, so this sees the fault even
 * though nothing else will.  It only reports, never handles: it returns
 * EXCEPTION_CONTINUE_SEARCH so the normal machinery still runs and a fault
 * that would have been fatal still is.
 *
 * The filtering matters, because a first-chance vectored handler sees every
 * exception in the process, including the ones that are a normal part of
 * how other code works (C++ throws, the debugger's thread-naming exception,
 * guard-page hits used to grow stacks).  Only the hardware faults that mean
 * "this code is broken" are reported, only while the pump is dispatching a
 * message or inside a refresh, and only a few times, so that a fault in a
 * message that repeats cannot itself become the problem.
 */
#define WIN32_MAX_FAULT_REPORTS 8

static bool win32_in_refresh;
static int win32_fault_reports;
static PVOID win32_fault_handle;

static LONG CALLBACK win32_fault_handler(PEXCEPTION_POINTERS ep)
{
    const EXCEPTION_RECORD *er;

    if (!ep || !ep->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (!win32_in_dispatch && !win32_in_refresh) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (win32_fault_reports >= WIN32_MAX_FAULT_REPORTS) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    er = ep->ExceptionRecord;
    switch (er->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    win32_fault_reports++;
    if (win32_in_dispatch) {
        error_report("win32: fault 0x%08lx at %p while dispatching message "
                     "0x%04x to window %p. Windows discards this message "
                     "silently; the display may miss updates or input. "
                     "Please report it",
                     (unsigned long)er->ExceptionCode, er->ExceptionAddress,
                     (unsigned)win32_dispatch_msg,
                     (void *)win32_dispatch_hwnd);
    } else {
        error_report("win32: fault 0x%08lx at %p during display refresh -- "
                     "please report it",
                     (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    }
    if (win32_fault_reports == WIN32_MAX_FAULT_REPORTS) {
        error_report("win32: further faults will not be reported");
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * dpy_refresh: QEMU thread, BQL held.
 *
 * What is left here is only the half that belongs on this thread:
 * qemu_console_hw_update() pulls the guest's pending damage out of the
 * device model, which is emulator state and must not move.  Everything it
 * causes -- the InvalidateRect(), the repaint -- happens on the UI thread,
 * driven by the dpy_gfx_update calls hw_update makes from inside here.
 */
static void win32_2d_refresh(DisplayChangeListener *dcl)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    static bool was_running;
    bool running = runstate_is_running();

    win32_last_refresh_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    win32_in_refresh = true;

    qemu_console_hw_update(dcl->con);

    if (running != was_running) {
        was_running = running;
        win32_ui_post(WIN32_UI_CAPTION, 0, 0);
    }

    win32_pace_refresh(wcon);
    win32_in_refresh = false;
}

/*
 * dpy_mouse_set: QEMU thread.  SetCursor() and ShowCursor() are both
 * per-thread state in Win32, so they have to happen where the window is.
 * The position fits in an LPARAM the way every Win32 mouse message's does,
 * and nothing reads it back out of the guest's range.
 */
static void win32_mouse_warp(DisplayChangeListener *dcl,
                             int x, int y, bool on)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);

    if (!qemu_console_is_graphic(dcl->con) || !wcon->hwnd) {
        return;
    }
    win32_ui_post(WIN32_UI_MOUSE_SET, on,
                  MAKELPARAM((WORD)x, (WORD)y));
}

/* UI thread */
static void win32_ui_mouse_set(bool on, int x, int y)
{
    struct win32_console *wcon = win32_active;
    bool absolute = false;

    if (!wcon || win32_console_is_term(wcon) || !wcon->hwnd) {
        return;
    }
    {
        BQL_LOCK_GUARD();
        absolute = qemu_input_is_absolute(wcon->dcl.con);
    }

    if (on) {
        if (!guest_cursor) {
            win32_show_cursor(true);
        }
        if (gui_grab || absolute || absolute_enabled) {
            SetCursor(guest_sprite);
        }
    } else if (gui_grab) {
        win32_show_cursor(false);
    }
    guest_cursor = on;
    guest_x = x;
    guest_y = y;
}

/*
 * dpy_cursor_define: QEMU thread.  The QEMUCursor belongs to the device
 * model and is refcounted, not owned by us, so take a reference for the
 * message and let the UI thread drop it once the HCURSOR has been built.
 */
static void win32_mouse_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    win32_ui_post(WIN32_UI_CURSOR_DEFINE, 0, (LPARAM)cursor_ref(c));
}

/* UI thread; consumes the reference win32_mouse_define() took. */
static void win32_ui_cursor_define(QEMUCursor *c)
{
    BITMAPV5HEADER bi = { 0 };
    HBITMAP color, mask;
    HDC hdc;
    void *bits = NULL;
    ICONINFO ii;
    HCURSOR cur;
    bool absolute;

    bi.bV5Size = sizeof(bi);
    bi.bV5Width = c->width;
    bi.bV5Height = -c->height;   /* top-down */
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00ff0000;
    bi.bV5GreenMask = 0x0000ff00;
    bi.bV5BlueMask  = 0x000000ff;
    bi.bV5AlphaMask = 0xff000000;

    hdc = GetDC(NULL);
    color = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
                             &bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!color || !bits) {
        goto out;
    }
    memcpy(bits, c->data, (size_t)c->width * c->height * 4);

    /* the alpha channel does the masking; an all-zero mask keeps it intact */
    mask = CreateBitmap(c->width, c->height, 1, 1, NULL);
    if (!mask) {
        DeleteObject(color);
        goto out;
    }

    ii.fIcon = FALSE;
    ii.xHotspot = c->hot_x;
    ii.yHotspot = c->hot_y;
    ii.hbmMask = mask;
    ii.hbmColor = color;

    cur = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    if (!cur) {
        goto out;
    }

    if (guest_sprite) {
        DestroyIcon(guest_sprite);
    }
    guest_sprite = cur;

    absolute = false;
    if (win32_active && !win32_console_is_term(win32_active)) {
        BQL_LOCK_GUARD();
        absolute = qemu_input_is_absolute(win32_active->dcl.con);
    }
    if (guest_cursor && (gui_grab || absolute || absolute_enabled)) {
        SetCursor(guest_sprite);
    }

out:
    /*
     * cursor_unref() manipulates a plain refcount owned by the device
     * model, so it is emulator state like any other and is taken with the
     * BQL held.
     */
    {
        BQL_LOCK_GUARD();
        cursor_unref(c);
    }
}

static const DisplayChangeListenerOps dcl_2d_ops = {
    .dpy_name             = "win32",
    .dpy_gfx_update       = win32_2d_update,
    .dpy_gfx_switch       = win32_2d_switch,
    .dpy_gfx_check_format = win32_2d_check_format,
    .dpy_refresh          = win32_2d_refresh,
    .dpy_mouse_set        = win32_mouse_warp,
    .dpy_cursor_define    = win32_mouse_define,
};

/* ------------------------------------------------------------------ */
/* the UI thread                                                        */

/*
 * Everything the QEMU thread asked for, executed here where the windows
 * are.  Called from win32_uiproc() for a posted message.
 */
static void win32_ui_handle(UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct win32_console *wcon = NULL;

    switch (msg) {
    case WIN32_UI_DAMAGE:
    case WIN32_UI_SWITCH:
    case WIN32_UI_TERM_OUTPUT:
        if (!win32_consoles || (int)wparam >= win32_num_outputs) {
            return;
        }
        wcon = &win32_consoles[wparam];
        break;
    default:
        break;
    }

    switch (msg) {
    case WIN32_UI_DAMAGE:
        win32_ui_damage(wcon);
        break;
    case WIN32_UI_SWITCH:
        win32_ui_switch(wcon);
        break;
    case WIN32_UI_CAPTION:
        win32_update_caption();
        break;
    case WIN32_UI_MOUSE_SET:
        win32_ui_mouse_set(wparam, (short)LOWORD(lparam),
                           (short)HIWORD(lparam));
        break;
    case WIN32_UI_CURSOR_DEFINE:
        win32_ui_cursor_define((QEMUCursor *)lparam);
        break;
    case WIN32_UI_MOUSE_MODE:
        win32_mouse_mode_changed();
        break;
    case WIN32_UI_TERM_OUTPUT:
        win32_term_console_drain(wcon);
        break;
    case WIN32_UI_SHUTDOWN:
        win32_ui_teardown();
        break;
    default:
        break;
    }
}

#define WIN32_UI_CLASS "QemuWin32Ui"

/*
 * A message-only window, created on the UI thread.  It exists so that
 * win32_ui_post() has somewhere to post to that is not bound up with the
 * frame's lifetime, and so that the cross-thread messages cannot be
 * confused with, or delayed behind subclassing of, the frame's own.
 */
static void win32_ui_window_init(void)
{
    WNDCLASSEX wc = {
        .cbSize        = sizeof(wc),
        .lpfnWndProc   = win32_uiproc,
        .hInstance     = GetModuleHandle(NULL),
        .lpszClassName = WIN32_UI_CLASS,
    };

    win32_ui_class_atom = RegisterClassEx(&wc);
    if (!win32_ui_class_atom) {
        error_report("win32: could not register the UI message class "
                     "(error %lu)", GetLastError());
        exit(1);
    }
    win32_uiwnd = CreateWindowEx(0, WIN32_UI_CLASS, NULL, 0, 0, 0, 0, 0,
                                 HWND_MESSAGE, NULL, GetModuleHandle(NULL),
                                 NULL);
    if (!win32_uiwnd) {
        error_report("win32: could not create the UI message window "
                     "(error %lu)", GetLastError());
        exit(1);
    }
    SetTimer(win32_uiwnd, WIN32_UI_TIMER_ID, WIN32_UI_TIMER_MS, NULL);
}

/*
 * Run the messages posted so far to completion, on the thread that will go
 * on to own them.  win32_display_init() calls this once at the end: the
 * listener registrations it has just done fire dpy_gfx_switch synchronously,
 * and the windows those create have to exist before init returns, because
 * the caller may immediately ask for fullscreen and because anything that
 * looks at win32_active would otherwise see nothing there.
 */
static void win32_ui_drain(void)
{
    MSG msg;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        win32_dispatch_one(&msg);
    }
}

/*
 * Tear the windows down.  UI thread, from WIN32_UI_SHUTDOWN, with the QEMU
 * thread waiting on win32_ui_done and *not* holding the BQL -- see
 * win32_display_cleanup().
 */
static void win32_ui_teardown(void)
{
    int i;

    if (win32_fault_handle) {
        RemoveVectoredExceptionHandler(win32_fault_handle);
        win32_fault_handle = NULL;
    }

    /*
     * The low-level keyboard hook is delivered to the thread that installed
     * it, which is this one, so it also has to be removed from here.
     */
    win32_kbd_set_grab(false);
    win32_kbd_set_window(NULL);
    ClipCursor(NULL);
    win32_show_cursor(true);

    for (i = 0; win32_consoles && i < win32_num_outputs; i++) {
        struct win32_console *wcon = &win32_consoles[i];

        if (win32_console_is_term(wcon)) {
            win32_term_console_fini(wcon);
            continue;
        }
        win32_window_destroy(wcon);
        qemu_free_displaysurface(wcon->surface);
        wcon->surface = NULL;
        qemu_free_displaysurface(wcon->pending_surface);
        wcon->pending_surface = NULL;
    }
    win32_frame_fini();
    win32_active = NULL;

    if (guest_sprite) {
        DestroyIcon(guest_sprite);
        guest_sprite = NULL;
    }
    if (win32_class_atom) {
        UnregisterClass(WIN32_WINDOW_CLASS, GetModuleHandle(NULL));
        win32_class_atom = 0;
    }

    KillTimer(win32_uiwnd, WIN32_UI_TIMER_ID);
    DestroyWindow(win32_uiwnd);
    win32_uiwnd = NULL;
    if (win32_ui_class_atom) {
        UnregisterClass(WIN32_UI_CLASS, GetModuleHandle(NULL));
        win32_ui_class_atom = 0;
    }

    SetEvent(win32_ui_done);
    PostQuitMessage(0);
}

/*
 * qemu_main(): the real main thread, handed over by system/main.c once it
 * has spawned the thread QEMU's main loop runs on.
 */
static int win32_main(void)
{
    MSG msg;

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        win32_dispatch_one(&msg);
    }

    /*
     * The loop only ends because win32_ui_teardown() asked it to, which
     * means the QEMU thread is already on its way through the rest of
     * qemu_cleanup() and will finish the process with exit().  Returning
     * from here would run main()'s own return path at the same time, so
     * park instead and let that exit() be the only one.
     */
    for (;;) {
        SleepEx(INFINITE, FALSE);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* init / cleanup                                                       */

static void win32_display_cleanup(void)
{
    int i;

    if (!win32_consoles) {
        return;
    }

    /*
     * QEMU thread, BQL held, from qemu_cleanup().  The windows belong to
     * the UI thread and DestroyWindow() from anywhere else silently does
     * nothing, so the teardown is a handshake: do the emulator-side half
     * here, ask the UI thread for its half, wait for it to finish.
     */
    if (win32_pump_timer) {
        timer_free(win32_pump_timer);
        win32_pump_timer = NULL;
    }
    qemu_remove_mouse_mode_change_notifier(&mouse_mode_notifier);

    for (i = 0; i < win32_num_outputs; i++) {
        struct win32_console *wcon = &win32_consoles[i];

        if (win32_console_is_term(wcon)) {
            continue;
        }
        /* after this no DisplayChangeListener op can arrive any more */
        qemu_console_unregister_listener(&wcon->dcl);
        /*
         * The UI thread reads wcon->kbd under the BQL and re-checks it
         * after taking the lock, so clearing it here -- with the BQL held
         * -- is what makes a keystroke that is already in flight harmless.
         */
        qkbd_state_free(wcon->kbd);
        wcon->kbd = NULL;
    }

    /*
     * This is the one place the QEMU thread waits for the UI thread, and
     * it drops the BQL for the duration so that it is not a cycle: a UI
     * thread parked on bql_lock() inside a modal loop can then make
     * progress and get to the shutdown message.  The wait is bounded
     * anyway, because a UI thread that never comes back must not stop the
     * process from exiting.
     */
    win32_ui_post(WIN32_UI_SHUTDOWN, 0, 0);
    bql_unlock();
    if (WaitForSingleObject(win32_ui_done, 5000) == WAIT_OBJECT_0) {
        bql_lock();
        g_clear_pointer(&win32_consoles, g_free);
        win32_num_outputs = 0;
    } else {
        bql_lock();
        warn_report("win32: the UI thread did not shut down in time; "
                    "leaving its windows to the process exit");
    }
}

static void win32_display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_WIN32);

    /*
     * Claim TYPE_CHARDEV_VC before ui/console-vc.c can, so that serial
     * ports and the monitor get the PuTTY terminal instead of QEMU's
     * minimal built-in one.  First registrant wins and a display's
     * early_init runs first; ui/gtk.c does exactly this for VTE.
     */
    win32_term_chardev_register();

    /*
     * There is no GL path any more; see the top of this file.  Fail rather
     * than quietly fall back to the 2D one, so that a command line asking
     * for acceleration is never silently not getting it.
     */
    if (o->has_gl && o->gl != DISPLAY_GL_MODE_OFF) {
        error_report("win32: OpenGL is not supported by this display "
                     "backend");
        exit(1);
    }
}

static void win32_register_class(void)
{
    WNDCLASSEX wc = {
        .cbSize        = sizeof(wc),
        .style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
        .lpfnWndProc   = win32_wndproc,
        .hInstance     = GetModuleHandle(NULL),
        .hIcon         = NULL,   /* the frame carries the icon */
        .hCursor       = NULL,   /* handled in WM_SETCURSOR */
        .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
        .lpszClassName = WIN32_WINDOW_CLASS,
    };

    win32_class_atom = RegisterClassEx(&wc);
    if (!win32_class_atom) {
        error_report("win32: could not register window class (error %lu)",
                     GetLastError());
        exit(1);
    }
}

static void win32_register_raw_input(void)
{
    RAWINPUTDEVICE rid = {
        .usUsagePage = 0x01,   /* generic desktop */
        .usUsage     = 0x02,   /* mouse */
        .dwFlags     = 0,
        .hwndTarget  = NULL,   /* follows keyboard focus */
    };

    /*
     * Raw input supplies the unaccelerated deltas needed for relative-mode
     * guests.  Legacy WM_MOUSEMOVE is deliberately left enabled, since
     * absolute-mode guests want window coordinates instead.
     */
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        warn_report("win32: raw mouse input unavailable (error %lu); "
                    "relative pointer motion may be imprecise",
                    GetLastError());
    }
}

static void win32_display_init(DisplayState *ds, DisplayOptions *o)
{
    int i, n_gfx, n_term;

    assert(o->type == DISPLAY_TYPE_WIN32);

    win32_opts = o;

    qemu_mutex_init(&win32_ui_mutex);
    win32_ui_done = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!win32_ui_done) {
        error_report("win32: could not create the shutdown event (error %lu)",
                     GetLastError());
        exit(1);
    }

    /*
     * Take the process's main thread for the UI and let system/main.c put
     * QEMU's main loop on a thread of its own -- the hook is already there
     * for ui/cocoa.m and needs nothing but a non-NULL qemu_main.  This is
     * set here, in init rather than early_init, because it must not happen
     * unless the backend really is going to come up.
     */
    qemu_main = win32_main;
    win32_ui_window_init();

    if (o->u.win32.has_grab_mod) {
        if (o->u.win32.grab_mod == HOT_KEY_MOD_LSHIFT_LCTRL_LALT) {
            alt_grab = true;
        } else if (o->u.win32.grab_mod == HOT_KEY_MOD_RCTRL) {
            ctrl_grab = true;
        }
    }

    win32_register_class();
    win32_register_raw_input();
    cursor_arrow = LoadCursor(NULL, IDC_ARROW);

    for (i = 0; qemu_console_lookup_by_index(i) != NULL; i++) {
        /* count the consoles */
    }
    n_gfx = i;
    n_term = win32_term_nb_vcs();
    win32_num_outputs = n_gfx + n_term;
    if (win32_num_outputs == 0) {
        return;
    }

    /*
     * Fill in the console array before the frame exists: the frame lays
     * itself out as soon as it is created, and that walks this array.
     */
    win32_consoles = g_new0(struct win32_console, win32_num_outputs);
    for (i = 0; i < win32_num_outputs; i++) {
        win32_consoles[i].idx = i;
        win32_consoles[i].tab = -1;
        win32_consoles[i].scale_x = 1.0;
        win32_consoles[i].scale_y = 1.0;
    }

    /* the frame has to exist before anything can become a child of it */
    win32_frame_init();

    /*
     * The terminal tabs go last, after every graphics console, so that
     * Ctrl-Alt-<n> keeps addressing the graphics consoles by the numbers
     * it always did.
     */
    for (i = 0; i < n_term; i++) {
        win32_term_console_init(&win32_consoles[n_gfx + i], i);
    }

    for (i = 0; i < n_gfx; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        const DisplayChangeListenerOps *ops = &dcl_2d_ops;

        assert(con != NULL);
        win32_consoles[i].kbd = qkbd_state_init(con);
        win32_consoles[i].con_index = qemu_console_get_index(con);
        qemu_console_register_listener(con, &win32_consoles[i].dcl, ops);
    }

    win32_have_gfx_console = (n_gfx > 0);
    win32_last_refresh_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    win32_pump_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                    win32_pump_tick, NULL);
    timer_mod(win32_pump_timer,
              win32_last_refresh_ms + GUI_REFRESH_INTERVAL_DEFAULT);

    win32_fault_handle = AddVectoredExceptionHandler(1, win32_fault_handler);

    mouse_mode_notifier.notify = win32_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    /*
     * Everything above ran on the process's main thread, which is about to
     * become the UI thread, so the windows it created are already owned by
     * the right one.  What it has *not* done is run the messages those
     * registrations posted: do that now, so that init returns with the
     * windows that exist actually created.
     */
    win32_ui_drain();

    if (o->has_full_screen && o->full_screen) {
        win32_toggle_fullscreen();
    }
}

static QemuDisplay qemu_display_win32 = {
    .type       = DISPLAY_TYPE_WIN32,
    /*
     * Plain "vc", with no size: the default is "vc:80Cx24C", and a size
     * only means anything to the built-in QemuConsole-backed chardev we
     * have just displaced.  ui/gtk.c does the same for VTE.
     */
    .vc         = "vc",
    .early_init = win32_display_early_init,
    .init       = win32_display_init,
    .cleanup    = win32_display_cleanup,
};

static void register_win32(void)
{
    qemu_display_register(&qemu_display_win32);
}

type_init(register_win32);
