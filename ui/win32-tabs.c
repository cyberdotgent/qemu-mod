/*
 * QEMU native Win32 display driver -- frame window, tab strip and menu bar
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * There is exactly one top-level window for the lifetime of the process.
 * It carries a menu bar and a SysTabControl32 tab strip, and every console's
 * render window is a WS_CHILD of it, shown when its tab is selected and
 * hidden otherwise.  Keeping one child HWND per console rather than one
 * shared render window is deliberate: under -display win32,gl=on each child
 * owns an EGLSurface created from its HWND, and a child that is only ever
 * shown and hidden never invalidates that surface.
 *
 * The shape mirrors ui/gtk.c -- a GtkNotebook of per-console drawing areas
 * under one GtkWindow -- so the two backends behave the same way and the
 * menu semantics can be lifted from there directly.
 */

#include "qemu/osdep.h"

#include <windows.h>
#include <commctrl.h>

#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu/error-report.h"
#include "qemu/help-texts.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "ui/console.h"
#include "ui/win32-display.h"
#include "ui/win32-kbd-hook.h"

/* not in mingw-w64 11's headers */
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)(LONG_PTR)(-4))
#endif
#ifndef USER_DEFAULT_SCREEN_DPI
#define USER_DEFAULT_SCREEN_DPI 96
#endif

enum {
    IDM_PAUSE = 0x100,
    IDM_RESET,
    IDM_POWERDOWN,
    IDM_QUIT,

    IDM_FULLSCREEN,
    IDM_GRAB_INPUT,
    IDM_GRAB_HOVER,
    IDM_ZOOM_IN,
    IDM_ZOOM_OUT,
    IDM_ZOOM_FIXED,
    IDM_ZOOM_FIT,
    IDM_SHOW_TABS,
};

#define WIN32_TABCTL_ID 1

HWND win32_frame;
HWND win32_tabctl;
struct win32_console *win32_active;

bool gui_fullscreen;
bool gui_free_scale;
bool gui_grab_on_hover;
bool gui_show_tabs = true;

static HMENU win32_menu;
static ATOM win32_frame_atom;
static WNDPROC win32_tabctl_oldproc;
static bool gui_saved_grab;
static WINDOWPLACEMENT saved_placement;
static LONG_PTR saved_style;

/* DPI of the monitor the frame is on; 96 is Windows' unscaled baseline */
static UINT win32_dpi = USER_DEFAULT_SCREEN_DPI;
static HFONT win32_tab_font;

/* ------------------------------------------------------------------ */
/* DPI awareness                                                        */

/*
 * Without this the frame's menu bar and tab strip are rendered at 96 DPI and
 * stretched by the compositor, which is exactly the kind of blur that only
 * becomes visible once there is chrome around the guest image.  The API is
 * Windows 10 1703 and later, so it is resolved at run time and the process
 * falls back to the system-DPI-aware behaviour it had before on anything
 * older.
 */
static void win32_set_dpi_awareness(void)
{
    typedef BOOL(WINAPI * set_ctx_fn)(HANDLE);
    set_ctx_fn set_ctx;
    HMODULE user32 = GetModuleHandle("user32.dll");

    if (user32) {
        set_ctx = (set_ctx_fn)(void *)GetProcAddress(
            user32, "SetProcessDpiAwarenessContext");
        if (set_ctx &&
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            return;
        }
    }
    SetProcessDPIAware();
}

/*
 * Having declared the process DPI-aware above, Windows stops bitmap-scaling
 * anything this process draws for itself.  It keeps scaling what *it* draws
 * -- the menu bar, the non-client frame -- so that chrome comes out the right
 * physical size on its own, but the guest image does not: it is drawn by us,
 * into a window we size, and unless the DPI is folded into that size a 720x400
 * guest occupies 720x400 physical pixels next to chrome that is 1.5x bigger.
 * So the factor is obtained here and applied to the *window geometry* only.
 */
static UINT win32_query_dpi(HWND hwnd)
{
    typedef UINT(WINAPI * get_dpi_fn)(HWND);
    HMODULE user32 = GetModuleHandle("user32.dll");
    get_dpi_fn get_dpi = NULL;
    UINT dpi = 0;
    HDC hdc;

    if (user32) {
        get_dpi = (get_dpi_fn)(void *)GetProcAddress(user32,
                                                     "GetDpiForWindow");
    }
    if (get_dpi && hwnd) {
        dpi = get_dpi(hwnd);
        if (dpi) {
            return dpi;
        }
    }

    /*
     * Windows 10 1607 and older: there is no per-monitor DPI to ask for, and
     * the process is only system-DPI-aware anyway, so the screen DC's
     * LOGPIXELSX is exactly the factor that applies.
     */
    hdc = GetDC(NULL);
    if (hdc) {
        dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
    }
    return dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
}

/*
 * The tab control labels itself with whatever font it is given, and nothing
 * rescales that font for us: a stock font would leave the strip a fixed number
 * of physical pixels tall at any DPI, which also feeds back into the geometry
 * through TabCtrl_AdjustRect().  Ask for the shell's message font at the
 * frame's current DPI instead, and re-ask whenever the DPI changes.
 */
static void win32_update_tab_font(void)
{
    typedef BOOL(WINAPI * spi_dpi_fn)(UINT, UINT, PVOID, UINT, UINT);
    HMODULE user32 = GetModuleHandle("user32.dll");
    spi_dpi_fn spi_dpi = NULL;
    NONCLIENTMETRICSW ncm;
    HFONT font;

    if (!win32_tabctl) {
        return;
    }
    if (user32) {
        spi_dpi = (spi_dpi_fn)(void *)GetProcAddress(
            user32, "SystemParametersInfoForDpi");
    }

    /*
     * Explicitly the wide structure: SystemParametersInfoForDpi() has no ANSI
     * counterpart, so it fills a NONCLIENTMETRICSW whatever the rest of this
     * file is compiled as, and handing it the (smaller) ANSI one overruns the
     * stack.  SystemParametersInfoW() is used for the same reason below.
     */
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    /*
     * Without the ForDpi variant the process is at most system-DPI-aware, so
     * the plain call already reports metrics in the units we draw in.
     */
    if (spi_dpi) {
        if (!spi_dpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                     win32_dpi)) {
            return;
        }
    } else if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm),
                                      &ncm, 0)) {
        return;
    }

    font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!font) {
        return;
    }
    SendMessage(win32_tabctl, WM_SETFONT, (WPARAM)font, TRUE);
    if (win32_tab_font) {
        DeleteObject(win32_tab_font);
    }
    win32_tab_font = font;
}

/* adopt a new DPI: the geometry factor and everything sized in points */
static void win32_set_dpi(UINT dpi)
{
    win32_dpi = dpi ? dpi : USER_DEFAULT_SCREEN_DPI;
    win32_update_tab_font();
}

double win32_dpi_scale(void)
{
    return (double)win32_dpi / USER_DEFAULT_SCREEN_DPI;
}

/* ------------------------------------------------------------------ */
/* menu bar                                                             */

static HMENU win32_build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();

    AppendMenu(machine, MF_STRING, IDM_PAUSE, "Pause");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_RESET, "Reset");
    AppendMenu(machine, MF_STRING, IDM_POWERDOWN, "Power Down");
    AppendMenu(machine, MF_SEPARATOR, 0, NULL);
    AppendMenu(machine, MF_STRING, IDM_QUIT, "Quit");

    AppendMenu(view, MF_STRING, IDM_FULLSCREEN, "Fullscreen");
    AppendMenu(view, MF_SEPARATOR, 0, NULL);
    AppendMenu(view, MF_STRING, IDM_GRAB_INPUT, "Grab Input");
    AppendMenu(view, MF_STRING, IDM_GRAB_HOVER, "Grab On Hover");
    AppendMenu(view, MF_SEPARATOR, 0, NULL);
    AppendMenu(view, MF_STRING, IDM_ZOOM_IN, "Zoom In");
    AppendMenu(view, MF_STRING, IDM_ZOOM_OUT, "Zoom Out");
    AppendMenu(view, MF_STRING, IDM_ZOOM_FIXED, "Best Fit");
    AppendMenu(view, MF_STRING, IDM_ZOOM_FIT, "Zoom To Fit");
    AppendMenu(view, MF_SEPARATOR, 0, NULL);
    AppendMenu(view, MF_STRING, IDM_SHOW_TABS, "Show Tabs");

    AppendMenu(bar, MF_POPUP, (UINT_PTR)machine, "Machine");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)view, "View");

    return bar;
}

static void win32_check_item(HMENU menu, UINT id, bool checked)
{
    CheckMenuItem(menu, id, MF_BYCOMMAND | (checked ? MF_CHECKED
                                                    : MF_UNCHECKED));
}

/*
 * Every one of these is derived state, so rather than keeping the menu in
 * sync with each toggle it is refreshed once, just before it is drawn.
 */
static void win32_refresh_menu(HMENU menu)
{
    bool graphic = win32_active &&
                   qemu_console_is_graphic(win32_active->dcl.con);

    win32_check_item(menu, IDM_PAUSE, !runstate_is_running());
    win32_check_item(menu, IDM_FULLSCREEN, gui_fullscreen);
    win32_check_item(menu, IDM_GRAB_INPUT, gui_grab);
    win32_check_item(menu, IDM_GRAB_HOVER, gui_grab_on_hover);
    win32_check_item(menu, IDM_ZOOM_FIT, gui_free_scale);
    win32_check_item(menu, IDM_SHOW_TABS, gui_show_tabs);

    EnableMenuItem(menu, IDM_GRAB_INPUT,
                   MF_BYCOMMAND | (graphic ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, IDM_GRAB_HOVER,
                   MF_BYCOMMAND | (graphic ? MF_ENABLED : MF_GRAYED));
}

static void win32_menu_command(UINT id)
{
    switch (id) {
    case IDM_PAUSE:
        if (runstate_is_running()) {
            qmp_stop(NULL);
        } else {
            qmp_cont(NULL);
        }
        break;
    case IDM_RESET:
        qmp_system_reset(NULL);
        break;
    case IDM_POWERDOWN:
        qmp_system_powerdown(NULL);
        break;
    case IDM_QUIT:
        qmp_quit(NULL);
        break;

    case IDM_FULLSCREEN:
        win32_toggle_fullscreen();
        break;
    case IDM_GRAB_INPUT:
        if (gui_grab) {
            win32_grab_end();
        } else {
            win32_grab_start();
        }
        break;
    case IDM_GRAB_HOVER:
        gui_grab_on_hover = !gui_grab_on_hover;
        break;
    case IDM_ZOOM_IN:
        win32_zoom_step(WIN32_SCALE_STEP);
        break;
    case IDM_ZOOM_OUT:
        win32_zoom_step(-WIN32_SCALE_STEP);
        break;
    case IDM_ZOOM_FIXED:
        win32_zoom_fixed();
        break;
    case IDM_ZOOM_FIT:
        win32_toggle_free_scale();
        break;
    case IDM_SHOW_TABS:
        gui_show_tabs = !gui_show_tabs;
        ShowWindow(win32_tabctl, gui_show_tabs ? SW_SHOW : SW_HIDE);
        win32_frame_fit();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* zoom                                                                 */

void win32_zoom_step(double delta)
{
    if (!win32_active) {
        return;
    }
    gui_free_scale = false;
    win32_active->scale_x = MIN(MAX(win32_active->scale_x + delta,
                                    WIN32_SCALE_MIN), WIN32_SCALE_MAX);
    win32_active->scale_y = MIN(MAX(win32_active->scale_y + delta,
                                    WIN32_SCALE_MIN), WIN32_SCALE_MAX);
    win32_frame_fit();
}

void win32_zoom_fixed(void)
{
    if (!win32_active) {
        return;
    }
    gui_free_scale = false;
    win32_active->scale_x = 1.0;
    win32_active->scale_y = 1.0;
    win32_frame_fit();
}

void win32_toggle_free_scale(void)
{
    gui_free_scale = !gui_free_scale;
    if (!gui_free_scale && win32_active) {
        win32_active->scale_x = 1.0;
        win32_active->scale_y = 1.0;
    }
    win32_frame_fit();
}

/* ------------------------------------------------------------------ */
/* geometry                                                             */

static bool win32_tabs_visible(void)
{
    return gui_show_tabs && !gui_fullscreen;
}

/*
 * The tab control covers the whole client area and the active render window
 * sits on top of it, inside the display rectangle the control reports.  That
 * is the layout the common controls documentation describes, and it keeps the
 * render window's client area exactly equal to the area the guest image is
 * stretched into -- which is what both the 2D blit and the GL viewport use.
 */
static void win32_frame_display_rect(RECT *r)
{
    GetClientRect(win32_frame, r);
    if (win32_tabs_visible()) {
        TabCtrl_AdjustRect(win32_tabctl, FALSE, r);
    }
}

/* size of the frame window needed to give the guest image w x h pixels */
static void win32_frame_outer_size(int w, int h, int *ow, int *oh)
{
    RECT r = { 0, 0, w, h };
    LONG_PTR style = GetWindowLongPtr(win32_frame, GWL_STYLE);
    LONG_PTR ex = GetWindowLongPtr(win32_frame, GWL_EXSTYLE);

    if (win32_tabs_visible()) {
        TabCtrl_AdjustRect(win32_tabctl, TRUE, &r);
    }
    AdjustWindowRectEx(&r, style, GetMenu(win32_frame) != NULL, ex);
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
}

void win32_frame_layout(void)
{
    RECT disp;
    int i;

    if (!win32_frame) {
        return;
    }

    if (win32_tabs_visible()) {
        RECT client;
        GetClientRect(win32_frame, &client);
        SetWindowPos(win32_tabctl, HWND_BOTTOM, 0, 0,
                     client.right, client.bottom,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
    } else {
        ShowWindow(win32_tabctl, SW_HIDE);
    }

    win32_frame_display_rect(&disp);

    for (i = 0; win32_consoles && i < win32_num_outputs; i++) {
        struct win32_console *wcon = &win32_consoles[i];

        if (!wcon->hwnd) {
            continue;
        }
        if (wcon != win32_active) {
            ShowWindow(wcon->hwnd, SW_HIDE);
            continue;
        }
        SetWindowPos(wcon->hwnd, HWND_TOP, disp.left, disp.top,
                     disp.right - disp.left, disp.bottom - disp.top,
                     SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    if (gui_grab) {
        win32_clip_cursor(true);
    }
}

void win32_frame_fit(void)
{
    int w, h, ow, oh;

    if (!win32_frame) {
        return;
    }
    if (gui_fullscreen || gui_free_scale || !win32_active) {
        win32_frame_layout();
        if (win32_active) {
            win32_console_redraw(win32_active);
        }
        return;
    }

    win32_console_size(win32_active, &w, &h);
    if (w <= 0 || h <= 0) {
        win32_frame_layout();
        return;
    }

    win32_frame_outer_size(w, h, &ow, &oh);
    SetWindowPos(win32_frame, NULL, 0, 0, ow, oh,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    win32_frame_layout();
    win32_console_redraw(win32_active);
}

static void win32_frame_minmax(MINMAXINFO *mmi)
{
    int w, h, ow, oh;

    if (!win32_active || gui_fullscreen) {
        return;
    }

    win32_console_size(win32_active, &w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }
    if (gui_free_scale) {
        /* free scaling only bottoms out at the same minimum gtk uses */
        w = w * WIN32_SCALE_MIN;
        h = h * WIN32_SCALE_MIN;
    }
    win32_frame_outer_size(w, h, &ow, &oh);
    mmi->ptMinTrackSize.x = ow;
    mmi->ptMinTrackSize.y = oh;
}

/* ------------------------------------------------------------------ */
/* caption                                                              */

void win32_update_caption(void)
{
    char title[1024];
    const char *status = "";
    g_autofree char *label = NULL;

    if (!win32_frame) {
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

    if (win32_active) {
        label = qemu_console_get_label(win32_active->dcl.con);
    }

    if (qemu_name && label) {
        snprintf(title, sizeof(title), QEMU_UI_NAME " (%s) - %s%s",
                 qemu_name, label, status);
    } else if (qemu_name) {
        snprintf(title, sizeof(title), QEMU_UI_NAME " (%s)%s",
                 qemu_name, status);
    } else if (label) {
        snprintf(title, sizeof(title), QEMU_UI_NAME " - %s%s",
                 label, status);
    } else {
        snprintf(title, sizeof(title), QEMU_UI_NAME "%s", status);
    }
    SetWindowText(win32_frame, title);
}

/* ------------------------------------------------------------------ */
/* fullscreen                                                           */

void win32_toggle_fullscreen(void)
{
    HMONITOR mon;
    MONITORINFO mi = { .cbSize = sizeof(mi) };

    if (!win32_frame) {
        return;
    }

    gui_fullscreen = !gui_fullscreen;

    if (gui_fullscreen) {
        saved_placement.length = sizeof(saved_placement);
        GetWindowPlacement(win32_frame, &saved_placement);
        saved_style = GetWindowLongPtr(win32_frame, GWL_STYLE);

        mon = MonitorFromWindow(win32_frame, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfo(mon, &mi);

        SetMenu(win32_frame, NULL);
        SetWindowLongPtr(win32_frame, GWL_STYLE, WS_POPUP | WS_VISIBLE |
                         WS_CLIPCHILDREN);
        SetWindowPos(win32_frame, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);

        gui_saved_grab = gui_grab;
        win32_grab_start();
    } else {
        if (!gui_saved_grab) {
            win32_grab_end();
        }
        SetMenu(win32_frame, win32_menu);
        SetWindowLongPtr(win32_frame, GWL_STYLE, saved_style);
        SetWindowPlacement(win32_frame, &saved_placement);
        SetWindowPos(win32_frame, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    win32_frame_layout();
    if (win32_active) {
        win32_console_redraw(win32_active);
        InvalidateRect(win32_active->hwnd, NULL, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* tab strip                                                            */

/*
 * The tab items are rebuilt wholesale rather than patched in place.  A
 * console's window appears and disappears as its surface comes and goes, so
 * the mapping from console index to tab index is not stable, and there are
 * at most a handful of tabs -- keeping one authoritative ordering (console
 * index order, as Ctrl-Alt-<n> expects) is worth more than the saved calls.
 */
static void win32_tabs_rebuild(void)
{
    int i, pos = 0;

    if (!win32_tabctl) {
        return;
    }

    TabCtrl_DeleteAllItems(win32_tabctl);

    for (i = 0; win32_consoles && i < win32_num_outputs; i++) {
        struct win32_console *wcon = &win32_consoles[i];
        g_autofree char *label = NULL;
        TCITEM item = { .mask = TCIF_TEXT };

        if (!wcon->hwnd) {
            wcon->tab = -1;
            continue;
        }

        label = qemu_console_get_label(wcon->dcl.con);
        item.pszText = label;
        if (TabCtrl_InsertItem(win32_tabctl, pos, &item) < 0) {
            wcon->tab = -1;
            continue;
        }
        wcon->tab = pos++;
    }

    if (win32_active && win32_active->tab >= 0) {
        TabCtrl_SetCurSel(win32_tabctl, win32_active->tab);
    }
}

/*
 * The tab strip must never hold the keyboard focus.  Clicking a tab makes
 * Win32 focus the control, and a focused SysTabControl32 consumes Enter and
 * the arrow keys for its own navigation -- so keystrokes meant for the guest
 * are silently eaten, and the arrows switch consoles behind the user's back.
 * Handing the focus straight back to the render child keeps every key going
 * where it always did, while the mouse still selects tabs normally: tab
 * selection is driven by the click, not by the focus.
 */
static LRESULT CALLBACK win32_tabproc(HWND hwnd, UINT msg,
                                      WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_SETFOCUS && win32_active && win32_active->hwnd) {
        SetFocus(win32_active->hwnd);
        return 0;
    }
    return CallWindowProc(win32_tabctl_oldproc, hwnd, msg, wparam, lparam);
}

void win32_frame_activate(struct win32_console *wcon)
{
    if (!wcon || !wcon->hwnd) {
        return;
    }

    if (wcon != win32_active) {
        if (gui_grab) {
            win32_grab_end();
        }
        /*
         * Switching tabs moves the keyboard focus from one child window to
         * another, which the guest never sees.  Any key held down at that
         * moment would stay down in the console being left -- and a key the
         * guest believes is held is a key the guest's own keyboard driver
         * keeps repeating.  Lift them here, the way the per-console
         * WM_KILLFOCUS used to before there was only one top-level window.
         */
        if (win32_active) {
            win32_release_modifiers(win32_active);
        }
    }

    win32_active = wcon;
    if (wcon->tab >= 0) {
        TabCtrl_SetCurSel(win32_tabctl, wcon->tab);
    }

    /*
     * The low-level keyboard hook compares its window against GetFocus(), so
     * it has to name the child that actually holds the focus.  Following the
     * tab selection means it is re-pointed once per tab switch instead of on
     * every focus change, which is the only time it can really move.
     */
    win32_kbd_set_window(wcon->hwnd);

    win32_frame_fit();
    SetFocus(wcon->hwnd);
    InvalidateRect(wcon->hwnd, NULL, FALSE);
    win32_update_caption();
}

void win32_frame_add_console(struct win32_console *wcon)
{
    win32_tabs_rebuild();
    if (!win32_active) {
        win32_frame_activate(wcon);
    } else {
        win32_frame_layout();
    }
}

void win32_frame_del_console(struct win32_console *wcon)
{
    int i;

    if (win32_active == wcon) {
        win32_active = NULL;
        win32_kbd_set_window(NULL);
        for (i = 0; i < win32_num_outputs; i++) {
            if (win32_consoles[i].hwnd) {
                win32_active = &win32_consoles[i];
                break;
            }
        }
    }

    win32_tabs_rebuild();
    if (win32_active) {
        win32_frame_activate(win32_active);
    } else {
        win32_frame_layout();
        win32_update_caption();
    }
}

static void win32_tab_selected(void)
{
    int sel = TabCtrl_GetCurSel(win32_tabctl);
    int i;

    for (i = 0; i < win32_num_outputs; i++) {
        if (win32_consoles[i].hwnd && win32_consoles[i].tab == sel) {
            win32_frame_activate(&win32_consoles[i]);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* frame window procedure                                               */

static LRESULT CALLBACK win32_frameproc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_SIZE:
        win32_frame_layout();
        if (win32_active) {
            win32_console_redraw(win32_active);
        }
        return 0;

    case WM_MOVE:
        if (gui_grab) {
            win32_clip_cursor(true);
        }
        return 0;

    case WM_GETMINMAXINFO:
        win32_frame_minmax((MINMAXINFO *)lparam);
        return 0;

    case WM_NOTIFY:
        if (((NMHDR *)lparam)->code == TCN_SELCHANGE) {
            win32_tab_selected();
            return 0;
        }
        break;

    case WM_COMMAND:
        if (HIWORD(wparam) == 0 || HIWORD(wparam) == 1) {
            win32_menu_command(LOWORD(wparam));
            return 0;
        }
        break;

    case WM_INITMENUPOPUP:
        win32_refresh_menu((HMENU)wparam);
        return 0;

    case WM_SETFOCUS:
        if (win32_active && win32_active->hwnd) {
            SetFocus(win32_active->hwnd);
        }
        return 0;

    case WM_ACTIVATE:
        /*
         * With a single top-level window, deactivation unambiguously means
         * the user has left QEMU: drop the grab and lift every key, which
         * also covers Ctrl-Alt-Del since that cannot be hooked.
         */
        if (LOWORD(wparam) == WA_INACTIVE) {
            int i;
            for (i = 0; i < win32_num_outputs; i++) {
                win32_release_modifiers(&win32_consoles[i]);
            }
            if (gui_grab) {
                win32_grab_end();
            }
        }
        return 0;

    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lparam;

        /* the new factor first, so anything sized below uses it */
        win32_set_dpi(HIWORD(wparam));

        /*
         * Windows' suggested rectangle puts the frame on the new monitor at
         * the right place, but it only rescales the window as a whole.  The
         * content size is ours, so re-fit it: without this, dragging between
         * a 96 and a 144 DPI monitor would keep the guest image at the old
         * physical size.  win32_frame_fit() resizes with SWP_NOMOVE, so the
         * position chosen here survives; when the user is free-scaling or
         * fullscreen it only re-lays out, which is what we want.
         */
        SetWindowPos(hwnd, NULL, r->left, r->top,
                     r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        win32_frame_fit();
        return 0;
    }

    case WM_CLOSE:
        if (win32_opts->has_window_close && !win32_opts->window_close) {
            return 0;
        }
        shutdown_action = SHUTDOWN_ACTION_POWEROFF;
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        return 0;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

/* ------------------------------------------------------------------ */
/* setup and teardown                                                   */

void win32_frame_init(void)
{
    INITCOMMONCONTROLSEX icc = {
        .dwSize = sizeof(icc),
        .dwICC  = ICC_TAB_CLASSES,
    };
    WNDCLASSEX wc = {
        .cbSize        = sizeof(wc),
        .style         = 0,
        .lpfnWndProc   = win32_frameproc,
        .hInstance     = GetModuleHandle(NULL),
        .hIcon         = LoadIcon(GetModuleHandle(NULL), "QEMU_ICON"),
        .hCursor       = LoadCursor(NULL, IDC_ARROW),
        .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1),
        .lpszClassName = WIN32_FRAME_CLASS,
    };

    win32_set_dpi_awareness();

    /*
     * The application manifest in version.rc asks for common controls v6;
     * without both that and this call the tab control either fails to create
     * or comes up in the unthemed Windows-95 styling.
     */
    if (!InitCommonControlsEx(&icc)) {
        error_report("win32: could not initialise the common controls");
        exit(1);
    }

    win32_frame_atom = RegisterClassEx(&wc);
    if (!win32_frame_atom) {
        error_report("win32: could not register the frame window class "
                     "(error %lu)", GetLastError());
        exit(1);
    }

    win32_menu = win32_build_menu();

    win32_frame = CreateWindowEx(0, WIN32_FRAME_CLASS, QEMU_UI_NAME,
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                 NULL, win32_menu, GetModuleHandle(NULL),
                                 NULL);
    if (!win32_frame) {
        error_report("win32: could not create the frame window (error %lu)",
                     GetLastError());
        exit(1);
    }

    win32_tabctl = CreateWindowEx(0, WC_TABCONTROL, NULL,
                                  WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                                  0, 0, 0, 0, win32_frame,
                                  (HMENU)(UINT_PTR)WIN32_TABCTL_ID,
                                  GetModuleHandle(NULL), NULL);
    if (!win32_tabctl) {
        error_report("win32: could not create the tab control (error %lu)",
                     GetLastError());
        exit(1);
    }

    /*
     * The tab control draws its labels with a stock bitmap font unless it is
     * told otherwise, which at any non-default DPI looks nothing like the
     * rest of the window chrome.  This also establishes the DPI factor the
     * guest image is sized with, now that there is a window to ask about.
     */
    win32_set_dpi(win32_query_dpi(win32_frame));

    win32_tabctl_oldproc = (WNDPROC)(void *)SetWindowLongPtr(
        win32_tabctl, GWLP_WNDPROC, (LONG_PTR)win32_tabproc);

    ShowWindow(win32_frame, SW_SHOW);
    win32_frame_layout();
}

void win32_frame_fini(void)
{
    if (win32_tabctl && win32_tabctl_oldproc) {
        SetWindowLongPtr(win32_tabctl, GWLP_WNDPROC,
                         (LONG_PTR)win32_tabctl_oldproc);
        win32_tabctl_oldproc = NULL;
    }
    if (win32_frame) {
        SetMenu(win32_frame, NULL);
        DestroyWindow(win32_frame);
        win32_frame = NULL;
        win32_tabctl = NULL;
    }
    if (win32_tab_font) {
        DeleteObject(win32_tab_font);
        win32_tab_font = NULL;
    }
    if (win32_menu) {
        DestroyMenu(win32_menu);
        win32_menu = NULL;
    }
    if (win32_frame_atom) {
        UnregisterClass(WIN32_FRAME_CLASS, GetModuleHandle(NULL));
        win32_frame_atom = 0;
    }
}
