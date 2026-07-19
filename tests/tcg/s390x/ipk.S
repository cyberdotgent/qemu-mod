/*
 * Test Insert PSW Key.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000,0            /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    spka 0x50
    lghi %r2,-1
    ipk
    lghi %r3,-176                      /* 0xffffffffffffff50 */
    cgr %r2,%r3
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0
