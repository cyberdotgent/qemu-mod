/*
 * Decimal-floating-point zoned- and packed-conversion facilities.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

    j _start
    .org 0x8e
program_interruption_code:
    .org 0x150
program_old_psw:
    .org 0x1d0
    .quad 0x180000000,pgm
    .org 0x200

    .globl _start
_start:
    lctlg %c0,%c0,afp_cr0

    /* Long DFP, including a signed negative zoned operand. */
    larl %r2,zoned64
    .byte 0xed,0x03,0x20,0x00,0x08,0xaa /* CDZT %f0,0(4,%r2),8 */
    larl %r2,result64
    .byte 0xed,0x03,0x20,0x00,0x08,0xa8 /* CZDT %f0,0(4,%r2),8 */
    clc result64(4),zoned64
    jne failure

    /* Extended DFP at its architectural maximum operand length. */
    larl %r2,zoned128
    .byte 0xed,0x21,0x20,0x00,0x48,0xab /* CXZT %f4,0(34,%r2),8 */
    larl %r2,result128
    .byte 0xed,0x21,0x20,0x00,0x48,0xa9 /* CZXT %f4,0(34,%r2),8 */
    clc result128(34),zoned128
    jne failure

    /* Packed conversions have the same long and extended boundaries. */
    larl %r2,packed64
    .byte 0xed,0x08,0x20,0x00,0x08,0xae /* CDPT %f0,0(9,%r2),8 */
    larl %r2,packed_result64
    .byte 0xed,0x08,0x20,0x00,0x08,0xac /* CPDT %f0,0(9,%r2),8 */
    clc packed_result64(9),packed64
    jne failure

    larl %r2,packed128
    .byte 0xed,0x11,0x20,0x00,0x48,0xaf /* CXPT %f4,0(18,%r2),8 */
    larl %r2,packed_result128
    .byte 0xed,0x11,0x20,0x00,0x48,0xad /* CPXT %f4,0(18,%r2),8 */
    clc packed_result128(18),packed128
    jne failure

    /* An extended result may only designate a valid FP-register pair. */
    larl %r2,zoned128
    .byte 0xed,0x21,0x20,0x00,0x28,0xab /* invalid CXZT %f2,... */
after_invalid_pair:
    j failure

pgm:
    chhsi program_interruption_code,0x6
    jne failure
    lg %r0,program_old_psw+8
    larl %r1,after_invalid_pair
    cgr %r0,%r1
    jne failure

    larl %r1,success_psw
    lpswe 0(%r1)

failure:
    larl %r1,failure_psw
    lpswe 0(%r1)

    .align 8
afp_cr0:
    .quad 0x40000
success_psw:
    .quad 0x0002000180000000,0xfff
failure_psw:
    .quad 0x0002000180000000,0

zoned64:
    .byte 0xf1,0xf2,0xf3,0xd4
result64:
    .space 4
zoned128:
    .byte 0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xf0
    .byte 0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xf0
    .byte 0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xf0
    .byte 0xf1,0xf2,0xf3,0xd4
result128:
    .space 34
packed64:
    .byte 0x01,0x23,0x45,0x67,0x89,0x01,0x23,0x45,0x6d
    .align 2
packed_result64:
    .space 9
    .align 2
packed128:
    .byte 0x01,0x23,0x45,0x67,0x89,0x01,0x23,0x45,0x67
    .byte 0x89,0x01,0x23,0x45,0x67,0x89,0x01,0x23,0x4d
packed_result128:
    .space 18
