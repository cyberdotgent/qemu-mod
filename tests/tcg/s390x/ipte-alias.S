/*
 * Verify that IPTE purges every cached translation for the page frame.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

    .org 0x200
    .globl _start
_start:
    lghi %r1,0x11
    stg %r1,0x6000
    lghi %r1,0x22
    stg %r1,0x7000

    lctlg %c1,%c1,primary_asce
    lpswe dat_psw

dat_enabled:
    /* Populate two TLB entries that alias the same real page. */
    lg %r4,0x1000
    lg %r5,0x2000
    cghi %r4,0x11
    jne failure
    cghi %r5,0x11
    jne failure

    /*
     * Invalidating virtual page one must also purge the cached translation
     * for virtual page two because both PTEs name real page 0x6000.
     */
    larl %r1,page_table
    lghi %r2,0x1000
    ipte %r1,%r2

    lghi %r3,0x7000
    la %r1,16(%r1)
    sturg %r3,%r1
    lg %r4,0x2000
    cghi %r4,0x22
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
primary_asce:
    .quad segment_table
dat_psw:
    .quad 0x0400000180000000,dat_enabled
success_psw:
    .quad 0x0002000000000000,0xfff
failure_psw:
    .quad 0x0002000000000000,0

    .org 0x8000
segment_table:
    .quad page_table

    .org 0x9000
page_table:
    .quad 0x0000
    .quad 0x6000
    .quad 0x6000
    .quad 0x9000

    .org 0xa000
    .space 0x1000
