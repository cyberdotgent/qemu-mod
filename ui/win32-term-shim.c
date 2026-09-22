/*
 * QEMU-mod: the environment PuTTY's terminal emulator expects
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original code.  The PuTTY files under ui/putty/ are vendored unmodified
 * and are MIT; this file is not part of PuTTY.
 *
 * terminal.c is written against a fair slice of the rest of PuTTY: a
 * session configuration, a local line discipline, a session log, a printer,
 * a toplevel-callback queue and a timer wheel.  Only three of those are
 * vendored as-is (callback.c, timing.c and the Conf container itself); the
 * rest are replaced here, because the real implementations drag in the
 * registry, the filesystem and PuTTY's own main loop, none of which belong
 * in a QEMU display backend.
 *
 * What is faithful and what is not:
 *
 *   Conf       fully faithful.  We use PuTTY's own conf.c/conf_data.c, and
 *              seed it from the DEFAULT_* metadata in conf.h, so every
 *              setting has exactly the value a freshly installed PuTTY
 *              would give it.  A short, documented list of overrides then
 *              tailors it for a serial console.
 *   Ldisc      deliberately not faithful.  PuTTY's ldisc does local line
 *              editing and local echo; a QEMU serial port or HMP monitor
 *              wants raw bytes.  Ours forwards and does nothing else.
 *   logging    stubbed.  Session logging to a file is a PuTTY feature we do
 *              not expose; CONF_logtype is pinned to LGTYP_NONE so the
 *              terminal never calls these except through paths that check
 *              it first.
 *   printer    stubbed.  The VT100 "print screen" escapes are accepted and
 *              discarded rather than being routed to a Windows printer.
 *   timers     PuTTY's timing.c, vendored.  timer_change_notify() is ours;
 *              it just records the deadline, and win32_term_pump() runs the
 *              wheel from the frame's WM_TIMER.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "win32-term-internal.h"

const char *const appname = "QEMU-mod";
const char commitid[] = "qemu-mod";

/*
 * ----------------------------------------------------------------
 * Fatal errors.
 *
 * PuTTY calls this from places where it has detected an internal
 * inconsistency it cannot continue past.  There is no useful way to carry
 * on, and we cannot call QEMU's error path from this side of the boundary,
 * so put up a message box and die the way PuTTY itself would.
 */
NORETURN void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *text;

    va_start(ap, fmt);
    text = dupvprintf(fmt, ap);
    va_end(ap);

    MessageBoxA(NULL, text, "QEMU-mod terminal: fatal error",
                MB_ICONERROR | MB_OK);
    exit(1);
}

/*
 * ----------------------------------------------------------------
 * Session logging: not offered.
 */
void logtraffic(LogContext *logctx, unsigned char c, int logmode)
{
}

void logflush(LogContext *logctx)
{
}

/*
 * ----------------------------------------------------------------
 * Printing: the VT100 printer escapes are swallowed.
 */
printer_job *printer_start_job(char *printer)
{
    return NULL;
}

void printer_job_data(printer_job *pj, const void *data, size_t len)
{
}

void printer_finish_job(printer_job *pj)
{
}

/*
 * ----------------------------------------------------------------
 * Line discipline.
 */
Ldisc *win32_term_ldisc_new(Terminal *term,
                            void (*send)(void *opaque,
                                         const char *buf, int len),
                            void *opaque)
{
    Ldisc *ldisc = snew(Ldisc);

    ldisc->term = term;
    ldisc->send = send;
    ldisc->opaque = opaque;
    return ldisc;
}

void win32_term_ldisc_free(Ldisc *ldisc)
{
    sfree(ldisc);
}

void ldisc_send(Ldisc *ldisc, const void *buf, int len, bool interactive)
{
    /*
     * PuTTY uses len < 0 to mean "this is a NUL-terminated string of
     * special significance" -- specifically, that the line discipline
     * should treat it as a line of input rather than raw keystrokes.  We
     * have no line discipline, so the two are the same thing.
     */
    if (len < 0) {
        len = strlen(buf);
    }
    if (len > 0 && ldisc->send) {
        ldisc->send(ldisc->opaque, buf, len);
    }
}

void ldisc_echoedit_update(Ldisc *ldisc)
{
    /*
     * Tells the ldisc that the terminal's echo/edit modes changed.  Ours
     * has no modes.
     */
}

void ldisc_provide_userpass_le(Ldisc *ldisc, TermLineEditor *le)
{
    /*
     * Only used when the terminal is presenting an SSH password prompt on
     * the backend's behalf, which cannot happen here.
     */
}

/*
 * ----------------------------------------------------------------
 * Timers.
 *
 * PuTTY's timing.c keeps the wheel; the front end only has to notice when
 * the earliest deadline moves.  We have no idle loop of our own, so all we
 * do is remember it for win32_term_pump().
 */
static bool timer_pending;
static unsigned long timer_next;

void timer_change_notify(unsigned long next)
{
    timer_pending = true;
    timer_next = next;
}

/*
 * How many queued callbacks one pump may run before giving the caller its
 * thread back.  A callback is free to queue another -- term_out_cb() does
 * exactly that when there is more output than it wants to process in one
 * go -- so this loop needs a bound or a fast producer could hold the QEMU
 * main loop indefinitely.
 */
#define WIN32_TERM_CALLBACK_BUDGET 64

bool win32_term_pump(unsigned long *next)
{
    static bool running;
    unsigned long now;
    int budget = WIN32_TERM_CALLBACK_BUDGET;

    /*
     * A callback can reach back into us: a terminal query reply goes out
     * through the ldisc to the chardev, whose far end may write straight
     * back, and that write kicks the pump again.  PuTTY's queue is not
     * re-entrant, and it does not need to be -- the outer call will pick up
     * anything left behind.
     */
    if (running) {
        return false;
    }
    running = true;

    /*
     * run_toplevel_callbacks() runs exactly one callback and says whether
     * it found one.  Calling it once per pump, which is what this used to
     * do, left the rest of the queue stranded until something happened to
     * kick us again -- so a burst of terminal output was processed one
     * fragment per keystroke.
     */
    while (budget-- > 0 && run_toplevel_callbacks()) {
        /* nothing; the work is the callback */
    }

    now = GETTICKCOUNT();
    if (timer_pending && (long)(now - timer_next) >= 0) {
        /*
         * run_timers() wants to be told the current time and gives back
         * the next deadline, if any.  Comparing tick counts by subtraction
         * is deliberate: GetTickCount() wraps every 49 days.
         */
        unsigned long then;

        timer_pending = false;
        if (run_timers(now, &then)) {
            timer_pending = true;
            timer_next = then;
        }
    }

    running = false;

    /*
     * Work left over -- either the budget ran out or a timer is due later.
     * Either way the caller must arrange to come back, or the terminal
     * stops dead until the next keystroke.
     */
    if (toplevel_callback_pending()) {
        *next = GETTICKCOUNT();
        return true;
    }
    if (timer_pending) {
        *next = timer_next;
        return true;
    }
    return false;
}

/*
 * ----------------------------------------------------------------
 * Configuration.
 */

/*
 * PuTTY's default colour palette, from the Colour0..Colour21 defaults in
 * settings.c: foreground, bold foreground, background, bold background,
 * cursor text, cursor, then the sixteen ANSI colours in
 * normal/bold pairs.
 */
static const unsigned char default_colours[22][3] = {
    {187, 187, 187}, {255, 255, 255}, {  0,   0,   0}, { 85,  85,  85},
    {  0,   0,   0}, {  0, 255,   0}, {  0,   0,   0}, { 85,  85,  85},
    {187,   0,   0}, {255,  85,  85}, {  0, 187,   0}, { 85, 255,  85},
    {187, 187,   0}, {255, 255,  85}, {  0,   0, 187}, { 85,  85, 255},
    {187,   0, 187}, {255,  85, 255}, {  0, 187, 187}, { 85, 255, 255},
    {187, 187, 187}, {255, 255, 255},
};

/*
 * Character classes for double-click word selection, from the Wordness0..
 * Wordness224 defaults in settings.c.  0 is whitespace, 1 is punctuation,
 * 2 is a word character; a double-click extends over a run of one class.
 */
static const unsigned char default_wordness[256] = {
    /* 0x00 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x20 */
    0, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    /* 0x40 */
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2,
    /* 0x60 */
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
    /* 0x80 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 0xa0 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 0xc0 */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2,
    /* 0xe0 */
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 2, 2,
};

Conf *win32_term_conf_new(int cols, int rows, const char *line_codepage)
{
    Conf *conf = conf_new();
    Filename *empty_filename;
    FontSpec *empty_fontspec;
    int key;

    /*
     * Seed every key that has no subkey from the DEFAULT_* metadata that
     * conf.h attaches to it, which is the same data settings.c would use
     * if it found nothing in the registry.  Keys that declare no default
     * get the zero of their type; conf.h gives no default only to keys
     * whose loading settings.c does by hand, none of which the terminal
     * reads.
     */
    empty_filename = filename_from_str("");
    empty_fontspec = fontspec_new("", false, 0, 0);

    for (key = 0; key < N_CONFIG_OPTIONS; key++) {
        const ConfKeyInfo *info = &conf_key_info[key];

        if (info->subkey_type != CONF_TYPE_NONE) {
            continue;
        }

        switch (info->value_type) {
        case CONF_TYPE_BOOL:
            conf_set_bool(conf, key, info->default_value.bval);
            break;
        case CONF_TYPE_INT:
            conf_set_int(conf, key, info->default_value.ival);
            break;
        case CONF_TYPE_STR:
        case CONF_TYPE_STR_AMBI:
            conf_set_str(conf, key,
                         info->default_value.sval ?
                         info->default_value.sval : "");
            break;
        case CONF_TYPE_UTF8:
            conf_set_utf8(conf, key,
                          info->default_value.sval ?
                          info->default_value.sval : "");
            break;
        case CONF_TYPE_FILENAME:
            conf_set_filename(conf, key, empty_filename);
            break;
        case CONF_TYPE_FONT:
            conf_set_fontspec(conf, key, empty_fontspec);
            break;
        default:
            break;
        }
    }

    filename_free(empty_filename);
    fontspec_free(empty_fontspec);

    /*
     * The two keys the terminal reads that do have subkeys, and therefore
     * no declared default: the palette and the word-selection classes.
     */
    for (int i = 0; i < 22; i++) {
        for (int j = 0; j < 3; j++) {
            conf_set_int_int(conf, CONF_colours, i * 3 + j,
                             default_colours[i][j]);
        }
    }
    for (int i = 0; i < 256; i++) {
        conf_set_int_int(conf, CONF_wordness, i, default_wordness[i]);
    }

    /*
     * Now the overrides.  Everything here is a deliberate departure from
     * what PuTTY ships with, chosen for "this is a serial console into a
     * Unix machine" rather than "this is an SSH client".
     */

    /* The whole point of the exercise: vi, less and top need these. */
    conf_set_bool(conf, CONF_no_alt_screen, false);
    conf_set_bool(conf, CONF_no_mouse_rep, false);
    conf_set_bool(conf, CONF_no_bracketed_paste, false);
    conf_set_bool(conf, CONF_no_dbackspace, false);

    /*
     * The terminal lives in a tab inside QEMU's frame window, so escape
     * sequences that would move, resize, retitle or restack a top-level
     * window have nowhere sensible to go.  Resizing is refused outright;
     * the title is accepted, because we use it as the tab label.
     */
    conf_set_bool(conf, CONF_no_remote_resize, true);
    conf_set_bool(conf, CONF_no_remote_wintitle, false);
    conf_set_int(conf, CONF_remote_qtitle_action, TITLE_NONE);

    /*
     * The terminal's own size.  PuTTY reads this once, at term_init(); the
     * window then resizes it to whatever actually fits.
     */
    if (cols > 0) {
        conf_set_int(conf, CONF_width, cols);
    }
    if (rows > 0) {
        conf_set_int(conf, CONF_height, rows);
    }

    /*
     * A Unicode terminal by default, with UTF-8 line drawing rather than
     * SCO ACS.  A caller that knows the far end speaks something else --
     * "-chardev vc,encoding=cp437" -- names the codepage instead, and
     * PuTTY's own tables do the translation.
     */
    conf_set_str(conf, CONF_line_codepage,
                 line_codepage ? line_codepage : "UTF-8");
    conf_set_bool(conf, CONF_utf8linedraw, true);
    conf_set_bool(conf, CONF_true_colour, true);
    conf_set_bool(conf, CONF_xterm_256_colour, true);

    /*
     * $TERM is effectively xterm, so answer back as one: cursor and keypad
     * application modes on request, ~-style function keys, and no rxvt
     * Home/End.  These are PuTTY's defaults too, but they matter enough to
     * a working curses session to be worth pinning explicitly.
     */
    conf_set_bool(conf, CONF_no_applic_c, false);
    conf_set_bool(conf, CONF_no_applic_k, false);
    conf_set_int(conf, CONF_funky_type, FUNKY_TILDE);
    conf_set_bool(conf, CONF_rxvt_homeend, false);
    conf_set_bool(conf, CONF_nethack_keypad, false);

    /*
     * Backspace sends ^? (DEL).  This is PuTTY's default and the right one
     * for Linux and the BSDs; AIX's default terminal settings also expect
     * DEL as erase.
     */
    conf_set_bool(conf, CONF_bksp_is_delete, true);

    /* No answerback string: ^E should not leak a product name to a guest. */
    conf_set_str(conf, CONF_answerback, "");

    /* No session logging, so the log shims above are never reached. */
    conf_set_int(conf, CONF_logtype, LGTYP_NONE);
    conf_set_bool(conf, CONF_logflush, false);

    /* No printing. */
    conf_set_str(conf, CONF_printer, "");

    /*
     * Scroll to the bottom when the user types, but not merely because the
     * guest produced output -- otherwise reading scrollback while a boot
     * log streams past is impossible.
     */
    conf_set_bool(conf, CONF_scroll_on_key, true);
    conf_set_bool(conf, CONF_scroll_on_disp, false);
    conf_set_bool(conf, CONF_erase_to_scrollback, true);

    /* The bell is a visual flash; a console beeping in a VM tab is rude. */
    conf_set_int(conf, CONF_beep, BELL_VISUAL);

    /*
     * Bell overload: if the far end rings the bell more than bellovl_n
     * times in bellovl_t, shut it up until bellovl_s of quiet.  These two
     * are LOAD_CUSTOM in conf.h, so they carry no DEFAULT_INT and the
     * seeding loop above left them at zero, which would make the overload
     * logic nonsense.  The figures are settings.c's, converted the way its
     * Windows branch converts them (TICKSPERSEC is milliseconds here).
     */
    conf_set_int(conf, CONF_bellovl_t, 2 * TICKSPERSEC);
    conf_set_int(conf, CONF_bellovl_s, 5 * TICKSPERSEC);

    /*
     * Clipboard bindings.  Also LOAD_CUSTOM, so also zero -- and zero is
     * CLIPUI_NONE, which would silently disable every copy and paste
     * shortcut.  CLIPUI_EXPLICIT means "the system clipboard".
     *
     * mousepaste and ctrlshiftins get PuTTY's own Windows defaults, so
     * right-click pastes and Ctrl-Ins / Shift-Ins copy and paste.
     * ctrlshiftcv is a deliberate departure: PuTTY leaves Ctrl-Shift-C and
     * Ctrl-Shift-V unbound, but every Linux terminal emulator the users of
     * this fork also run binds them, and a guest cannot see those
     * combinations anyway.
     */
    conf_set_int(conf, CONF_mousepaste, CLIPUI_EXPLICIT);
    conf_set_int(conf, CONF_ctrlshiftins, CLIPUI_EXPLICIT);
    conf_set_int(conf, CONF_ctrlshiftcv, CLIPUI_EXPLICIT);
    conf_set_bool(conf, CONF_mouseautocopy, true);

    return conf;
}
