/*
 * PROGRAM CALL/RETURN with space switching.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DAT_64_PSW 0x0400000180000000

    .org 0x1d0                         /* program-new PSW */
    .quad 0x0000000180000000,failure
    .org 0x200

    .globl _start
_start:
    lctlg %c1,%c1,real_asce
    lctlg %c3,%c5,initial_cr3
    lctlg %c7,%c7,real_asce
    lctlg %c13,%c15,home_controls
    lpswe dat_psw

start_dat:
    pc 1
after_pc:
    stctg %c1,%c1,result_cr1
    stctg %c3,%c5,result_cr3
    stctg %c7,%c7,result_cr7
    stctg %c15,%c15,result_cr15
    clc result_cr1(8),real_asce
    jne failure
    clc result_cr3(24),expected_return_cr3
    jne failure
    clc result_cr7(8),real_asce
    jne failure
    clc result_cr15(8),initial_cr15
    jne failure
    lpswe success_psw

space_switch_target:
    stctg %c1,%c1,result_cr1
    stctg %c3,%c5,result_cr3
    stctg %c7,%c7,result_cr7
    clc result_cr1(8),real_asce
    jne failure
    clc result_cr3(24),expected_called_cr3
    jne failure
    clc result_cr7(8),real_asce
    jne failure
    pr
    j failure

failure:
    lpswe failure_psw

    .align 8
real_asce:
    .quad 0x20
initial_cr3:
    .quad 0xffff000000000003       /* PKM, SASN 3 */
initial_cr4:
    .quad 0x11110001               /* AX, PASN 1 */
initial_cr5:
    .quad primary_aste
home_controls:
    .quad 0x20                     /* CR13 */
initial_cr14:
    .quad 0x0000000000080004       /* ASN translation, AFTO 0x4000 */
initial_cr15:
    .quad stack_header_desc
dat_psw:
    .quad DAT_64_PSW,start_dat
result_cr1:
    .quad 0
result_cr3:
    .space 24
result_cr7:
    .quad 0
result_cr15:
    .quad 0
expected_called_cr3:
    .quad 0xffff000000000001       /* old primary becomes secondary */
    .quad 0x22220002               /* called AX and PASN */
    .quad called_aste              /* PASTEO */
expected_return_cr3:
    .quad 0xffff000000000003
    .quad 0x00330001               /* AX reloaded by ASN translation */
    .quad return_aste
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0

    .org 0x1000
stack_header:
    .quad 0
stack_header_desc:
    .byte 0x09,0x44
    .short 0x0400
    .short 0
    .short 0
stack_state:
    .space 0x128
    .space 0x2d8
stack_trailer:
    .quad 0
    .byte 0x0a,0x44
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
    .space 32
space_switch_ete:
    .quad space_switch_target
    .short 0xffff                 /* AKM */
    .short 2                      /* new PASN */
    .short 0                      /* EKM */
    .short 0
    .byte 0xc0                    /* stacking, resulting 64-bit */
    .space 3
    .long called_aste             /* ASTE real address */
    .quad 0                       /* entry parameter */

    .org 0x4000
asn_first_table:
    .long asn_second_table
    .space 60
asn_second_table:
    .space 64
return_aste:                      /* ASN 1, ASX 1 */
    .long 0
    .long 0x00330000              /* AX */
    .quad 0x20                    /* ASCE */
    .space 48

    .space 64                    /* unused ASX 2 */
secondary_aste:                  /* ASN 3, ASX 3 */
    .long authority_table        /* ATO */
    .long 0x00000030             /* ATL covers AX 0x0033 */
    .quad 0x20
    .space 48

    .org 0x4300
authority_table:
    /* The address itself is used as ATO; selected byte is at 0x430c. */

    .org 0x430c
    .byte 0x01                   /* secondary-authority bit for AX ...3 */

    .org 0x5000
called_aste:
    .long 0
    .long 0x22220000              /* AX */
    .quad 0x20                    /* ASCE */
    .space 48
