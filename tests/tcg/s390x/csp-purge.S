/*
 * Verify that a successful CSP purge affects the immediately following load.
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
    /* Populate the translation for virtual page two -> real page 0x6000. */
    lg %r4,0x2000
    cghi %r4,0x11
    jne failure

    /*
     * R2 bit zero requests a TLB purge.  CSP masks the request bits from the
     * aligned operand address, replacing PTE 2 with real page 0x7000.
     */
    lghi %r2,0x6000
    lghi %r3,0x7000
    larl %r4,page_table+20
    oilf %r4,1
    csp %r2,%r4
    jne failure

    lg %r5,0x2000
    cghi %r5,0x22
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
    .org page_table + 9 * 8
    .quad 0x9000

    .org 0xa000
    .space 0x1000
