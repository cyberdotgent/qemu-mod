    .org 0x200                         /* lowcore padding */
    .globl _start
_start:
    lgrl %r1,initial_r1
    lra %r1,0(%r1)
    cgrl %r1,expected_r1
    jne 1f

    lctlg %c1,%c1,primary_asce
    lpswe dat_psw

dat_enabled:
    llilf %r1,0x510458
    lrag %r1,0(%r1)
    ipm %r2
    srl %r2,28
    chi %r2,2
    jne 1f
    cgrl %r1,expected_pte
    jne 1f

    llilf %r1,0x610000
    lrag %r1,0(%r1)
    ipm %r2
    srl %r2,28
    chi %r2,1
    jne 1f
    cgrl %r1,expected_ste
    jne 1f

    llilf %r1,0x20000000
    lrag %r1,0(%r1)
    ipm %r2
    srl %r2,28
    chi %r2,3
    jne 1f
    cgrl %r1,expected_ste_length
    jne 1f
    lpswe success_psw
1:
    lpswe failure_psw
    .align 8
initial_r1:
    .quad 0x8765432112345678
expected_r1:
    .quad 0x8765432180000038           /* ASCE type exception */
primary_asce:
    .quad segment_table
dat_psw:
    .quad 0x0400000180000000,dat_enabled
expected_pte:
    .quad page_table + 0x80
expected_ste:
    .quad segment_table + 6 * 8
expected_ste_length:
    .quad segment_table + 0x1000
success_psw:
    .quad 0x2000000000000,0xfff        /* see is_special_wait_psw() */
failure_psw:
    .quad 0x2000000000000,0            /* disabled wait */

    .org 0x2000
segment_table:
    .quad page_table                    /* map segment zero */
    .org segment_table + 5 * 8
    .quad page_table                    /* map segment containing 0x510458 */
    .quad 0x20                          /* invalid segment */

    .org 0x3000
page_table:
    .quad 0                             /* identity-map page zero */
    .org page_table + 0x10 * 8
    .quad 0x400                         /* invalid page */
