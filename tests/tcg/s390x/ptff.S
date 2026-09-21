/*
 * Perform Timing Facility Function.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

    .org 0x1d0
    .quad 0x0002000180000000, failure
    .org 0x200

    .globl _start
_start:
    larl %r1,qaf
    xgr %r0,%r0
    .insn e,0x0104                    /* PTFF QAF */
    jne failure
    llgc %r2,qaf                      /* query functions 0-5 */
    cfi %r2,0xfc
    jne failure

    larl %r1,qto
    lghi %r0,1
    .insn e,0x0104                    /* PTFF QTO */
    jne failure
    ltg %r2,qto
    jz failure
    ltg %r2,qto+8
    jnz failure

    larl %r1,qsi
    lghi %r0,2
    .insn e,0x0104                    /* PTFF QSI */
    jne failure
    ltg %r2,qsi
    jz failure

    larl %r1,qpt
    lghi %r0,3
    .insn e,0x0104                    /* PTFF QPT */
    jne failure
    ltg %r2,qpt
    jz failure

    larl %r1,qtou
    lghi %r0,5
    .insn e,0x0104                    /* PTFF QTOU */
    jne failure
    ltg %r2,qtou
    jz failure
    ltg %r2,qtou+32
    jnz failure

    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
qaf:
    .space 16
qto:
    .space 32
qsi:
    .space 56
qpt:
    .space 8
qtou:
    .space 40
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0
