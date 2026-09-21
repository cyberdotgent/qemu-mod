/*
 * Test explicit tracing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x0000000180000000,trace_exception
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    /*
     * Trace-table stores ignore key-controlled protection.  Give the trace
     * area a nonzero storage-access key while leaving the PSW key at zero.
     */
    lghi %r1,0xb0
    larl %r2,trace_area
    sske %r1,%r2
    spka 0xa0

    lctlg %c12,%c12,trace_control
    llilf %r4,0x11223344
    llilf %r5,0x55667788
    llilf %r6,0x99aabbcc
    trace %r4,%r6,trace_operand

    larl %r7,trace_area
    clc 0(2,%r7),trace_header
    jne failure                        /* format + two-register span */
    clc 8(4,%r7),trace_operand
    jne failure
    clc 12(12,%r7),registers
    jne failure

    spka 0
    /*
     * CR12 advancement must not carry into the next 4K block.  TRACE uses
     * its 76-byte maximum for this check, even when the actual entry is
     * shorter.  An entry beginning exactly 76 bytes before the boundary
     * must therefore raise a trace-table exception.
     */
    lctlg %c12,%c12,trace_edge_control
    mvi expect_trace_exception,1
    trace %r4,%r6,trace_operand
    j failure

trace_exception:
    cli expect_trace_exception,1
    jne failure
    chhsi 0x8e,0x16                    /* trace-table exception */
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
trace_control:
    .quad trace_area+1                 /* explicit trace enabled */
trace_edge_control:
    .quad 0xfb5                        /* 0x1000 - 76, plus enable bit */
trace_operand:
    .long 0x12345678
registers:
    .long 0x11223344,0x55667788,0x99aabbcc
trace_header:
    .byte 0x72,0x00
expect_trace_exception:
    .byte 0
    .align 8
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0

    .align 4096
trace_area:
    .space 256
