/*
 * QEMU-mod: the PuTTY-terminal-backed text console -- internal interface
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This header is for the files that live on the *PuTTY* side of the
 * boundary: ui/win32-term-shim.c, ui/win32-term.c and the vendored code
 * under ui/putty/.  It pulls in putty.h, which is not co-installable with
 * qemu/osdep.h (both define a large number of the same generic names), so
 * nothing that includes this may include any QEMU header.
 *
 * The QEMU side talks to this code only through ui/win32-term.h, which is
 * written in plain C plus <windows.h> and includes neither tree.
 */

#ifndef UI_WIN32_TERM_INTERNAL_H
#define UI_WIN32_TERM_INTERNAL_H

#include "putty.h"
#include "terminal/terminal.h"

#include "win32-term.h"

/*
 * PuTTY's Ldisc is its local line discipline: the thing a terminal hands
 * keystrokes to, which decides whether to echo them locally and when to
 * pass them to the backend.  A QEMU serial port or HMP monitor always wants
 * the raw, unechoed, unbuffered path -- the guest (or the monitor's own
 * readline) does all the editing -- so ours is just a forwarding shim.
 *
 * struct Ldisc_tag is only ever declared, never defined, by PuTTY's own
 * headers, so defining it here replaces ldisc.c wholesale.
 */
struct Ldisc_tag {
    Terminal *term;
    void (*send)(void *opaque, const char *buf, int len);
    void *opaque;
};

Ldisc *win32_term_ldisc_new(Terminal *term,
                            void (*send)(void *opaque,
                                         const char *buf, int len),
                            void *opaque);
void win32_term_ldisc_free(Ldisc *ldisc);

/*
 * Build a Conf carrying PuTTY's own compiled-in defaults, overridden with
 * the settings that make sense for a QEMU serial/monitor console.  See
 * ui/win32-term-shim.c for the list of deliberate overrides.
 */
Conf *win32_term_conf_new(void);

/*
 * PuTTY expects its front end to run a main loop that services toplevel
 * callbacks and timers.  We are a guest inside QEMU's main loop instead,
 * so the shim exports a single pump that ui/win32-term.c calls from the
 * frame window's timer and after every batch of terminal input.
 *
 * Returns true if a timer is pending, and if so stores the tick count it
 * is due at in *next.
 */
bool win32_term_pump(unsigned long *next);

#endif /* UI_WIN32_TERM_INTERNAL_H */
