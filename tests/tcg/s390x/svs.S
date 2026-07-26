/*
 * Test Set Vector Summary without installed list-notification vectors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000,0            /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    lghi %r1,1                         /* set global summary */
    lghi %r2,-1
    .insn rre,0xb2650000,0,0
    ipm %r3
    srlg %r3,%r3,28
    ltgr %r3,%r3
    jne failure

    lghi %r1,3                         /* reset global summary */
    lghi %r3,-1
    .insn rre,0xb2650000,2,0
    ltgr %r3,%r3                       /* no active local summaries */
    jne failure
    ipm %r4
    srlg %r4,%r4,28
    ltgr %r4,%r4
    jne failure

    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0
