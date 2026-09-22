/*
 * QEMU native Win32 display driver
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A display backend built directly on the Win32 API, as an alternative to
 * SDL on Windows hosts.  It is deliberately shaped like the other native
 * backend (ui/cocoa.m): one self-contained file, implementing nothing but
 * QemuDisplay and DisplayChangeListenerOps, with no hooks into the emulator
 * core beyond those two interfaces.  Keeping the surface that narrow is what
 * lets upstream changes to ui/console.h be absorbed mechanically.
 *
 * The 2D path blits the guest surface straight from pixman memory with
 * StretchDIBits(), so there is no intermediate copy and no texture to keep in
 * sync.  It is the default and needs nothing beyond the Win32 API.
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
 * upstream changes to the shared helpers stay easy to absorb.  dma-buf is
 * deliberately absent: it is a Linux concept and CONFIG_GBM is off here.
 */

#include "qemu/osdep.h"

#include <windows.h>
#include <windowsx.h>   /* GET_X_LPARAM / GET_Y_LPARAM */

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
#include "ui/win32-kbd-hook.h"
#include "standard-headers/linux/input-event-codes.h"

#ifdef CONFIG_OPENGL
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#include "ui/shader.h"
#endif

#define WIN32_WINDOW_CLASS  "QemuWin32Display"

/*
 * Refresh pacing, mirroring ui/sdl2.c: poll aggressively while events are
 * arriving, then fall back to the default interval once things go quiet.
 */
#define WIN32_REFRESH_INTERVAL_BUSY 10
#define WIN32_MAX_IDLE_COUNT (2 * GUI_REFRESH_INTERVAL_DEFAULT \
                              / WIN32_REFRESH_INTERVAL_BUSY + 1)

struct win32_console {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    DisplayOptions *opts;
    HWND hwnd;
    QKbdState *kbd;
    int idx;
    bool hidden;
    int idle_counter;

    /* saved geometry, for leaving fullscreen again */
    WINDOWPLACEMENT saved_placement;
    LONG_PTR saved_style;

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

static int win32_num_outputs;
static struct win32_console *win32_consoles;
static ATOM win32_class_atom;

static bool gui_grab;
static bool gui_fullscreen;
static bool gui_saved_grab;
static bool alt_grab;
static bool ctrl_grab;

static bool absolute_enabled;
static bool guest_cursor;
static int guest_x, guest_y;
static HCURSOR guest_sprite;
static HCURSOR cursor_arrow;
static bool cursor_visible = true;
static Notifier mouse_mode_notifier;

static void win32_update_caption(struct win32_console *wcon);
static void win32_grab_start(struct win32_console *wcon);
static void win32_grab_end(struct win32_console *wcon);

#ifdef CONFIG_OPENGL
static void win32_gl_init(struct win32_console *wcon);
static void win32_gl_fini(struct win32_console *wcon);
static void win32_gl_redraw(struct win32_console *wcon);
#endif

static struct win32_console *win32_console_from_hwnd(HWND hwnd)
{
    return (struct win32_console *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
}

/* ------------------------------------------------------------------ */
/* window management                                                    */

/*
 * Size the window so that its *client* area matches the guest surface;
 * AdjustWindowRect() accounts for the frame and caption.
 */
static void win32_window_set_client_size(struct win32_console *wcon,
                                         int w, int h)
{
    RECT r = { 0, 0, w, h };
    LONG_PTR style = GetWindowLongPtr(wcon->hwnd, GWL_STYLE);

    if (gui_fullscreen) {
        return;
    }

    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(wcon->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void win32_window_create(struct win32_console *wcon)
{
    int w, h;

    if (!wcon->surface) {
        return;
    }
    assert(!wcon->hwnd);

    w = surface_width(wcon->surface);
    h = surface_height(wcon->surface);

    wcon->hwnd = CreateWindowEx(0, WIN32_WINDOW_CLASS, QEMU_UI_NAME,
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!wcon->hwnd) {
        error_report("win32: could not create window (error %lu)",
                     GetLastError());
        exit(1);
    }

    SetWindowLongPtr(wcon->hwnd, GWLP_USERDATA, (LONG_PTR)wcon);
    win32_window_set_client_size(wcon, w, h);

#ifdef CONFIG_OPENGL
    /*
     * The EGL window surface is tied to this HWND, so it is created and
     * destroyed in step with it rather than once at init time.
     */
    if (wcon->opengl) {
        win32_gl_init(wcon);
    }
#endif

    if (!wcon->hidden) {
        ShowWindow(wcon->hwnd, SW_SHOW);
    }
    win32_update_caption(wcon);
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
}

static void win32_update_caption(struct win32_console *wcon)
{
    char title[1024];
    const char *status = "";

    if (!wcon->hwnd) {
        return;
    }

    if (!runstate_is_running()) {
        status = " [Stopped]";
    } else if (gui_grab) {
        if (alt_grab) {
            status = " - Press Ctrl-Alt-Shift-G to exit grab";
        } else if (ctrl_grab) {
            status = " - Press Right-Ctrl-G to exit grab";
        } else {
            status = " - Press Ctrl-Alt-G to exit grab";
        }
    }

    if (qemu_name) {
        snprintf(title, sizeof(title), QEMU_UI_NAME " (%s-%d)%s",
                 qemu_name, wcon->idx, status);
    } else {
        snprintf(title, sizeof(title), QEMU_UI_NAME "%s", status);
    }
    SetWindowText(wcon->hwnd, title);
}

static void win32_toggle_fullscreen(struct win32_console *wcon)
{
    HMONITOR mon;
    MONITORINFO mi = { .cbSize = sizeof(mi) };

    if (!wcon->hwnd) {
        return;
    }

    gui_fullscreen = !gui_fullscreen;

    if (gui_fullscreen) {
        wcon->saved_placement.length = sizeof(wcon->saved_placement);
        GetWindowPlacement(wcon->hwnd, &wcon->saved_placement);
        wcon->saved_style = GetWindowLongPtr(wcon->hwnd, GWL_STYLE);

        mon = MonitorFromWindow(wcon->hwnd, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfo(mon, &mi);

        SetWindowLongPtr(wcon->hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(wcon->hwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);

        gui_saved_grab = gui_grab;
        win32_grab_start(wcon);
    } else {
        if (!gui_saved_grab) {
            win32_grab_end(wcon);
        }
        SetWindowLongPtr(wcon->hwnd, GWL_STYLE, wcon->saved_style);
        SetWindowPlacement(wcon->hwnd, &wcon->saved_placement);
        SetWindowPos(wcon->hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    InvalidateRect(wcon->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* cursor and input grab                                                */

static void win32_show_cursor(struct win32_console *wcon, bool show)
{
    if (wcon->opts->has_show_cursor && wcon->opts->show_cursor) {
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

static void win32_clip_cursor(struct win32_console *wcon, bool clip)
{
    RECT r;

    if (!clip) {
        ClipCursor(NULL);
        return;
    }
    if (GetClientRect(wcon->hwnd, &r)) {
        MapWindowPoints(wcon->hwnd, NULL, (POINT *)&r, 2);
        ClipCursor(&r);
    }
}

static void win32_grab_start(struct win32_console *wcon)
{
    QemuConsole *con = wcon ? wcon->dcl.con : NULL;

    if (!con || !qemu_console_is_graphic(con) || !wcon->hwnd) {
        return;
    }
    if (GetForegroundWindow() != wcon->hwnd) {
        return;
    }

    if (guest_cursor) {
        SetCursor(guest_sprite);
    } else {
        win32_show_cursor(wcon, false);
    }
    win32_clip_cursor(wcon, true);
    win32_kbd_set_grab(true);
    gui_grab = true;
    win32_update_caption(wcon);
}

static void win32_grab_end(struct win32_console *wcon)
{
    win32_clip_cursor(wcon, false);
    win32_kbd_set_grab(false);
    gui_grab = false;
    win32_show_cursor(wcon, true);
    win32_update_caption(wcon);
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
            win32_grab_end(&win32_consoles[0]);
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
         * is actually pointing at.
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

static void win32_release_modifiers(struct win32_console *wcon)
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
        win32_toggle_fullscreen(wcon);
        win32_release_modifiers(wcon);
        return true;
    case 'G':
        if (gui_grab) {
            win32_grab_end(wcon);
        } else {
            win32_grab_start(wcon);
        }
        win32_release_modifiers(wcon);
        return true;
    case 'U':
        /* restore the window to the guest's own resolution */
        if (wcon->surface && !gui_fullscreen) {
            win32_window_set_client_size(wcon,
                                         surface_width(wcon->surface),
                                         surface_height(wcon->surface));
            InvalidateRect(wcon->hwnd, NULL, FALSE);
        }
        win32_release_modifiers(wcon);
        return true;
    case '1' ... '9':
        win = wparam - '1';
        if (win >= win32_num_outputs) {
            return false;
        }
        if (gui_grab) {
            win32_grab_end(wcon);
        }
        win32_consoles[win].hidden = !win32_consoles[win].hidden;
        if (win32_consoles[win].hwnd) {
            ShowWindow(win32_consoles[win].hwnd,
                       win32_consoles[win].hidden ? SW_HIDE : SW_SHOW);
        }
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

    if (down && win32_handle_hotkey(wcon, wparam)) {
        return;
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
/* window procedure                                                     */

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
            win32_clip_cursor(wcon, true);
        }
        return 0;

    case WM_MOVE:
        if (gui_grab) {
            win32_clip_cursor(wcon, true);
        }
        return 0;

    case WM_CLOSE:
        if (qemu_console_is_graphic(wcon->dcl.con)) {
            if (wcon->opts->has_window_close && !wcon->opts->window_close) {
                return 0;
            }
            shutdown_action = SHUTDOWN_ACTION_POWEROFF;
            qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        } else {
            ShowWindow(hwnd, SW_HIDE);
            wcon->hidden = true;
        }
        return 0;

    case WM_SETFOCUS:
        win32_kbd_set_window(hwnd);
        return 0;

    case WM_KILLFOCUS:
        /*
         * Keys held when focus is lost would otherwise stay stuck down in
         * the guest; this also covers Ctrl-Alt-Del, which cannot be hooked.
         */
        win32_release_modifiers(wcon);
        if (gui_grab) {
            win32_grab_end(wcon);
        }
        win32_kbd_set_window(NULL);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        win32_handle_key(wcon, wparam, lparam, true);
        return 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        win32_handle_key(wcon, wparam, lparam, false);
        return 0;

    case WM_MOUSEMOVE:
        win32_send_mouse_motion(wcon, GET_X_LPARAM(lparam),
                                GET_Y_LPARAM(lparam), 0, 0, false);
        win32_send_mouse_buttons(wcon, wparam);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_XBUTTONDOWN:
        /*
         * Clicking into an ungrabbed window with a relative-mode guest is
         * how the user asks for the pointer, matching the SDL behaviour.
         */
        if (!gui_grab && !qemu_input_is_absolute(wcon->dcl.con)) {
            win32_grab_start(wcon);
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

    if (!wcon->hwnd || !wcon->surface) {
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

static void win32_2d_switch(DisplayChangeListener *dcl,
                            DisplaySurface *new_surface)
{
    struct win32_console *wcon =
        container_of(dcl, struct win32_console, dcl);
    DisplaySurface *old_surface = wcon->surface;

    wcon->surface = new_surface;

    if (!new_surface) {
        win32_window_destroy(wcon);
        return;
    }

    if (surface_is_placeholder(new_surface) &&
        qemu_console_get_index(dcl->con)) {
        win32_window_destroy(wcon);
        return;
    }

    if (!wcon->hwnd) {
        win32_window_create(wcon);
    } else if (old_surface &&
               (surface_width(old_surface) != surface_width(new_surface) ||
                surface_height(old_surface) != surface_height(new_surface))) {
        win32_window_set_client_size(wcon,
                                     surface_width(new_surface),
                                     surface_height(new_surface));
    }

    if (wcon->hwnd) {
        InvalidateRect(wcon->hwnd, NULL, FALSE);
    }
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
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        idle = false;
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
        win32_update_caption(wcon);
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
            win32_show_cursor(wcon, true);
        }
        if (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled) {
            SetCursor(guest_sprite);
        }
    } else if (gui_grab) {
        win32_show_cursor(wcon, false);
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
     * EGLNativeWindowType is an HWND.
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

    if (!new_surface ||
        (surface_is_placeholder(new_surface) &&
         qemu_console_get_index(dcl->con))) {
        win32_window_destroy(wcon);
        return;
    }

    if (!wcon->hwnd) {
        /* this also brings up the EGL surface, context and shader */
        win32_window_create(wcon);
    } else if (old_surface &&
               (surface_width(old_surface) != surface_width(new_surface) ||
                surface_height(old_surface) != surface_height(new_surface))) {
        win32_window_set_client_size(wcon,
                                     surface_width(new_surface),
                                     surface_height(new_surface));
    }

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
        win32_update_caption(wcon);
    }

    if (wcon->updates && wcon->hwnd) {
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
    win32_show_cursor(&win32_consoles[0], true);

    for (i = 0; i < win32_num_outputs; i++) {
        qemu_console_unregister_listener(&win32_consoles[i].dcl);
        qkbd_state_free(win32_consoles[i].kbd);
        win32_window_destroy(&win32_consoles[i]);
    }
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
        .hIcon         = LoadIcon(GetModuleHandle(NULL), "QEMU_ICON"),
        .hCursor       = NULL, /* handled in WM_SETCURSOR */
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

    win32_consoles = g_new0(struct win32_console, win32_num_outputs);
    for (i = 0; i < win32_num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        const DisplayChangeListenerOps *ops = &dcl_2d_ops;

        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            win32_consoles[i].hidden = true;
        }
        win32_consoles[i].idx = i;
        win32_consoles[i].opts = o;
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
        win32_toggle_fullscreen(&win32_consoles[0]);
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
