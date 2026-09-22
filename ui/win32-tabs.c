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
#include <shellapi.h>

#include "qapi/error.h"
#include "qapi/qapi-commands-control.h"
#include "qapi/qapi-commands-machine.h"
#include "qapi/qapi-commands-misc.h"
#include "qemu-version.h"
#include "qemu/accel.h"
#include "qemu/error-report.h"
#include "qemu/help-texts.h"
#include "qemu/target-info.h"
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

/*
 * version.rc declares the icon as "IDI_ICON1", and windres turns an
 * identifier that is not a number into a resource *name*, so that string is
 * what LoadIcon() has to be given.
 */
#define WIN32_ICON_NAME "IDI_ICON1"

static HICON win32_app_icon(void)
{
    HICON icon = LoadIcon(GetModuleHandle(NULL), WIN32_ICON_NAME);

    return icon ? icon : LoadIcon(NULL, IDI_APPLICATION);
}
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

    IDM_ABOUT,
};

#define WIN32_TABCTL_ID 1

HWND win32_frame;
HWND win32_tabctl;
struct win32_console *win32_active;

/*
 * True once the user has picked a tab, by clicking one or by pressing
 * Ctrl-Alt-<n>.  Until then the selection is only this code's best guess,
 * and win32_frame_add_console() is free to revise it -- see there.
 */
static bool win32_tab_user_selected;

/*
 * Set while win32_tabs_rebuild() is deleting and re-inserting the tab
 * items.  comctl32 changes the current selection as it does so, and sends
 * TCN_SELCHANGE for it -- which is not a choice by the user and must not be
 * mistaken for one, or adding a second tab silently switches to it.
 */
static bool win32_tabs_rebuilding;

bool gui_fullscreen;
bool gui_free_scale;
bool gui_grab_on_hover;
bool gui_show_tabs = true;

static HMENU win32_menu;
static ATOM win32_frame_atom;
static ATOM win32_about_atom;
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
    /*
     * The terminal tabs size their font in points too, so they have to
     * re-measure; their whole character grid follows from the cell size.
     */
    win32_term_consoles_set_dpi(win32_dpi);
}

double win32_dpi_scale(void)
{
    return (double)win32_dpi / USER_DEFAULT_SCREEN_DPI;
}

/* ------------------------------------------------------------------ */
/* about dialog                                                         */

/*
 * The box is a hand-built top-level window rather than a DialogBox() over a
 * resource template, for two reasons.  DialogBox() runs its own modal
 * message loop, and this backend's pump is win32_poll_events(), called from
 * dpy_refresh on the main thread -- a nested loop would stop the guest's
 * display updating for as long as the box was open.  And the contents are
 * almost entirely computed (version, target, build options), so a template
 * would describe little more than an empty frame for the code to fill in.
 * The rest of this backend builds its windows by hand too.
 *
 * Modality is done the way a modeless dialog does it: the frame is disabled
 * while the box is up and re-enabled when it goes away, so it cannot be
 * driven from behind, while the ordinary pump keeps running.
 * win32_dialog_filter() hands messages to IsDialogMessage(), which works on
 * any window and is what gives Tab navigation, Esc to close and Enter to
 * press the default button.
 *
 * Nothing here touches the grab or the keyboard hook.  Showing the box
 * deactivates the frame, whose WM_ACTIVATE handler already ends the grab and
 * lifts every held key, and the low-level hook only forwards keys while the
 * window it was given holds the focus -- which it does not while the box
 * has it.
 */

#define WIN32_ABOUT_CLASS  "QemuWin32About"
#define WIN32_FORK_URL     "https://github.com/cyberdotgent/qemu-mod"
#define WIN32_UPSTREAM_URL "https://gitlab.com/qemu-project/qemu"

enum {
    IDC_ABOUT_FORK = 0x200,
    IDC_ABOUT_UPSTREAM,
    IDC_ABOUT_DETAILS,
};

static HWND win32_about;
static HFONT win32_about_font;
static HFONT win32_about_title_font;
static HFONT win32_about_mono_font;

static void win32_about_opt(GString *s, const char *name, bool present)
{
    g_string_append_printf(s, "  %-22s %s\r\n", name, present ? "yes" : "no");
}

/*
 * What this binary can do is decided in two different places, so it is read
 * from two different places.  The target and the accelerators belong to
 * *this* emulator and are only knowable at run time: ui/ is built once as
 * common code (system_ss in ui/meson.build), so TARGET_NAME and CONFIG_WHPX
 * are not merely unset here, they are poisoned.  Everything else is a
 * host-wide build decision recorded in config-host.h, which common code may
 * read.
 */
static char *win32_about_details(void)
{
    GString *s = g_string_new(NULL);
    GSList *el, *accels;
    bool first = true;

    g_string_append_printf(s, "Emulated target       %s (%u-bit)\r\n",
                           target_name(), target_long_bits());

    g_string_append(s, "Accelerators built in ");
    accels = object_class_get_list(TYPE_ACCEL, false);
    for (el = accels; el; el = el->next) {
        const char *type = object_class_get_name(OBJECT_CLASS(el->data));
        g_autofree char *name = NULL;

        /* qtest exists for the test suite, not for users */
        if (!g_str_has_suffix(type, ACCEL_CLASS_SUFFIX) ||
            g_str_equal(type, ACCEL_CLASS_NAME("qtest"))) {
            continue;
        }
        name = g_strndup(type, strlen(type) - strlen(ACCEL_CLASS_SUFFIX));
        g_string_append_printf(s, "%s%s", first ? "" : ", ", name);
        first = false;
    }
    g_slist_free(accels);
    g_string_append(s, first ? "none\r\n" : "\r\n");

    if (current_accel()) {
        g_string_append_printf(s, "Accelerator in use    %s\r\n",
                               current_accel_name());
    }

    g_string_append(s, "\r\nUser interface\r\n");
#ifdef CONFIG_WIN32_UI
    win32_about_opt(s, "native Win32 (this)", true);
#else
    win32_about_opt(s, "native Win32 (this)", false);
#endif
#ifdef CONFIG_SDL
    win32_about_opt(s, "SDL", true);
#else
    win32_about_opt(s, "SDL", false);
#endif
#ifdef CONFIG_GTK
    win32_about_opt(s, "GTK", true);
#else
    win32_about_opt(s, "GTK", false);
#endif
#ifdef CONFIG_CURSES
    win32_about_opt(s, "curses", true);
#else
    win32_about_opt(s, "curses", false);
#endif

    g_string_append(s, "\r\nRemote display\r\n");
#ifdef CONFIG_VNC
    g_string_append_printf(s, "  %-22s yes (JPEG: %s, SASL: %s)\r\n", "VNC",
#ifdef CONFIG_VNC_JPEG
                           "yes",
#else
                           "no",
#endif
#ifdef CONFIG_VNC_SASL
                           "yes");
#else
                           "no");
#endif
#else
    win32_about_opt(s, "VNC", false);
#endif
#ifdef CONFIG_SPICE
    win32_about_opt(s, "SPICE", true);
#else
    win32_about_opt(s, "SPICE", false);
#endif
#ifdef CONFIG_DBUS_DISPLAY
    win32_about_opt(s, "D-Bus display", true);
#else
    win32_about_opt(s, "D-Bus display", false);
#endif

    g_string_append(s, "\r\nGraphics\r\n");
#ifdef CONFIG_OPENGL
    win32_about_opt(s, "OpenGL (EGL/ANGLE)", true);
#else
    win32_about_opt(s, "OpenGL (EGL/ANGLE)", false);
#endif
#ifdef VIRGL_VERSION_MAJOR
    g_string_append_printf(s, "  %-22s %d.%d.%d\r\n", "virglrenderer",
                           VIRGL_VERSION_MAJOR, VIRGL_VERSION_MINOR,
                           VIRGL_VERSION_MICRO);
#else
    win32_about_opt(s, "virglrenderer", false);
#endif
#ifdef CONFIG_PIXMAN
    g_string_append_printf(s, "  %-22s %s\r\n", "pixman",
                           PIXMAN_VERSION_STRING);
#else
    win32_about_opt(s, "pixman", false);
#endif
#ifdef CONFIG_PNG
    win32_about_opt(s, "PNG", true);
#else
    win32_about_opt(s, "PNG", false);
#endif

    g_string_append(s, "\r\nOther build options\r\n");
#ifdef CONFIG_TCG
    win32_about_opt(s, "TCG", true);
#else
    win32_about_opt(s, "TCG", false);
#endif
#ifdef CONFIG_SLIRP
    win32_about_opt(s, "user-mode net (slirp)", true);
#else
    win32_about_opt(s, "user-mode net (slirp)", false);
#endif
#ifdef CONFIG_FDT
    win32_about_opt(s, "device tree (FDT)", true);
#else
    win32_about_opt(s, "device tree (FDT)", false);
#endif
#ifdef CONFIG_ZSTD
    win32_about_opt(s, "zstd", true);
#else
    win32_about_opt(s, "zstd", false);
#endif
#ifdef CONFIG_GNUTLS
    win32_about_opt(s, "TLS (gnutls)", true);
#else
    win32_about_opt(s, "TLS (gnutls)", false);
#endif
#ifdef CONFIG_CAPSTONE
    win32_about_opt(s, "disassembly (capstone)", true);
#else
    win32_about_opt(s, "disassembly (capstone)", false);
#endif
    g_string_append_printf(s, "  %-22s %d.%d.%d\r\n", "glib",
                           GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION,
                           GLIB_MICRO_VERSION);

    return g_string_free(s, FALSE);
}

/*
 * Dismissal has to re-enable the frame *before* the box is destroyed.  A
 * disabled window is not a candidate for activation, so tearing the box down
 * first leaves the process with no active window at all -- re-enabling the
 * frame afterwards does not bring the activation back, and the frame is left
 * looking alive while ignoring every click.
 */
static void win32_about_close(void)
{
    HWND about = win32_about;

    if (!about) {
        return;
    }
    win32_about = NULL;
    if (win32_frame) {
        EnableWindow(win32_frame, TRUE);
        SetActiveWindow(win32_frame);
    }
    DestroyWindow(about);
}

static LRESULT CALLBACK win32_aboutproc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDOK:
        case IDCANCEL:
            win32_about_close();
            return 0;
        default:
            break;
        }
        break;

    case WM_NOTIFY: {
        const NMHDR *hdr = (const NMHDR *)lparam;

        if (hdr->code == NM_CLICK || hdr->code == NM_RETURN) {
            const char *url = NULL;

            if (hdr->idFrom == IDC_ABOUT_FORK) {
                url = WIN32_FORK_URL;
            } else if (hdr->idFrom == IDC_ABOUT_UPSTREAM) {
                url = WIN32_UPSTREAM_URL;
            }
            if (url) {
                ShellExecute(hwnd, "open", url, NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }
        }
        break;
    }

    case DM_GETDEFID:
        /*
         * IsDialogMessage() asks the window which button is the default one
         * before it turns Enter into a command; unanswered, Enter does
         * nothing.
         */
        return MAKELONG(IDOK, DC_HASDEFID);

    case WM_CLOSE:
        win32_about_close();
        return 0;

    case WM_DESTROY:
        /*
         * Also reached when the frame is destroyed with the box still up, so
         * the enable is repeated here rather than only in win32_about_close().
         */
        win32_about = NULL;
        if (win32_frame && IsWindow(win32_frame)) {
            EnableWindow(win32_frame, TRUE);
        }
        if (win32_about_font) {
            DeleteObject(win32_about_font);
            win32_about_font = NULL;
        }
        if (win32_about_title_font) {
            DeleteObject(win32_about_title_font);
            win32_about_title_font = NULL;
        }
        /* a stock font is never stored here, so this only frees our own */
        if (win32_about_mono_font) {
            DeleteObject(win32_about_mono_font);
            win32_about_mono_font = NULL;
        }
        return 0;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static HWND win32_about_control(const char *cls, const char *text,
                                DWORD style, int x, int y, int w, int h,
                                int id, HFONT font)
{
    HWND ctl = CreateWindowEx(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                              x, y, w, h, win32_about,
                              (HMENU)(UINT_PTR)id, GetModuleHandle(NULL),
                              NULL);

    if (ctl) {
        SendMessage(ctl, WM_SETFONT, (WPARAM)font, TRUE);
    }
    return ctl;
}

/*
 * The details are laid out in columns padded with spaces, so they only line
 * up in a fixed-pitch face.  CreateFont() never fails over a face name the
 * system does not have -- it substitutes silently, and the substitute can be
 * proportional -- so each candidate is created, measured and kept only if
 * what came back really is the face that was asked for and really is fixed
 * pitch.  (TMPF_FIXED_PITCH is set for *variable* pitch fonts; the name is a
 * long-standing wart in the Win32 API.)  NULL means the caller should fall
 * back to a stock fixed font, which is always there.
 */
static HFONT win32_about_mono(int dpi)
{
    static const char *const faces[] = {
        "Consolas",         /* Vista and later */
        "Lucida Console",   /* NT 4 and later */
        "Courier New",
    };
    HDC hdc = GetDC(NULL);
    size_t i;

    if (!hdc) {
        return NULL;
    }

    for (i = 0; i < ARRAY_SIZE(faces); i++) {
        LOGFONT lf = {
            .lfHeight         = -MulDiv(9, dpi, 72),
            .lfCharSet        = DEFAULT_CHARSET,
            .lfOutPrecision   = OUT_TT_PRECIS,
            .lfQuality        = CLEARTYPE_QUALITY,
            .lfPitchAndFamily = FIXED_PITCH | FF_MODERN,
        };
        char face[LF_FACESIZE] = "";
        TEXTMETRIC tm;
        HFONT font, old;

        g_strlcpy(lf.lfFaceName, faces[i], sizeof(lf.lfFaceName));
        font = CreateFontIndirect(&lf);
        if (!font) {
            continue;
        }

        old = SelectObject(hdc, font);
        GetTextFace(hdc, sizeof(face), face);
        GetTextMetrics(hdc, &tm);
        SelectObject(hdc, old);

        if (!g_ascii_strcasecmp(face, faces[i]) &&
            !(tm.tmPitchAndFamily & TMPF_FIXED_PITCH)) {
            ReleaseDC(NULL, hdc);
            return font;
        }
        DeleteObject(font);
    }

    ReleaseDC(NULL, hdc);
    return NULL;
}

static void win32_about_fonts(int dpi)
{
    NONCLIENTMETRICS ncm = { .cbSize = sizeof(ncm) };
    LOGFONT lf;

    if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        lf = ncm.lfMessageFont;
    } else {
        GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    }

    lf.lfHeight = -MulDiv(9, dpi, 72);
    win32_about_font = CreateFontIndirect(&lf);

    lf.lfHeight = -MulDiv(14, dpi, 72);
    lf.lfWeight = FW_BOLD;
    win32_about_title_font = CreateFontIndirect(&lf);

    win32_about_mono_font = win32_about_mono(dpi);
}

/* the font the details box is drawn in, never a proportional one */
static HFONT win32_about_details_font(void)
{
    if (win32_about_mono_font) {
        return win32_about_mono_font;
    }
    return (HFONT)GetStockObject(ANSI_FIXED_FONT);
}

static void win32_about_show(void)
{
    const int base = USER_DEFAULT_SCREEN_DPI;
    int dpi;
    g_autofree char *details = NULL;
    g_autofree char *version = NULL;
    g_autofree char *fork_link = NULL;
    g_autofree char *upstream_link = NULL;
    HWND ctl;
    RECT r;
    int y, cw, ch, pad, hline;

    if (win32_about) {
        SetForegroundWindow(win32_about);
        return;
    }

    /*
     * The frame's current DPI, kept up to date by WM_DPICHANGED.  The box is
     * owned by the frame and so comes up on the same monitor; everything
     * below is written for 96 DPI and scaled from there, because the process
     * is per-monitor-DPI-aware and Windows therefore scales nothing it does
     * not draw itself.
     */
    dpi = win32_dpi;

#define SC(v) MulDiv((v), dpi, base)

    pad = SC(14);
    hline = SC(18);
    cw = SC(470);
    ch = SC(530);

    win32_about = CreateWindowEx(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                 WIN32_ABOUT_CLASS, "About " QEMU_UI_NAME,
                                 WS_POPUPWINDOW | WS_CAPTION | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, cw, ch,
                                 win32_frame, NULL, GetModuleHandle(NULL),
                                 NULL);
    if (!win32_about) {
        error_report("win32: could not create the about window (error %lu)",
                     GetLastError());
        return;
    }

    /* grow the window so the *client* area is the cw x ch laid out below */
    GetClientRect(win32_about, &r);
    SetWindowPos(win32_about, NULL, 0, 0,
                 cw + cw - (r.right - r.left),
                 ch + ch - (r.bottom - r.top),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    win32_about_fonts(dpi);

    y = pad;
    ctl = win32_about_control("Static", NULL, SS_ICON | SS_REALSIZECONTROL,
                              pad, y, SC(32), SC(32), -1, win32_about_font);
    if (ctl) {
        SendMessage(ctl, STM_SETICON, (WPARAM)win32_app_icon(), 0);
    }

    win32_about_control("Static", QEMU_UI_NAME, SS_LEFT,
                        pad + SC(44), y, cw - pad * 2 - SC(44), SC(24), -1,
                        win32_about_title_font);
    version = g_strdup_printf("Version %s", QEMU_FULL_VERSION);
    win32_about_control("Static", version, SS_LEFT | SS_ENDELLIPSIS,
                        pad + SC(44), y + SC(24),
                        cw - pad * 2 - SC(44), hline, -1, win32_about_font);

    y += SC(50);
    win32_about_control("Static", QEMU_COPYRIGHT, SS_LEFT,
                        pad, y, cw - pad * 2, hline * 2, -1,
                        win32_about_font);
    y += hline * 2 + SC(4);
    win32_about_control("Static",
                        "QEMU is a trademark of Fabrice Bellard.  "
                        QEMU_UI_NAME " is an unofficial derivative of QEMU "
                        "and is not endorsed by the QEMU project.",
                        SS_LEFT, pad, y, cw - pad * 2, hline * 2, -1,
                        win32_about_font);

    y += hline * 2 + SC(8);
    fork_link = g_strdup_printf("This fork's sources: <a href=\"%s\">%s</a>",
                                WIN32_FORK_URL, WIN32_FORK_URL);
    win32_about_control("SysLink", fork_link, WS_TABSTOP,
                        pad, y, cw - pad * 2, hline,
                        IDC_ABOUT_FORK, win32_about_font);
    y += hline + SC(2);
    upstream_link = g_strdup_printf(
        "Upstream QEMU sources: <a href=\"%s\">%s</a>",
        WIN32_UPSTREAM_URL, WIN32_UPSTREAM_URL);
    win32_about_control("SysLink", upstream_link, WS_TABSTOP,
                        pad, y, cw - pad * 2, hline,
                        IDC_ABOUT_UPSTREAM, win32_about_font);

    y += hline + SC(10);
    win32_about_control("Static", "This binary:", SS_LEFT,
                        pad, y, cw - pad * 2, hline, -1, win32_about_font);
    y += hline + SC(2);

    /*
     * A read-only multi-line edit rather than a static: the point of the
     * details is that they can be selected and pasted into a bug report.
     */
    details = win32_about_details();
    win32_about_control("Edit", details,
                        WS_BORDER | WS_VSCROLL | WS_TABSTOP | ES_LEFT |
                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                        pad, y, cw - pad * 2, ch - y - SC(30) - pad * 2,
                        IDC_ABOUT_DETAILS, win32_about_details_font());

    ctl = win32_about_control("Button", "Close",
                              WS_TABSTOP | BS_DEFPUSHBUTTON,
                              cw - pad - SC(90), ch - pad - SC(26),
                              SC(90), SC(26), IDOK, win32_about_font);

    EnableWindow(win32_frame, FALSE);
    ShowWindow(win32_about, SW_SHOW);
    SetFocus(ctl);

#undef SC
}

/*
 * Called from the backend's message pump.  IsDialogMessage() is what turns a
 * plain window into one with dialog keyboard behaviour, and it has to see
 * the messages before TranslateMessage()/DispatchMessage() do.
 */
bool win32_dialog_filter(MSG *msg)
{
    return win32_about && IsDialogMessage(win32_about, msg);
}

/* ------------------------------------------------------------------ */
/* menu bar                                                             */

static HMENU win32_build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU machine = CreatePopupMenu();
    HMENU view = CreatePopupMenu();
    HMENU help = CreatePopupMenu();

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

    AppendMenu(help, MF_STRING, IDM_ABOUT, "About...");

    AppendMenu(bar, MF_POPUP, (UINT_PTR)machine, "Machine");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)view, "View");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)help, "Help");

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
    bool graphic = win32_active && !win32_console_is_term(win32_active) &&
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

    /*
     * Zoom stretches a pixel buffer, and a terminal tab has none -- its
     * size is a character grid, set by the font.  Grey the items rather
     * than leave them looking as though they do nothing.
     */
    for (int i = 0; i < 4; i++) {
        static const int zoom_items[] = {
            IDM_ZOOM_IN, IDM_ZOOM_OUT, IDM_ZOOM_FIXED, IDM_ZOOM_FIT
        };
        EnableMenuItem(menu, zoom_items[i],
                       MF_BYCOMMAND |
                       (win32_console_is_term(win32_active) ? MF_GRAYED
                                                            : MF_ENABLED));
    }
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

    case IDM_ABOUT:
        win32_about_show();
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
    RECT work;

    if (win32_tabs_visible()) {
        TabCtrl_AdjustRect(win32_tabctl, TRUE, &r);
    }
    AdjustWindowRectEx(&r, style, GetMenu(win32_frame) != NULL, ex);
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;

    /*
     * Never ask for a window bigger than the desktop.  A wide terminal --
     * the monitor comes up 132x43 -- can easily want more than a small
     * screen has, and a window that opens with its edges past the taskbar
     * cannot be resized back by the user.  Whatever is clipped off here
     * simply becomes a smaller character grid, which the terminal notices
     * on WM_SIZE.
     */
    if (SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0)) {
        int maxw = work.right - work.left;
        int maxh = work.bottom - work.top;

        if (maxw > 0 && *ow > maxw) {
            *ow = maxw;
        }
        if (maxh > 0 && *oh > maxh) {
            *oh = maxh;
        }
    }
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

    if (win32_console_is_term(win32_active)) {
        label = g_strdup(win32_term_console_label(win32_active));
    } else if (win32_active) {
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

    win32_tabs_rebuilding = true;
    TabCtrl_DeleteAllItems(win32_tabctl);

    for (i = 0; win32_consoles && i < win32_num_outputs; i++) {
        struct win32_console *wcon = &win32_consoles[i];
        g_autofree char *label = NULL;
        TCITEM item = { .mask = TCIF_TEXT };

        if (!wcon->hwnd) {
            wcon->tab = -1;
            continue;
        }

        if (win32_console_is_term(wcon)) {
            label = g_strdup(win32_term_console_label(wcon));
        } else {
            label = qemu_console_get_label(wcon->dcl.con);
        }
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
    win32_tabs_rebuilding = false;
}

/*
 * A terminal tab's label follows the guest's window title (OSC 0/2), so it
 * can change at any moment; ui/win32-term-chardev.c calls this when it
 * does.
 */
void win32_frame_relabel(void)
{
    win32_tabs_rebuild();
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
    /*
     * The low-level keyboard hook steals system key combinations for the
     * guest.  A terminal tab is not the guest: Alt-Tab and the Windows key
     * must keep working there, so the hook is pointed at nothing.
     */
    win32_kbd_set_window(win32_console_is_term(wcon) ? NULL : wcon->hwnd);

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
        return;
    }

    /*
     * Tabs do not all arrive at once, and they do not arrive in the order a
     * user would expect to see them in.  Chardevs are created early in
     * startup, so a serial port or the monitor is registered long before
     * any graphics QemuConsole exists -- which would leave QEMU opening on
     * the monitor tab rather than on the machine's display.
     *
     * So while no tab has been chosen by the user, treat the current
     * selection as provisional and let the first graphics console displace
     * a terminal.  A configuration with no graphics console at all keeps
     * whatever it has, and once the user has picked a tab nothing moves it.
     */
    if (!win32_tab_user_selected &&
        win32_console_is_term(win32_active) && !win32_console_is_term(wcon)) {
        win32_frame_activate(wcon);
        return;
    }

    win32_frame_layout();
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

void win32_frame_note_user_selection(void)
{
    win32_tab_user_selected = true;
}

static void win32_tab_selected(void)
{
    int sel = TabCtrl_GetCurSel(win32_tabctl);
    int i;

    if (win32_tabs_rebuilding) {
        return;
    }

    for (i = 0; i < win32_num_outputs; i++) {
        if (win32_consoles[i].hwnd && win32_consoles[i].tab == sel) {
            win32_frame_note_user_selection();
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
                /*
                 * Only a graphics tab has keys the guest believes are
                 * held; a terminal tab has no QKbdState at all.
                 */
                if (win32_console_is_term(&win32_consoles[i])) {
                    continue;
                }
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
        .dwICC  = ICC_TAB_CLASSES | ICC_LINK_CLASS,
    };
    WNDCLASSEX wc = {
        .cbSize        = sizeof(wc),
        .style         = 0,
        .lpfnWndProc   = win32_frameproc,
        .hInstance     = GetModuleHandle(NULL),
        .hIcon         = win32_app_icon(),
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

    wc.lpfnWndProc   = win32_aboutproc;
    wc.lpszClassName = WIN32_ABOUT_CLASS;
    win32_about_atom = RegisterClassEx(&wc);
    if (!win32_about_atom) {
        error_report("win32: could not register the about window class "
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
    /* owned by the frame, so tear it down before its owner goes away */
    win32_about_close();
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
    if (win32_about_atom) {
        UnregisterClass(WIN32_ABOUT_CLASS, GetModuleHandle(NULL));
        win32_about_atom = 0;
    }
}
