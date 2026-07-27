/*
 * ASN-control and authority/subspace branch instruction smoke tests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define DAT_PRIMARY_64 0x0400000180000000

    .org 0x1d0
    .quad 0x0000000180000000,failure
    .org 0x200

    .globl _start
_start:
    lctlg %c0,%c7,initial_crs
    lctlg %c13,%c13,initial_cr13
    lctlg %c14,%c14,initial_cr14
    lpswe dat_psw

start_dat:
    /*
     * IAC's mode number uses AR * 2 + SPACE, not the physical ordering
     * of the two ASC bits in the PSW.  Verify all four SAC/IAC round trips.
     */
    sac 0
    iac %r4
    ipm %r5
    nilf %r4,0xf00
    ltgr %r4,%r4
    jne failure
    srlg %r5,%r5,28
    ltgr %r5,%r5
    jne failure

    sac 0x100
    iac %r4
    ipm %r5
    nilf %r4,0xf00
    cghi %r4,0x100
    jne failure
    srlg %r5,%r5,28
    cghi %r5,1
    jne failure

    sac 0x200
    iac %r4
    ipm %r5
    nilf %r4,0xf00
    cghi %r4,0x200
    jne failure
    srlg %r5,%r5,28
    cghi %r5,2
    jne failure

    sac 0x300
    iac %r4
    ipm %r5
    nilf %r4,0xf00
    cghi %r4,0x300
    jne failure
    srlg %r5,%r5,28
    cghi %r5,3
    jne failure

    sac 0

    /* EPAR/ESAR replace the low word and preserve the high word. */
    llihf %r8,0x89abcdef
    epar %r8
    srlg %r9,%r8,32
    clfi %r9,0x89abcdef
    jne failure
    llgfr %r9,%r8
    clfi %r9,0x00001234
    jne failure

    llihf %r8,0x76543210
    esar %r8
    srlg %r9,%r8,32
    clfi %r9,0x76543210
    jne failure
    llgfr %r9,%r8
    clfi %r9,0x00001234
    jne failure

    /* SSAR-cp copies the primary ASN/ASCE to the secondary state. */
    lghi %r1,0x1234
    ssar %r1
    stctg %c3,%c3,saved_cr3
    stctg %c7,%c7,saved_cr7
    llgf %r8,saved_cr3+4
    nilf %r8,0xffff
    cghi %r8,0x1234
    jne failure
    lg %r8,saved_cr7
    cghi %r8,0x20
    jne failure

    /* SSAR-ss translates and authorizes a different secondary ASN. */
    lghi %r1,0x1240
    ssar %r1
    stctg %c3,%c3,saved_cr3
    llgf %r8,saved_cr3+4
    nilf %r8,0xffff
    cghi %r8,0x1240
    jne failure
    lghi %r1,0x1234
    ssar %r1

    /* The instance-extract forms copy both ASTEIN and ASN. */
    llihf %r6,0xa5a5a5a5
    iilf %r6,0xa5a5a5a5
    epair %r6
    llihf %r7,0x55667788
    iilf %r7,0x00001234
    clgr %r6,%r7
    jne failure
    llihf %r6,0xa5a5a5a5
    iilf %r6,0xa5a5a5a5
    esair %r6
    llilf %r7,0x00001234
    clgr %r6,%r7
    jne failure

    /*
     * SSAIR-cp ignores the supplied ASTE instance and copies PASTEIN, even
     * when ASN-and-LX reuse is installed but disabled.
     */
    llihf %r1,0xdeadbeef
    iilf %r1,0x00001234
    ssair %r1
    stctg %c3,%c3,saved_cr3
    lg %r8,saved_cr3
    srlg %r8,%r8,32
    clfi %r8,0x55667788
    jne failure

    /* SSAIR-ss validates ASTEIN and installs it as the new SASTEIN. */
    llihf %r1,0x11223344
    iilf %r1,0x00001240
    ssair %r1
    stctg %c3,%c3,saved_cr3
    lg %r8,saved_cr3
    srlg %r8,%r8,32
    clfi %r8,0x11223344
    jne failure
    llgf %r8,saved_cr3+4
    nilf %r8,0xffff
    cghi %r8,0x1240
    jne failure
    lghi %r1,0x1234
    ssar %r1

    /* PT-cp changes PKM/SASN and branches without translating an ASN. */
    llilf %r1,0xffff1234
    larl %r2,after_pt
    pt %r1,%r2
    j failure
after_pt:
    stctg %c3,%c3,saved_cr3
    llgf %r8,saved_cr3+4
    clfi %r8,0xffff1234
    jne failure

    /*
     * PTI-cp ignores the supplied instance, branches, and copies the current
     * primary instance into the secondary state even when reuse is disabled.
     */
    llihf %r1,0xdeadbeef
    iilf %r1,0xffff1234
    larl %r2,after_pti
    pti %r1,%r2
    j failure
after_pti:
    stctg %c3,%c3,saved_cr3
    lg %r8,saved_cr3
    srlg %r8,%r8,32
    clfi %r8,0x55667788
    jne failure

    /* BSG with ALET zero selects the base ASTE and saves the return. */
    larl %r2,after_bsg
    bsg %r1,%r2
bsg_return:
    j failure
after_bsg:
    larl %r8,bsg_return
    cgr %r1,%r8
    jne failure

    /* BSG special ART ignores ALESN and does not apply ALE authority. */
    llilf %r8,0x00aa0001
    sar %a2,%r8
    larl %r2,after_bsg_art
    bsg %r1,%r2
bsg_art_return:
    j failure
after_bsg_art:
    larl %r8,bsg_art_return
    cgr %r1,%r8
    jne failure
    j do_bsa

do_bsa:
    /* Enter reduced authority, then return through the saved DUCT state. */
    llilf %r1,0xffff0000
    larl %r2,reduced
    bsa %r1,%r2
after_bsa:
    larl %r8,reduced_return
    cgr %r1,%r8
    jne failure
    l %r8,duct+20
    tmll %r8,8
    jnz failure

    /* PT-ss translates/authorizes a new primary ASN and branches. */
    llilf %r1,0xffff1240
    larl %r2,after_pt_ss
    pt %r1,%r2
    j failure
after_pt_ss:
    stctg %c3,%c5,saved_cr3
    llgf %r8,saved_cr3+12
    nilf %r8,0xffff
    cghi %r8,0x1240
    jne failure
    llgf %r8,saved_cr3+20
    larl %r9,destination_aste
    clgr %r8,%r9
    jne failure

    /* PTI-ss validates and installs the supplied primary ASTE instance. */
    llihf %r1,0x99aabbcc
    iilf %r1,0xffff1244
    larl %r2,after_pti_ss
    pti %r1,%r2
    j failure
after_pti_ss:
    stctg %c3,%c4,saved_cr3
    lg %r8,saved_cr3
    srlg %r8,%r8,32
    clfi %r8,0x99aabbcc
    jne failure
    lg %r8,saved_cr3+8
    srlg %r8,%r8,32
    clfi %r8,0x99aabbcc
    jne failure
    lpswe success_psw

reduced:
    bsa %r1,%r0
reduced_return:
    j failure

failure:
    lpswe failure_psw

    .align 8
initial_crs:
    .quad 0x000000000c000000       /* extraction auth + secondary */
    .quad 0x20                     /* CR1: real-space ASCE */
    .quad duct                     /* CR2: DUCT origin */
    .quad 0x00000000ffff1234       /* CR3: PKM, SASN */
    .quad 0x5566778800001234       /* CR4: PASTEIN, AX, PASN */
    .quad base_aste                /* CR5: primary ASTE origin */
    .quad 0                        /* CR6 */
    .quad 0x20                     /* CR7: real-space ASCE */
initial_cr14:
    .quad 0x0000000000080004       /* ASN translation + AFT origin */
initial_cr13:
    .quad 0x20                     /* CR13: real-space ASCE */
dat_psw:
    .quad DAT_PRIMARY_64,start_dat
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0
saved_cr3:
    .quad 0
saved_cr7:
    .quad 0
    .space 16

    .org 0x3000
    .align 64
base_aste:
    .long 0,0
    .quad 0x20
    .space 8
    .long 0x80000000               /* subsystem-linkage control */
    .space 36

    .align 64
duct:
    .long base_aste                /* base ASTE origin */
    .space 12
    .long access_list              /* dispatchable-unit ALD */
    .space 44

    .org 0x4124
    .long destination_aste         /* ASN 1240 first-table entry */
    .long destination_aste2        /* ASN 1244 first-table entry */

    .org 0x4200
    .align 64
destination_aste:
    .long authority_table          /* authority-table origin */
    .long 0                        /* AX zero, authority length zero */
    .quad 0x20                     /* real-space ASCE */
    .space 8
    .long 0x80000000               /* subsystem-linkage control */
    .space 16
    .long 0x11223344               /* ASTE instance */
    .space 16

    .org 0x4300
destination_aste2:
    .long authority_table          /* authority-table origin */
    .long 0                        /* AX zero, authority length zero */
    .quad 0x20                     /* real-space ASCE */
    .space 8
    .long 0x80000000               /* subsystem-linkage control */
    .space 16
    .long 0x99aabbcc               /* ASTE instance */
    .space 16

    .org 0x4400
authority_table:
    .byte 0xc0                     /* primary + secondary for AX 0 */

    .org 0x4500
access_list:
    .space 16
    .long 0                        /* valid, deliberately wrong ALESN */
    .long 0
    .long base_aste
    .long 0                        /* matches base ASTE sequence */
