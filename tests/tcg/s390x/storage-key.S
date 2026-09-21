/*
 * Key-controlled storage protection and PSW-key TLB invalidation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

    .org 0x1d0
    .quad 0x0000000180000000,program_exception
    .org 0x200

    .globl _start
_start:
    larl %r2,target
    lghi %r1,0xb0
    sske %r1,%r2

    /* Populate a writable TLB entry while the PSW and storage keys match. */
    spka 0xb0
    mvi 0(%r2),0x11

    /* SPKA must invalidate that permission when the PSW key changes. */
    spka 0xa0
faulting_store:
    mvi 0(%r2),0x22
    j failure

after_store_exception:
    cli 0(%r2),0x11
    jne failure

    /* A mismatched key may fetch when the storage key's F bit is clear. */
    spka 0xa0
    ic %r3,0(%r2)
    spka 0
    chi %r3,0x11
    jne failure

    /* The same fetch is protected once the F bit is set. */
    lghi %r1,0xb8
    sske %r1,%r2
    spka 0xa0
faulting_fetch:
    ic %r3,0(%r2)
    j failure

after_fetch_exception:
    /* Access key zero bypasses both store and fetch key protection. */
    mvi 0(%r2),0x33
    ic %r3,0(%r2)
    chi %r3,0x33
    jne failure

    /* CR0 storage-protection override admits key-9 storage. */
    lghi %r1,0x98
    sske %r1,%r2
    lctlg %c0,%c0,store_override
    spka 0xa0
    mvi 0(%r2),0x44
    ic %r3,0(%r2)
    spka 0
    lctlg %c0,%c0,cr0_zero
    chi %r3,0x44
    jne failure

    /* CR0 fetch-protection override admits the first nonprivate 2K. */
    lghi %r1,0xb8
    lghi %r2,0
    sske %r1,%r2
    lctlg %c0,%c0,fetch_override
    spka 0xa0
    ic %r3,low_target
    spka 0
    lctlg %c0,%c0,cr0_zero
    lghi %r1,0
    sske %r1,%r2
    chi %r3,0x5a
    jne failure
    lpswe success_psw

program_exception:
    chhsi 0x8e,0x0004              /* protection exception */
    jne failure
    cli stage,0
    jne second_exception
    mvi stage,1
    lpswe resume_store_psw
second_exception:
    cli stage,1
    jne failure
    mvi stage,2
    lpswe resume_fetch_psw

failure:
    lpswe failure_psw

    .align 8
resume_store_psw:
    .quad 0x0000000180000000,after_store_exception
resume_fetch_psw:
    .quad 0x0000000180000000,after_fetch_exception
success_psw:
    .quad 0x0002000000000000,0xfff
failure_psw:
    .quad 0x0002000000000000,0
stage:
    .byte 0
low_target:
    .byte 0x5a

    .align 8
cr0_zero:
    .quad 0
fetch_override:
    .quad 0x02000000
store_override:
    .quad 0x01000000

    .align 4096
target:
    .byte 0
