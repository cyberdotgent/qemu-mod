/*
 * Inter-user communication vehicle (IUCV) provided by QEMU
 *
 * QEMU executes the IUCV instruction (B2F0) of the guest the way z/VM's
 * CP does and presents the IUCV external interruptions (code 0x4000).
 * The guest's partners are services inside QEMU (TYPE_IUCV_SERVICE).
 *
 * Written from the description of the IUCV functions in z/VM CP
 * Programming Services and the behaviour of guests that use them.
 *
 * Without any IUCV device, B2F0 keeps its operation exception (as on a
 * machine without a hypervisor). "-device s390-iucv,available=off" makes
 * it end with condition code 3 in the supervisor state, which is what a
 * guest sees under a hypervisor that does not offer IUCV to it.
 *
 * Not provided: control buffers and control paths, paths between two
 * guests, connections a service opens to the guest, TEST MESSAGE, the
 * storage-key checks of the data areas.

 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/atomic.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/boards.h"
#include "hw/s390x/iucv.h"
#include "hw/s390x/ebcdic.h"
#include "migration/blocker.h"
#include "system/reset.h"
#include "system/address-spaces.h"
#include "system/kvm.h"
#include "cpu.h"
#include "target/s390x/s390x-internal.h"
#include "target/s390x/kvm/kvm_s390x.h"
#include "trace.h"
#include "hw/s390x/s390-virtio-ccw.h"
#include <linux/kvm.h>

/* function codes in general register 0 */
enum {
    IUCV_QUERY = 0,
    IUCV_TESTMSG = 1,
    IUCV_RTRVBFR = 2,
    IUCV_DESCRIBE = 3,
    IUCV_SEND = 4,
    IUCV_RECEIVE = 5,
    IUCV_REPLY = 6,
    IUCV_TESTCMPL = 7,
    IUCV_REJECT = 8,
    IUCV_PURGE = 9,
    IUCV_ACCEPT = 10,
    IUCV_CONNECT = 11,
    IUCV_DCLBFR = 12,
    IUCV_QUIESCE = 13,
    IUCV_RESUME = 14,
    IUCV_SEVER = 15,
    IUCV_SETMASK = 16,
    IUCV_SETCMASK = 17,
    IUCV_IPOLL = 21,
};

/* parameter list (IPARML) and interruption record offsets */
#define P_PATHID        0x00
#define P_FLAGS1        0x02
#define P_RCODE         0x03    /* IPTYPE in an interruption record */
#define P_MSGLIM        0x04
#define P_MSGID         0x04
#define P_VMID          0x08
#define P_TRGCLS        0x08
#define P_AUDIT         0x08
#define P_BFADR1        0x0c
#define P_RMMSG         0x0c
#define P_BFLN1F        0x10
#define P_USER          0x10
#define P_SRCCLS        0x14
#define P_MSGTAG        0x18
#define P_BFADR2        0x1c
#define P_BFLN2F        0x20
#define P_POLLFG        0x24
#define P_LEN           40

/* IPFLAGS1 */
#define F_RMDATA        0x80    /* also IPALL */
#define F_BUFLST        0x40    /* also IPQUSCE */
#define F_PRTY          0x20
#define F_NORPY         0x10
#define F_ANSLST        0x08    /* also IPAPPC */
#define F_FGMID         0x04    /* also IPCNTRL */
#define F_FGPID         0x02
#define F_FGMCL         0x01

/* IPAUDIT */
#define AUD_RPLE        0x800000    /* reply longer than the answer area */
#define AUD_ANAX        0x080000    /* addressing exception, answer area */
#define AUD_RJCT        0x040000    /* rejected */
#define AUD_SVRD        0x000800    /* path severed */

/* interruption types, which are also their order of presentation */
enum {
    T_CONN_PENDING = 1,
    T_CONN_COMPLETE,
    T_SEVERED,
    T_QUIESCED,
    T_RESUMED,
    T_MSG_COMPLETE_PRTY,
    T_MSG_COMPLETE,
    T_MSG_PENDING_PRTY,
    T_MSG_PENDING,
};

#define IUCV_EXT_CODE   0x4000
#define CR0_IUCV        0x0000000000000002ULL   /* bit 62 */
#define MSGLIM_DEFAULT  10

typedef struct IUCVIrq {
    QTAILQ_ENTRY(IUCVIrq) next;
    uint8_t type;
    uint16_t pathid;
    uint32_t msgid;
    uint8_t rec[P_LEN];
} IUCVIrq;

typedef enum { PATH_ACTIVE, PATH_SEVERED } IUCVPathState;

/* a message on a path, in either direction */
typedef struct IUCVMsg {
    QTAILQ_ENTRY(IUCVMsg) next;
    uint32_t id;
    bool from_guest;
    bool two_way;
    bool priority;
    bool prmdata;
    uint32_t trgcls;
    /* from the guest: what comes back with the completion */
    uint32_t srccls;
    uint32_t tag;
    uint32_t ans_addr;
    uint32_t ans_len;
    uint32_t ans_amask;
    bool ans_list;
    IUCVMessage pub;
    /* from the service */
    uint8_t *data;
    uint32_t len;
    uint32_t done;              /* bytes the guest received */
    uint32_t reply_max;
    bool received;              /* two-way: waiting for REPLY */
    bool busy;                  /* the service's message() runs */
    bool dead;                  /* completed during message(): free after */
} IUCVMsg;

struct IUCVPath {
    uint16_t id;
    IUCVPathState state;
    IUCVService *svc;
    void *opaque;
    uint16_t msglim;
    bool guest_prmdata;         /* the guest takes data in the plist */
    bool guest_quiesced;        /* the guest takes no messages */
    bool svc_quiesced;
    uint8_t user[16];           /* user data of the completion */
    unsigned guest_out;         /* two-way messages of the guest */
    unsigned svc_out;           /* messages of the service not finished */
    QTAILQ_HEAD(, IUCVMsg) msgs;
};

typedef struct IUCVCpu {
    bool declared;
    uint32_t buffer;
    uint8_t mask;
    uint8_t cmask;
    bool pending;               /* read without the BQL */
} IUCVCpu;

struct S390IUCVState {
    DeviceState parent_obj;

    char *userid_str;
    bool available;
    uint32_t maxconn;

    uint8_t userid[8];
    IUCVCpu *cpus;
    unsigned ncpus;
    IUCVPath **paths;
    uint32_t next_msgid;
    QTAILQ_HEAD(, IUCVIrq) irqs;
    QTAILQ_HEAD(, IUCVService) services;
    Error *migration_blocker;
};

static S390IUCVState *iucv_state;

/* guest memory */

static bool abs_read(hwaddr addr, void *buf, hwaddr len)
{
    return address_space_read(&address_space_memory, addr,
                              MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

static bool abs_write(hwaddr addr, const void *buf, hwaddr len)
{
    return address_space_write(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED, buf, len) == MEMTX_OK;
}

static uint16_t get16(const uint8_t *p, int off)
{
    return lduw_be_p(p + off);
}

static uint32_t get32(const uint8_t *p, int off)
{
    return ldl_be_p(p + off);
}

static void put16(uint8_t *p, int off, uint16_t v)
{
    stw_be_p(p + off, v);
}

static void put32(uint8_t *p, int off, uint32_t v)
{
    stl_be_p(p + off, v);
}

/* address fields are 31 bits wide, 24 in the 24-bit addressing mode */
static uint32_t addr_mask(CPUS390XState *env)
{
    return env->psw.mask & (PSW_MASK_32 | PSW_MASK_64) ? 0x7fffffff :
                                                         0x00ffffff;
}

/*
 * A data area of the guest: a buffer, or a list of address and length
 * pairs whose lengths add up to @total.
 */
typedef struct IUCVArea {
    bool list;
    uint32_t addr;
    uint32_t total;
    uint32_t amask;             /* addressing mode of the guest */
} IUCVArea;

/* returns 0, or an IPRCODE for a bad list; -1 for an addressing error */
static int area_check(IUCVArea *a, int rc_align)
{
    uint64_t sum = 0;
    uint32_t n;

    if (a->total & 0x80000000) {
        return 0x0a;
    }
    if (!a->list) {
        return 0;
    }
    if (a->addr & 7) {
        return rc_align;
    }
    for (n = 0; sum < a->total; n++) {
        uint8_t e[8];

        if (!abs_read((a->addr + n * 8) & a->amask, e, 8)) {
            return -1;
        }
        if (get32(e, 4) & 0x80000000) {
            return 0x17;
        }
        sum += get32(e, 4);
        if (n > 65536) {
            return 0x16;
        }
    }
    return sum == a->total ? 0 : 0x18;
}

/*
 * Copy between QEMU and a guest area, starting @skip bytes into it.
 * Returns false on an addressing error.
 */
static bool area_rw(IUCVArea *a, uint32_t skip,
                    uint8_t *buf, uint32_t len, bool write)
{
    uint32_t n = 0;

    if (!a->list) {
        hwaddr ad = (a->addr + skip) & a->amask;

        return write ? abs_write(ad, buf, len) : abs_read(ad, buf, len);
    }
    while (len) {
        uint8_t e[8];
        uint32_t eaddr, elen, chunk;

        if (!abs_read((a->addr + n++ * 8) & a->amask, e, 8)) {
            return false;
        }
        eaddr = get32(e, 0);
        elen = get32(e, 4);
        if (skip >= elen) {
            skip -= elen;
            continue;
        }
        chunk = MIN(elen - skip, len);
        if (write ? !abs_write((eaddr + skip) & a->amask, buf, chunk) :
                    !abs_read((eaddr + skip) & a->amask, buf, chunk)) {
            return false;
        }
        skip = 0;
        buf += chunk;
        len -= chunk;
    }
    return true;
}

/* interruptions */

static bool cpu_enabled_for(IUCVCpu *c, uint8_t type)
{
    static const uint8_t mask_bit[] = {
        [T_MSG_COMPLETE_PRTY] = 0x10, [T_MSG_COMPLETE] = 0x20,
        [T_MSG_PENDING_PRTY] = 0x40, [T_MSG_PENDING] = 0x80,
    };

    if (!c->declared) {
        return false;
    }
    if (type <= T_RESUMED) {
        return (c->mask & 0x08) && (c->cmask & (0x80 >> (type - 1)));
    }
    return c->mask & mask_bit[type];
}

/* the next interruption @c can take: control ones in order, then by type */
static IUCVIrq *next_irq(S390IUCVState *s, IUCVCpu *c)
{
    IUCVIrq *irq, *best = NULL;

    QTAILQ_FOREACH(irq, &s->irqs, next) {
        if (!c || cpu_enabled_for(c, irq->type)) {
            if (irq->type <= T_RESUMED) {
                return irq;
            }
            if (!best || irq->type < best->type) {
                best = irq;
            }
        }
    }
    return best;
}

static bool msg_irq_queued(S390IUCVState *s)
{
    IUCVIrq *irq;

    QTAILQ_FOREACH(irq, &s->irqs, next) {
        if (irq->type > T_RESUMED) {
            return true;
        }
    }
    return false;
}

static void update_pending(S390IUCVState *s)
{
    CPUState *cs;

    CPU_FOREACH(cs) {
        IUCVCpu *c;
        bool p;

        if (cs->cpu_index >= s->ncpus) {
            continue;
        }
        c = &s->cpus[cs->cpu_index];
        p = c->declared && next_irq(s, c);
        if (p && !qatomic_read(&c->pending)) {
            qatomic_set(&c->pending, true);
            if (!qemu_cpu_is_self(cs)) {
                kvm_s390_user_ext_irq_kick(S390_CPU(cs));
            }
        } else if (!p) {
            qatomic_set(&c->pending, false);
        }
    }
}

static void queue_irq(S390IUCVState *s, uint8_t type, uint16_t pathid,
                      uint32_t msgid, uint8_t *rec)
{
    IUCVIrq *irq = g_new0(IUCVIrq, 1);

    irq->type = type;
    irq->pathid = pathid;
    irq->msgid = msgid;
    memcpy(irq->rec, rec, P_LEN);
    irq->rec[P_RCODE] = type;
    put16(irq->rec, P_PATHID, pathid);
    QTAILQ_INSERT_TAIL(&s->irqs, irq, next);
    trace_iucv_queue_irq(type, pathid, msgid);
    update_pending(s);
}

/* drop queued interruptions of a path (msgid 0: all) of the given types */
static void drop_irqs(S390IUCVState *s, uint16_t pathid, uint32_t msgid,
                      uint8_t type_lo, uint8_t type_hi)
{
    IUCVIrq *irq, *n;

    QTAILQ_FOREACH_SAFE(irq, &s->irqs, next, n) {
        if (irq->pathid == pathid && (!msgid || irq->msgid == msgid) &&
            irq->type >= type_lo && irq->type <= type_hi) {
            QTAILQ_REMOVE(&s->irqs, irq, next);
            g_free(irq);
        }
    }
    update_pending(s);
}

static bool iucv_irq_pending(S390CPU *cpu)
{
    S390IUCVState *s = iucv_state;
    int i = CPU(cpu)->cpu_index;

    return s && i < s->ncpus && qatomic_read(&s->cpus[i].pending);
}

static bool iucv_irq_present(S390CPU *cpu, struct kvm_s390_ext_user_info *info)
{
    S390IUCVState *s = iucv_state;
    IUCVCpu *c = &s->cpus[CPU(cpu)->cpu_index];
    IUCVIrq *irq = next_irq(s, c);

    if (!irq) {
        update_pending(s);
        return false;
    }
    QTAILQ_REMOVE(&s->irqs, irq, next);
    if (irq->type > T_RESUMED && !msg_irq_queued(s)) {
        irq->rec[P_POLLFG] = 0x80;
    }
    if (!abs_write(c->buffer, irq->rec, P_LEN)) {
        warn_report_once("IUCV: interruption buffer at 0x%x is not in "
                         "guest storage", c->buffer);
    }
    trace_iucv_present(CPU(cpu)->cpu_index, irq->type, irq->pathid,
                       irq->msgid);
    g_free(irq);
    update_pending(s);

    memset(info, 0, sizeof(*info));
    info->subclass = CR0_IUCV;
    info->code = IUCV_EXT_CODE;
    return true;
}

static const S390UserExtIrqOps iucv_irq_ops = {
    .pending = iucv_irq_pending,
    .present = iucv_irq_present,
};

/* paths and messages */

static IUCVPath *path_get(S390IUCVState *s, uint16_t id)
{
    return id < s->maxconn ? s->paths[id] : NULL;
}

static uint32_t new_msgid(S390IUCVState *s)
{
    if (!++s->next_msgid) {
        s->next_msgid = 1;
    }
    return s->next_msgid;
}

static void msg_free(IUCVPath *path, IUCVMsg *m)
{
    QTAILQ_REMOVE(&path->msgs, m, next);
    g_free(m->pub.data);
    g_free(m->data);
    g_free(m);
}

/* the completion of a message of the guest */
static void complete_guest_msg(S390IUCVState *s, IUCVPath *path, IUCVMsg *m,
                               uint32_t audit, uint32_t residual,
                               const uint8_t *prm)
{
    uint8_t rec[P_LEN] = {};

    rec[P_FLAGS1] = (m->priority ? F_PRTY : 0) | (m->two_way ? 0 : F_NORPY) |
                    (prm ? F_RMDATA : 0);
    put32(rec, P_MSGID, m->id);
    put32(rec, P_AUDIT, audit << 8);
    if (prm) {
        memcpy(rec + P_RMMSG, prm, 8);
    }
    put32(rec, P_SRCCLS, m->srccls);
    put32(rec, P_MSGTAG, m->tag);
    put32(rec, P_BFLN2F, residual);
    queue_irq(s, m->priority ? T_MSG_COMPLETE_PRTY : T_MSG_COMPLETE,
              path->id, m->id, rec);
}

static void path_free(S390IUCVState *s, IUCVPath *path)
{
    IUCVMsg *m, *n;

    QTAILQ_FOREACH_SAFE(m, &path->msgs, next, n) {
        msg_free(path, m);
    }
    drop_irqs(s, path->id, 0, T_CONN_PENDING, T_MSG_PENDING);
    s->paths[path->id] = NULL;
    g_free(path);
}

/* the service is told and lets go of everything it holds */
static void path_sever_service(S390IUCVState *s, IUCVPath *path)
{
    IUCVServiceClass *sc;

    if (path->state == PATH_SEVERED || !path->svc) {
        return;
    }
    sc = IUCV_SERVICE_GET_CLASS(path->svc);
    path->state = PATH_SEVERED;
    trace_iucv_sever(path->id, "guest");
    if (sc->severed) {
        sc->severed(path->svc, path);
    }
}

static void sever_all(S390IUCVState *s)
{
    uint32_t i;

    for (i = 0; i < s->maxconn; i++) {
        if (s->paths[i]) {
            path_sever_service(s, s->paths[i]);
            path_free(s, s->paths[i]);
        }
    }
}

/* the service API */

uint16_t iucv_path_id(IUCVPath *path)
{
    return path->id;
}

void *iucv_path_get_opaque(IUCVPath *path)
{
    return path->opaque;
}

void iucv_path_set_opaque(IUCVPath *path, void *opaque)
{
    path->opaque = opaque;
}

uint16_t iucv_path_msglim(IUCVPath *path)
{
    return path->msglim;
}

void iucv_path_set_msglim(IUCVPath *path, uint16_t msglim)
{
    if (msglim && msglim < path->msglim) {
        path->msglim = msglim;
    }
}

void iucv_path_set_user(IUCVPath *path, const uint8_t *user)
{
    memcpy(path->user, user, 16);
}

void iucv_sever(IUCVService *svc, IUCVPath *path, const uint8_t *user)
{
    S390IUCVState *s = svc->iucv;
    uint8_t rec[P_LEN] = {};
    IUCVMsg *m, *n;

    if (path->state == PATH_SEVERED) {
        return;
    }
    path->state = PATH_SEVERED;
    trace_iucv_sever(path->id, "service");
    /* the guest's two-way messages end with the path */
    QTAILQ_FOREACH_SAFE(m, &path->msgs, next, n) {
        if (!m->from_guest || m->dead) {
            continue;
        }
        if (m->two_way) {
            complete_guest_msg(s, path, m, AUD_SVRD, m->ans_len, NULL);
        }
        if (m->busy) {
            m->dead = true;
        } else {
            msg_free(path, m);
        }
    }
    path->guest_out = 0;
    if (user) {
        memcpy(rec + P_USER, user, 16);
    }
    queue_irq(s, T_SEVERED, path->id, 0, rec);
}

uint32_t iucv_send(IUCVService *svc, IUCVPath *path, uint32_t trgcls,
                   const void *data, uint32_t len, bool two_way,
                   uint32_t reply_max)
{
    S390IUCVState *s = svc->iucv;
    uint8_t rec[P_LEN] = {};
    IUCVMsg *m;

    if (path->state != PATH_ACTIVE || path->guest_quiesced ||
        path->svc_out >= path->msglim) {
        return 0;
    }
    m = g_new0(IUCVMsg, 1);
    m->id = new_msgid(s);
    m->two_way = two_way;
    m->trgcls = trgcls;
    m->data = g_memdup2(data, len);
    m->len = len;
    m->reply_max = reply_max;
    QTAILQ_INSERT_TAIL(&path->msgs, m, next);
    path->svc_out++;

    rec[P_FLAGS1] = (two_way ? 0 : F_NORPY) | F_FGMID | F_FGPID | F_FGMCL;
    put32(rec, P_MSGID, m->id);
    put32(rec, P_TRGCLS, trgcls);
    put32(rec, P_BFLN1F, len);
    put32(rec, P_BFLN2F, reply_max);
    queue_irq(s, T_MSG_PENDING, path->id, m->id, rec);
    return m->id;
}

unsigned iucv_send_outstanding(IUCVPath *path)
{
    return path->svc_out;
}

static IUCVMsg *guest_msg(IUCVPath *path, IUCVMessage *msg)
{
    return container_of(msg, IUCVMsg, pub);
}

void iucv_reply(IUCVService *svc, IUCVPath *path, IUCVMessage *msg,
                const void *data, uint32_t len)
{
    S390IUCVState *s = svc->iucv;
    IUCVMsg *m = guest_msg(path, msg);
    IUCVArea a = { .list = m->ans_list, .addr = m->ans_addr,
                   .total = m->ans_len, .amask = m->ans_amask };
    uint32_t audit = 0, residual, n = MIN(len, m->ans_len);

    if (len > m->ans_len) {
        audit |= AUD_RPLE;
        residual = len - m->ans_len;
    } else {
        residual = m->ans_len - len;
    }
    if (n && !area_rw(&a, 0, (uint8_t *)data, n, true)) {
        audit |= AUD_ANAX;
    }
    trace_iucv_reply(path->id, m->id, len);
    complete_guest_msg(s, path, m, audit, residual, NULL);
    path->guest_out--;
    if (m->busy) {
        m->dead = true;
    } else {
        msg_free(path, m);
    }
}

void iucv_reject(IUCVService *svc, IUCVPath *path, IUCVMessage *msg)
{
    IUCVMsg *m = guest_msg(path, msg);

    complete_guest_msg(svc->iucv, path, m, AUD_RJCT, m->ans_len, NULL);
    path->guest_out--;
    if (m->busy) {
        m->dead = true;
    } else {
        msg_free(path, m);
    }
}

void iucv_userid_from_str(uint8_t *userid, const char *str)
{
    char tmp[9];
    int i;

    for (i = 0; i < 8; i++) {
        tmp[i] = (str && *str) ? g_ascii_toupper(*str++) : ' ';
    }
    tmp[8] = 0;
    ebcdic_put(userid, tmp, 8);
}

const char *s390_iucv_userid(void)
{
    if (iucv_state && iucv_state->userid_str) {
        return iucv_state->userid_str;
    }
    return s390_zvm_userid();
}

/* the instruction */

typedef struct IUCVCall {
    S390CPU *cpu;
    CPUS390XState *env;
    S390IUCVState *s;
    IUCVCpu *c;
    hwaddr plist;               /* absolute */
    uint8_t p[P_LEN];
} IUCVCall;

static int call_cc(IUCVCall *k, int cc)
{
    setcc(k->cpu, cc);
    if (k->plist && !abs_write(k->plist, k->p, P_LEN)) {
        kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
    }
    return 0;
}

static int call_rc(IUCVCall *k, uint8_t rc)
{
    k->p[P_RCODE] = rc;
    return call_cc(k, rc ? 1 : 0);
}

static IUCVService *find_service(S390IUCVState *s, const uint8_t *vmid)
{
    IUCVService *svc;

    QTAILQ_FOREACH(svc, &s->services, next) {
        if (!memcmp(svc->userid, vmid, 8)) {
            return svc;
        }
    }
    return NULL;
}

static int f_dclbfr(IUCVCall *k)
{
    uint32_t buf = get32(k->p, P_BFADR1) & addr_mask(k->env);
    uint8_t tmp[P_LEN];

    if (k->c->declared) {
        return call_rc(k, 0x13);
    }
    if (!abs_read(buf, tmp, P_LEN)) {
        kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
        return 0;
    }
    k->c->declared = true;
    k->c->buffer = buf;
    k->c->mask = 0xf8;
    k->c->cmask = 0xf8;
    trace_iucv_dclbfr(CPU(k->cpu)->cpu_index, buf);
    update_pending(k->s);
    return call_rc(k, 0);
}

static int f_rtrvbfr(IUCVCall *k)
{
    S390IUCVState *s = k->s;
    unsigned i;

    k->c->declared = false;
    qatomic_set(&k->c->pending, false);
    for (i = 0; i < s->ncpus; i++) {
        if (s->cpus[i].declared) {
            return call_cc(k, 0);
        }
    }
    /* the last buffer: all paths end */
    sever_all(s);
    return call_cc(k, 0);
}

static int f_connect(IUCVCall *k)
{
    S390IUCVState *s = k->s;
    IUCVService *svc = find_service(s, k->p + P_VMID);
    uint8_t flags = k->p[P_FLAGS1];
    uint16_t msglim = get16(k->p, P_MSGLIM);
    uint8_t rec[P_LEN] = {};
    IUCVServiceClass *sc;
    IUCVPath *path;
    uint32_t id;
    int rc;

    if (flags & F_ANSLST) {
        return call_rc(k, 0x1e);        /* APPC/VM */
    }
    if (!svc) {
        return call_rc(k, IUCV_RC_NO_PARTNER);
    }
    for (id = 0; id < s->maxconn && s->paths[id]; id++) {
        /* lowest free path ID */
    }
    if (id == s->maxconn) {
        return call_rc(k, 0x0d);
    }
    path = g_new0(IUCVPath, 1);
    path->id = id;
    path->svc = svc;
    path->state = PATH_ACTIVE;
    path->msglim = msglim ? msglim : MSGLIM_DEFAULT;
    path->guest_prmdata = flags & F_RMDATA;
    path->guest_quiesced = flags & F_BUFLST;
    QTAILQ_INIT(&path->msgs);
    s->paths[id] = path;

    sc = IUCV_SERVICE_GET_CLASS(svc);
    rc = sc->connect(svc, path, k->p + P_USER);
    trace_iucv_connect(id, svc->name, rc);
    if (rc) {
        s->paths[id] = NULL;
        g_free(path);
        return call_rc(k, rc);
    }
    put16(k->p, P_PATHID, id);
    put16(k->p, P_MSGLIM, path->msglim);
    k->p[P_FLAGS1] = flags & F_PRTY;

    /* the service accepted at once */
    rec[P_FLAGS1] = F_RMDATA | F_PRTY;
    put16(rec, P_MSGLIM, path->msglim);
    memcpy(rec + P_USER, path->user, 16);
    queue_irq(s, T_CONN_COMPLETE, id, 0, rec);
    return call_rc(k, 0);
}

/* SEVER, QUIESCE and RESUME of one or all paths */
static int f_pathctl(IUCVCall *k, uint8_t fc)
{
    S390IUCVState *s = k->s;
    uint32_t i, first, last;

    if (k->p[P_FLAGS1] & F_RMDATA) {
        first = 0;
        last = s->maxconn - 1;
    } else {
        first = last = get16(k->p, P_PATHID);
        if (!path_get(s, first)) {
            return call_rc(k, 1);
        }
    }
    for (i = first; i <= last; i++) {
        IUCVPath *path = path_get(s, i);
        IUCVServiceClass *sc;

        if (!path) {
            continue;
        }
        sc = IUCV_SERVICE_GET_CLASS(path->svc);
        switch (fc) {
        case IUCV_SEVER:
            path_sever_service(s, path);
            path_free(s, path);
            break;
        case IUCV_QUIESCE:
        case IUCV_RESUME:
            if (path->state != PATH_ACTIVE) {
                if (first == last) {
                    return call_rc(k, 1);
                }
                break;
            }
            path->guest_quiesced = fc == IUCV_QUIESCE;
            if (sc->quiesced) {
                sc->quiesced(path->svc, path, path->guest_quiesced);
            }
            break;
        }
    }
    return call_rc(k, 0);
}

static int f_send(IUCVCall *k)
{
    S390IUCVState *s = k->s;
    IUCVPath *path = path_get(s, get16(k->p, P_PATHID));
    uint8_t flags = k->p[P_FLAGS1];
    IUCVArea data = { .list = flags & F_BUFLST,
                      .addr = get32(k->p, P_BFADR1),
                      .total = get32(k->p, P_BFLN1F),
                      .amask = addr_mask(k->env) };
    IUCVServiceClass *sc;
    IUCVMsg *m;
    int rc;

    if (!path || path->state != PATH_ACTIVE) {
        return call_rc(k, 1);
    }
    if (path->svc_quiesced) {
        return call_rc(k, 2);
    }
    if (flags & F_FGPID) {
        return call_rc(k, 0x1e);
    }
    if ((flags & F_RMDATA) && (flags & F_BUFLST)) {
        return call_rc(k, 0x19);
    }
    if (!(flags & F_NORPY) && path->guest_out >= path->msglim) {
        return call_rc(k, 3);
    }
    if (!(flags & F_RMDATA)) {
        rc = area_check(&data, 0x1a);
        if (rc < 0) {
            kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
            return 0;
        }
        if (rc) {
            return call_rc(k, rc);
        }
    }

    m = g_new0(IUCVMsg, 1);
    m->from_guest = true;
    m->id = new_msgid(s);
    m->two_way = !(flags & F_NORPY);
    m->priority = flags & F_PRTY;
    m->prmdata = flags & F_RMDATA;
    m->trgcls = get32(k->p, P_TRGCLS);
    m->srccls = get32(k->p, P_SRCCLS);
    m->tag = get32(k->p, P_MSGTAG);
    if (m->two_way) {
        m->ans_list = flags & F_ANSLST;
        m->ans_addr = get32(k->p, P_BFADR2);
        m->ans_len = get32(k->p, P_BFLN2F);
        m->ans_amask = addr_mask(k->env);
    }
    m->pub.id = m->id;
    m->pub.trgcls = m->trgcls;
    m->pub.two_way = m->two_way;
    m->pub.priority = m->priority;
    m->pub.prmdata = m->prmdata;
    m->pub.reply_max = m->ans_len;
    if (m->prmdata) {
        m->pub.data = g_memdup2(k->p + P_RMMSG, 8);
        m->pub.len = 8;
    } else {
        m->pub.data = g_malloc(data.total ? data.total : 1);
        m->pub.len = data.total;
        if (!area_rw(&data, 0, m->pub.data, data.total, false)) {
            g_free(m->pub.data);
            g_free(m);
            kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
            return 0;
        }
    }
    put32(k->p, P_MSGID, m->id);
    QTAILQ_INSERT_TAIL(&path->msgs, m, next);
    trace_iucv_send(path->id, m->id, m->trgcls, m->pub.len, m->two_way);
    if (m->two_way) {
        path->guest_out++;
    }
    call_rc(k, 0);

    sc = IUCV_SERVICE_GET_CLASS(path->svc);
    m->busy = true;
    sc->message(path->svc, path, &m->pub);
    m->busy = false;
    if (m->dead) {
        msg_free(path, m);
    } else if (!m->two_way) {
        /* received by the service; one-way PRMDATA has no completion */
        if (!m->prmdata && path->state == PATH_ACTIVE) {
            complete_guest_msg(s, path, m, 0, 0, NULL);
        }
        msg_free(path, m);
    }
    return 0;
}

/*
 * Find a message of the service by the qualifiers of RECEIVE, REJECT or
 * REPLY. Returns NULL with *rc set for a CC 1, NULL with *rc 0 for none.
 */
static IUCVMsg *find_svc_msg(IUCVCall *k, uint8_t flags, IUCVPath **pathp,
                             int *rc)
{
    S390IUCVState *s = k->s;
    uint16_t pathid = get16(k->p, P_PATHID);
    uint32_t msgid = get32(k->p, P_MSGID);
    uint32_t cls = get32(k->p, P_TRGCLS);
    uint32_t i;

    *rc = 0;
    if ((flags & F_FGPID) && !path_get(s, pathid)) {
        *rc = 1;
        return NULL;
    }
    for (i = 0; i < s->maxconn; i++) {
        IUCVPath *path = s->paths[i];
        IUCVMsg *m;

        if (!path || ((flags & F_FGPID) && i != pathid)) {
            continue;
        }
        QTAILQ_FOREACH(m, &path->msgs, next) {
            if (m->from_guest) {
                continue;
            }
            if ((flags & F_FGMID) && m->id != msgid) {
                continue;
            }
            if ((flags & F_FGMCL) && m->trgcls != cls) {
                if (flags & F_FGMID) {
                    *rc = 8;
                    return NULL;
                }
                continue;
            }
            *pathp = path;
            return m;
        }
    }
    return NULL;
}

static void svc_msg_done(S390IUCVState *s, IUCVPath *path, IUCVMsg *m)
{
    path->svc_out--;
    drop_irqs(s, path->id, m->id, T_MSG_PENDING_PRTY, T_MSG_PENDING);
    msg_free(path, m);
}

static int f_receive(IUCVCall *k)
{
    S390IUCVState *s = k->s;
    uint8_t flags = k->p[P_FLAGS1];
    IUCVArea a = { .list = flags & F_BUFLST, .addr = get32(k->p, P_BFADR1),
                   .total = get32(k->p, P_BFLN1F), .amask = addr_mask(k->env) };
    IUCVPath *path = NULL;
    IUCVServiceClass *sc;
    uint32_t left, n;
    IUCVMsg *m;
    int rc;

    if ((flags & F_FGMID) && !((flags & F_FGPID) && (flags & F_FGMCL))) {
        kvm_s390_program_interrupt(k->cpu, PGM_SPECIFICATION);
        return 0;
    }
    if (flags & F_ANSLST) {
        return call_rc(k, 0x1e);
    }
    m = find_svc_msg(k, flags, &path, &rc);
    if (!m) {
        return rc ? call_rc(k, rc) : call_cc(k, 2);
    }
    rc = area_check(&a, 0x1a);
    if (rc) {
        if (rc < 0) {
            kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
            return 0;
        }
        return call_rc(k, rc);
    }
    left = m->len - m->done;
    n = MIN(left, a.total);
    if (n && !area_rw(&a, 0, m->data + m->done, n, true)) {
        kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
        return 0;
    }
    m->done += n;
    put16(k->p, P_PATHID, path->id);
    k->p[P_FLAGS1] = (m->priority ? F_PRTY : 0) | (m->two_way ? 0 : F_NORPY) |
                     F_FGMID | F_FGPID | F_FGMCL;
    put32(k->p, P_MSGID, m->id);
    put32(k->p, P_TRGCLS, m->trgcls);
    put32(k->p, P_BFLN2F, m->reply_max);
    if (!a.list) {
        put32(k->p, P_BFADR1, a.addr + n);
    }
    trace_iucv_receive(path->id, m->id, n, m->len - m->done);
    /* the pending interruption is not presented any more */
    drop_irqs(s, path->id, m->id, T_MSG_PENDING_PRTY, T_MSG_PENDING);
    if (m->done < m->len) {
        put32(k->p, P_BFLN1F, m->len - m->done);
        return call_rc(k, 5);
    }
    put32(k->p, P_BFLN1F, a.total - n);
    call_rc(k, 0);
    if (!m->two_way) {
        uint32_t id = m->id;

        svc_msg_done(s, path, m);
        sc = IUCV_SERVICE_GET_CLASS(path->svc);
        if (sc->received) {
            sc->received(path->svc, path, id);
        }
    } else {
        m->received = true;
    }
    return 0;
}

static int f_reply(IUCVCall *k)
{
    S390IUCVState *s = k->s;
    uint8_t flags = k->p[P_FLAGS1];
    IUCVArea a = { .list = flags & F_ANSLST, .addr = get32(k->p, P_BFADR2),
                   .total = get32(k->p, P_BFLN2F), .amask = addr_mask(k->env) };
    IUCVPath *path = NULL;
    IUCVServiceClass *sc;
    g_autofree uint8_t *buf = NULL;
    uint32_t len, id;
    IUCVMsg *m;
    int rc;

    m = find_svc_msg(k, F_FGMID | F_FGPID | F_FGMCL, &path, &rc);
    if (!m || !m->two_way) {
        return rc ? call_rc(k, rc) : call_cc(k, 2);
    }
    if (flags & F_RMDATA) {
        buf = g_memdup2(k->p + P_RMMSG, 8);
        len = 8;
    } else {
        rc = area_check(&a, 0x1b);
        if (rc) {
            if (rc < 0) {
                kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
                return 0;
            }
            return call_rc(k, rc);
        }
        len = MIN(a.total, m->reply_max ? m->reply_max : a.total);
        buf = g_malloc(len ? len : 1);
        if (len && !area_rw(&a, 0, buf, len, false)) {
            kvm_s390_program_interrupt(k->cpu, PGM_ADDRESSING);
            return 0;
        }
        put32(k->p, P_BFLN2F, a.total - len);
    }
    id = m->id;
    trace_iucv_guest_reply(path->id, id, len);
    svc_msg_done(s, path, m);
    call_rc(k, len < a.total && !(flags & F_RMDATA) ? 5 : 0);
    sc = IUCV_SERVICE_GET_CLASS(path->svc);
    if (sc->replied) {
        sc->replied(path->svc, path, id, buf, len);
    }
    return 0;
}

static int f_reject(IUCVCall *k)
{
    uint8_t flags = k->p[P_FLAGS1];
    IUCVPath *path = NULL;
    IUCVServiceClass *sc;
    uint32_t id;
    IUCVMsg *m;
    int rc;

    if ((flags & F_FGMID) && !((flags & F_FGPID) && (flags & F_FGMCL))) {
        kvm_s390_program_interrupt(k->cpu, PGM_SPECIFICATION);
        return 0;
    }
    m = find_svc_msg(k, flags, &path, &rc);
    if (!m) {
        return rc ? call_rc(k, rc) : call_cc(k, 2);
    }
    id = m->id;
    svc_msg_done(k->s, path, m);
    call_rc(k, 0);
    sc = IUCV_SERVICE_GET_CLASS(path->svc);
    if (sc->rejected) {
        sc->rejected(path->svc, path, id);
    }
    return 0;
}

/* the guest withdraws one of its messages */
static int f_purge(IUCVCall *k)
{
    S390IUCVState *s = k->s;
    uint8_t flags = k->p[P_FLAGS1];
    uint16_t pathid = get16(k->p, P_PATHID);
    uint32_t msgid = get32(k->p, P_MSGID);
    uint32_t cls = get32(k->p, P_SRCCLS);
    IUCVPath *path;
    IUCVIrq *irq;
    IUCVMsg *m;

    if ((flags & F_FGMID) && !((flags & F_FGPID) && (flags & F_FGMCL))) {
        kvm_s390_program_interrupt(k->cpu, PGM_SPECIFICATION);
        return 0;
    }
    if ((flags & F_FGPID) && !path_get(s, pathid)) {
        return call_rc(k, 1);
    }
    /* a completion not presented yet */
    QTAILQ_FOREACH(irq, &s->irqs, next) {
        if ((irq->type == T_MSG_COMPLETE || irq->type == T_MSG_COMPLETE_PRTY) &&
            (!(flags & F_FGPID) || irq->pathid == pathid) &&
            (!(flags & F_FGMID) || irq->msgid == msgid) &&
            (!(flags & F_FGMCL) || get32(irq->rec, P_SRCCLS) == cls)) {
            put16(k->p, P_PATHID, irq->pathid);
            put32(k->p, P_MSGID, irq->msgid);
            memcpy(k->p + P_AUDIT, irq->rec + P_AUDIT, 3);
            put32(k->p, P_SRCCLS, get32(irq->rec, P_SRCCLS));
            put32(k->p, P_MSGTAG, get32(irq->rec, P_MSGTAG));
            k->p[P_FLAGS1] = irq->rec[P_FLAGS1] & (F_PRTY | F_NORPY);
            QTAILQ_REMOVE(&s->irqs, irq, next);
            g_free(irq);
            update_pending(s);
            return call_rc(k, 0);
        }
    }
    /* a two-way message the service has not answered */
    path = (flags & F_FGPID) ? path_get(s, pathid) : NULL;
    for (uint32_t i = 0; i < s->maxconn; i++) {
        IUCVPath *p = path ? path : s->paths[i];

        if (!p) {
            continue;
        }
        QTAILQ_FOREACH(m, &p->msgs, next) {
            IUCVServiceClass *sc = IUCV_SERVICE_GET_CLASS(p->svc);

            if (!m->from_guest || ((flags & F_FGMID) && m->id != msgid) ||
                ((flags & F_FGMCL) && m->srccls != cls)) {
                continue;
            }
            put16(k->p, P_PATHID, p->id);
            put32(k->p, P_MSGID, m->id);
            memset(k->p + P_AUDIT, 0, 3);
            put32(k->p, P_SRCCLS, m->srccls);
            put32(k->p, P_MSGTAG, m->tag);
            k->p[P_FLAGS1] = m->priority ? F_PRTY : 0;
            if (sc->cancelled) {
                sc->cancelled(p->svc, p, &m->pub);
            }
            p->guest_out--;
            msg_free(p, m);
            return call_rc(k, 0);
        }
        if (path) {
            break;
        }
    }
    return call_cc(k, 2);
}

static int f_setmask(IUCVCall *k, bool control)
{
    if (control) {
        k->c->cmask = k->p[0] & 0xf8;
    } else {
        k->c->mask = k->p[0] & 0xf8;
    }
    update_pending(k->s);
    k->p[P_RCODE] = 0;
    return call_cc(k, 0);
}

/* DESCRIBE, TEST COMPLETION and INTERRUPT POLL take records off the queue */
static IUCVIrq *take_msg_irq(S390IUCVState *s, uint8_t lo, uint8_t hi,
                             uint8_t flags, const uint8_t *p)
{
    IUCVIrq *irq, *best = NULL;

    QTAILQ_FOREACH(irq, &s->irqs, next) {
        if (irq->type < lo || irq->type > hi) {
            continue;
        }
        if ((flags & F_FGPID) && irq->pathid != get16(p, P_PATHID)) {
            continue;
        }
        if ((flags & F_FGMID) && irq->msgid != get32(p, P_MSGID)) {
            continue;
        }
        if ((flags & F_FGMCL) &&
            get32(irq->rec, P_SRCCLS) != get32(p, P_SRCCLS)) {
            continue;
        }
        if (!best || irq->type < best->type) {
            best = irq;
        }
    }
    if (best) {
        QTAILQ_REMOVE(&s->irqs, best, next);
        if (!msg_irq_queued(s)) {
            best->rec[P_POLLFG] = 0x80;
        }
        update_pending(s);
    }
    return best;
}

static int f_describe(IUCVCall *k)
{
    IUCVIrq *irq = take_msg_irq(k->s, T_MSG_PENDING_PRTY, T_MSG_PENDING, 0,
                                k->p);

    if (!irq) {
        return call_cc(k, 2);
    }
    memcpy(k->p, irq->rec, P_LEN);
    k->p[P_RCODE] = 0;
    g_free(irq);
    return call_cc(k, 0);
}

static int f_testcmpl(IUCVCall *k)
{
    IUCVIrq *irq = take_msg_irq(k->s, T_MSG_COMPLETE_PRTY, T_MSG_COMPLETE,
                                k->p[P_FLAGS1], k->p);
    bool audit;

    if (!irq) {
        return call_cc(k, 2);
    }
    memcpy(k->p, irq->rec, P_LEN);
    k->p[P_RCODE] = 0;
    audit = get32(irq->rec, P_AUDIT) >> 8;
    g_free(irq);
    return call_cc(k, audit ? 3 : 0);
}

static int f_ipoll(IUCVCall *k)
{
    uint32_t buf = get32(k->p, P_BFADR1) & addr_mask(k->env);
    uint32_t len = get32(k->p, P_BFLN1F), n = 0;
    IUCVIrq *irq;

    if (len < P_LEN || (buf & ~0xfffULL) !=
                       ((buf + len - 1) & ~0xfffULL)) {
        kvm_s390_program_interrupt(k->cpu, PGM_SPECIFICATION);
        return 0;
    }
    while (n + P_LEN <= len &&
           (irq = take_msg_irq(k->s, T_MSG_COMPLETE_PRTY, T_MSG_PENDING, 0,
                               k->p))) {
        if (!abs_write(buf + n, irq->rec, P_LEN)) {
            g_free(irq);
            return call_rc(k, 0x5c);
        }
        g_free(irq);
        n += P_LEN;
    }
    if (!n) {
        return call_cc(k, 2);
    }
    put32(k->p, P_BFLN1F, n);
    return call_rc(k, 0);
}

static int iucv_instruction(S390CPU *cpu, uint16_t ipa, uint32_t ipb)
{
    S390IUCVState *s = iucv_state;
    CPUS390XState *env = &cpu->env;
    uint8_t fc = env->regs[0] & 0xff;
    uint8_t b2 = ipb >> 28;
    uint64_t addr = (b2 ? env->regs[b2] : 0) + ((ipb >> 16) & 0xfff);
    IUCVCall k = { .cpu = cpu, .env = env, .s = s };
    unsigned i;
    bool any = false;

    if (env->psw.mask & PSW_MASK_PSTATE) {
        kvm_s390_program_interrupt(cpu, PGM_OPERATION);
        return 0;
    }
    trace_iucv_instruction(CPU(cpu)->cpu_index, fc);
    if (!s->available) {
        /* as under a hypervisor without IUCV for this guest */
        setcc(cpu, 3);
        return 0;
    }
    k.c = &s->cpus[CPU(cpu)->cpu_index];
    for (i = 0; i < s->ncpus; i++) {
        any |= s->cpus[i].declared;
    }
    if (!any && fc != IUCV_QUERY && fc != IUCV_DCLBFR) {
        kvm_s390_program_interrupt(cpu, PGM_OPERATION);
        return 0;
    }

    switch (fc) {
    case IUCV_QUERY:
        env->regs[0] = deposit64(env->regs[0], 0, 32, P_LEN);
        env->regs[1] = deposit64(env->regs[1], 0, 32, s->maxconn);
        setcc(cpu, 0);
        return 0;
    case IUCV_RTRVBFR:
        return f_rtrvbfr(&k);
    case IUCV_TESTMSG:
        /* would wait for a message: not provided */
        kvm_s390_program_interrupt(cpu, PGM_OPERATION);
        return 0;
    }

    if (!(env->psw.mask & PSW_MASK_64)) {
        addr &= (env->psw.mask & PSW_MASK_32) ? 0x7fffffff : 0x00ffffff;
    }
    if (addr & 7) {
        kvm_s390_program_interrupt(cpu, PGM_SPECIFICATION);
        return 0;
    }
    k.plist = mmu_real2abs(env, addr);
    if (!abs_read(k.plist, k.p, P_LEN)) {
        kvm_s390_program_interrupt(cpu, PGM_ADDRESSING);
        return 0;
    }

    switch (fc) {
    case IUCV_DCLBFR:
        return f_dclbfr(&k);
    case IUCV_CONNECT:
        return f_connect(&k);
    case IUCV_ACCEPT:
        /* services do not connect to the guest */
        return call_rc(&k, 1);
    case IUCV_SEVER:
    case IUCV_QUIESCE:
    case IUCV_RESUME:
        return f_pathctl(&k, fc);
    case IUCV_SEND:
        return f_send(&k);
    case IUCV_RECEIVE:
        return f_receive(&k);
    case IUCV_REPLY:
        return f_reply(&k);
    case IUCV_REJECT:
        return f_reject(&k);
    case IUCV_PURGE:
        return f_purge(&k);
    case IUCV_SETMASK:
        return f_setmask(&k, false);
    case IUCV_SETCMASK:
        return f_setmask(&k, true);
    case IUCV_DESCRIBE:
        return f_describe(&k);
    case IUCV_TESTCMPL:
        return f_testcmpl(&k);
    case IUCV_IPOLL:
        return f_ipoll(&k);
    default:
        kvm_s390_program_interrupt(cpu, PGM_SPECIFICATION);
        return 0;
    }
}

/* the core object */

static void iucv_reset(void *opaque)
{
    S390IUCVState *s = opaque;
    IUCVIrq *irq, *n;

    /* as CP retrieves the buffers at a system reset */
    sever_all(s);
    QTAILQ_FOREACH_SAFE(irq, &s->irqs, next, n) {
        QTAILQ_REMOVE(&s->irqs, irq, next);
        g_free(irq);
    }
    memset(s->cpus, 0, sizeof(*s->cpus) * s->ncpus);
}

static void iucv_realize(DeviceState *dev, Error **errp)
{
    S390IUCVState *s = S390_IUCV(dev);
    MachineState *ms = MACHINE(qdev_get_machine());

    if (!kvm_enabled()) {
        error_setg(errp, "IUCV needs KVM");
        return;
    }
    if (!s->maxconn || s->maxconn > 65535) {
        error_setg(errp, "maxconn must be 1 to 65535");
        return;
    }
    if (!s->available) {
        /* only the instruction: condition code 3 */
        if (kvm_s390_user_operexec_init(0xb2f0, iucv_instruction, errp)) {
            iucv_state = s;
        }
        return;
    }
    if (!kvm_s390_user_ext_irq_init(&iucv_irq_ops, CR0_IUCV, errp) ||
        !kvm_s390_user_operexec_init(0xb2f0, iucv_instruction, errp)) {
        return;
    }
    error_setg(&s->migration_blocker,
               "IUCV connections cannot be migrated");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }
    iucv_userid_from_str(s->userid, s390_iucv_userid());
    s->ncpus = ms->smp.max_cpus;
    s->cpus = g_new0(IUCVCpu, s->ncpus);
    s->paths = g_new0(IUCVPath *, s->maxconn);
    QTAILQ_INIT(&s->irqs);
    QTAILQ_INIT(&s->services);
    qemu_register_reset(iucv_reset, s);
    iucv_state = s;
}

static const Property iucv_properties[] = {
    DEFINE_PROP_STRING("userid", S390IUCVState, userid_str),
    DEFINE_PROP_UINT32("maxconn", S390IUCVState, maxconn, 255),
    /*
     * off: the instruction ends with condition code 3 in the supervisor
     * state ("IUCV not available", as under a hypervisor that offers no
     * IUCV to the guest) and no services can be added.
     */
    DEFINE_PROP_BOOL("available", S390IUCVState, available, true),
};

static void iucv_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = iucv_realize;
    dc->hotpluggable = false;
    device_class_set_props(dc, iucv_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo iucv_info = {
    .name = TYPE_S390_IUCV,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(S390IUCVState),
    .class_init = iucv_class_init,
};

/* created with the first service */
static S390IUCVState *iucv_get(Error **errp)
{
    Object *obj;

    if (iucv_state) {
        return iucv_state;
    }
    obj = object_new(TYPE_S390_IUCV);
    object_property_add_child(qdev_get_machine(), "iucv", obj);
    object_unref(obj);
    if (!qdev_realize(DEVICE(obj), NULL, errp)) {
        object_unparent(obj);
        return NULL;
    }
    return iucv_state;
}

/* the base type of the services */

static void iucv_service_realize(DeviceState *dev, Error **errp)
{
    IUCVService *svc = IUCV_SERVICE(dev);
    S390IUCVState *s;

    if (!svc->name || !*svc->name || strlen(svc->name) > 8) {
        error_setg(errp, "an IUCV service needs a name of 1 to 8 characters");
        return;
    }
    s = iucv_get(errp);
    if (!s) {
        return;
    }
    if (!s->available) {
        error_setg(errp, "IUCV is configured as not available");
        return;
    }
    iucv_userid_from_str(svc->userid, svc->name);
    if (find_service(s, svc->userid)) {
        error_setg(errp, "IUCV service %s exists already", svc->name);
        return;
    }
    svc->iucv = s;
    QTAILQ_INSERT_TAIL(&s->services, svc, next);
}

static void iucv_service_unrealize(DeviceState *dev)
{
    IUCVService *svc = IUCV_SERVICE(dev);
    S390IUCVState *s = svc->iucv;
    uint32_t i;

    if (!s) {
        return;
    }
    for (i = 0; i < s->maxconn; i++) {
        if (s->paths[i] && s->paths[i]->svc == svc) {
            iucv_sever(svc, s->paths[i], NULL);
        }
    }
    QTAILQ_REMOVE(&s->services, svc, next);
}

static const Property iucv_service_properties[] = {
    DEFINE_PROP_STRING("name", IUCVService, name),
};

static void iucv_service_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = iucv_service_realize;
    dc->unrealize = iucv_service_unrealize;
    device_class_set_props(dc, iucv_service_properties);
}

static const TypeInfo iucv_service_info = {
    .name = TYPE_IUCV_SERVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IUCVService),
    .class_size = sizeof(IUCVServiceClass),
    .class_init = iucv_service_class_init,
    .abstract = true,
};

static void iucv_register_types(void)
{
    type_register_static(&iucv_info);
    type_register_static(&iucv_service_info);
}

type_init(iucv_register_types)
