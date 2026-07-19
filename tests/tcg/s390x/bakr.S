/*
 * BRANCH AND STACK basic state-entry test.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DAT_64_PSW 0x0400000180000000

    .org 0x150
program_old_psw:
    .org 0x1d0                         /* program-new PSW */
    .quad 0x0000000180000000,failure
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    lctlg %c1,%c1,real_asce
    lctlg %c13,%c13,real_asce
    lctlg %c15,%c15,initial_cr15
    lpswe dat_psw

start_dat:
    lgrl %r10,saved_r10
    larl %r2,branch_target
    bakr %r0,%r2
after_bakr:
    stctg %c15,%c15,result_cr15
    clc result_cr15(8),initial_cr15
    jne failure
    clc stack_header_desc(8),initial_header_desc
    jne failure
    lpswe success_psw

branch_target:
    stctg %c15,%c15,result_cr15
    clc result_cr15(8),expected_cr15
    jne failure
    clc stack_header_desc(8),expected_header_desc
    jne failure
    clc stack_state+136(8),expected_saved_mask
    jne failure
    clc stack_state+144(8),expected_call_addr
    jne failure
    clc stack_state+168(8),expected_return_addr
    jne failure
    clc stack_state+288(8),expected_state_desc
    jne failure

    lgrl %r4,modify_word0
    lgrl %r5,modify_word1
    msta %r4
    lgrl %r6,extract_seed0
    lgrl %r7,extract_seed1
    lghi %r5,3
    esta %r6,%r5
    cgrl %r6,extracted_word0
    jne failure
    cgrl %r7,extracted_word1
    jne failure

    lghi %r10,-1
    eregg %r10,%r10
    cgrl %r10,saved_r10
    jne failure
    pr
    j failure

failure:
    lpswe failure_psw

    .align 8
real_asce:
    .quad 0x20                        /* real-space ASCE */
initial_cr15:
    .quad stack_header_desc
dat_psw:
    .quad DAT_64_PSW,start_dat
result_cr15:
    .quad 0
expected_cr15:
    .quad stack_state+288
expected_header_desc:
    .byte 0x09,0x5a
    .short 0x0400
    .short 0x0128
    .short 0
initial_header_desc:
    .byte 0x09,0x5a
    .short 0x0400
    .short 0
    .short 0
expected_saved_mask:
    .quad DAT_64_PSW
expected_call_addr:
    .quad branch_target+1
expected_return_addr:
    .quad after_bakr
expected_state_desc:
    .byte 0x0c,0x5a
    .short 0x02d8
    .short 0
    .short 0
saved_r10:
    .quad 0x123456789abcdef0
modify_word0:
    .quad 0xaaaaaaaa11223344
modify_word1:
    .quad 0xbbbbbbbb55667788
extract_seed0:
    .quad 0xcccccccc00000000
extract_seed1:
    .quad 0xdddddddd00000000
extracted_word0:
    .quad 0xcccccccc11223344
extracted_word1:
    .quad 0xdddddddd55667788
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0

    .org 0x800
stack_header:
    .quad 0
stack_header_desc:
    .byte 0x09,0x5a
    .short 0x0400
    .short 0
    .short 0
stack_state:
    .space 0x128
    .space 0x2d8
stack_trailer:
    .quad 0
    .byte 0x0a,0x5a
    .short 0
    .short 0
    .short 0
