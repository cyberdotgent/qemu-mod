    .org 0x200                         /* lowcore padding */
    .globl _start
_start:
    /* Give primary page zero and secondary real page six distinct keys. */
    lghi %r1,0x10
    lghi %r3,0
    sske %r1,%r3
    lghi %r1,0xae
    llill %r3,0x6000
    sske %r1,%r3

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

    /* In AR mode LRA selects the access register named by B2. */
    lctlg %c7,%c7,secondary_asce
    lghi %r2,1
    sar %a2,%r2
    llilf %r2,0x1000
    lpswe ar_psw
ar_mode:
    lrag %r1,0(%r2)
    ipm %r2
    srl %r2,28
    chi %r2,2
    jne 1f
    cgrl %r1,expected_secondary_pte
    jne 1f

    /* IVSK also selects the access register named by R2 in AR mode. */
    llilf %r2,0
    lgrl %r1,initial_r1
    cr %r0,%r0
    ivsk %r1,%r2
    jne 1f
    cgrl %r1,expected_ivsk
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
secondary_asce:
    .quad secondary_segment_table
dat_psw:
    .quad 0x0400000180000000,dat_enabled
ar_psw:
    .quad 0x0400400180000000,ar_mode
expected_pte:
    .quad page_table + 0x80
expected_ste:
    .quad segment_table + 6 * 8
expected_ste_length:
    .quad segment_table + 0x1000
expected_secondary_pte:
    .quad secondary_page_table + 8
expected_ivsk:
    .quad 0x87654321123456a8
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

    .org 0x4000
secondary_segment_table:
    .quad secondary_page_table
    .org 0x5000
secondary_page_table:
    .quad 0x6000
    .quad 0x400                         /* invalid page one */

    .org 0x6000
secondary_page:
    .quad 0
