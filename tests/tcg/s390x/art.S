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
    lctlg %c5,%c5,primary_aste_address
    lctlg %c7,%c7,real_asce
    lctlg %c13,%c13,real_asce
    lctlg %c15,%c15,initial_cr15
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

    /*
     * The two operands of TR/TRT/TRTR independently select their address
     * spaces through B1 and B2.  Exercise both selections with translation
     * tables and input bytes that deliberately differ between primary and
     * AR-selected storage.
     */
    sgr %r8,%r8
    sar %a6,%r8
    larl %r6,trt_table              /* AR6: primary translation table */
    lghi %r1,-1
    lghi %r2,-1
    trt 0(1,%r4),0(%r6)             /* AR4 operand contains 0x12 */
    ipm %r8
    srlg %r8,%r8,28
    cgfi %r8,2
    jne failure
    cgr %r1,%r4
    jne failure
    llgcr %r8,%r2
    cgfi %r8,0x5a
    jne failure

    cpya %a6,%a2
    lghi %r1,-1
    lghi %r2,-1
    trt data(1),0(%r6)              /* primary operand contains 0x11 */
    ipm %r8
    srlg %r8,%r8,28
    cgfi %r8,2
    jne failure
    larl %r8,data
    cgr %r1,%r8
    jne failure
    llgcr %r8,%r2
    cgfi %r8,0xa5
    jne failure

    sgr %r8,%r8
    sar %a6,%r8
    larl %r4,translate_data
    tr 0(1,%r4),0(%r6)
    llgc %r8,0(%r4)
    cgfi %r8,0x5a
    jne failure
    mvi 0(%r4),0x12

    cpya %a6,%a2
    tr translate_data(1),0(%r6)
    cli translate_data,0xa5
    jne failure
    mvi translate_data,0x11
    cpya %a4,%a2

    /*
     * CSP's second operand is the address in R2, so AR2 must select its
     * address space in access-register mode.  RRE has no B2 field for the
     * generic translator to infer this from.
     */
    larl %r2,data
    llilf %r4,0x12345678
    llilf %r5,0x55667788
    csp %r4,%r2
    jne failure
    l %r8,0(%r2)
    cfi %r8,0x55667788
    jne failure
    l %r8,data
    cfi %r8,0x11223344
    jne failure
    llilf %r4,0x12345678
    st %r4,0(%r2)                  /* restore selected-space fixture */

    /* LAE changes AR4: an AR4 TLB entry for the old ALET must not survive. */
    lae %r4,data                    /* B2 zero selects primary space */
    ear %r8,%a4
    ltgr %r8,%r8
    jne failure
    l %r8,0(%r4)
    cfi %r8,0x11223344
    jne failure
    cpya %a4,%a2                    /* restore the selected address space */

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

    /*
     * Different ARs may select the same address space.  Destructive overlap
     * must still have MVC's left-to-right byte propagation semantics.
     */
    sgr %r4,%r4
    sar %a4,%r4                    /* AR4 and AR6 both select primary */
    larl %r4,overlap_data
    aghi %r4,1
    larl %r6,overlap_data
    mvc 0(8,%r4),0(%r6)
    clc overlap_data(9),overlap_expected
    jne failure
    cpya %a4,%a2                   /* restore selected address space */

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

    /*
     * TPROT operand 1 is subject to access-register translation selected by
     * B1.  Virtual address 0x9000 has an invalid PTE in the space selected by
     * AR4, but is valid through the primary real-space ASCE.
     */
    llill %r4,0x9000
    tprot 0(%r4),0
    ipm %r8
    srlg %r8,%r8,28
    cgfi %r8,3
    jne failure

    larl %r6,copy_dest
    mvc 0(4,%r6),0(%r2)            /* primary destination, AR2 source */
    clc 0(4,%r6),0(%r2)
    jne failure
    l %r5,copy_dest
    cfi %r5,0x12345678
    jne failure

    /*
     * MVCK must use the access registers selected by B1 and B2, just like
     * MVC.  Use the PSW key for the destination and an explicit zero source
     * key while copying between the selected and primary address spaces.
     */
    larl %r4,copy_dest              /* AR4: selected destination space */
    larl %r6,data                   /* AR6: primary source space */
    lghi %r3,4
    sgr %r8,%r8                     /* source key zero */
    mvck 0(%r3,%r4),0(%r6),%r8
    jne failure
    l %r8,0(%r4)
    cfi %r8,0x11223344
    jne failure
    l %r5,copy_dest                 /* primary destination unchanged */
    cfi %r5,0x12345678
    jne failure

    /*
     * Exercise the reverse selection too: primary destination and selected
     * source.  The source fixture was modified by NI below in the original
     * test, so perform this check before that modification.
     */
    larl %r4,copy_dest
    sgr %r8,%r8
    sar %a4,%r8                    /* AR4: primary destination space */
    larl %r6,data                  /* AR6: selected source space */
    cpya %a6,%a2
    lghi %r3,4
    mvck 0(%r3,%r4),0(%r6),%r8
    jne failure
    l %r5,copy_dest
    cfi %r5,0x12345678
    jne failure
    cpya %a4,%a2                   /* restore later test assumptions */
    sgr %r8,%r8
    sar %a6,%r8

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
    cli expected_exception,0
    jne check_extended_authority
    chhsi 0x8e,0x11                /* page-translation exception */
    jne failure
    cli 0xa0,4                     /* exception access identification */
    jne failure
    mvi expected_exception,1
    lpswe extended_authority_psw

check_extended_authority:
    chhsi 0x8e,0x2d                /* extended-authority exception */
    jne failure
    cli 0xa0,4                     /* exception access identification */
    jne failure
    cli expected_exception,1
    jne success
    mvi expected_exception,2
    lpswe esea_authority_psw

success:
    lpswe success_psw

extended_authority_test:
    /*
     * Earlier AR4 accesses cached a translation authorized with EAX zero.
     * A stacking PC changes EAX to one without switching primary address
     * spaces.  The old translation must be discarded before its target
     * accesses the same private ALE.
     */
    larl %r6,selected_ale
    llilf %r8,0x01120000
    st %r8,0(%r6)                    /* make the ALE private, EAX zero */
    larl %r4,data
    l %r8,0(%r4)
    cfi %r8,0x10345678               /* NI above modified this fixture */
    jne failure
    pc 1
    j failure

pc_eax_target:
    l %r8,0(%r4)
    j failure

esea_authority_test:
    /*
     * Return EAX to zero, cache the now-authorized translation again, then
     * verify that ESEA invalidates it when installing EAX one.
     */
    sgr %r8,%r8
    esea %r8
    larl %r4,data
    l %r8,0(%r4)
    cfi %r8,0x10345678
    jne failure
    lghi %r8,1
    esea %r8
    l %r8,0(%r4)
    j failure

failure:
    lpswe failure_psw

    .align 8
initial_cr0:
    .quad 0                        /* ASF is always enabled in z/Architecture */
initial_cr1:
    .quad 0x20
initial_cr2:
    .quad duct
initial_cr3:
    .quad 0
primary_aste_address:
    .quad primary_aste
initial_cr15:
    .quad stack_header_desc
real_asce:
    .quad 0x20
ar_psw:
    .quad DAT_AR_PSW,start_ar
data:
    .long 0x11223344
copy_dest:
    .long 0
translate_data:
    .byte 0x11,0
overlap_data:
    .byte 0x5a,0,0,0,0,0,0,0,0
overlap_expected:
    .fill 9,1,0x5a
    .align 256
trt_table:
    .fill 0x12,1,0
    .byte 0x5a
    .fill 256 - 0x13,1,0
expected_exception:
    .byte 0
    .align 8
extended_authority_psw:
    .quad DAT_AR_PSW,extended_authority_test
esea_authority_psw:
    .quad DAT_AR_PSW,esea_authority_test
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

    .align 64
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
pc_eax_ete:
    .quad pc_eax_target
    .short 0xffff                   /* AKM */
    .short 0                        /* current-primary ASN */
    .short 0                        /* EKM */
    .short 0
    .byte 0xc6                      /* stacking, 64-bit, AR mode, EAX */
    .byte 0
    .short 1                        /* new EAX */
    .space 12

    .org 0x4000
stack_header:
    .quad 0
stack_header_desc:
    .byte 0x09,0x55
    .short 0x0400
    .short 0
    .short 0
stack_state:
    .space 0x128
    .space 0x2d8
stack_trailer:
    .quad 0
    .byte 0x0a,0x55
    .short 0
    .short 0
    .short 0

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
    .org 0x8000 + translate_data
    .byte 0x12,0
    .org 0x8000 + trt_table + 0x11
    .byte 0xa5
