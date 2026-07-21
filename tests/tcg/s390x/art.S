/*
 * Basic access-register translation through DUCT, ALE, and ASTE.
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
    sar %a2,%r3
    cpya %a4,%a2
    ear %r8,%a4
    cgr %r8,%r3
    jne failure
    larl %r4,data
    l %r8,0(%r4)                    /* copied AR4 selects same space */
    cfi %r8,0x12345678
    jne failure

    tar %a2,%r3
    ipm %r8
    srlg %r8,%r8,28
    cgfi %r8,1
    jne failure
    sgr %r6,%r6
    sar %a6,%r6                    /* ALET zero selects primary */
    tar %a6,%r3
    ipm %r8
    srlg %r8,%r8,28
    ltgr %r8,%r8
    jne failure

    lghi %r7,1
    sar %a7,%r7
    tar %a7,%r3
    ipm %r8
    srlg %r8,%r8,28
    cgfi %r8,3
    jne failure
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

    /* EX must retain the target SS instruction's B1/B2 address spaces. */
    larl %r4,copy_dest               /* AR4: selected space */
    larl %r6,data                    /* AR6: primary space */
    ex 0,ex_mvc
    l %r8,0(%r4)
    cfi %r8,0x11223344
    jne failure
    l %r5,copy_dest                  /* primary destination unchanged */
    cfi %r5,0x12345678
    jne failure
    j ex_mvc_done
ex_mvc:
    mvc 0(4,%r4),0(%r6)
ex_mvc_done:

    ni 0(%r2),0xf0                 /* modify selected space only */
    palb                            /* no separate ALB: purge AR TLB entries */
    l %r4,0(%r2)
    cfi %r4,0x10345678
    jne failure
    l %r5,data
    cfi %r5,0x11223344
    jne failure

    /* DAT exceptions through an AR-specified ASCE identify that AR. */
    llill %r4,0x9000
    l %r8,0(%r4)
    j failure                       /* the invalid PTE must interrupt */

program_exception:
    chhsi 0x8e,0x11                /* page-translation exception */
    jne failure
    cli 0xa0,4                     /* exception access identification */
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
initial_cr0:
    .quad 0x0000000000010000       /* address-space function */
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

    .org 0x3180                    /* exercise format-0 ALO bit 24 */
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
    .org 0x7000 + 9 * 8
    .quad 0x400                     /* invalid PTE for virtual 0x9000 */

    .org 0x8000 + data
selected_data:
    .long 0x12345678
