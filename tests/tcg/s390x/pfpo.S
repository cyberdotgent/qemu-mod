/*
 * Test the PFPO test-operation interface.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1d0                         /* program-new PSW */
    .quad 0x2000000000000,0            /* disabled wait */
    .org 0x200                         /* lowcore padding */

    .globl _start
_start:
    /*
     * Test BFP-short to HFP-short conversion.  TCG exposes no PFPO
     * conversion subfunctions, so test mode must return CC 3 and clear
     * the low word of the return-code register.
     */
    llilf %r0,0x81000500
    lghi %r1,-1
    .byte 0x01,0x0a                    /* PFPO */
    ipm %r2

    lghi %r3,-1
    sllg %r3,%r3,32
    cgr %r1,%r3
    jne failure
    srl %r2,28
    chi %r2,3
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0
