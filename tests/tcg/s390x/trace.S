/*
 * Test explicit tracing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000,0            /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    lctlg %c12,%c12,trace_control
    llilf %r4,0x11223344
    llilf %r5,0x55667788
    llilf %r6,0x99aabbcc
    trace %r4,%r6,trace_operand

    llgc %r0,trace_area
    chi %r0,0x72                       /* format + two-register span */
    jne failure
    clc trace_area+8(4),trace_operand
    jne failure
    clc trace_area+12(12),registers
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
trace_control:
    .quad trace_area+1                 /* explicit trace enabled */
trace_operand:
    .long 0x12345678
registers:
    .long 0x11223344,0x55667788,0x99aabbcc
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0

    .align 256
trace_area:
    .space 256
