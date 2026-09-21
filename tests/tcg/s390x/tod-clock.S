/*
 * TOD-clock uniqueness and clock-comparator strict-greater behavior.
 *
 * This image is also runnable unchanged as a Hercules loadcore image.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x130
external_old_psw:
    .org 0x1a0
    .quad 0x0100000180000000, _start
    .org 0x1b0
    .quad 0x0000000180000000, external_handler
    .org 0x1d0
    .quad 0x0002000000000000, 0
    .org 0x200

    .globl _start
_start:
    lpswe enabled_psw
enabled:
    stctg %c0,%c0,cr0
    oi cr0+6,8                       /* clock-comparator subclass */
    lctlg %c0,%c0,cr0

    stck clock1
    stck clock2
    clc clock2(8),clock1
    jnh failure                      /* STCK values are unique/ascending */

    stcke extended1
    stcke extended2
    clc extended2+1(13),extended1+1
    jnh failure                      /* compare the 104-bit clock fields */
    clc extended1+9(5),zero5
    je failure                       /* STCKE extension is always nonzero */
    clc extended2+9(5),zero5
    je failure

    stck comparator
    agfi %r0,0                       /* keep an instruction boundary here */
    lg %r0,comparator
    agfi %r0,0x100000                /* about 256 microseconds */
    stg %r0,comparator
    sckc comparator
0:
    brasl %r14,1f
    jg 0b
1:
    br %r14

external_handler:
    chhsi 0x86,0x1004
    jne failure
    stck observed
    clc observed(8),comparator
    jnh failure                      /* TOD must strictly exceed CKC */
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
enabled_psw:
    .quad 0x0100000180000000, enabled
cr0:
    .quad 0
clock1:
    .quad 0
clock2:
    .quad 0
comparator:
    .quad 0
observed:
    .quad 0
extended1:
    .space 16
extended2:
    .space 16
zero5:
    .space 5
    .align 8
success_psw:
    .quad 0x0002000000000000,0xfff
failure_psw:
    .quad 0x0002000000000000,0
