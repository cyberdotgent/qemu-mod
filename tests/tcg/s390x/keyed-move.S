    .org 0x200
    .globl _start
_start:
    /* The keyed operands below all reside in this 4K block. */
    larl %r3,source
    lghi %r1,0xb0
    sske %r1,%r3

    larl %r2,source
    larl %r4,dest
    lghi %r0,15
    lghi %r1,0xb0
    cr %r0,%r0
    mvcsk 0(%r4),0(%r2)
    jne failure
    clc 0(16,%r4),0(%r2)
    jne failure

    larl %r4,dest2
    cr %r0,%r0
    mvcdk 0(%r4),0(%r2)
    jne failure
    clc 0(16,%r4),0(%r2)
    jne failure

    /* MVCK uses the true length in R1 and sets CC from that length. */
    lghi %r3,16
    larl %r4,dest3
    larl %r5,source
    lghi %r6,0xb0
    mvck 0(%r3,%r4),0(%r5),%r6
    jne failure
    clc 0(16,%r4),0(%r2)
    jne failure

    /* Destructive overlap is processed one byte at a time, like MVC. */
    larl %r4,overlap
    lghi %r3,8
    lghi %r6,0
    mvck 1(%r3,%r4),0(%r4),%r6
    jne failure
    cli 0(%r4),0x5a
    jne failure
    clc 1(8,%r4),0(%r4)
    jne failure

    /* A true length above 256 is truncated to 256 and produces CC 3. */
    lghi %r3,257
    larl %r4,large_dest
    larl %r5,large_source
    mvck 0(%r3,%r4),0(%r5),%r6
    jno failure
    clc 0(256,%r4),0(%r5)
    jne failure
    cli 256(%r4),0
    jne failure

    /*
     * The explicit source key, rather than the PSW key, controls MVCSK's
     * fetch.  Put the operands on differently keyed pages and enable fetch
     * protection so an accidental PSW-key access cannot pass silently.
     */
    larl %r2,explicit_source
    lghi %r1,0xb8
    sske %r1,%r2
    larl %r4,explicit_dest
    lghi %r1,0xa0
    sske %r1,%r4
    spka 0xa0
    lghi %r0,15
    lghi %r1,0xb0
    mvcsk 0(%r4),0(%r2)
    spka 0
    clc 0(16,%r4),0(%r2)
    jne failure

    lpswe success_psw
failure:
    lpswe failure_psw

    .align 8
success_psw:
    .quad 0x2000000000000,0xfff
failure_psw:
    .quad 0x2000000000000,0

source:
    .ascii "keyed move works"
dest:
    .fill 16,1,0
dest2:
    .fill 16,1,0
dest3:
    .fill 16,1,0
overlap:
    .byte 0x5a
    .fill 8,1,0
large_source:
    .fill 257,1,0xa5
large_dest:
    .fill 257,1,0

    .align 4096
explicit_source:
    .ascii "explicit key ok!"
    .align 4096
explicit_dest:
    .fill 16,1,0
