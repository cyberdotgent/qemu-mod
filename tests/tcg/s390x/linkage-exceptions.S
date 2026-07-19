/*
 * Linkage trace-table exception priority and rollback.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DAT_64_PSW 0x0400000180000000

    .org 0x1d0
    .quad 0x0000000180000000,program_handler
    .org 0x200

    .globl _start
_start:
    lctlg %c1,%c1,real_asce
    lctlg %c13,%c13,real_asce
    lctlg %c5,%c5,primary_aste_address
    lctlg %c12,%c12,initial_cr12
    lctlg %c15,%c15,initial_cr15
    lpswe dat_psw

start_dat:
    llilf %r2,0
    iihf %r2,1
faulting_bakr:
    bakr %r0,%r2
    j failure

program_handler:
    cli stage,0
    jne pc_program_handler
    clc 0x8e(2),expected_code
    jne failure
    clc 0x158(8),expected_old_address
    jne failure
    stctg %c12,%c12,result_cr12
    clc result_cr12(8),initial_cr12
    jne failure
    stctg %c15,%c15,result_cr15
    clc result_cr15(8),initial_cr15
    jne failure
    larl %r4,stack_header_desc
    clc 0(8,%r4),initial_header_desc
    jne failure
    mvi stage,1
    lctlg %c12,%c12,pc_cr12
    lpswe pc_test_psw

pc_program_handler:
    cli stage,1
    jne failure
    clc 0x8e(2),expected_code
    jne failure
    clc 0x158(8),expected_pc_old_address
    jne failure
    stctg %c12,%c12,result_cr12
    clc result_cr12(8),pc_cr12
    jne failure
    lpswe success_psw

pc_test:
faulting_pc:
    pc 0
    j failure

failure:
    lpswe failure_psw

    .align 8
real_asce:
    .quad 0x20
initial_cr12:
    .quad 0x8000000000000ffc       /* branch trace, 12-byte entry crosses */
pc_cr12:
    .quad 0x0000000000000ffe       /* ASN trace, 12-byte entry crosses */
primary_aste_address:
    .quad primary_aste
initial_cr15:
    .quad stack_header_desc
dat_psw:
    .quad DAT_64_PSW,start_dat
expected_code:
    .short 0x0016
    .align 8
expected_old_address:
    .quad faulting_bakr
expected_pc_old_address:
    .quad faulting_pc
pc_test_psw:
    .quad DAT_64_PSW,pc_test
result_cr12:
    .quad 0
result_cr15:
    .quad 0
initial_header_desc:
    .byte 0x09,0x66
    .short 0x0400
    .short 0
    .short 0
    .align 8
stage:
    .byte 0
    .align 8
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0

    .org 0x1800
stack_header:
    .quad 0
stack_header_desc:
    .byte 0x09,0x66
    .short 0x0400
    .short 0
    .short 0
stack_state:
    .space 0x128
    .space 0x2d8
stack_trailer:
    .quad 0
    .byte 0x0a,0x66
    .short 0
    .short 0
    .short 0

    .org 0x3000
primary_aste:
    .space 24
    .long 0x80004000              /* subsystem-linkage control */
    .space 64 - 28
