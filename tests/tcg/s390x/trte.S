/*
 * Test Translate and Test Extended and its reverse form.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000,0            /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    larl %r1,table
    larl %r4,input
    lghi %r5,3
    lghi %r6,-1
    trte %r4,%r6,0
    jne 1f
    j failure
1:
    larl %r7,input
    aghi %r7,1
    cgr %r4,%r7
    jne failure
    chi %r5,2
    jne failure
    chi %r6,7
    jne failure

    larl %r4,input+2
    lghi %r5,3
    lghi %r6,-1
    trtre %r4,%r6,0
    jne 2f
    j failure
2:
    larl %r7,input
    aghi %r7,1
    cgr %r4,%r7
    jne failure
    chi %r5,2
    jne failure
    chi %r6,7
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0

    .align 256
table:
    .space 0x42
    .byte 7
    .space 0xbd
input:
    .byte 0x41,0x42,0x43
