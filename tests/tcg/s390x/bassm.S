/*
 * Test that BASSM observes the unmasked branch-address mode bit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000,0            /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    lpswe start24_psw
start24:
    larl %r2,target31
    llilf %r3,0x80000000
    ogr %r2,%r3
    bassm %r1,%r2
    j failure

target31:
    bassm %r4,%r0                     /* save the current mode */
    srlg %r4,%r4,31
    nill %r4,1
    cghi %r4,1
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
start24_psw:
    .quad 0,start24                   /* 24-bit mode */
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0
