    .org 0x200                         /* lowcore padding */
    .globl _start
_start:
    /* With DAT off, the translated real address is the effective address. */
    larl %r2,dat_off_source
    strag dat_off_result,0(%r2)
    clg %r2,dat_off_result
    jne failure

    lctlg %c1,%c1,primary_asce
    lpswe dat_psw

dat_enabled:
    /*
     * Virtual page 1 maps to real page 6.  STRAG stores the real address,
     * including the byte displacement, rather than the prefixed absolute
     * address or the page-table-entry address returned by a failing LRA.
     */
    llilf %r2,0x1234
    strag result,0(%r2)
    lgrl %r3,expected
    clg %r3,result
    jne failure
    lpswe success_psw

failure:
    lpswe failure_psw

    .align 8
dat_off_source:
    .quad 0
dat_off_result:
    .quad 0
result:
    .quad 0
expected:
    .quad 0x6234
primary_asce:
    .quad segment_table
dat_psw:
    .quad 0x0400000180000000,dat_enabled
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0            /* disabled wait */

    .org 0x2000
segment_table:
    .quad page_table

    .org 0x3000
page_table:
    .quad 0                            /* virtual page zero */
    .quad 0x6000                       /* virtual page one */

    .org 0x6000
mapped_page:
    .quad 0
