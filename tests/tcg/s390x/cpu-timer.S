/*
 * Test signed CPU-timer values.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x130                         /* external-old PSW */
ext_old_psw:
    .org 0x1b0                         /* external-new PSW */
    .quad 0x180000000, ext_handler     /* external enabled, 64-bit mode */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000, 0           /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    stctg %c0,%c0,c0
    oi c0+6,4                          /* CPU-timer subclass */
    lctlg %c0,%c0,c0

    lpswe active_psw

timer_test:
    spt expired
retry_test:
    mvi marker,1                       /* neither interruption may be lost */
    j failure

ext_handler:
    chhsi 0x86,0x1005                 /* CPU-timer external interruption */
    jne failure
    cli marker,0
    jne failure
    stpt observed
    ltg %r0,observed
    jhe failure                        /* an expired timer stays negative */

    cli count,0
    jne second_interrupt
    mvi count,1
    lpswe retry_psw                    /* expired condition must interrupt */

second_interrupt:
    spt future                         /* clear the level-triggered condition */
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
c0:
    .quad 0
expired:
    .quad -1
future:
    .quad 0x7fffffffffffffff
observed:
    .quad 0
marker:
    .byte 0
count:
    .byte 0
    .align 8
active_psw:
    .quad 0x100000180000000, timer_test
retry_psw:
    .quad 0x100000180000000, retry_test
success_psw:
    .quad 0x2000000000000, 0xfff       /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000, 0
