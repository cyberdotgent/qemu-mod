/*
 * Test Process Queue Auxiliary Processor without configured AP queues.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000,0            /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    lghi %r0,0                         /* TAPQ, AP queue 0 */
    lghi %r1,-1
    lghi %r2,0
    .insn rre,0xb2af0000,0,0           /* PQAP(TAPQ) */
    ipm %r2

    clgfi %r1,0x00010000               /* queue not available */
    jne failure
    srlg %r2,%r2,28
    clgfi %r2,3
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0
