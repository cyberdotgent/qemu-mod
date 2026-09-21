/*
 * Constrained transaction commit and abort/rollback behavior.
 *
 * This image is also runnable unchanged as a Hercules loadcore image.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
    .org 0x1a0
    .quad 0x0000000180000000, _start
    .org 0x1d0
    .quad 0x0000000180000000, program_handler
    .org 0x200

    .globl _start
_start:
    stctg %c0,%c0,cr0
    oi cr0+1,0x80                    /* CR0 bit 8: transactional execution */
    lctlg %c0,%c0,cr0

    mvi target,0x11
    tbeginc 0,0                       /* no GPR restoration */
    jne failure
    mvi target,0x22
    tend
    jne failure
    cli target,0x22
    jne failure

    tend                              /* outside transaction */
    ipm %r2
    srlg %r2,%r2,28
    cgfi %r2,2
    jne failure

    mvi target,0x33
    lghi %r0,0x1234
    tbeginc 0,0x8000                  /* restore GPR pair 0/1 on abort */
    jne failure
    mvi target,0x44
    lghi %r0,0x5678
nested_tbegin:
    tbeginc 0,0                       /* constraint: nested TBEGINC */
    j failure

program_handler:
    chhsi 0x8e,0x0218                /* transaction-event + constraint */
    jne failure
    cli target,0x33                  /* transactional store rolled back */
    jne failure
    cghi %r0,0x1234                  /* selected GPR pair restored */
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
cr0:
    .quad 0
target:
    .byte 0
    .align 8
success_psw:
    .quad 0x0002000000000000,0xfff
failure_psw:
    .quad 0x0002000000000000,0
