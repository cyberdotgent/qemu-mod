/*
 * Basic access-register translation through DUCT, ALE, and ASTE.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DAT_AR_PSW 0x0400400180000000

    .org 0x1d0
    .quad 0x0000000180000000,failure
    .org 0x200

    .globl _start
_start:
    lctlg %c1,%c3,initial_cr1
    lctlg %c7,%c7,real_asce
    lctlg %c13,%c13,real_asce
    lpswe ar_psw

start_ar:
    lgfi %r3,0x00120001             /* ALESN 0x12, ALEN 1 */
    sar %a2,%r3
    sgr %r6,%r6
    sar %a6,%r6                    /* ALET zero selects primary */
    larl %r2,data
    l %r4,0(%r2)                    /* select AR2 */
    cfi %r4,0x12345678
    jne failure
    l %r5,data                      /* base register zero selects primary */
    cfi %r5,0x11223344
    jne failure

    larl %r6,copy_dest
    mvc 0(4,%r6),0(%r2)            /* primary destination, AR2 source */
    clc 0(4,%r6),0(%r2)
    jne failure
    l %r5,copy_dest
    cfi %r5,0x12345678
    jne failure

    ni 0(%r2),0xf0                 /* modify selected space only */
    palb                            /* no separate ALB: purge AR TLB entries */
    l %r4,0(%r2)
    cfi %r4,0x10345678
    jne failure
    l %r5,data
    cfi %r5,0x11223344
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
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
data:
    .long 0x11223344
copy_dest:
    .long 0
    .align 8
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0

    .org 0x3000
duct:
    .space 16
    .long access_list               /* ALD: ALO, ALL 0 */

    .org 0x3100
access_list:
    .space 16                       /* ALEN 0 */
selected_ale:                       /* ALEN 1 */
    .long 0x00120000                /* valid, ALESN 0x12 */
    .long 0
    .long selected_aste
    .long 0x89abcdef                /* ASTESN */

    .org 0x3200
selected_aste:
    .long 0
    .long 0
    .quad segment_table             /* segment-table ASCE */
    .space 4
    .long 0x89abcdef                /* ASTESN, word 5 */
    .space 40

    .org 0x6000
segment_table:
    .quad page_table

    .org 0x7000
page_table:
    .quad 0x8000

    .org 0x8000 + data
selected_data:
    .long 0x12345678
