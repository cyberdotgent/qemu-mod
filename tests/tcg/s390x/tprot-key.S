/*
 * TPROT must use the explicit key in operand-2 address bits 56-59.
 *
 * This image is also runnable unchanged as a Hercules loadcore image.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1a0
    .quad 0x0000000180000000, _start
    .org 0x1d0
    .quad 0x0002000000000000, 0
    .org 0x200

    .globl _start
_start:
    larl %r2,target

    lghi %r1,0xb8                    /* key B, fetch protected */
    sske %r1,%r2
    spka 0xa0                        /* deliberately different PSW key */

    tprot 0(%r2),0xb0                /* explicit key B, not PSW key A */
    jne failure                      /* fetch and store permitted */
    tprot 0(%r2),0xa0
    ipm %r3
    srlg %r3,%r3,28
    cgfi %r3,2                       /* fetch protected */
    jne failure

    lghi %r1,0xb0                    /* clear fetch-protection bit */
    sske %r1,%r2
    tprot 0(%r2),0xa0
    ipm %r3
    srlg %r3,%r3,28
    cgfi %r3,1                       /* fetch allowed, store protected */
    jne failure

    tprot 0(%r2),0                   /* key zero bypasses key protection */
    jne failure

    spka 0
    lghi %r1,0
    sske %r1,%r2
    lpswe success_psw

failure:
    spka 0
    lpswe failure_psw

    .align 8
success_psw:
    .quad 0x0002000000000000,0xfff
failure_psw:
    .quad 0x0002000000000000,0

    .align 4096
target:
    .byte 0
