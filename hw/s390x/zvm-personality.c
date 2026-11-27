/*
 * z/VM personality of the s390-ccw-virtio machine (zvm-personality=on)
 *
 * Some guest drivers work only under z/VM; Linux, for instance, uses IUCV
 * (netiucv, af_iucv) only when STORE SYSTEM INFORMATION 3.2.2 names z/VM
 * as the hypervisor. With the personality the machine says so there, STORE
 * CPU ID reports version X'FF', and DIAGNOSE X'08' answers the few CP
 * commands such guests issue on their own. It is meant for guests that use
 * the IUCV services of QEMU; it is not a z/VM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "hw/s390x/s390-virtio-ccw.h"
#include "hw/s390x/ebcdic.h"
#include "system/address-spaces.h"
#include "cpu.h"
#include "target/s390x/s390x-internal.h"
#include "trace.h"

#define DIAG8_RESPONSE  0x40000000U
#define DIAG8_CMD_MAX   240

static const char *zvm_response(const char *cmd, int *rc, char *buf,
                                size_t len)
{
    *rc = 0;
    if (!strcmp(cmd, "QUERY USERID") || !strcmp(cmd, "Q USERID")) {
        snprintf(buf, len, "%-8s AT QEMU", s390_zvm_userid());
        return buf;
    }
    if (g_str_has_prefix(cmd, "QUERY CONS") ||
        g_str_has_prefix(cmd, "Q CONS")) {
        /* no virtual console device: the guest uses its SCLP console */
        return "CONS 0009 ON LDEV L0000   TERM START\n"
               "0009 CL T NOCONT NOHOLD COPY 001    READY FORM STANDARD\n"
               "0009 TO       SUBCHANNEL = 0000";
    }
    if (g_str_has_prefix(cmd, "QUERY TERM") ||
        g_str_has_prefix(cmd, "Q TERM")) {
        return "LINEND #, LINEDEL OFF, CHARDEL OFF, ESCAPE \", TABCHAR ON\n"
               "LINESIZE 080, ATTN OFF, APL OFF, TEXT OFF, MODE VM, "
               "HILIGHT OFF";
    }
    if (g_str_has_prefix(cmd, "QUERY CPLEVEL") ||
        g_str_has_prefix(cmd, "Q CPLEVEL")) {
        return "QEMU z/VM personality";
    }
    if (g_str_has_prefix(cmd, "TERM ") || g_str_has_prefix(cmd, "TERMINAL ") ||
        g_str_has_prefix(cmd, "SET SMSG")) {
        return "";
    }
    *rc = 1;
    snprintf(buf, len, "Unknown CP command: %s", cmd);
    return buf;
}

/* DIAGNOSE X'08': CP command, optionally with its response */
void s390_zvm_diag8(S390CPU *cpu, int rx, int ry)
{
    CPUS390XState *env = &cpu->env;
    uint32_t ylen = env->regs[ry];
    uint32_t clen = ylen & 0x00ffffff;
    bool want = ylen & DIAG8_RESPONSE;
    uint8_t ebc[DIAG8_CMD_MAX];
    char cmd[DIAG8_CMD_MAX + 1], buf[DIAG8_CMD_MAX + 32];
    const char *resp;
    g_autofree uint8_t *out = NULL;
    uint32_t rlen, n;
    int rc, i;

    if (clen > DIAG8_CMD_MAX || (want && ((rx & 1) || (ry & 1)))) {
        s390_program_interrupt(env, PGM_SPECIFICATION, RA_IGNORED);
        return;
    }
    if (address_space_read(&address_space_memory,
                           mmu_real2abs(env, env->regs[rx] & 0x7fffffff),
                           MEMTXATTRS_UNSPECIFIED, ebc, clen) != MEMTX_OK) {
        s390_program_interrupt(env, PGM_ADDRESSING, RA_IGNORED);
        return;
    }
    ascii_put((uint8_t *)cmd, (const char *)ebc, clen);
    cmd[clen] = 0;
    for (i = 0; cmd[i]; i++) {
        cmd[i] = g_ascii_toupper(cmd[i]);
    }
    g_strstrip(cmd);
    resp = zvm_response(cmd, &rc, buf, sizeof(buf));
    trace_zvm_diag8(cmd, rc);

    if (want) {
        rlen = strlen(resp);
        out = g_malloc(rlen + 1);
        ebcdic_put(out, resp, rlen);
        for (i = 0; i < rlen; i++) {
            if (resp[i] == '\n') {
                out[i] = 0x15;          /* EBCDIC new line */
            }
        }
        n = MIN(rlen, (uint32_t)env->regs[ry + 1]);
        if (n && address_space_write(&address_space_memory,
                        mmu_real2abs(env, env->regs[rx + 1] & 0x7fffffff),
                        MEMTXATTRS_UNSPECIFIED, out, n) != MEMTX_OK) {
            s390_program_interrupt(env, PGM_ADDRESSING, RA_IGNORED);
            return;
        }
        if (rlen > (uint32_t)env->regs[ry + 1]) {
            env->regs[ry + 1] = deposit64(env->regs[ry + 1], 0, 32,
                                          rlen - (uint32_t)env->regs[ry + 1]);
            setcc(cpu, 1);
        } else {
            env->regs[ry + 1] = deposit64(env->regs[ry + 1], 0, 32, rlen);
            setcc(cpu, 0);
        }
    } else {
        setcc(cpu, 0);
    }
    env->regs[ry] = deposit64(env->regs[ry], 0, 32, rc);
}
