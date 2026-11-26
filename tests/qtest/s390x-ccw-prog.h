/*
 * Run channel programs on emulated s390x CCW devices from a qtest
 *
 * A tiny guest program, IPLed as a raw kernel image, enables the
 * subchannel and then waits for the test.  Each time the test sets the GO
 * byte, the program issues START SUBCHANNEL with the ORB at CCW_PROG_ORB,
 * polls TEST SUBCHANNEL until status is pending, stores the IRB at
 * CCW_PROG_IRB and sets the DONE byte.  The test writes CCWs, data and IDAWs
 * anywhere in guest memory with qtest_memwrite() and checks the IRB.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TESTS_QTEST_S390X_CCW_PROG_H
#define TESTS_QTEST_S390X_CCW_PROG_H

#include "libqtest.h"
#include "qemu/bswap.h"

#define CCW_PROG_CODE    0x200
#define CCW_PROG_CCWS    0x800   /* room for 32 format-1 CCWs */
#define CCW_PROG_ORB     0x900
#define CCW_PROG_IRB     0xa00
#define CCW_PROG_SCHIB   0xb00
#define CCW_PROG_SID     0xc00
#define CCW_PROG_SSCH_CC 0xc04
#define CCW_PROG_GO      0xc10
#define CCW_PROG_DONE    0xc11
#define CCW_PROG_IMAGE   0xc20
#define CCW_PROG_DATA    0x1000  /* free for the test from here */

#define CCW_FLAG_DC  0x80
#define CCW_FLAG_CC  0x40
#define CCW_FLAG_SLI 0x20
#define CCW_FLAG_SKIP 0x10
#define CCW_FLAG_IDA 0x04

#define CCW_ORB_FMT  0x0080
#define CCW_ORB_C64  0x0002

#define CCW_DSTAT_CE_DE 0x0c
#define CCW_DSTAT_UC    0x02
#define CCW_CSTAT_PROG_CHECK 0x20

/* The subsystem-identification word of subchannel 0.0.@schno. */
#define CCW_PROG_SID_OF(schno) (0x00010000u | (schno))

typedef struct CcwProgIrb {
    uint32_t cpa;
    uint8_t dstat;
    uint8_t cstat;
    uint16_t count;
    uint8_t sense0;
} CcwProgIrb;

/*
 * Write the raw IPL image of the guest program to a temporary file.
 * @sid is the subsystem-identification word of the subchannel to drive.
 */
static inline char *ccw_prog_create_image(uint32_t sid)
{
    static const uint8_t code[] = {
        0x58, 0x10, 0x0c, 0x00,     /* 200 l     %r1,SID         */
        0xb2, 0x34, 0x0b, 0x00,     /* 204 stsch SCHIB           */
        0x96, 0x80, 0x0b, 0x05,     /* 208 oi    SCHIB+5,0x80    */
        0xb2, 0x32, 0x0b, 0x00,     /* 20c msch  SCHIB           */
        0x95, 0x00, 0x0c, 0x10,     /* 210 cli   GO,0            */
        0xa7, 0x84, 0xff, 0xfe,     /* 214 je    210             */
        0x92, 0x00, 0x0c, 0x10,     /* 218 mvi   GO,0            */
        0xb2, 0x33, 0x09, 0x00,     /* 21c ssch  ORB             */
        0xb2, 0x22, 0x00, 0x20,     /* 220 ipm   %r2             */
        0x50, 0x20, 0x0c, 0x04,     /* 224 st    %r2,SSCH_CC     */
        0xb2, 0x35, 0x0a, 0x00,     /* 228 tsch  IRB             */
        0xa7, 0x74, 0xff, 0xfe,     /* 22c jnz   228             */
        0x92, 0x01, 0x0c, 0x11,     /* 230 mvi   DONE,1          */
        0xa7, 0xf4, 0xff, 0xee,     /* 234 j     210             */
    };
    g_autofree uint8_t *image = g_malloc0(CCW_PROG_IMAGE);
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-ccw-prog-XXXXXX.img", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    /* Short IPL PSW: 31-bit addressing, start at CCW_PROG_CODE. */
    stl_be_p(image, 0x00080000);
    stl_be_p(image + 4, 0x80000000 | CCW_PROG_CODE);
    memcpy(image + CCW_PROG_CODE, code, sizeof(code));
    stl_be_p(image + CCW_PROG_SID, sid);
    g_assert_cmpint(write(fd, image, CCW_PROG_IMAGE), ==, CCW_PROG_IMAGE);
    close(fd);
    return path;
}

static inline void ccw_prog_set_ccw(QTestState *qts, unsigned int index,
                                    uint8_t cmd, uint8_t flags,
                                    uint16_t count, uint32_t cda)
{
    uint8_t ccw[8];

    ccw[0] = cmd;
    ccw[1] = flags;
    stw_be_p(ccw + 2, count);
    stl_be_p(ccw + 4, cda);
    qtest_memwrite(qts, CCW_PROG_CCWS + index * 8, ccw, sizeof(ccw));
}

/* Start the channel program at CCW_PROG_CCWS. */
static inline void ccw_prog_go(QTestState *qts, uint16_t orb_ctrl0)
{
    uint8_t orb[12] = { 0 };

    stw_be_p(orb + 4, orb_ctrl0);
    orb[6] = 0xff;                  /* logical-path mask */
    stl_be_p(orb + 8, CCW_PROG_CCWS);
    qtest_memwrite(qts, CCW_PROG_ORB, orb, sizeof(orb));
    qtest_memset(qts, CCW_PROG_IRB, 0, 96);
    qtest_writeb(qts, CCW_PROG_DONE, 0);
    qtest_writeb(qts, CCW_PROG_GO, 1);
}

/*
 * Wait for the final status of the channel program started by
 * ccw_prog_go().  Returns the condition code of START SUBCHANNEL.
 */
static inline int ccw_prog_wait(QTestState *qts, CcwProgIrb *irb)
{
    uint8_t scsw[12];
    uint32_t ipm;
    int i;

    for (i = 0; i < 200000 && !qtest_readb(qts, CCW_PROG_DONE); i++) {
        g_usleep(50);
    }
    g_assert_cmpint(qtest_readb(qts, CCW_PROG_DONE), ==, 1);
    qtest_memread(qts, CCW_PROG_IRB, scsw, sizeof(scsw));
    irb->cpa = ldl_be_p(scsw + 4);
    irb->dstat = scsw[8];
    irb->cstat = scsw[9];
    irb->count = lduw_be_p(scsw + 10);
    /* The IRB: SCSW (12 bytes), ESW (20), ECW: sense byte 0 first. */
    irb->sense0 = qtest_readb(qts, CCW_PROG_IRB + 32);
    ipm = qtest_readl(qts, CCW_PROG_SSCH_CC);
    return (ipm >> 28) & 3;
}

/* Run the channel program at CCW_PROG_CCWS and wait for its status. */
static inline int ccw_prog_run(QTestState *qts, uint16_t orb_ctrl0,
                               CcwProgIrb *irb)
{
    ccw_prog_go(qts, orb_ctrl0);
    return ccw_prog_wait(qts, irb);
}

/*
 * The accelerator that runs the guest program: KVM only (this tree does not
 * run guests under TCG); NULL where the host does not offer it.
 */
static inline const char *ccw_prog_accel(void)
{
    if (qtest_has_accel("kvm") && !access("/dev/kvm", R_OK | W_OK)) {
        return "kvm";
    }
    return NULL;
}

/* Start QEMU with the guest program and the given devices. */
static inline QTestState *ccw_prog_start(const char *image, const char *mem,
                                         const char *devices)
{
    return qtest_initf("-nodefaults -accel %s -m %s -kernel %s %s",
                       ccw_prog_accel(), mem, image, devices);
}

#endif
