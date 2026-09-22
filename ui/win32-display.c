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
 * The 2D path blits the guest surface straight from pixman memory with
 * StretchDIBits(), so there is no intermediate copy and no texture to keep in
 * sync.  Because the render window's client area is always exactly the area
 * the image is stretched into, the zoom factor is expressed purely as a
 * window size and the blit itself never has to know about it.
 *
 * The GL path (-display win32,gl=on) goes through EGL rather than WGL.  That
 * is deliberate: QEMU's blit shaders are '#version 300 es', and the GL context
 * virtio-gpu-gl/virgl needs must come from the same stack, so an ES-capable
 * implementation is required.  On Windows that means ANGLE, which is an EGL
 * implementation, and ui/egl-helpers.c already knows how to drive it
 * (qemu_egl_init_dpy_win32() even prefers ES for exactly this reason).  WGL
 * would give a desktop-GL context that neither the shaders nor virgl can use.
 *
 * The GL code below is structured like ui/gtk-egl.c and ui/sdl2-gl.c -- one
 * EGL context and one window surface per console, a QemuGLShader for the
 * surface blit, and a scanout mode for guest-supplied textures -- so that
 * upstream changes to the shared helpers stay easy to absorb.  Each console's
 * EGLSurface is created from its child HWND and lives exactly as long as that
 * HWND does; tab switches only show and hide the window, so no surface is
 * ever recreated behind the guest's back.  dma-buf is deliberately absent: it
 * is a Linux concept and CONFIG_GBM is off here.
 */

#include "qemu/osdep.h"

#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM */
#include <math.h>

#include "qemu/error-report.h"
#include "qemu/help-texts.h"
#include "qemu/module.h"
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

#ifdef CONFIG_OPENGL
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#include "ui/shader.h"
#endif

/*
 * Refresh pacing, mirroring ui/sdl2.c: poll aggressively while events are
 * arriving, then fall back to the default interval once things go quiet.
 */
#define WIN32_REFRESH_INTERVAL_BUSY 10
#define WIN32_MAX_IDLE_COUNT (2 * GUI_REFRESH_INTERVAL_DEFAULT \
                              / WIN32_REFRESH_INTERVAL_BUSY + 1)

int win32_num_outputs;
struct win32_console *win32_consoles;
DisplayOptions *win32_opts;

bool gui_grab;
bool alt_grab;
bool ctrl_grab;

bool absolute_enabled;
bool guest_cursor;
HCURSOR guest_sprite;
HCURSOR cursor_arrow;

static ATOM win32_class_atom;
static bool swallow_next_char;
static int guest_x, guest_y;
static bool cursor_visible = true;
static Notifier mouse_mode_notifier;

#ifdef CONFIG_OPENGL
static void win32_gl_init(struct win32_console *wcon);
static void win32_gl_fini(struct win32_console *wcon);
static void win32_gl_redraw(struct win32_console *wcon);
#endif

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
 * This is the *only* place the DPI factor is applied.  The blit stretches the
 * guest surface into the render window's client rectangle -- StretchDIBits()
 * in the 2D path, surface_gl_setup_viewport() in the GL path -- so both scale
 * to whatever window size comes out of here, and applying the factor again
 * inside either of them would square it.
 */
void win32_console_size(struct win32_console *wcon, int *w, int *h)
{
    double dpi = win32_dpi_scale();

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
#ifdef CONFIG_OPENGL
    if (wcon->opengl) {
        win32_gl_redraw(wcon);
        return;
    }
#endif
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

#ifdef CONFIG_OPENGL
    /*
     * The EGL window surface is tied to this HWND, so it is created and
     * destroyed in step with it rather than once at init time.
     */
    if (wcon->opengl) {
        win32_gl_init(wcon);
    }
#endif

    win32_frame_add_console(wcon);
}

static void win32_window_destroy(struct win32_console *wcon)
{
    if (!wcon->hwnd) {
        return;
    }
#ifdef CONFIG_OPENGL
    if (wcon->opengl) {
        win32_gl_fini(wcon);
    }
#endif
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

    if (!con || !qemu_console_is_graphic(con) || !wcon->hwnd) {
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

static void win32_mouse_mode_change(Notifier *notify, void *data)
{
    if (!win32_consoles) {
        return;
    }
    if (qemu_input_is_absolute(win32_consoles[0].dcl.con)) {
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
    qkbd_state_lift_all_keys(wcon->kbd);
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

    qemu_text_console_put_string(QEMU_TEXT_CONSOLE(con), utf8, u8len);
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

static void win32_paint(struct win32_console *wcon, HDC hdc)
{
    DisplaySurface *surf = wcon->surface;
    BITMAPINFO bmi;
    RECT client;

    if (!surf || !GetClientRect(wcon->hwnd, &client)) {
        return;
    }
    if (client.right <= 0 || client.bottom <= 0) {
        return;
    }

    win32_fill_bitmapinfo(surf, &bmi);
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, NULL);
    StretchDIBits(hdc,
                  0, 0, client.right, client.bottom,
                  0, 0, surface_width(surf), surface_height(surf),
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
#ifdef CONFIG_OPENGL
        if (wcon->opengl) {
            win32_gl_redraw(wcon);
        } else
#endif
        {
            win32_paint(wcon, hdc);
        }
        EndPaint(hwnd, &ps);
        return 0;

    case WM_ERASEBKGND:
        /* every pixel is repainted by WM_PAINT; erasing only causes flicker */
        return 1;

    case WM_SIZE:
#ifdef CONFIG_OPENGL
        /*
         * The EGL surface follows the HWND, but nothing repaints it by
         * itself; redraw here so a resize does not leave a stale image.
         */
        if (wcon->opengl) {
            win32_gl_redraw(wcon);
        }
#endif
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
        if (!gui_grab && !qemu_input_is_absolute(wcon->dcl.con)) {
            win32_grab_start();
        } else {
            SetCapture(hwnd);
            win32_send_mouse_buttons(wcon, wparam);
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

        if (!gui_grab || qemu_input_is_absolute(wcon->dcl.con)) {
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

static void win32_2d_update(DisplayChangeListener *dcl,
                            int x, int y, int w, int h)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    RECT client, dirty;

    if (!wcon->hwnd || !wcon->surface || wcon != win32_active) {
        return;
    }
    if (!GetClientRect(wcon->hwnd, &client) ||
        client.right <= 0 || client.bottom <= 0) {
        return;
    }

    /* map the guest-side dirty rectangle onto the (possibly scaled) window */
    dirty.left   = x * client.right / surface_width(wcon->surface);
    dirty.top    = y * client.bottom / surface_height(wcon->surface);
    dirty.right  = ((x + w) * client.right
                    + surface_width(wcon->surface) - 1)
                   / surface_width(wcon->surface);
    dirty.bottom = ((y + h) * client.bottom
                    + surface_height(wcon->surface) - 1)
                   / surface_height(wcon->surface);

    InvalidateRect(wcon->hwnd, &dirty, FALSE);
}

/*
 * Common to both paths: decide whether this console still deserves a window,
 * create or drop it, and re-fit the frame if it is the visible one.
 */
static void win32_switch_common(struct win32_console *wcon,
                                DisplaySurface *old_surface)
{
    DisplaySurface *new_surface = wcon->surface;

    if (!new_surface ||
        (surface_is_placeholder(new_surface) &&
         qemu_console_get_index(wcon->dcl.con))) {
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

static void win32_2d_switch(DisplayChangeListener *dcl,
                            DisplaySurface *new_surface)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    DisplaySurface *old_surface = wcon->surface;

    wcon->surface = new_surface;
    win32_switch_common(wcon, old_surface);
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

static void win32_poll_events(struct win32_console *wcon)
{
    MSG msg;
    bool idle = true;

    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        idle = false;
        /*
         * A dialog owned by the frame needs IsDialogMessage() to see its
         * input before it is translated and dispatched, or it has no Tab
         * navigation and neither Esc nor Enter reaches it.
         */
        if (win32_dialog_filter(&msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (idle) {
        if (wcon->idle_counter < WIN32_MAX_IDLE_COUNT) {
            wcon->idle_counter++;
            if (wcon->idle_counter >= WIN32_MAX_IDLE_COUNT) {
                wcon->dcl.update_interval = GUI_REFRESH_INTERVAL_DEFAULT;
            }
        }
    } else {
        wcon->idle_counter = 0;
        wcon->dcl.update_interval = WIN32_REFRESH_INTERVAL_BUSY;
    }
}

static void win32_2d_refresh(DisplayChangeListener *dcl)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    static bool was_running;
    bool running = runstate_is_running();

    qemu_console_hw_update(dcl->con);

    if (running != was_running) {
        was_running = running;
        win32_update_caption();
    }

    win32_poll_events(wcon);
}

static void win32_mouse_warp(DisplayChangeListener *dcl,
                             int x, int y, bool on)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);

    if (!qemu_console_is_graphic(dcl->con) || !wcon->hwnd) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            win32_show_cursor(true);
        }
        if (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled) {
            SetCursor(guest_sprite);
        }
    } else if (gui_grab) {
        win32_show_cursor(false);
    }
    guest_cursor = on;
    guest_x = x;
    guest_y = y;
}

static void win32_mouse_define(DisplayChangeListener *dcl, QEMUCursor *c)
{
    BITMAPV5HEADER bi = { 0 };
    HBITMAP color, mask;
    HDC hdc;
    void *bits = NULL;
    ICONINFO ii;
    HCURSOR cur;

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
        return;
    }
    memcpy(bits, c->data, (size_t)c->width * c->height * 4);

    /* the alpha channel does the masking; an all-zero mask keeps it intact */
    mask = CreateBitmap(c->width, c->height, 1, 1, NULL);
    if (!mask) {
        DeleteObject(color);
        return;
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
        return;
    }

    if (guest_sprite) {
        DestroyIcon(guest_sprite);
    }
    guest_sprite = cur;

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SetCursor(guest_sprite);
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
/* OpenGL (EGL/ANGLE) path                                              */

#ifdef CONFIG_OPENGL

/*
 * Bind this console's context together with its own window surface, the way
 * gd_egl_make_current() does: every console draws into its own HWND, so the
 * surface must be re-bound and not left wherever the previous console put it.
 */
static bool win32_gl_make_current(struct win32_console *wcon)
{
    if (!wcon->esurface || !wcon->ectx) {
        return false;
    }
    if (!eglMakeCurrent(qemu_egl_display, wcon->esurface,
                        wcon->esurface, wcon->ectx)) {
        error_report("win32: eglMakeCurrent failed: %s",
                     qemu_egl_get_error_string());
        return false;
    }
    return true;
}

static void win32_gl_init(struct win32_console *wcon)
{
    assert(wcon->hwnd);
    assert(!wcon->esurface);

    wcon->ectx = qemu_egl_init_ctx();
    if (!wcon->ectx) {
        error_report("win32: could not create an EGL context");
        exit(1);
    }

    /*
     * Despite its name this helper is platform independent -- it is just
     * eglCreateWindowSurface() plus eglMakeCurrent() -- and on Windows the
     * EGLNativeWindowType is an HWND.  A child HWND works exactly as well as
     * a top-level one, and since the child is never reparented the surface
     * stays valid across every tab switch.
     */
    wcon->esurface = qemu_egl_init_surface_x11(wcon->ectx,
                                               (EGLNativeWindowType)wcon->hwnd);
    if (!wcon->esurface) {
        error_report("win32: could not create an EGL surface for the window");
        exit(1);
    }

    wcon->gls = qemu_gl_init_shader();
}

static void win32_gl_fini(struct win32_console *wcon)
{
    if (wcon->esurface) {
        eglMakeCurrent(qemu_egl_display, wcon->esurface,
                       wcon->esurface, wcon->ectx);
    }

    if (wcon->gls) {
        surface_gl_destroy_texture(wcon->gls, wcon->surface);
        egl_fb_destroy(&wcon->guest_fb);
        egl_fb_destroy(&wcon->win_fb);
        qemu_gl_fini_shader(wcon->gls);
        wcon->gls = NULL;
    }
    wcon->scanout_mode = false;
    wcon->updates = 0;

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);

    if (wcon->esurface) {
        eglDestroySurface(qemu_egl_display, wcon->esurface);
        wcon->esurface = NULL;
    }
    if (wcon->ectx) {
        eglDestroyContext(qemu_egl_display, wcon->ectx);
        wcon->ectx = NULL;
    }
}

/* mirrors sdl2_set_scanout_mode() / gtk_egl_set_scanout_mode() */
static void win32_gl_set_scanout_mode(struct win32_console *wcon, bool scanout)
{
    if (wcon->scanout_mode == scanout) {
        return;
    }

    wcon->scanout_mode = scanout;
    if (!wcon->scanout_mode) {
        egl_fb_destroy(&wcon->guest_fb);
        if (wcon->surface && wcon->gls) {
            surface_gl_destroy_texture(wcon->gls, wcon->surface);
            surface_gl_create_texture(wcon->gls, wcon->surface);
        }
    }
}

/*
 * The client area of the render window is the whole of the area the guest
 * image occupies, so this is also where the zoom factor is honoured: it has
 * already been applied to the window size by the frame.
 */
static bool win32_gl_client_size(struct win32_console *wcon, int *w, int *h)
{
    RECT client;

    if (!wcon->hwnd || !GetClientRect(wcon->hwnd, &client)) {
        return false;
    }
    if (client.right <= 0 || client.bottom <= 0) {
        return false;
    }
    *w = client.right;
    *h = client.bottom;
    return true;
}

static void win32_gl_render_surface(struct win32_console *wcon)
{
    int ww, wh;

    if (!wcon->gls || !wcon->surface) {
        return;
    }
    if (!win32_gl_client_size(wcon, &ww, &wh)) {
        return;
    }
    if (!win32_gl_make_current(wcon)) {
        return;
    }

    surface_gl_setup_viewport(wcon->gls, wcon->surface, ww, wh);
    surface_gl_render_texture(wcon->gls, wcon->surface);
    eglSwapBuffers(qemu_egl_display, wcon->esurface);
}

static void win32_gl_scanout_flush(DisplayChangeListener *dcl,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    int ww, wh;

    if (!wcon->scanout_mode || !wcon->guest_fb.framebuffer) {
        return;
    }
    if (!win32_gl_client_size(wcon, &ww, &wh)) {
        return;
    }
    if (!win32_gl_make_current(wcon)) {
        return;
    }

    egl_fb_setup_default(&wcon->win_fb, ww, wh, 0, 0);
    egl_fb_blit(&wcon->win_fb, &wcon->guest_fb, !wcon->y0_top);
    eglSwapBuffers(qemu_egl_display, wcon->esurface);
}

/* repaint the window from whatever the current source is */
static void win32_gl_redraw(struct win32_console *wcon)
{
    if (!wcon->hwnd) {
        return;
    }
    if (wcon->scanout_mode) {
        /* only the dcl argument of the flush is used */
        win32_gl_scanout_flush(&wcon->dcl, 0, 0, 0, 0);
        return;
    }
    win32_gl_render_surface(wcon);
}

static void win32_gl_update(DisplayChangeListener *dcl,
                            int x, int y, int w, int h)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);

    if (!wcon->gls || !wcon->surface) {
        return;
    }
    if (!win32_gl_make_current(wcon)) {
        return;
    }
    surface_gl_update_texture(wcon->gls, wcon->surface, x, y, w, h);
    wcon->updates++;
}

static void win32_gl_switch(DisplayChangeListener *dcl,
                            DisplaySurface *new_surface)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    DisplaySurface *old_surface = wcon->surface;

    if (wcon->gls && win32_gl_make_current(wcon)) {
        surface_gl_destroy_texture(wcon->gls, wcon->surface);
    }

    wcon->surface = new_surface;
    win32_switch_common(wcon, old_surface);

    if (wcon->gls && win32_gl_make_current(wcon)) {
        surface_gl_create_texture(wcon->gls, new_surface);
        wcon->updates++;
    }
}

static void win32_gl_refresh(DisplayChangeListener *dcl)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    static bool was_running;
    bool running = runstate_is_running();

    qemu_console_hw_update(dcl->con);

    if (running != was_running) {
        was_running = running;
        win32_update_caption();
    }

    if (wcon->updates && wcon->hwnd && wcon == win32_active) {
        wcon->updates = 0;
        win32_gl_render_surface(wcon);
    }

    win32_poll_events(wcon);
}

static void win32_gl_scanout_disable(DisplayChangeListener *dcl)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);

    if (!win32_gl_make_current(wcon)) {
        /* no context to delete objects with; just drop out of scanout mode */
        wcon->scanout_mode = false;
        return;
    }
    win32_gl_set_scanout_mode(wcon, false);
}

static void win32_gl_scanout_texture(DisplayChangeListener *dcl,
                                     uint32_t backing_id,
                                     bool backing_y_0_top,
                                     uint32_t backing_width,
                                     uint32_t backing_height,
                                     uint32_t x, uint32_t y,
                                     uint32_t w, uint32_t h,
                                     void *d3d_tex2d)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);

    wcon->y0_top = backing_y_0_top;

    if (!win32_gl_make_current(wcon)) {
        return;
    }

    win32_gl_set_scanout_mode(wcon, true);
    egl_fb_setup_for_tex(&wcon->guest_fb, backing_width, backing_height,
                         backing_id, false);
}

static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name               = "win32-gl",
    .dpy_gfx_update         = win32_gl_update,
    .dpy_gfx_switch         = win32_gl_switch,
    .dpy_gfx_check_format   = console_gl_check_format,
    .dpy_refresh            = win32_gl_refresh,
    .dpy_mouse_set          = win32_mouse_warp,
    .dpy_cursor_define      = win32_mouse_define,

    .dpy_gl_scanout_disable = win32_gl_scanout_disable,
    .dpy_gl_scanout_texture = win32_gl_scanout_texture,
    .dpy_gl_update          = win32_gl_scanout_flush,
};

static bool win32_gl_is_compatible_dcl(DisplayGLCtx *dgc,
                                       DisplayChangeListener *dcl)
{
    return dcl->ops == &dcl_gl_ops;
}

/*
 * Contexts handed to the guest device (virtio-gpu-gl) share ours, so the
 * textures it renders into can be blitted by the scanout path above.
 */
static QEMUGLContext win32_gl_create_context(DisplayGLCtx *dgc,
                                             QEMUGLParams *params)
{
    struct win32_console *wcon =
        container_of(dgc, struct win32_console, dgc);

    win32_gl_make_current(wcon);
    return qemu_egl_create_context(dgc, params, wcon->ectx);
}

static int win32_gl_make_context_current(DisplayGLCtx *dgc,
                                         QEMUGLContext ctx)
{
    struct win32_console *wcon =
        container_of(dgc, struct win32_console, dgc);

    if (!eglMakeCurrent(qemu_egl_display, wcon->esurface,
                        wcon->esurface, ctx)) {
        error_report("win32: eglMakeCurrent failed: %s",
                     qemu_egl_get_error_string());
        return -1;
    }
    return 0;
}

static const DisplayGLCtxOps gl_ctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = win32_gl_is_compatible_dcl,
    .dpy_gl_ctx_create            = win32_gl_create_context,
    .dpy_gl_ctx_destroy           = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current      = win32_gl_make_context_current,
};

#endif /* CONFIG_OPENGL */

/* ------------------------------------------------------------------ */
/* init / cleanup                                                       */

static void win32_display_cleanup(void)
{
    int i;

    if (!win32_consoles) {
        return;
    }

    qemu_remove_mouse_mode_change_notifier(&mouse_mode_notifier);
    win32_kbd_set_grab(false);
    win32_kbd_set_window(NULL);
    ClipCursor(NULL);
    win32_show_cursor(true);

    for (i = 0; i < win32_num_outputs; i++) {
        qemu_console_unregister_listener(&win32_consoles[i].dcl);
        qkbd_state_free(win32_consoles[i].kbd);
        win32_window_destroy(&win32_consoles[i]);
    }
    win32_frame_fini();
    win32_active = NULL;
    g_clear_pointer(&win32_consoles, g_free);
    win32_num_outputs = 0;

    if (guest_sprite) {
        DestroyIcon(guest_sprite);
        guest_sprite = NULL;
    }
    if (win32_class_atom) {
        UnregisterClass(WIN32_WINDOW_CLASS, GetModuleHandle(NULL));
        win32_class_atom = 0;
    }
}

static void win32_display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_WIN32);

    if (!o->has_gl || o->gl == DISPLAY_GL_MODE_OFF) {
        return;
    }

#ifndef CONFIG_OPENGL
    error_report("win32: this QEMU was built without OpenGL support, "
                 "so -display win32,gl=on is not available");
    exit(1);
#else
    /*
     * epoxy resolves the EGL entry points lazily with LoadLibrary(), and
     * aborts the process if it cannot; ask it first so that a host without
     * ANGLE gets a diagnostic instead of a crash.
     */
    if (!epoxy_has_egl()) {
        error_report("win32: no EGL implementation could be loaded; "
                     "-display win32,gl=on requires ANGLE -- put libEGL.dll "
                     "and libGLESv2.dll next to the QEMU executable");
        exit(1);
    }

    /*
     * Do this here rather than in init(): display_opengl has to be set
     * before the guest devices are created, and a GL-capable device such as
     * virtio-gpu-gl picks up qemu_egl_display at realize time.
     */
    if (qemu_egl_init_dpy_win32(EGL_DEFAULT_DISPLAY, o->gl) < 0) {
        error_report("win32: could not initialise EGL; "
                     "-display win32,gl=on is not usable on this host");
        exit(1);
    }

    display_opengl = 1;
#endif
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
    int i;

    assert(o->type == DISPLAY_TYPE_WIN32);

    win32_opts = o;

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
    win32_num_outputs = i;
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

    for (i = 0; i < win32_num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        const DisplayChangeListenerOps *ops = &dcl_2d_ops;

        assert(con != NULL);
        win32_consoles[i].kbd = qkbd_state_init(con);
#ifdef CONFIG_OPENGL
        win32_consoles[i].opengl = display_opengl;
        if (display_opengl) {
            ops = &dcl_gl_ops;
            win32_consoles[i].dgc.ops = &gl_ctx_ops;
            qemu_console_set_display_gl_ctx(con, &win32_consoles[i].dgc);
        }
#endif
        qemu_console_register_listener(con, &win32_consoles[i].dcl, ops);
    }

    mouse_mode_notifier.notify = win32_mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    if (o->has_full_screen && o->full_screen) {
        win32_toggle_fullscreen();
    }

    /*
     * The message pump runs from dpy_refresh on the main thread, so the
     * main loop stays where it is -- no equivalent of Cocoa's main-thread
     * takeover is needed here.
     */
    qemu_main = NULL;
}

static QemuDisplay qemu_display_win32 = {
    .type       = DISPLAY_TYPE_WIN32,
    .early_init = win32_display_early_init,
    .init       = win32_display_init,
    .cleanup    = win32_display_cleanup,
};

static void register_win32(void)
{
    qemu_display_register(&qemu_display_win32);
}

type_init(register_win32);
