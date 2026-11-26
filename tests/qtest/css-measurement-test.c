/*
 * QTest testcase for the channel-subsystem monitoring facilities
 *
 * A tiny guest program executes one I/O instruction at a time on behalf
 * of the test, which checks MODIFY SUBCHANNEL / STORE SUBCHANNEL, SET
 * CHANNEL MONITOR and the measurement blocks the channel subsystem
 * stores in guest memory (z/Architecture Principles of Operation,
 * chapters 14, 15 and 17).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "s390x-ccw-prog.h"

/* Guest program layout */
#define G_CODE      0x200
#define G_INSN      0x218   /* the instruction to execute */
#define G_PGM_ENTRY 0x22c
#define G_CCWS      0x800
#define G_ORB       0x900
#define G_IRB       0xa00
#define G_SCHIB     0xb00
#define G_R1        0xc00
#define G_R2        0xc08
#define G_CC        0xc10
#define G_PGM       0xc14
#define G_GO        0xc18
#define G_DONE      0xc19
#define G_IMAGE     0xc20
#define G_CHSC      0x2000  /* CHSC command block */
#define G_DATA      0x3000
#define G_MBO_A     0x20000 /* measurement-block areas */
#define G_MBO_B     0x28000
#define G_MB1       0x30000 /* format-1 measurement block */

#define INSN_MSCH   0xb2320b00u
#define INSN_SSCH   0xb2330900u
#define INSN_STSCH  0xb2340b00u
#define INSN_TSCH   0xb2350a00u
#define INSN_CSCH   0xb2300000u
#define INSN_HSCH   0xb2310000u
#define INSN_RSCH   0xb2380000u
#define INSN_SCHM   0xb23c0000u
#define INSN_CHSC   0xb25f0010u

#define PGM_OPERAND 0x15

/* SCHIB offsets and bits */
#define SCHIB_LEN        52
#define SCHIB_FLAGS      4
#define SCHIB_DEVNO      6
#define SCHIB_MBI        12
#define SCHIB_CHARS      24
#define SCHIB_SCSW_CTRL  30
#define SCHIB_MBA        40
#define PMCW_ENA         0x0080
#define PMCW_MBU         0x0010     /* measurement-block-update enable */
#define PMCW_DCT         0x0008     /* device-connect-time enable */
#define PMCW_TF          0x0002     /* timing facility */
#define PMCW_F           0x00000004 /* measurement-block-format control */
#define PMCW_X           0x00000002 /* extended-measurement-word enable */

#define SCSW_SUSPENDED   0x0020
#define SCSW_SECONDARY   0x0002
#define SCSW_PENDING     0x0001

#define SCHM_M           0x2
#define SCHM_D           0x1

static const char *accel;

static char *create_guest_image(void)
{
    static const uint8_t code[] = {
        0x95, 0x00, 0x0c, 0x18,             /* 200 cli  GO,0        */
        0xa7, 0x84, 0xff, 0xfe,             /* 204 je   200         */
        0x92, 0x00, 0x0c, 0x18,             /* 208 mvi  GO,0        */
        0xe3, 0x10, 0x0c, 0x00, 0x00, 0x04, /* 20c lg   %r1,R1      */
        0xe3, 0x20, 0x0c, 0x08, 0x00, 0x04, /* 212 lg   %r2,R2      */
        0x47, 0x00, 0x00, 0x00,             /* 218 (instruction)    */
        0xb2, 0x22, 0x00, 0x30,             /* 21c ipm  %r3         */
        0x50, 0x30, 0x0c, 0x10,             /* 220 st   %r3,CC      */
        0x92, 0x01, 0x0c, 0x19,             /* 224 mvi  DONE,1      */
        0xa7, 0xf4, 0xff, 0xec,             /* 228 j    200         */
        0xd2, 0x01, 0x0c, 0x14, 0x00, 0x8e, /* 22c mvc  PGM(2),0x8e */
        0x92, 0x02, 0x0c, 0x19,             /* 232 mvi  DONE,2      */
        0xa7, 0xf4, 0xff, 0xe5,             /* 236 j    200         */
    };
    g_autofree uint8_t *image = g_malloc0(G_IMAGE);
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-css-meas-XXXXXX.img", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    /* Short IPL PSW: 31-bit addressing, start at G_CODE. */
    stl_be_p(image, 0x00080000);
    stl_be_p(image + 4, 0x80000000 | G_CODE);
    /* Program new PSW: 31-bit addressing, disabled. */
    stq_be_p(image + 0x1d0, 0x0000000080000000ULL);
    stq_be_p(image + 0x1d8, G_PGM_ENTRY);
    memcpy(image + G_CODE, code, sizeof(code));
    g_assert_cmpint(write(fd, image, G_IMAGE), ==, G_IMAGE);
    close(fd);
    return path;
}

static char *create_disk(void)
{
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-css-meas-XXXXXX.raw", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1024 * 1024), ==, 0);
    close(fd);
    return path;
}

typedef struct Guest {
    QTestState *qts;
    char *image;
    char *disk;
} Guest;

#define DEVNO_DISK 0x0200
#define DEVNO_TERM 0x0300

static void guest_start_machine(Guest *g, const char *machine,
                                const char *machine_opts)
{
    g->image = create_guest_image();
    g->disk = create_disk();
    g->qts = qtest_initf(
        "-nodefaults -accel %s -machine %s%s -m %s -kernel %s "
        "-drive if=none,id=d0,file=%s,format=raw "
        "-device fba-ccw,drive=d0,devno=fe.0.0200 "
        "-chardev ringbuf,id=tn0 "
        "-device x-terminal3270,chardev=tn0,devno=fe.0.0300",
        accel, machine, machine_opts,
        g_str_equal(machine, "czam") ? "1G" : "64M", g->image, g->disk);
}

static void guest_start(Guest *g, const char *machine_opts)
{
    guest_start_machine(g, "s390-ccw-virtio", machine_opts);
}

static void guest_stop(Guest *g)
{
    qtest_quit(g->qts);
    unlink(g->image);
    unlink(g->disk);
    g_free(g->image);
    g_free(g->disk);
}

/*
 * Execute @insn in the guest with general registers 1 and 2 loaded.
 * Returns the condition code, or -1 after a program interruption, whose
 * code is then stored in @pgm.
 */
static int guest_exec(Guest *g, uint32_t insn, uint64_t r1, uint64_t r2,
                      int *pgm)
{
    uint8_t done = 0;
    int i;

    qtest_writel(g->qts, G_INSN, insn);
    qtest_writeq(g->qts, G_R1, r1);
    qtest_writeq(g->qts, G_R2, r2);
    qtest_writeb(g->qts, G_DONE, 0);
    qtest_writeb(g->qts, G_GO, 1);
    for (i = 0; i < 200000; i++) {
        done = qtest_readb(g->qts, G_DONE);
        if (done) {
            break;
        }
        g_usleep(50);
    }
    g_assert_cmpint(done, !=, 0);
    if (done == 2) {
        g_assert_nonnull(pgm);
        *pgm = qtest_readw(g->qts, G_PGM);
        return -1;
    }
    if (pgm) {
        *pgm = 0;
    }
    return (qtest_readl(g->qts, G_CC) >> 28) & 3;
}

static int stsch(Guest *g, uint32_t sid, uint8_t *schib)
{
    int cc = guest_exec(g, INSN_STSCH, sid, 0, NULL);

    if (cc == 0) {
        qtest_memread(g->qts, G_SCHIB, schib, SCHIB_LEN);
    }
    return cc;
}

static int msch(Guest *g, uint32_t sid, const uint8_t *schib, int *pgm)
{
    qtest_memwrite(g->qts, G_SCHIB, schib, SCHIB_LEN);
    return guest_exec(g, INSN_MSCH, sid, 0, pgm);
}

/* SET CHANNEL MONITOR leaves the condition code unchanged. */
static int schm(Guest *g, uint64_t r1, uint64_t r2, int *pgm)
{
    return guest_exec(g, INSN_SCHM, r1, r2, pgm) < 0 ? -1 : 0;
}

/* The subsystem-identification word of the subchannel of device @devno. */
static uint32_t find_sid(Guest *g, uint16_t devno)
{
    uint8_t schib[SCHIB_LEN];
    uint32_t sid;

    for (sid = 0x00010000; sid < 0x00010010; sid++) {
        if (stsch(g, sid, schib) == 0 &&
            lduw_be_p(schib + SCHIB_DEVNO) == devno) {
            return sid;
        }
    }
    g_assert_not_reached();
}

/* Set the measurement controls of a subchannel and enable it. */
static void set_measurement(Guest *g, uint32_t sid, uint16_t mm, uint16_t mbi,
                            bool fmt1, uint64_t mba)
{
    uint8_t schib[SCHIB_LEN];
    uint16_t flags;
    uint32_t chars;

    g_assert_cmpint(stsch(g, sid, schib), ==, 0);
    flags = lduw_be_p(schib + SCHIB_FLAGS);
    flags &= ~(PMCW_MBU | PMCW_DCT);
    flags |= PMCW_ENA | mm;
    stw_be_p(schib + SCHIB_FLAGS, flags);
    stw_be_p(schib + SCHIB_MBI, mbi);
    chars = ldl_be_p(schib + SCHIB_CHARS) & ~PMCW_F;
    stl_be_p(schib + SCHIB_CHARS, chars | (fmt1 ? PMCW_F : 0));
    stq_be_p(schib + SCHIB_MBA, mba);
    g_assert_cmpint(msch(g, sid, schib, NULL), ==, 0);
}

static void set_ccw(Guest *g, uint8_t cmd, uint8_t flags, uint16_t count,
                    uint32_t cda)
{
    uint8_t ccw[8];

    ccw[0] = cmd;
    ccw[1] = flags;
    stw_be_p(ccw + 2, count);
    stl_be_p(ccw + 4, cda);
    qtest_memwrite(g->qts, G_CCWS, ccw, sizeof(ccw));
}

static int ssch(Guest *g, uint32_t sid, uint16_t orb_ctrl0)
{
    uint8_t orb[12] = { 0 };

    stw_be_p(orb + 4, orb_ctrl0 | CCW_ORB_FMT);
    orb[6] = 0xff;
    stl_be_p(orb + 8, G_CCWS);
    qtest_memwrite(g->qts, G_ORB, orb, sizeof(orb));
    return guest_exec(g, INSN_SSCH, sid, 0, NULL);
}

/* Wait for status and clear it; returns the SCSW control bits. */
static uint16_t tsch_wait(Guest *g, uint32_t sid)
{
    int i;

    for (i = 0; i < 20000; i++) {
        if (guest_exec(g, INSN_TSCH, sid, 0, NULL) == 0) {
            return qtest_readw(g->qts, G_IRB + 2);
        }
        g_usleep(100);
    }
    g_assert_not_reached();
}

/* Run a no-operation channel program to completion. */
static void run_nop(Guest *g, uint32_t sid)
{
    uint16_t ctrl;

    set_ccw(g, 0x03, CCW_FLAG_SLI, 1, G_DATA);
    g_assert_cmpint(ssch(g, sid, 0), ==, 0);
    ctrl = tsch_wait(g, sid);
    g_assert_cmphex(ctrl & (SCSW_SECONDARY | SCSW_PENDING), ==,
                    SCSW_SECONDARY | SCSW_PENDING);
    g_assert_cmphex(qtest_readb(g->qts, G_IRB + 8), ==, CCW_DSTAT_CE_DE);
    g_assert_cmphex(qtest_readb(g->qts, G_IRB + 9), ==, 0);
}

static uint16_t mb0_count(Guest *g, uint64_t origin, uint16_t mbi)
{
    return qtest_readw(g->qts, origin + mbi * 32);
}

/* All of a measurement block but its SSCH+RSCH count must stay zero. */
static void assert_times_zero(Guest *g, uint64_t addr, int count_len, int len)
{
    g_autofree uint8_t *mb = g_malloc(len);
    int i;

    qtest_memread(g->qts, addr, mb, len);
    for (i = count_len; i < len; i++) {
        g_assert_cmphex(mb[i], ==, 0);
    }
}

/* CHSC store channel-subsystem characteristics: general bit @bit. */
static bool chsc_general_char(Guest *g, int bit)
{
    uint8_t req[16] = { 0 };
    uint8_t byte;

    qtest_memset(g->qts, G_CHSC, 0, 4096);
    stw_be_p(req, 0x0010);
    stw_be_p(req + 2, 0x0010);
    qtest_memwrite(g->qts, G_CHSC, req, sizeof(req));
    g_assert_cmpint(guest_exec(g, INSN_CHSC, G_CHSC, 0, NULL), ==, 0);
    g_assert_cmphex(qtest_readw(g->qts, G_CHSC + 0x10 + 2), ==, 0x0001);
    byte = qtest_readb(g->qts, G_CHSC + 0x10 + 8 + bit / 8);
    return byte & (0x80 >> (bit % 8));
}

static void test_characteristics(void)
{
    Guest g;

    guest_start(&g, "");
    g_assert_true(chsc_general_char(&g, 48));
    /* The extended-I/O-measurement-word facility is not provided. */
    g_assert_false(chsc_general_char(&g, 50));
    guest_stop(&g);

    guest_start(&g, ",ext-measurement-block=off");
    g_assert_false(chsc_general_char(&g, 48));
    g_assert_false(chsc_general_char(&g, 50));
    guest_stop(&g);
}

/* The machine types that mirror a logical partition of the KVM host */
static void test_characteristics_mainframe(void)
{
    Guest g;

    /* Off by default on the machine types that mirror a partition */
    guest_start_machine(&g, "czam", "");
    g_assert_false(chsc_general_char(&g, 48));
    g_assert_false(chsc_general_char(&g, 50));
    guest_stop(&g);

    guest_start_machine(&g, "czam", ",ext-measurement-block=on");
    g_assert_true(chsc_general_char(&g, 48));
    g_assert_false(chsc_general_char(&g, 50));
    guest_stop(&g);

    guest_start_machine(&g, "czam", ",ext-measurement-block=off");
    g_assert_false(chsc_general_char(&g, 48));
    g_assert_false(chsc_general_char(&g, 50));
    /* The other characteristics are not affected. */
    g_assert_true(chsc_general_char(&g, 46));
    guest_stop(&g);
}

static void test_msch_stsch(void)
{
    Guest g;
    uint8_t schib[SCHIB_LEN], back[SCHIB_LEN];
    uint32_t sid;

    guest_start(&g, "");
    sid = find_sid(&g, DEVNO_DISK);

    /* Initial values */
    g_assert_cmpint(stsch(&g, sid, schib), ==, 0);
    g_assert_cmphex(lduw_be_p(schib + SCHIB_FLAGS) & (PMCW_MBU | PMCW_DCT),
                    ==, 0);
    g_assert_cmphex(lduw_be_p(schib + SCHIB_MBI), ==, 0);
    g_assert_cmphex(ldl_be_p(schib + SCHIB_CHARS) & (PMCW_F | PMCW_X), ==, 0);
    g_assert_cmphex(ldq_be_p(schib + SCHIB_MBA), ==, 0);
    /* No channel-subsystem timing for emulated devices. */
    g_assert_cmphex(lduw_be_p(schib + SCHIB_FLAGS) & PMCW_TF, ==, 0);

    /* Format 0: measurement modes and index */
    set_measurement(&g, sid, PMCW_MBU | PMCW_DCT, 0x1234, false, 0);
    g_assert_cmpint(stsch(&g, sid, back), ==, 0);
    g_assert_cmphex(lduw_be_p(back + SCHIB_FLAGS) & (PMCW_MBU | PMCW_DCT),
                    ==, PMCW_MBU | PMCW_DCT);
    g_assert_cmphex(lduw_be_p(back + SCHIB_MBI), ==, 0x1234);
    g_assert_cmphex(ldl_be_p(back + SCHIB_CHARS) & PMCW_F, ==, 0);

    /* Format 1: format control and a 64-bit measurement-block address */
    set_measurement(&g, sid, PMCW_MBU, 0x1234, true, 0x0000000123456780ULL);
    g_assert_cmpint(stsch(&g, sid, back), ==, 0);
    g_assert_cmphex(lduw_be_p(back + SCHIB_FLAGS) & (PMCW_MBU | PMCW_DCT),
                    ==, PMCW_MBU);
    g_assert_cmphex(ldl_be_p(back + SCHIB_CHARS) & PMCW_F, ==, PMCW_F);
    g_assert_cmphex(ldq_be_p(back + SCHIB_MBA), ==, 0x0000000123456780ULL);
    g_assert_cmphex(lduw_be_p(back + SCHIB_MBI), ==, 0x1234);

    /* Back to the initial state */
    set_measurement(&g, sid, 0, 0, false, 0);
    g_assert_cmpint(stsch(&g, sid, back), ==, 0);
    g_assert_cmphex(lduw_be_p(back + SCHIB_FLAGS) & (PMCW_MBU | PMCW_DCT),
                    ==, 0);
    g_assert_cmphex(ldl_be_p(back + SCHIB_CHARS) & PMCW_F, ==, 0);
    g_assert_cmphex(ldq_be_p(back + SCHIB_MBA), ==, 0);
    guest_stop(&g);
}

static void test_msch_operands(void)
{
    Guest g;
    uint8_t schib[SCHIB_LEN], bad[SCHIB_LEN], back[SCHIB_LEN];
    uint32_t sid;
    int pgm;

    guest_start(&g, "");
    sid = find_sid(&g, DEVNO_DISK);
    g_assert_cmpint(stsch(&g, sid, schib), ==, 0);

    /* A format-1 measurement block lies on a 64-byte boundary. */
    memcpy(bad, schib, SCHIB_LEN);
    stl_be_p(bad + SCHIB_CHARS, ldl_be_p(bad + SCHIB_CHARS) | PMCW_F);
    stq_be_p(bad + SCHIB_MBA, G_MB1 + 0x20);
    g_assert_cmpint(msch(&g, sid, bad, &pgm), ==, -1);
    g_assert_cmphex(pgm, ==, PGM_OPERAND);

    /* The alignment is not checked for format 0. */
    memcpy(bad, schib, SCHIB_LEN);
    stq_be_p(bad + SCHIB_MBA, G_MB1 + 0x20);
    g_assert_cmpint(msch(&g, sid, bad, &pgm), ==, 0);

    /* The extended-I/O-measurement-word facility is not installed. */
    memcpy(bad, schib, SCHIB_LEN);
    stl_be_p(bad + SCHIB_CHARS, ldl_be_p(bad + SCHIB_CHARS) | PMCW_X);
    g_assert_cmpint(msch(&g, sid, bad, &pgm), ==, -1);
    g_assert_cmphex(pgm, ==, PGM_OPERAND);

    /* Bits 0-28 of word 6 are zeros. */
    memcpy(bad, schib, SCHIB_LEN);
    stl_be_p(bad + SCHIB_CHARS, 0x00000008);
    g_assert_cmpint(msch(&g, sid, bad, &pgm), ==, -1);
    g_assert_cmphex(pgm, ==, PGM_OPERAND);

    /* A rejected MODIFY SUBCHANNEL changes nothing. */
    g_assert_cmpint(stsch(&g, sid, back), ==, 0);
    g_assert_cmphex(ldl_be_p(back + SCHIB_CHARS) & (PMCW_F | PMCW_X), ==, 0);
    guest_stop(&g);

    /* Without the extended-I/O-measurement-block facility */
    guest_start(&g, ",ext-measurement-block=off");
    sid = find_sid(&g, DEVNO_DISK);
    g_assert_cmpint(stsch(&g, sid, schib), ==, 0);
    memcpy(bad, schib, SCHIB_LEN);
    stl_be_p(bad + SCHIB_CHARS, ldl_be_p(bad + SCHIB_CHARS) | PMCW_F);
    stq_be_p(bad + SCHIB_MBA, G_MB1);
    g_assert_cmpint(msch(&g, sid, bad, &pgm), ==, -1);
    g_assert_cmphex(pgm, ==, PGM_OPERAND);
    /* Words 10 and 11 are not a measurement-block address. */
    memcpy(bad, schib, SCHIB_LEN);
    stq_be_p(bad + SCHIB_MBA, G_MB1);
    g_assert_cmpint(msch(&g, sid, bad, &pgm), ==, 0);
    g_assert_cmpint(stsch(&g, sid, back), ==, 0);
    g_assert_cmphex(ldq_be_p(back + SCHIB_MBA), ==, 0);
    /* Format-0 measurement still works. */
    qtest_memset(g.qts, G_MBO_A, 0, 0x1000);
    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_A, NULL), ==, 0);
    set_measurement(&g, sid, PMCW_MBU, 2, false, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 2), ==, 1);
    guest_stop(&g);
}

static void test_schm_operands(void)
{
    Guest g;
    int pgm;

    guest_start(&g, "");
    /* Bits 36-61 of general register 1 are zeros. */
    g_assert_cmpint(schm(&g, 0x4, 0, &pgm), ==, -1);
    g_assert_cmphex(pgm, ==, PGM_OPERAND);
    g_assert_cmpint(schm(&g, 0x08000000 | SCHM_M, G_MBO_A, &pgm), ==, -1);
    g_assert_cmphex(pgm, ==, PGM_OPERAND);
    /* The measurement-block origin lies on a 32-byte boundary. */
    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_A + 0x10, &pgm), ==, -1);
    g_assert_cmphex(pgm, ==, PGM_OPERAND);
    /* General register 2 is ignored when M is zero. */
    g_assert_cmpint(schm(&g, SCHM_D, 0x11, &pgm), ==, 0);
    g_assert_cmpint(pgm, ==, 0);
    /* Bits 0-31 are ignored; the key and both modes are accepted. */
    g_assert_cmpint(schm(&g, 0xffffffff00000000ULL | 0x10000000 | SCHM_M |
                         SCHM_D, G_MBO_A, &pgm), ==, 0);
    g_assert_cmpint(schm(&g, 0, 0, &pgm), ==, 0);
    guest_stop(&g);
}

static void test_format0_update(void)
{
    Guest g;
    uint32_t sid;

    guest_start(&g, "");
    sid = find_sid(&g, DEVNO_DISK);
    qtest_memset(g.qts, G_MBO_A, 0, 0x1000);
    qtest_memset(g.qts, G_MBO_B, 0, 0x1000);

    /* Subchannel enabled, mode inactive: nothing is stored. */
    set_measurement(&g, sid, PMCW_MBU, 3, false, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 0);

    /* Mode active: the SSCH+RSCH count of block 3 is incremented. */
    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_A, NULL), ==, 0);
    run_nop(&g, sid);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 2);
    /*
     * Without the channel-subsystem-timing facility the count is the only
     * measurement data; the sample count and the times stay zero.
     */
    assert_times_zero(&g, G_MBO_A + 3 * 32, 2, 32);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 0), ==, 0);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 2), ==, 0);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 4), ==, 0);

    /* The count wraps around. */
    qtest_writew(g.qts, G_MBO_A + 3 * 32, 0xffff);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 0);
    assert_times_zero(&g, G_MBO_A + 3 * 32, 2, 32);

    /* Only the device-connect-time mode enabled: no block update. */
    set_measurement(&g, sid, PMCW_DCT, 3, false, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 0);
    g_assert_cmpint(schm(&g, SCHM_M | SCHM_D, G_MBO_A, NULL), ==, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 0);

    /* A new index takes effect with the next start. */
    set_measurement(&g, sid, PMCW_MBU | PMCW_DCT, 5, false, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 5), ==, 1);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 0);

    /* SET CHANNEL MONITOR while active replaces the origin. */
    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_B, NULL), ==, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_B, 5), ==, 1);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 5), ==, 1);

    /* Mode inactive again: nothing is stored. */
    g_assert_cmpint(schm(&g, SCHM_D, 0, NULL), ==, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_B, 5), ==, 1);

    /* Subchannel not enabled for the mode: nothing is stored. */
    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_B, NULL), ==, 0);
    set_measurement(&g, sid, 0, 5, false, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_B, 5), ==, 1);
    guest_stop(&g);
}

static void test_format1_update(void)
{
    Guest g;
    uint32_t sid;

    guest_start(&g, "");
    sid = find_sid(&g, DEVNO_DISK);
    qtest_memset(g.qts, G_MBO_A, 0, 0x1000);
    qtest_memset(g.qts, G_MB1, 0, 0x100);

    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_A, NULL), ==, 0);
    set_measurement(&g, sid, PMCW_MBU, 3, true, G_MB1 + 0x40);
    run_nop(&g, sid);
    run_nop(&g, sid);
    run_nop(&g, sid);
    /* A 32-bit count in the block the subchannel designates ... */
    g_assert_cmpint(qtest_readl(g.qts, G_MB1 + 0x40), ==, 3);
    assert_times_zero(&g, G_MB1 + 0x40, 4, 64);
    assert_times_zero(&g, G_MB1, 0, 64);
    assert_times_zero(&g, G_MB1 + 0x80, 0, 64);
    /* ... and none in the format-0 area. */
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 0);

    /* The count wraps around at 32 bits. */
    qtest_writel(g.qts, G_MB1 + 0x40, 0xffffffff);
    run_nop(&g, sid);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1 + 0x40), ==, 0);

    /* Switching the subchannel back to format 0 */
    set_measurement(&g, sid, PMCW_MBU, 3, false, 0);
    run_nop(&g, sid);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 3), ==, 1);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1 + 0x40), ==, 0);

    /* The mode is inactive: no format-1 update either. */
    g_assert_cmpint(schm(&g, 0, 0, NULL), ==, 0);
    set_measurement(&g, sid, PMCW_MBU, 3, true, G_MB1 + 0x40);
    run_nop(&g, sid);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1 + 0x40), ==, 0);
    guest_stop(&g);
}

/*
 * The measurement block is updated when the operation completes at the
 * device (or is suspended), not when START SUBCHANNEL is accepted.
 */
static void test_update_at_completion(void)
{
    Guest g;
    uint8_t schib[SCHIB_LEN];
    uint32_t sid;
    uint16_t ctrl;

    guest_start(&g, "");
    sid = find_sid(&g, DEVNO_TERM);
    qtest_memset(g.qts, G_MB1, 0, 0x100);
    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_A, NULL), ==, 0);
    set_measurement(&g, sid, PMCW_MBU, 0, true, G_MB1);

    /* A read from a terminal without a client stays pending. */
    set_ccw(&g, 0x06, CCW_FLAG_SLI, 16, G_DATA);
    g_assert_cmpint(ssch(&g, sid, 0), ==, 0);
    g_assert_cmpint(stsch(&g, sid, schib), ==, 0);
    g_assert_cmphex(lduw_be_p(schib + SCHIB_SCSW_CTRL) & SCSW_PENDING, ==, 0);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1), ==, 0);

    /* HALT SUBCHANNEL ends it; the data is stored with the status. */
    g_assert_cmpint(guest_exec(&g, INSN_HSCH, sid, 0, NULL), ==, 0);
    ctrl = tsch_wait(&g, sid);
    g_assert_cmphex(ctrl & SCSW_PENDING, ==, SCSW_PENDING);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1), ==, 1);

    /* CLEAR SUBCHANNEL discards the operation and its measurement. */
    g_assert_cmpint(ssch(&g, sid, 0), ==, 0);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1), ==, 1);
    g_assert_cmpint(guest_exec(&g, INSN_CSCH, sid, 0, NULL), ==, 0);
    tsch_wait(&g, sid);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1), ==, 1);

    /* The next completed operation counts once. */
    run_nop(&g, sid);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1), ==, 2);
    guest_stop(&g);
}

static void test_suspend_resume(void)
{
    Guest g;
    uint8_t schib[SCHIB_LEN];
    uint32_t sid;
    uint16_t ctrl;

    guest_start(&g, "");
    sid = find_sid(&g, DEVNO_DISK);
    qtest_memset(g.qts, G_MBO_A, 0, 0x1000);
    g_assert_cmpint(schm(&g, SCHM_M, G_MBO_A, NULL), ==, 0);
    set_measurement(&g, sid, PMCW_MBU, 7, false, 0);

    /* A suspend flag in the first CCW with suspend control in the ORB */
    set_ccw(&g, 0x03, CCW_FLAG_SLI | 0x02, 1, G_DATA);
    g_assert_cmpint(ssch(&g, sid, 0x0800), ==, 0);
    g_assert_cmpint(stsch(&g, sid, schib), ==, 0);
    g_assert_cmphex(lduw_be_p(schib + SCHIB_SCSW_CTRL) & SCSW_SUSPENDED, ==,
                    SCSW_SUSPENDED);
    /* The suspension stores measurement data. */
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 7), ==, 1);

    /* Resume without the flag: the completion counts again. */
    set_ccw(&g, 0x03, CCW_FLAG_SLI, 1, G_DATA);
    g_assert_cmpint(guest_exec(&g, INSN_RSCH, sid, 0, NULL), ==, 0);
    ctrl = tsch_wait(&g, sid);
    g_assert_cmphex(ctrl & SCSW_SECONDARY, ==, SCSW_SECONDARY);
    g_assert_cmpint(mb0_count(&g, G_MBO_A, 7), ==, 2);
    guest_stop(&g);
}

/* Measurement blocks which cannot be addressed are not stored. */
static void test_invalid_block_address(void)
{
    Guest g;
    uint32_t sid;

    guest_start(&g, "");
    sid = find_sid(&g, DEVNO_DISK);
    /* Format 0: origin plus index beyond 2G - 1 */
    g_assert_cmpint(schm(&g, SCHM_M, 0x7fffffe0, NULL), ==, 0);
    set_measurement(&g, sid, PMCW_MBU, 1, false, 0);
    run_nop(&g, sid);
    /* Format 1: outside of the configuration */
    set_measurement(&g, sid, PMCW_MBU, 1, true, 0x0000100000000000ULL);
    run_nop(&g, sid);
    /* The subchannel keeps working. */
    qtest_memset(g.qts, G_MB1, 0, 0x100);
    set_measurement(&g, sid, PMCW_MBU, 1, true, G_MB1);
    run_nop(&g, sid);
    g_assert_cmpint(qtest_readl(g.qts, G_MB1), ==, 1);
    guest_stop(&g);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    accel = ccw_prog_accel();
    if (!accel || !qtest_has_device("fba-ccw") ||
        !qtest_has_device("x-terminal3270")) {
        g_test_skip("no accelerator or device to run the guest program");
        return g_test_run();
    }
    qtest_add_func("/css-measurement/characteristics",
                   test_characteristics);
    if (g_str_equal(accel, "kvm") && qtest_has_machine("czam")) {
        qtest_add_func("/css-measurement/characteristics/mainframe",
                       test_characteristics_mainframe);
    }
    qtest_add_func("/css-measurement/msch-stsch", test_msch_stsch);
    qtest_add_func("/css-measurement/msch-operands",
                   test_msch_operands);
    qtest_add_func("/css-measurement/schm-operands",
                   test_schm_operands);
    qtest_add_func("/css-measurement/format0", test_format0_update);
    qtest_add_func("/css-measurement/format1", test_format1_update);
    qtest_add_func("/css-measurement/completion",
                   test_update_at_completion);
    qtest_add_func("/css-measurement/suspend-resume",
                   test_suspend_resume);
    qtest_add_func("/css-measurement/invalid-address",
                   test_invalid_block_address);
    return g_test_run();
}
