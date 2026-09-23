/*
 * QEMU-mod: chardev-vc backed by the PuTTY terminal, for the Win32 UI
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original code.
 *
 * TYPE_CHARDEV_VC is what QEMU's -serial vc, -monitor vc and the default
 * "put the monitor and the serial port in the GUI" behaviour resolve to.
 * It is registered first-come-first-served:
 * qemu_console_early_init() in ui/console-vc.c registers the built-in
 * QemuConsole-backed implementation only `if (!object_class_by_name(
 * TYPE_CHARDEV_VC))`, and a display backend's early_init runs first.  So a
 * display that wants its own terminal simply registers the type itself,
 * which is exactly what ui/gtk.c does for VTE.  We do the same here, and
 * the core tree needs no change at all.
 *
 * The consequence is that with the Win32 UI in use, serial ports and the
 * HMP monitor stop being QemuConsoles.  They become chardevs with a
 * terminal of their own, and the frame's tab strip therefore has two kinds
 * of tab: QemuConsole-backed graphics tabs, and these.  That is the same
 * shape ui/gtk.c has had for years (GD_VC_GFX vs GD_VC_VTE).
 *
 * Both directions cross a thread boundary here, because the terminal
 * belongs to the UI thread and the chardev to QEMU's main loop:
 *
 *   QEMU -> UI.  chr_write arrives on the QEMU thread and may not touch
 *   the PuTTY terminal at all.  The bytes go into in_fifo and the UI
 *   thread is told to come and get them; win32_term_console_drain() is the
 *   other end.
 *
 *   UI -> QEMU.  The user typing arrives on the UI thread, via PuTTY's
 *   ldisc, and qemu_chr_be_write() needs the BQL.  out_fifo is the buffer
 *   that was already there for the chardev's own flow control; it is now
 *   also the handover, so it is taken under tcon->lock.
 *
 * Lock order is always BQL before tcon->lock.  Nothing takes them the
 * other way round, and nothing holds either across a call into the other
 * side.
 */

#include "qemu/osdep.h"

#include <math.h>

#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/fifo8.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "qom/object.h"

#include "win32-display.h"
#include "win32-term.h"

#ifndef USER_DEFAULT_SCREEN_DPI
#define USER_DEFAULT_SCREEN_DPI 96
#endif

#define TYPE_CHARDEV_VC "chardev-vc"

/*
 * The same cap ui/gtk.c uses.  There is no technical reason for a limit
 * beyond keeping the tab strip usable.
 */
#define WIN32_MAX_VCS 10

/* How much typed-but-not-yet-accepted input we are willing to hold. */
#define WIN32_VC_FIFO_SIZE 4096

/*
 * How much guest output we are willing to hold for a UI thread that is not
 * collecting it.  The only thing that stops it collecting is a Windows
 * modal loop -- a held menu, a drag -- and ten seconds of that at the rate
 * a 16550 can actually deliver is a couple of hundred kilobytes, so this is
 * generous.  It is a bound rather than a budget: the point is that a UI
 * thread that never comes back cannot turn into unbounded memory growth.
 */
#define WIN32_VC_IN_FIFO_MAX (4 * 1024 * 1024)

struct win32_term_console {
    Chardev *chr;
    Win32Term *term;
    /* covers out_fifo, in_fifo, write_posted and echo */
    QemuMutex lock;
    Fifo8 out_fifo;
    GByteArray *in_fifo;
    bool write_posted;
    bool echo;
    bool overflowed;
    int idx;                    /* index into win32_consoles */
    char *label;
};

struct VCChardev {
    Chardev parent;
    struct win32_term_console *tcon;
    bool echo;
    /* the geometry and encoding chosen at open time; see win32_vc_chr_open */
    int cols, rows;
    int width, height;
    const char *line_codepage;
};
typedef struct VCChardev VCChardev;

DECLARE_INSTANCE_CHECKER(VCChardev, WIN32_VC_CHARDEV, TYPE_CHARDEV_VC)

static int nb_vcs;
static Chardev *vcs[WIN32_MAX_VCS];

/*
 * ----------------------------------------------------------------
 * Terminal -> chardev.
 */

/* Called with the BQL held, from either thread. */
static void win32_vc_send_chars(struct win32_term_console *tcon)
{
    uint32_t len, avail;

    if (!tcon->chr) {
        return;
    }

    qemu_mutex_lock(&tcon->lock);
    len = qemu_chr_be_can_write(tcon->chr);
    avail = fifo8_num_used(&tcon->out_fifo);
    while (len > 0 && avail > 0) {
        const uint8_t *buf;
        uint32_t size;

        buf = fifo8_pop_bufptr(&tcon->out_fifo, MIN(len, avail), &size);
        /*
         * qemu_chr_be_write() runs the far end's read handler, which for
         * the HMP monitor is a whole command and can write straight back
         * to this same console.  Do not hold tcon->lock across it.
         */
        qemu_mutex_unlock(&tcon->lock);
        qemu_chr_be_write(tcon->chr, buf, size);
        qemu_mutex_lock(&tcon->lock);
        len = qemu_chr_be_can_write(tcon->chr);
        avail -= size;
    }
    qemu_mutex_unlock(&tcon->lock);
}

/*
 * The user typed something.  UI thread, from PuTTY's ldisc, so this is one
 * of the places the UI thread reaches into emulator state and has to take
 * the BQL to do it.
 */
static void win32_vc_send(void *opaque, const char *buf, int len)
{
    struct win32_term_console *tcon = opaque;
    uint32_t free_space;
    bool echo;

    if (len <= 0) {
        return;
    }

    BQL_LOCK_GUARD();

    qemu_mutex_lock(&tcon->lock);
    echo = tcon->echo;
    qemu_mutex_unlock(&tcon->lock);

    /*
     * chr_set_echo(true) is how the HMP monitor asks us to echo a password
     * prompt's input back.  Feed it to our own terminal, the way ui/gtk.c
     * feeds VTE, rather than expecting the far end to do it.  We are on the
     * UI thread, so the terminal may be written to directly.
     */
    if (echo) {
        for (int i = 0; i < len; i++) {
            uint8_t c = buf[i];

            if (c >= 128 || isprint(c)) {
                win32_term_write(tcon->term, (const char *)&c, 1);
            } else if (c == '\r' || c == '\n') {
                win32_term_write(tcon->term, "\r\n", 2);
            } else {
                char ctrl[2] = { '^', c ^ 64 };
                win32_term_write(tcon->term, ctrl, 2);
            }
        }
    }

    qemu_mutex_lock(&tcon->lock);
    free_space = fifo8_num_free(&tcon->out_fifo);
    fifo8_push_all(&tcon->out_fifo, (const uint8_t *)buf,
                   MIN(free_space, (uint32_t)len));
    qemu_mutex_unlock(&tcon->lock);

    win32_vc_send_chars(tcon);
}

/* UI thread */
static void win32_vc_send_break(void *opaque)
{
    struct win32_term_console *tcon = opaque;

    if (tcon->chr) {
        BQL_LOCK_GUARD();
        qemu_chr_be_event(tcon->chr, CHR_EVENT_BREAK);
    }
}

static void win32_vc_title(void *opaque, const char *title)
{
    /*
     * The guest set a window title on this console (OSC 0/2).  The tab
     * strip is rebuilt from the labels, so just ask for that.
     */
    win32_frame_relabel();
}

/*
 * ----------------------------------------------------------------
 * Chardev -> terminal.
 */

/*
 * Guest (or monitor) output.  QEMU thread: the terminal belongs to the UI
 * thread, so all this does is queue the bytes and make sure the UI thread
 * has been told to come and get them.  One message is in flight at a time,
 * so a guest draining a UART FIFO a few bytes at a time costs one post per
 * trip round the UI thread's message loop rather than one per write.
 */
static int win32_vc_chr_write(Chardev *chr, const uint8_t *buf, int len)
{
    VCChardev *vcd = WIN32_VC_CHARDEV(chr);
    struct win32_term_console *tcon = vcd->tcon;
    bool post = false;

    if (!tcon || !tcon->term) {
        /*
         * Output produced before the display came up.  Dropping it is what
         * the GTK backend does too; the alternative is an unbounded buffer
         * for a console nobody may ever open.
         */
        return len;
    }

    qemu_mutex_lock(&tcon->lock);
    if (tcon->in_fifo->len + len > WIN32_VC_IN_FIFO_MAX) {
        if (!tcon->overflowed) {
            tcon->overflowed = true;
            warn_report("win32: the '%s' terminal is more than %d bytes "
                        "behind and is dropping output; the UI thread has "
                        "not run for a very long time",
                        tcon->label, WIN32_VC_IN_FIFO_MAX);
        }
    } else {
        g_byte_array_append(tcon->in_fifo, buf, len);
        post = !tcon->write_posted;
        tcon->write_posted = true;
    }
    qemu_mutex_unlock(&tcon->lock);

    if (post) {
        win32_ui_post(WIN32_UI_TERM_OUTPUT, tcon->idx, 0);
    }
    return len;
}

/* UI thread: hand whatever has queued up to the terminal. */
void win32_term_console_drain(struct win32_console *wcon)
{
    struct win32_term_console *tcon = wcon->tcon;
    GByteArray *batch;

    if (!tcon || !tcon->term) {
        return;
    }

    qemu_mutex_lock(&tcon->lock);
    batch = tcon->in_fifo;
    tcon->in_fifo = g_byte_array_new();
    tcon->write_posted = false;
    tcon->overflowed = false;
    qemu_mutex_unlock(&tcon->lock);

    if (batch->len) {
        win32_term_write(tcon->term, (const char *)batch->data, batch->len);
    }
    g_byte_array_unref(batch);
}

static void win32_vc_chr_accept_input(Chardev *chr)
{
    VCChardev *vcd = WIN32_VC_CHARDEV(chr);

    if (vcd->tcon) {
        win32_vc_send_chars(vcd->tcon);
    }
}

static void win32_vc_chr_set_echo(Chardev *chr, bool echo)
{
    VCChardev *vcd = WIN32_VC_CHARDEV(chr);

    if (vcd->tcon) {
        qemu_mutex_lock(&vcd->tcon->lock);
        vcd->tcon->echo = echo;
        qemu_mutex_unlock(&vcd->tcon->lock);
    } else {
        vcd->echo = echo;
    }
}

static void win32_vc_chr_parse(QemuOpts *opts, ChardevBackend *backend,
                               Error **errp)
{
    ChardevVC *vc;
    const char *str;
    int val;

    backend->type = CHARDEV_BACKEND_KIND_VC;
    vc = backend->u.vc.data = g_new0(ChardevVC, 1);
    qemu_chr_parse_common(opts, qapi_ChardevVC_base(vc));

    val = qemu_opt_get_number(opts, "width", 0);
    if (val != 0) {
        vc->has_width = true;
        vc->width = val;
    }
    val = qemu_opt_get_number(opts, "height", 0);
    if (val != 0) {
        vc->has_height = true;
        vc->height = val;
    }
    val = qemu_opt_get_number(opts, "cols", 0);
    if (val != 0) {
        vc->has_cols = true;
        vc->cols = val;
    }
    val = qemu_opt_get_number(opts, "rows", 0);
    if (val != 0) {
        vc->has_rows = true;
        vc->rows = val;
    }

    str = qemu_opt_get(opts, "encoding");
    if (str) {
        int cs = qapi_enum_parse(&ChardevVCEncoding_lookup, str, -1, errp);
        if (cs < 0) {
            return;
        }
        vc->has_encoding = true;
        vc->encoding = cs;
    }
}

static bool win32_vc_chr_open(Chardev *chr, ChardevBackend *backend,
                              Error **errp)
{
    VCChardev *vcd = WIN32_VC_CHARDEV(chr);
    ChardevVC *vc = backend->u.vc.data;

    if (nb_vcs == WIN32_MAX_VCS) {
        error_setg(errp, "Maximum number of consoles reached");
        return false;
    }

    /*
     * An explicit size always wins: "-serial vc:120Cx50C" means what it
     * says, and has meant it for as long as there have been vc chardevs.
     */
    if (vc && backend->type == CHARDEV_BACKEND_KIND_VC) {
        if (vc->has_cols) {
            vcd->cols = vc->cols;
        }
        if (vc->has_rows) {
            vcd->rows = vc->rows;
        }
        if (vc->has_width) {
            vcd->width = vc->width;
        }
        if (vc->has_height) {
            vcd->height = vc->height;
        }
        if (vc->has_encoding && vc->encoding == CHARDEV_VC_ENCODING_CP437) {
            vcd->line_codepage = "CP437";
        }
    }

    if (!vcd->cols && !vcd->rows && !vcd->width && !vcd->height) {
        /*
         * No size was asked for, so choose one from what this chardev
         * appears to be for.
         *
         * A serial or parallel port has no way to tell its guest how big
         * the terminal is -- there is no SIGWINCH down a wire -- so the
         * guest keeps whatever its stty says, which is 80x24 essentially
         * everywhere.  Making the window wider than that does not help the
         * guest; it just makes long lines wrap in a place the guest did not
         * predict.  So serial and parallel stay at 80x24.
         *
         * The monitor has no guest at all: the other end is QEMU's own
         * readline, which asks the terminal how wide it is.  So it can have
         * the room, and 132x43 is the VT100's wide mode.
         *
         * Identifying the monitor by its label is a heuristic, and not a
         * good one -- it only matches the chardev that system/vl.c creates
         * implicitly for the default monitor, which monitor/monitor.c names
         * "compat_monitor<n>".  A user who writes "-chardev vc,id=mon0
         * -mon chardev=mon0" gets 80x24 instead.  There is no better signal
         * available here: monitor_parse() creates the chardev first and
         * only afterwards decides to put a monitor on it, so at this point
         * the chardev genuinely does not know.  Anyone who cares can say
         * what they want with cols= and rows=.
         */
        if (g_str_has_prefix(chr->label, "compat_monitor")) {
            vcd->cols = 132;
            vcd->rows = 43;
        } else {
            vcd->cols = 80;
            vcd->rows = 24;
        }
    }

    vcs[nb_vcs++] = chr;

    /*
     * The terminal itself cannot exist yet: chardevs are created long
     * before the display backend's init().  Defer everything, exactly as
     * ui/gtk.c does, and let win32_term_console_init() finish the job.
     */
    return true;
}

static void char_win32_vc_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_parse = win32_vc_chr_parse;
    cc->chr_open = win32_vc_chr_open;
    cc->chr_write = win32_vc_chr_write;
    cc->chr_accept_input = win32_vc_chr_accept_input;
    cc->chr_set_echo = win32_vc_chr_set_echo;
    cc->supports_size_opts = true;
    cc->supports_encoding_opts = true;
}

static const TypeInfo char_win32_vc_type_info = {
    .name = TYPE_CHARDEV_VC,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(VCChardev),
    .class_init = char_win32_vc_class_init,
};

/*
 * ----------------------------------------------------------------
 * The interface the rest of the Win32 backend uses.
 */

void win32_term_chardev_register(void)
{
    /*
     * First registrant wins, and a display's early_init runs before
     * qemu_console_early_init().  Be defensive anyway: registering a type
     * name twice is fatal.
     */
    if (object_class_by_name(TYPE_CHARDEV_VC)) {
        return;
    }
    type_register_static(&char_win32_vc_type_info);
}

int win32_term_nb_vcs(void)
{
    return nb_vcs;
}

void win32_term_console_init(struct win32_console *wcon, int vc_index)
{
    static const Win32TermCallbacks cb = {
        .send = win32_vc_send,
        .title = win32_vc_title,
        .send_break = win32_vc_send_break,
    };
    struct win32_term_console *tcon;
    Win32TermOptions topts = { 0 };
    Chardev *chr;
    VCChardev *vcd;

    assert(vc_index >= 0 && vc_index < nb_vcs);
    chr = vcs[vc_index];
    vcd = WIN32_VC_CHARDEV(chr);

    tcon = g_new0(struct win32_term_console, 1);
    tcon->chr = chr;
    tcon->echo = vcd->echo;
    tcon->idx = wcon->idx;
    tcon->label = g_strdup(chr->label ?: "vc");
    qemu_mutex_init(&tcon->lock);
    tcon->in_fifo = g_byte_array_new();
    fifo8_create(&tcon->out_fifo, WIN32_VC_FIFO_SIZE);

    topts.dpi = lround(win32_dpi_scale() * USER_DEFAULT_SCREEN_DPI);
    topts.cols = vcd->cols;
    topts.rows = vcd->rows;
    topts.width = vcd->width;
    topts.height = vcd->height;
    topts.line_codepage = vcd->line_codepage;

    tcon->term = win32_term_new(win32_frame, &cb, tcon, &topts);
    if (!tcon->term) {
        fifo8_destroy(&tcon->out_fifo);
        g_byte_array_unref(tcon->in_fifo);
        qemu_mutex_destroy(&tcon->lock);
        g_free(tcon->label);
        g_free(tcon);
        return;
    }

    vcd->tcon = tcon;
    wcon->tcon = tcon;
    wcon->hwnd = win32_term_hwnd(tcon->term);

    /*
     * The chardev has been sitting unopened since chr_open deferred it.
     * Tell whoever is on the other end that it is now usable; the HMP
     * monitor, in particular, does not print its banner until it sees
     * this.
     */
    qemu_chr_be_event(chr, CHR_EVENT_OPENED);

    win32_frame_add_console(wcon);
}

void win32_term_console_fini(struct win32_console *wcon)
{
    struct win32_term_console *tcon = wcon->tcon;

    if (!tcon) {
        return;
    }
    if (tcon->chr) {
        WIN32_VC_CHARDEV(tcon->chr)->tcon = NULL;
    }
    win32_term_free(tcon->term);
    fifo8_destroy(&tcon->out_fifo);
    g_byte_array_unref(tcon->in_fifo);
    qemu_mutex_destroy(&tcon->lock);
    g_free(tcon->label);
    g_free(tcon);
    wcon->tcon = NULL;
    wcon->hwnd = NULL;
}

const char *win32_term_console_label(struct win32_console *wcon)
{
    return wcon->tcon ? wcon->tcon->label : NULL;
}

Win32Term *win32_term_console_term(struct win32_console *wcon)
{
    return wcon->tcon ? wcon->tcon->term : NULL;
}

void win32_term_consoles_set_dpi(unsigned dpi)
{
    for (int i = 0; win32_consoles && i < win32_num_outputs; i++) {
        struct win32_term_console *tcon = win32_consoles[i].tcon;

        if (tcon && tcon->term) {
            win32_term_set_dpi(tcon->term, dpi);
        }
    }
}
