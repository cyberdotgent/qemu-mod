/*
 * Check that an XC result-operand translation exception is identified as a
 * store, including when both operands designate the same storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DAT_AR_PSW 0x0400400180000000

    .org 0x1d0
    .quad 0x0000000180000000,program_exception
    .org 0x200

    .globl _start
_start:
    lctlg %c0,%c0,initial_cr0
    lctlg %c1,%c3,initial_cr1
    lctlg %c7,%c7,real_asce
    lctlg %c13,%c13,real_asce
    lpswe ar_psw

start_ar:
    lgfi %r3,0x00120001             /* ALESN 0x12, ALEN 1 */
    sar %a4,%r3
    llill %r4,0x9000                /* invalid PTE in selected space */
    xc 0(256,%r4),0(%r4)
    j failure

program_exception:
    chhsi 0x8e,0x11                 /* page-translation exception */
    jne failure
    cli 0xa0,4                      /* exception access identification */
    jne failure
    tm 0xae,0x04                    /* TEC bit 53: store indication */
    jz failure
    tm 0xae,0x08                    /* TEC bit 52: fetch indication */
    jnz failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
initial_cr0:
    .quad 0
initial_cr1:
    .quad 0x20
initial_cr2:
    .quad duct
initial_cr3:
    .quad 0
real_asce:
    .quad 0x20
ar_psw:
    .quad DAT_AR_PSW,start_ar
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0

    .org 0x3000
duct:
    .space 16
    .long access_list

    .org 0x3180
access_list:
    .space 16
selected_ale:
    .long 0x00120000
    .long 0
    .long selected_aste
    .long 0x89abcdef

    .org 0x3200
selected_aste:
    .long 0
    .long 0
    .quad segment_table
    .space 4
    .long 0x89abcdef
    .space 40

    .org 0x6000
segment_table:
    .quad page_table

    .org 0x7000
page_table:
    .quad 0x8000
    .org 0x7000 + 9 * 8
    .quad 0x400
