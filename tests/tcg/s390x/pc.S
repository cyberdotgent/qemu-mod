/*
 * PROGRAM CALL current-primary basic and stacking tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DAT_64_PSW 0x0400000180000000

    .org 0x1d0                         /* program-new PSW */
    .quad 0x0000000180000000,failure
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    lctlg %c1,%c1,real_asce
    lctlg %c13,%c13,real_asce
    lctlg %c5,%c5,primary_aste_address
    lctlg %c12,%c12,initial_cr12
    lctlg %c15,%c15,initial_cr15
    lpswe dat_psw

start_dat:
    pc 1
after_basic_pc:
    pc 2
after_stacking_pc:
    stctg %c15,%c15,result_cr15
    clc result_cr15(8),initial_cr15
    jne failure
    stctg %c12,%c12,result_cr12
    clc result_cr12(8),expected_cr12
    jne failure
    larl %r2,trace_area
    larl %r3,expected_trace
    clc 0(12,%r2),0(%r3)
    jne failure
    clc 12(12,%r2),12(%r3)
    jne failure
    clc 24(12,%r2),24(%r3)
    jne failure
    lpswe success_psw

basic_target:
    cgrl %r14,expected_basic_link
    jne failure
    br %r14

stacking_target:
    stctg %c15,%c15,result_cr15
    clc result_cr15(8),expected_stacked_cr15
    jne failure
    pr
after_pr:
    j failure

failure:
    lpswe failure_psw

    .align 8
real_asce:
    .quad 0x20
primary_aste_address:
    .quad primary_aste
initial_cr15:
    .quad stack_header_desc
initial_cr12:
    .quad trace_area + 2               /* ASN trace */
dat_psw:
    .quad DAT_64_PSW,start_dat
result_cr15:
    .quad 0
expected_basic_link:
    .quad after_basic_pc
expected_stacked_cr15:
    .quad stack_state+288
result_cr12:
    .quad 0
expected_cr12:
    .quad trace_area + 36 + 2
expected_trace:
    .byte 0x22,0
    .short 1
    .quad after_basic_pc
    .byte 0x22,0
    .short 2
    .quad after_stacking_pc
    .byte 0x32,0x0a
    .short 0
    .long after_stacking_pc
    .long after_pr - 2
    .align 8
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0

    .org 0x1000
stack_header:
    .quad 0
stack_header_desc:
    .byte 0x09,0x33
    .short 0x0400
    .short 0
    .short 0
stack_state:
    .space 0x128
    .space 0x2d8
stack_trailer:
    .quad 0
    .byte 0x0a,0x33
    .short 0
    .short 0
    .short 0

    .org 0x2000
primary_aste:
    .space 24
    .long linkage_table + 0x80000000
    .space 64 - 28

    .align 128
linkage_table:
    .long entry_table
    .space 128 - 4

    .align 64
entry_table:
    .space 32                         /* EX 0 */
basic_ete:                            /* EX 1 */
    .quad basic_target
    .short 0xffff                     /* AKM */
    .short 0                          /* current-primary ASN */
    .short 0                          /* EKM */
    .short 0
    .byte 0x40                        /* G: resulting 64-bit mode */
    .space 15
stacking_ete:                         /* EX 2 */
    .quad stacking_target
    .short 0xffff
    .short 0
    .short 0
    .short 0
    .byte 0xc0                        /* T + G */
    .space 15

    .org 0x3000
trace_area:
    .space 64
