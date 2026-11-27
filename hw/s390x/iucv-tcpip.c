/*
 * IUCV TCP/IP service: the socket interface of a VM TCP/IP virtual
 * machine over IUCV, on a TCP/IP stack of its own
 *
 * Guests written for VM TCP/IP connect an IUCV path to the TCPIP user ID,
 * send an initialization message and then issue each socket call as a
 * two-way IUCV message: the target class holds the call number and the
 * socket number, the reply holds the return value, the error number and
 * the data. Calls that cannot complete at once are answered later.
 *
 * The service is a host on the network of its network backend (netdev=,
 * for instance a tap on a bridge), as the TCPIP virtual machine is with
 * its network adapter: it has its own MAC address and owns the configured
 * IPv4 address, answers ARP and ICMP echo for it and terminates TCP and
 * UDP itself, with lwIP. The guest's sockets are the stack's protocol
 * control blocks. Nothing goes through the sockets of the host.
 *
 * lwIP has one global state, so a process has at most one such service.
 * It runs under the BQL, in the main loop and in the IUCV instruction of
 * the guest. lwIP must not be entered again while it runs, but a backend
 * can answer a frame of the stack at once (user-mode networking does), so
 * received frames are queued and given to the stack by a bottom half. Its
 * callbacks note events and schedule the same bottom half, which then
 * works through the pending calls; a timer runs its timeouts.
 *
 * Written from the description of the interface in the VM TCP/IP
 * programmer's documentation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qemu/sockets.h"
#include "hw/core/qdev-properties.h"
#include "hw/s390x/iucv.h"
#include "hw/s390x/ebcdic.h"
#include "net/net.h"
#include "trace.h"
#include "qemu/bswap.h"
#include <net/if.h>

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"
#include "lwip/sys.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/prot/ip4.h"
#include "netif/etharp.h"
#include "netif/ethernet.h"

#define TYPE_IUCV_TCPIP "iucv-tcpip"
OBJECT_DECLARE_SIMPLE_TYPE(IUCVTcpip, IUCV_TCPIP)

/* call numbers (high halfword of the target class) */
enum {
    C_INIT = 0, C_ACCEPT = 1, C_BIND = 2, C_CLOSE = 3, C_CONNECT = 4,
    C_FCNTL = 5, C_GETHOSTID = 7, C_GETHOSTNAME = 8, C_GETPEERNAME = 9,
    C_GETSOCKNAME = 10, C_GETSOCKOPT = 11, C_IOCTL = 12, C_LISTEN = 13,
    C_READ = 14, C_RECV = 16, C_SELECT = 19, C_SEND = 20, C_SENDTO = 22,
    C_SETSOCKOPT = 23, C_SHUTDOWN = 24, C_SOCKET = 25, C_WRITE = 26,
    C_LASTERRNO = 29, C_GETCLIENTID = 30, C_GIVESOCKET = 31,
    C_TAKESOCKET = 32, C_CANCEL = 42,
};

/* error numbers of the interface (VM numbering) */
#define VE_PERM         1
#define VE_SRCH         3
#define VE_BADF         9
#define VE_ACCES        13
#define VE_BUSY         16
#define VE_INVAL        22
#define VE_MFILE        24
#define VE_PIPE         32
#define VE_WOULDBLOCK   35
#define VE_INPROGRESS   36
#define VE_ALREADY      37
#define VE_DESTADDRREQ  39
#define VE_MSGSIZE      40
#define VE_NOPROTOOPT   42
#define VE_PROTONOSUPPORT 43
#define VE_SOCKTNOSUPPORT 44
#define VE_OPNOTSUPP    45
#define VE_PFNOSUPPORT  46
#define VE_AFNOSUPPORT  47
#define VE_ADDRINUSE    48
#define VE_ADDRNOTAVAIL 49
#define VE_NETUNREACH   51
#define VE_CONNABORTED  53
#define VE_CONNRESET    54
#define VE_NOBUFS       55
#define VE_ISCONN       56
#define VE_NOTCONN      57
#define VE_TIMEDOUT     60
#define VE_CONNREFUSED  61
#define VE_BADCALL      1000
#define VE_BADLEN       1001
#define VE_RANGE        1002
#define VE_INUSE        1003
#define VE_CANCELED     1009

#define VM_AF_INET      2
#define VM_SOL_SOCKET   0xffff
#define VM_FNDELAY      4
#define SADDR           16      /* AF_INET socket address */
#define RECV_MAX        65536
#define SEND_MAX        (1024 * 1024)
#define UDP_MAX         65507
#define REQS_MAX        256
#define NSOCK_MIN       50
#define BACKLOG_MAX     64
#define DGRAM_QUEUE     64      /* datagrams waiting on one socket */
#define DGRAM_BYTES     (256 * 1024)
#define MTU_MIN         576
#define MTU_MAX         65535
#define DNS_MAX         3
#define RXQ_MAX         1024    /* received frames not yet in the stack */

#define T_STREAM        1
#define T_DGRAM         2
#define T_RAW           3

typedef struct TcpPath TcpPath;

/* a datagram received by a UDP or raw socket */
typedef struct Dgram {
    QSIMPLEQ_ENTRY(Dgram) next;
    uint32_t ip;
    uint16_t port;
    uint32_t len;
    uint8_t data[];
} Dgram;

/*
 * An endpoint of the stack (a protocol control block with its received
 * data). GIVESOCKET/TAKESOCKET let two socket numbers share one.
 */
typedef struct Conn {
    IUCVTcpip *s;
    int refs;
    int type;
    struct tcp_pcb *tcp;        /* NULL: closed by the stack (reset ...) */
    struct udp_pcb *udp;
    struct raw_pcb *raw;
    bool listening;
    bool connecting;
    bool connected;             /* a TCP connection existed */
    bool eof;                   /* the peer sent FIN */
    bool rd_shut;
    bool wr_shut;
    int so_error;               /* pending error, VM numbering */
    /* options without an effect on the stack */
    bool debug, dontroute, oobinline;
    bool linger_on;
    uint32_t linger;
    /* stream: received data */
    struct pbuf *rx;
    /* datagrams */
    QSIMPLEQ_HEAD(, Dgram) dq;
    unsigned dq_n;
    size_t dq_bytes;
    /* listening: connections the guest has not accepted yet */
    GQueue pending;
    unsigned backlog;
} Conn;

/* a socket number of a path */
typedef struct TcpSock {
    Conn *c;
    bool nonblock;
    /* GIVESOCKET */
    bool given;
    bool taken;
    uint8_t give_to[16];        /* user ID and subtask, blank = anyone */
} TcpSock;

/* a pending call */
typedef struct TcpReq {
    QTAILQ_ENTRY(TcpReq) next;
    TcpPath *tp;
    IUCVMessage *msg;
    uint16_t call;
    uint16_t lo;
    bool started;               /* CONNECT: this call started it */
    uint32_t done;              /* SEND on a blocking stream: bytes queued */
    QEMUTimer *timer;
    bool timed_out;
} TcpReq;

struct TcpPath {
    IUCVTcpip *s;
    IUCVPath *path;
    bool ready;
    uint16_t apitype;
    uint32_t nsock;
    uint8_t subtask[8];
    TcpSock **socks;
    QTAILQ_HEAD(, TcpReq) reqs;
    unsigned nreqs;
    int32_t last_rc;
    int32_t last_errno;
    QTAILQ_ENTRY(TcpPath) next;
};

struct IUCVTcpip {
    IUCVService parent_obj;

    NICConf conf;
    NICState *nic;
    char *ip_str;
    char *gateway_str;
    char *dns_str;
    char *hostname;
    char *domain;
    uint32_t mtu;
    uint32_t max_sockets;
    uint32_t max_open;

    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
    uint32_t dns[DNS_MAX];
    int ndns;
    uint32_t nconns;            /* endpoints of the stack */
    struct netif netif;
    bool netif_added;
    QEMUBH *bh;
    QEMUTimer *timer;
    GQueue rxq;                 /* GByteArray: received frames */
    QTAILQ_HEAD(, TcpPath) paths;
};

/* lwIP has one state per process */
static IUCVTcpip *the_stack;
static bool lwip_ready;

/* for lwIP: the time in milliseconds, random numbers */
u32_t sys_now(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

uint32_t qemu_lwip_rand(void)
{
    return g_random_int();
}

static void tcp_pump(TcpPath *tp);

/* events of the stack: work through the calls later, outside lwIP */
static void kick(IUCVTcpip *s)
{
    qemu_bh_schedule(s->bh);
}

static void stack_rearm(IUCVTcpip *s)
{
    u32_t ms = sys_timeouts_sleeptime();

    if (ms == SYS_TIMEOUTS_SLEEPTIME_INFINITE) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + ms);
    }
}

/* after anything that can have queued packets to the loopback */
static void stack_run(IUCVTcpip *s)
{
    netif_poll_all();
    stack_rearm(s);
}

static void stack_timer(void *opaque)
{
    sys_check_timeouts();
    stack_run(opaque);
}

static void stack_input(IUCVTcpip *s)
{
    GByteArray *f;

    while ((f = g_queue_pop_head(&s->rxq))) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, f->len, PBUF_RAM);

        if (p) {
            pbuf_take(p, f->data, f->len);
            if (s->netif.input(p, &s->netif) != ERR_OK) {
                pbuf_free(p);
            }
        }
        g_byte_array_free(f, true);
    }
}

static void stack_bh(void *opaque)
{
    IUCVTcpip *s = opaque;
    TcpPath *tp, *n;

    stack_input(s);
    QTAILQ_FOREACH_SAFE(tp, &s->paths, next, n) {
        if (tp->ready) {
            tcp_pump(tp);
        }
    }
    stack_run(s);
}

/* error numbers: lwIP to VM */
static int vm_err(err_t e)
{
    switch (e) {
    case ERR_MEM:
    case ERR_BUF:
        return VE_NOBUFS;
    case ERR_RTE:
        return VE_NETUNREACH;
    case ERR_USE:
        return VE_ADDRINUSE;
    case ERR_ISCONN:
        return VE_ISCONN;
    case ERR_ALREADY:
        return VE_ALREADY;
    case ERR_INPROGRESS:
        return VE_INPROGRESS;
    case ERR_CONN:
    case ERR_CLSD:
        return VE_NOTCONN;
    case ERR_RST:
        return VE_CONNRESET;
    case ERR_ABRT:
        return VE_CONNABORTED;
    case ERR_TIMEOUT:
        return VE_TIMEDOUT;
    default:
        return VE_INVAL;
    }
}

static void put_saddr(uint8_t *g, uint32_t ip, uint16_t port)
{
    memset(g, 0, SADDR);
    stw_be_p(g, VM_AF_INET);
    stw_be_p(g + 2, port);
    stl_be_p(g + 4, ip);
}

static uint32_t ip_get(const ip_addr_t *a)
{
    return a ? lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(a))) : 0;
}

static void ip_put(ip_addr_t *a, uint32_t ip)
{
    ip_addr_set_ip4_u32_val(*a, lwip_htonl(ip));
}

/* replies */

static void req_free(TcpReq *r)
{
    TcpPath *tp = r->tp;

    QTAILQ_REMOVE(&tp->reqs, r, next);
    tp->nreqs--;
    if (r->timer) {
        timer_free(r->timer);
    }
    g_free(r);
}

static void reply(TcpReq *r, int32_t rc, int32_t err, const void *data,
                  uint32_t len)
{
    TcpPath *tp = r->tp;
    IUCVService *svc = IUCV_SERVICE(tp->s);
    g_autofree uint8_t *buf = g_malloc(8 + len);

    trace_iucv_tcpip_reply(iucv_path_id(tp->path), r->call, r->lo, rc,
                           rc == -1 ? err : 0);
    stl_be_p(buf, rc);
    stl_be_p(buf + 4, rc == -1 ? err : 0);
    if (len) {
        memcpy(buf + 8, data, len);
    }
    if (r->msg->reply_max < 8) {
        tp->last_rc = rc;
        tp->last_errno = rc == -1 ? err : 0;
        iucv_reject(svc, tp->path, r->msg);
    } else {
        iucv_reply(svc, tp->path, r->msg, buf,
                   MIN(8 + len, r->msg->reply_max));
    }
    req_free(r);
}

static void reply_err(TcpReq *r, int err)
{
    reply(r, -1, err, NULL, 0);
}

/* the callbacks of the stack */

static err_t cb_tcp_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                         err_t err)
{
    Conn *c = arg;

    if (!p) {
        c->eof = true;
    } else if (c->rd_shut) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
    } else if (c->rx) {
        pbuf_cat(c->rx, p);
    } else {
        c->rx = p;
    }
    kick(c->s);
    return ERR_OK;
}

static err_t cb_tcp_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    Conn *c = arg;

    kick(c->s);
    return ERR_OK;
}

/* the stack freed the control block */
static void cb_tcp_err(void *arg, err_t err)
{
    Conn *c = arg;

    c->tcp = NULL;
    if (c->connecting) {
        /* a refused or unanswered connection attempt */
        c->so_error = err == ERR_RST ? VE_CONNREFUSED : VE_TIMEDOUT;
        c->connecting = false;
    } else if (err == ERR_RST) {
        c->so_error = VE_CONNRESET;
    } else if (err == ERR_ABRT) {
        /* retransmissions without an answer */
        c->so_error = VE_TIMEDOUT;
    } else if (err != ERR_CLSD) {
        c->so_error = vm_err(err);
    }
    kick(c->s);
}

static err_t cb_tcp_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    Conn *c = arg;

    c->connecting = false;
    c->connected = true;
    kick(c->s);
    return ERR_OK;
}

static void tcp_callbacks(Conn *c, struct tcp_pcb *pcb)
{
    tcp_arg(pcb, c);
    tcp_recv(pcb, cb_tcp_recv);
    tcp_sent(pcb, cb_tcp_sent);
    tcp_err(pcb, cb_tcp_err);
}

static Conn *conn_new(IUCVTcpip *s, int type)
{
    Conn *c = g_new0(Conn, 1);

    c->s = s;
    c->refs = 1;
    c->type = type;
    QSIMPLEQ_INIT(&c->dq);
    g_queue_init(&c->pending);
    s->nconns++;
    return c;
}

static err_t cb_tcp_accept(void *arg, struct tcp_pcb *npcb, err_t err)
{
    Conn *l = arg;
    IUCVTcpip *s = l->s;
    Conn *c;

    if (err != ERR_OK || !npcb) {
        return ERR_VAL;
    }
    if (s->nconns >= s->max_open) {
        tcp_abort(npcb);
        return ERR_ABRT;
    }
    c = conn_new(s, T_STREAM);
    c->tcp = npcb;
    c->connected = true;
    tcp_callbacks(c, npcb);
    /*
     * It counts against the backlog until the guest accepts it: with a
     * full backlog the stack ignores new connection requests, which the
     * peers repeat, as BSD sockets do.
     */
    tcp_backlog_delayed(npcb);
    g_queue_push_tail(&l->pending, c);
    kick(s);
    return ERR_OK;
}

static void dgram_queue(Conn *c, struct pbuf *p, uint32_t ip, uint16_t port)
{
    Dgram *d;

    if (c->rd_shut || c->dq_n >= DGRAM_QUEUE ||
        c->dq_bytes + p->tot_len > DGRAM_BYTES) {
        return;
    }
    d = g_malloc(sizeof(*d) + p->tot_len);
    d->ip = ip;
    d->port = port;
    d->len = pbuf_copy_partial(p, d->data, p->tot_len, 0);
    QSIMPLEQ_INSERT_TAIL(&c->dq, d, next);
    c->dq_n++;
    c->dq_bytes += d->len;
    kick(c->s);
}

static void cb_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr, u16_t port)
{
    dgram_queue(arg, p, ip_get(addr), port);
    pbuf_free(p);
}

/* raw ICMP: the guest gets the datagram with its IPv4 header */
static u8_t cb_raw_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                        const ip_addr_t *addr)
{
    dgram_queue(arg, p, ip_get(addr), 0);
    return 0;                   /* the stack handles it too (echo) */
}

static void dgram_drop(Conn *c)
{
    Dgram *d = QSIMPLEQ_FIRST(&c->dq);

    QSIMPLEQ_REMOVE_HEAD(&c->dq, next);
    c->dq_n--;
    c->dq_bytes -= d->len;
    g_free(d);
}

static void conn_destroy(Conn *c, bool reset)
{
    Conn *p;

    while ((p = g_queue_pop_head(&c->pending))) {
        conn_destroy(p, true);
    }
    if (c->tcp) {
        struct tcp_pcb *pcb = c->tcp;

        tcp_arg(pcb, NULL);
        if (c->listening) {
            tcp_accept(pcb, NULL);
            tcp_close(pcb);
        } else {
            tcp_recv(pcb, NULL);
            tcp_sent(pcb, NULL);
            tcp_err(pcb, NULL);
            /* unread data or a zero linger time end with a reset */
            if (reset || c->rx || (c->linger_on && !c->linger) ||
                tcp_close(pcb) != ERR_OK) {
                tcp_abort(pcb);
            }
        }
    }
    if (c->udp) {
        udp_remove(c->udp);
    }
    if (c->raw) {
        raw_remove(c->raw);
    }
    if (c->rx) {
        pbuf_free(c->rx);
    }
    while (!QSIMPLEQ_EMPTY(&c->dq)) {
        dgram_drop(c);
    }
    c->s->nconns--;
    g_free(c);
}

static void conn_unref(Conn *c, bool reset)
{
    if (--c->refs == 0) {
        conn_destroy(c, reset);
    }
}

/* the IP options (SOF_) of the endpoint */
static u8_t *conn_sopts(Conn *c)
{
    if (c->tcp) {
        return &c->tcp->so_options;
    } else if (c->udp) {
        return &c->udp->so_options;
    } else if (c->raw) {
        return &c->raw->so_options;
    }
    return NULL;
}

/* sockets */

static TcpSock *sock_get(TcpPath *tp, uint32_t n)
{
    return n < tp->nsock ? tp->socks[n] : NULL;
}

static void sock_close(TcpPath *tp, uint32_t n, bool reset)
{
    TcpSock *so = tp->socks[n];

    if (!so) {
        return;
    }
    conn_unref(so->c, reset);
    g_free(so);
    tp->socks[n] = NULL;
}

static TcpSock *sock_new(TcpPath *tp, uint32_t n, Conn *c)
{
    TcpSock *so = g_new0(TcpSock, 1);

    so->c = c;
    tp->socks[n] = so;
    return so;
}

/* the guest's address and port of a socket address; false: refused */
static bool get_saddr(const uint8_t *g, ip_addr_t *a, uint16_t *port,
                      int *err)
{
    if (lduw_be_p(g) != VM_AF_INET) {
        *err = VE_AFNOSUPPORT;
        return false;
    }
    ip_put(a, ldl_be_p(g + 4));
    *port = lduw_be_p(g + 2);
    return true;
}

static bool is_bcast(IUCVTcpip *s, uint32_t ip)
{
    return ip == 0xffffffff ||
           (s->mask < 0xfffffffe && ip == (s->ip | ~s->mask));
}

/* the calls; each returns true when it answered the request */

static bool c_socket(TcpReq *r)
{
    TcpPath *tp = r->tp;
    IUCVTcpip *s = tp->s;
    const uint8_t *b = r->msg->data;
    uint32_t dom, type, proto, n;
    Conn *c;

    if (r->msg->len < 16) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    dom = ldl_be_p(b);
    type = ldl_be_p(b + 4);
    proto = ldl_be_p(b + 8);
    n = ldl_be_p(b + 12);
    if (n >= tp->nsock) {
        reply_err(r, VE_RANGE);
        return true;
    }
    if (tp->socks[n]) {
        reply_err(r, VE_INUSE);
        return true;
    }
    if (dom != VM_AF_INET) {
        reply_err(r, VE_AFNOSUPPORT);
        return true;
    }
    if ((type == T_STREAM && proto && proto != IP_PROTO_TCP) ||
        (type == T_DGRAM && proto && proto != IP_PROTO_UDP) ||
        (type == T_RAW && proto != IP_PROTO_ICMP)) {
        reply_err(r, VE_PROTONOSUPPORT);
        return true;
    }
    if (type != T_STREAM && type != T_DGRAM && type != T_RAW) {
        reply_err(r, VE_SOCKTNOSUPPORT);
        return true;
    }
    if (s->nconns >= s->max_open) {
        reply_err(r, VE_MFILE);
        return true;
    }
    c = conn_new(s, type);
    switch (type) {
    case T_STREAM:
        c->tcp = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (c->tcp) {
            tcp_callbacks(c, c->tcp);
        }
        break;
    case T_DGRAM:
        c->udp = udp_new_ip_type(IPADDR_TYPE_V4);
        if (c->udp) {
            udp_recv(c->udp, cb_udp_recv, c);
        }
        break;
    default:
        c->raw = raw_new_ip_type(IPADDR_TYPE_V4, IP_PROTO_ICMP);
        if (c->raw) {
            raw_recv(c->raw, cb_raw_recv, c);
        }
    }
    if (!c->tcp && !c->udp && !c->raw) {
        conn_destroy(c, true);
        reply_err(r, VE_NOBUFS);
        return true;
    }
    sock_new(tp, n, c);
    reply(r, n, 0, NULL, 0);
    return true;
}

static bool c_bind(TcpReq *r, TcpSock *so)
{
    IUCVTcpip *s = r->tp->s;
    Conn *c = so->c;
    ip_addr_t a;
    uint16_t port;
    uint32_t ip;
    int e;
    err_t err;

    if (r->msg->len < SADDR) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    if (!get_saddr(r->msg->data, &a, &port, &e)) {
        reply_err(r, e);
        return true;
    }
    ip = ldl_be_p(r->msg->data + 4);
    if (ip && ip != s->ip && ip != 0x7f000001) {
        reply_err(r, VE_ADDRNOTAVAIL);
        return true;
    }
    if (c->tcp) {
        if (c->tcp->local_port || c->listening) {
            reply_err(r, VE_INVAL);
            return true;
        }
        err = tcp_bind(c->tcp, &a, port);
    } else if (c->udp) {
        if (c->udp->local_port) {
            reply_err(r, VE_INVAL);
            return true;
        }
        err = udp_bind(c->udp, &a, port);
    } else if (c->raw) {
        err = raw_bind(c->raw, &a);
    } else {
        reply_err(r, c->so_error ? c->so_error : VE_INVAL);
        return true;
    }
    if (err != ERR_OK) {
        reply_err(r, vm_err(err));
        return true;
    }
    reply(r, 0, 0, NULL, 0);
    return true;
}

static bool c_listen(TcpReq *r, TcpSock *so)
{
    Conn *c = so->c;
    uint32_t backlog = ldl_be_p(r->msg->data + 4);
    struct tcp_pcb *l;
    err_t err;

    if (c->type != T_STREAM) {
        reply_err(r, VE_OPNOTSUPP);
        return true;
    }
    backlog = MAX(1, MIN(backlog, BACKLOG_MAX));
    if (c->listening) {
        c->backlog = backlog;
        if (c->tcp) {
            tcp_backlog_set(c->tcp, backlog);
        }
        reply(r, 0, 0, NULL, 0);
        return true;
    }
    if (!c->tcp || c->connected || c->connecting) {
        reply_err(r, VE_INVAL);
        return true;
    }
    if (!c->tcp->local_port) {
        err = tcp_bind(c->tcp, IP4_ADDR_ANY, 0);
        if (err != ERR_OK) {
            reply_err(r, vm_err(err));
            return true;
        }
    }
    l = tcp_listen_with_backlog_and_err(c->tcp, backlog, &err);
    if (!l) {
        reply_err(r, vm_err(err));
        return true;
    }
    /* the stack freed the old control block */
    c->tcp = l;
    c->listening = true;
    c->backlog = backlog;
    tcp_arg(l, c);
    tcp_accept(l, cb_tcp_accept);
    reply(r, 0, 0, NULL, 0);
    return true;
}

static bool c_accept(TcpReq *r, TcpSock *so, bool first)
{
    TcpPath *tp = r->tp;
    uint32_t n = ldl_be_p(r->msg->data + 4);
    uint8_t g[SADDR];
    TcpSock *ns;
    Conn *c;

    if (!so->c->listening) {
        reply_err(r, VE_INVAL);
        return true;
    }
    if (n >= tp->nsock) {
        reply_err(r, VE_RANGE);
        return true;
    }
    if (tp->socks[n]) {
        reply_err(r, VE_INUSE);
        return true;
    }
    if (!first) {
        return false;
    }
    c = g_queue_pop_head(&so->c->pending);
    if (!c) {
        if (so->nonblock) {
            reply_err(r, VE_WOULDBLOCK);
            return true;
        }
        return false;
    }
    if (c->tcp) {
        tcp_backlog_accepted(c->tcp);
    }
    ns = sock_new(tp, n, c);
    ns->nonblock = so->nonblock;
    c->linger_on = so->c->linger_on;
    c->linger = so->c->linger;
    c->oobinline = so->c->oobinline;
    put_saddr(g, c->tcp ? ip_get(&c->tcp->remote_ip) : 0,
              c->tcp ? c->tcp->remote_port : 0);
    reply(r, n, 0, g, SADDR);
    return true;
}

static bool c_connect(TcpReq *r, TcpSock *so, bool first)
{
    IUCVTcpip *s = r->tp->s;
    Conn *c = so->c;
    ip_addr_t a;
    uint16_t port;
    uint32_t ip;
    int e;
    err_t err;

    if (r->started) {
        /* the attempt this call started */
        if (c->connecting) {
            return false;
        }
        if (c->so_error) {
            e = c->so_error;
            c->so_error = 0;
            reply_err(r, e);
        } else {
            reply(r, 0, 0, NULL, 0);
        }
        return true;
    }
    if (!first) {
        return false;
    }
    if (r->msg->len < SADDR) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    if (!get_saddr(r->msg->data, &a, &port, &e)) {
        reply_err(r, e);
        return true;
    }
    ip = ldl_be_p(r->msg->data + 4);
    if (c->type != T_STREAM) {
        if (c->udp) {
            err = udp_connect(c->udp, &a, port);
        } else if (c->raw) {
            err = raw_connect(c->raw, &a);
        } else {
            err = ERR_VAL;
        }
        if (err != ERR_OK) {
            reply_err(r, vm_err(err));
        } else {
            reply(r, 0, 0, NULL, 0);
        }
        return true;
    }
    if (c->connecting) {
        if (so->nonblock) {
            reply_err(r, VE_ALREADY);
            return true;
        }
        r->started = true;
        return false;
    }
    if (c->so_error) {
        /* the result of an earlier attempt */
        e = c->so_error;
        c->so_error = 0;
        reply_err(r, e);
        return true;
    }
    if (c->connected) {
        reply_err(r, VE_ISCONN);
        return true;
    }
    if (c->listening || !c->tcp) {
        reply_err(r, VE_OPNOTSUPP);
        return true;
    }
    if (!ip || !port || is_bcast(s, ip) || (ip & 0xf0000000) == 0xe0000000) {
        reply_err(r, VE_ADDRNOTAVAIL);
        return true;
    }
    err = tcp_connect(c->tcp, &a, port, cb_tcp_connected);
    if (err != ERR_OK) {
        reply_err(r, vm_err(err));
        return true;
    }
    c->connecting = true;
    if (so->nonblock) {
        reply_err(r, VE_INPROGRESS);
        return true;
    }
    r->started = true;
    return false;
}

/* a pending error of the socket, taken by the call */
static bool take_error(TcpReq *r, Conn *c)
{
    int e = c->so_error;

    if (!e) {
        return false;
    }
    c->so_error = 0;
    reply_err(r, e);
    return true;
}

/* READ (14) and RECV (16): reply cc, errno, source address, data */
static bool c_recv(TcpReq *r, TcpSock *so, bool first)
{
    Conn *c = so->c;
    uint32_t want = r->msg->reply_max > 24 ? r->msg->reply_max - 24 : 0;
    uint32_t flags = r->call == C_RECV ? ldl_be_p(r->msg->data + 4) : 0;
    bool peek = flags & 2;
    g_autofree uint8_t *buf = NULL;
    uint32_t n;

    if (!first) {
        return false;
    }
    if (c->listening) {
        reply_err(r, VE_NOTCONN);
        return true;
    }
    if (flags & 1) {
        /* the stack keeps no out-of-band data */
        reply_err(r, VE_INVAL);
        return true;
    }
    want = MIN(want, RECV_MAX);
    if (c->type != T_STREAM) {
        Dgram *d = QSIMPLEQ_FIRST(&c->dq);

        if (!d) {
            if (so->nonblock) {
                reply_err(r, VE_WOULDBLOCK);
                return true;
            }
            return false;
        }
        n = MIN(want, d->len);      /* the rest of a datagram is lost */
        buf = g_malloc(SADDR + MAX(n, 1));
        put_saddr(buf, d->ip, d->port);
        memcpy(buf + SADDR, d->data, n);
        if (!peek) {
            dgram_drop(c);
        }
        reply(r, n, 0, buf, SADDR + n);
        return true;
    }
    if (!c->rx) {
        if (take_error(r, c)) {
            return true;
        }
        if (c->eof || c->rd_shut || (c->connected && !c->tcp)) {
            uint8_t g[SADDR];

            put_saddr(g, 0, 0);
            reply(r, 0, 0, g, SADDR);       /* end of file */
            return true;
        }
        if (!c->connected && !c->connecting) {
            reply_err(r, VE_NOTCONN);
            return true;
        }
        if (so->nonblock) {
            reply_err(r, VE_WOULDBLOCK);
            return true;
        }
        return false;
    }
    n = MIN(want, c->rx->tot_len);
    buf = g_malloc(SADDR + MAX(n, 1));
    put_saddr(buf, c->tcp ? ip_get(&c->tcp->remote_ip) : 0,
              c->tcp ? c->tcp->remote_port : 0);
    pbuf_copy_partial(c->rx, buf + SADDR, n, 0);
    if (!peek && n) {
        c->rx = n == c->rx->tot_len ? (pbuf_free(c->rx), NULL)
                                     : pbuf_free_header(c->rx, n);
        if (c->tcp) {
            tcp_recved(c->tcp, n);
        }
    }
    reply(r, n, 0, buf, SADDR + n);
    return true;
}

static bool tcp_writable(Conn *c)
{
    return c->tcp && tcp_sndbuf(c->tcp) > 0 &&
           tcp_sndqueuelen(c->tcp) < TCP_SND_QUEUELEN - 1;
}

/* SEND (20), SENDTO (22), WRITE (26): +0 flags, +4 address, +20 data */
static bool c_send(TcpReq *r, TcpSock *so, bool first)
{
    IUCVTcpip *s = r->tp->s;
    Conn *c = so->c;
    const uint8_t *b = r->msg->data;
    uint32_t len = r->msg->len;
    ip_addr_t a;
    uint16_t port = 0;
    int e;
    err_t err;

    if (!first) {
        return false;
    }
    if (len < 20) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    len -= 20;
    if (c->type == T_STREAM) {
        /*
         * A blocking send completes when all its data is queued (as with
         * BSD sockets; guests do not always loop over a short count); a
         * non-blocking one takes what fits.
         */
        len = MIN(len, SEND_MAX);
        if (c->listening || (!c->connected && !c->connecting)) {
            reply_err(r, VE_NOTCONN);
            return true;
        }
        if (!r->done && take_error(r, c)) {
            return true;
        }
        if (!c->tcp || c->wr_shut) {
            if (r->done) {
                reply(r, r->done, 0, NULL, 0);
            } else {
                reply_err(r, c->so_error ? c->so_error : VE_PIPE);
                c->so_error = 0;
            }
            return true;
        }
        while (r->done < len && !c->connecting && tcp_writable(c)) {
            uint32_t n = MIN(len - r->done, tcp_sndbuf(c->tcp));

            err = tcp_write(c->tcp, b + 20 + r->done, n,
                            TCP_WRITE_FLAG_COPY |
                            (r->done + n < len ? TCP_WRITE_FLAG_MORE : 0));
            if (err == ERR_MEM) {
                break;
            }
            if (err != ERR_OK) {
                if (r->done) {
                    break;
                }
                reply_err(r, vm_err(err));
                return true;
            }
            r->done += n;
        }
        if (r->done) {
            tcp_output(c->tcp);
        }
        if (r->done == len || (so->nonblock && r->done)) {
            reply(r, r->done, 0, NULL, 0);
            return true;
        }
        if (so->nonblock) {
            reply_err(r, VE_WOULDBLOCK);
            return true;
        }
        return false;
    }

    /* datagrams */
    if (c->wr_shut) {
        reply_err(r, VE_PIPE);
        return true;
    }
    if (r->call == C_SENDTO) {
        if (!get_saddr(b + 4, &a, &port, &e)) {
            reply_err(r, e);
            return true;
        }
    } else if (c->udp && (c->udp->flags & UDP_FLAGS_CONNECTED)) {
        a = c->udp->remote_ip;
        port = c->udp->remote_port;
    } else if (c->raw && (c->raw->flags & RAW_FLAGS_CONNECTED)) {
        a = c->raw->remote_ip;
        port = 0;
    } else {
        reply_err(r, VE_DESTADDRREQ);
        return true;
    }
    if (len > UDP_MAX) {
        reply_err(r, VE_MSGSIZE);
        return true;
    }
    if (is_bcast(s, ip_get(&a)) && !(*conn_sopts(c) & SOF_BROADCAST)) {
        reply_err(r, VE_ACCES);
        return true;
    }
    {
        const uint8_t *data = b + 20;
        uint32_t dlen = len, hl = 0;
        struct pbuf *p;

        if (c->raw && dlen >= 20 && (data[0] >> 4) == 4) {
            /* raw ICMP with an IPv4 header: the stack makes its own */
            hl = (data[0] & 0xf) * 4;
            if (hl < 20 || hl > dlen) {
                reply_err(r, VE_INVAL);
                return true;
            }
            data += hl;
            dlen -= hl;
        }
        p = pbuf_alloc(PBUF_TRANSPORT, dlen, PBUF_RAM);
        if (!p) {
            reply_err(r, VE_NOBUFS);
            return true;
        }
        pbuf_take(p, data, dlen);
        err = c->udp ? udp_sendto(c->udp, p, &a, port)
                     : raw_sendto(c->raw, p, &a);
        pbuf_free(p);
        if (err != ERR_OK) {
            reply_err(r, vm_err(err));
            return true;
        }
        reply(r, len, 0, NULL, 0);
    }
    return true;
}

static void name_reply(TcpReq *r, TcpSock *so, bool peer)
{
    IUCVTcpip *s = r->tp->s;
    Conn *c = so->c;
    uint32_t ip = 0;
    uint16_t port = 0;
    uint8_t g[SADDR];

    if (peer) {
        if (c->type == T_STREAM && c->tcp && c->connected && !c->listening) {
            ip = ip_get(&c->tcp->remote_ip);
            port = c->tcp->remote_port;
        } else if (c->udp && (c->udp->flags & UDP_FLAGS_CONNECTED)) {
            ip = ip_get(&c->udp->remote_ip);
            port = c->udp->remote_port;
        } else {
            reply_err(r, VE_NOTCONN);
            return;
        }
    } else if (c->tcp) {
        ip = ip_get(&c->tcp->local_ip);
        port = c->tcp->local_port;
    } else if (c->udp) {
        ip = ip_get(&c->udp->local_ip);
        port = c->udp->local_port;
    }
    if (!ip && port && c->type == T_STREAM && c->connected) {
        ip = s->ip;
    }
    put_saddr(g, ip, port);
    reply(r, 0, 0, g, SADDR);
}

/* socket options (VM numbering) */
enum {
    O_DEBUG = 0x0001, O_REUSEADDR = 0x0004, O_KEEPALIVE = 0x0008,
    O_DONTROUTE = 0x0010, O_BROADCAST = 0x0020, O_LINGER = 0x0080,
    O_OOBINLINE = 0x0100, O_SNDBUF = 0x1001, O_RCVBUF = 0x1002,
    O_ERROR = 0x1007, O_TYPE = 0x1008,
    O_TCP_NODELAY = 0x10001,    /* level IPPROTO_TCP, name 1 */
};

static bool opt_known(uint32_t level, uint32_t name, uint32_t *opt)
{
    if (level == VM_SOL_SOCKET || level == 0xffffffff) {
        switch (name) {
        case O_DEBUG: case O_REUSEADDR: case O_KEEPALIVE: case O_DONTROUTE:
        case O_BROADCAST: case O_LINGER: case O_OOBINLINE: case O_SNDBUF:
        case O_RCVBUF: case O_ERROR: case O_TYPE:
            *opt = name;
            return true;
        }
        return false;
    }
    if (level == IP_PROTO_TCP && name == 1) {
        *opt = O_TCP_NODELAY;
        return true;
    }
    return false;
}

static u8_t sof_of(uint32_t opt)
{
    switch (opt) {
    case O_REUSEADDR:
        return SOF_REUSEADDR;
    case O_KEEPALIVE:
        return SOF_KEEPALIVE;
    case O_BROADCAST:
        return SOF_BROADCAST;
    }
    return 0;
}

static bool c_sockopt(TcpReq *r, TcpSock *so, bool set)
{
    Conn *c = so->c;
    const uint8_t *b = r->msg->data;
    uint32_t opt, v = 0;
    u8_t *sopts = conn_sopts(c);
    uint8_t out[8];

    if (r->msg->len < 8) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    if (!opt_known(ldl_be_p(b), ldl_be_p(b + 4), &opt)) {
        reply_err(r, VE_NOPROTOOPT);
        return true;
    }
    if (set) {
        if (r->msg->len < (opt == O_LINGER ? 16 : 12) || opt == O_ERROR ||
            opt == O_TYPE) {
            reply_err(r, VE_INVAL);
            return true;
        }
        v = ldl_be_p(b + 8);
        switch (opt) {
        case O_LINGER:
            c->linger_on = v;
            c->linger = ldl_be_p(b + 12);
            break;
        case O_DEBUG:
            c->debug = v;
            break;
        case O_DONTROUTE:
            c->dontroute = v;
            break;
        case O_OOBINLINE:
            c->oobinline = v;
            break;
        case O_TCP_NODELAY:
            if (c->type != T_STREAM) {
                reply_err(r, VE_NOPROTOOPT);
                return true;
            }
            if (c->tcp && !c->listening) {
                if (v) {
                    tcp_nagle_disable(c->tcp);
                } else {
                    tcp_nagle_enable(c->tcp);
                }
            }
            break;
        case O_SNDBUF:
        case O_RCVBUF:
            break;              /* the stack's sizes are fixed */
        default:
            if (sopts) {
                if (v) {
                    *sopts |= sof_of(opt);
                } else {
                    *sopts &= ~sof_of(opt);
                }
            }
        }
        reply(r, 0, 0, NULL, 0);
        return true;
    }
    switch (opt) {
    case O_LINGER:
        stl_be_p(out, c->linger_on);
        stl_be_p(out + 4, c->linger);
        reply(r, 0, 0, out, 8);
        return true;
    case O_DEBUG:
        v = c->debug;
        break;
    case O_DONTROUTE:
        v = c->dontroute;
        break;
    case O_OOBINLINE:
        v = c->oobinline;
        break;
    case O_SNDBUF:
        v = c->type == T_STREAM ? TCP_SND_BUF : UDP_MAX;
        break;
    case O_RCVBUF:
        v = c->type == T_STREAM ? TCP_WND : DGRAM_BYTES;
        break;
    case O_ERROR:
        v = c->so_error;
        c->so_error = 0;
        break;
    case O_TYPE:
        v = c->type;
        break;
    case O_TCP_NODELAY:
        v = c->type == T_STREAM && c->tcp && !c->listening &&
            tcp_nagle_disabled(c->tcp);
        break;
    default:
        v = sopts && (*sopts & sof_of(opt)) ? 1 : 0;
    }
    stl_be_p(out, v);
    reply(r, 0, 0, out, 4);
    return true;
}

static void put_ifreq(IUCVTcpip *s, uint8_t *ifr, uint32_t cmd)
{
    memset(ifr + 16, 0, 16);
    switch (cmd) {
    case 0xc020a711:                            /* flags */
        stw_be_p(ifr + 16, IFF_UP | IFF_BROADCAST | IFF_RUNNING);
        break;
    case 0xc020a715:                            /* netmask */
        put_saddr(ifr + 16, s->mask, 0);
        break;
    case 0xc020a712:                            /* broadcast */
        put_saddr(ifr + 16, s->ip | ~s->mask, 0);
        break;
    case 0xc020a70f:                            /* destination */
        put_saddr(ifr + 16, 0, 0);
        break;
    default:                                    /* address */
        put_saddr(ifr + 16, s->ip, 0);
    }
}

static bool c_ioctl(TcpReq *r, TcpSock *so)
{
    IUCVTcpip *s = r->tp->s;
    Conn *c = so->c;
    const uint8_t *b = r->msg->data;
    uint32_t cmd = r->msg->len >= 4 ? ldl_be_p(b) : 0;
    uint8_t out[32];
    uint32_t v = 0;

    switch (cmd) {
    case 0x8004a77e:                            /* FIONBIO */
        so->nonblock = r->msg->len >= 8 && ldl_be_p(b + 4);
        reply(r, 0, 0, NULL, 0);
        return true;
    case 0x4004a77f:                            /* FIONREAD */
        if (c->rx) {
            v = c->rx->tot_len;
        } else if (!QSIMPLEQ_EMPTY(&c->dq)) {
            v = QSIMPLEQ_FIRST(&c->dq)->len;
        }
        stl_be_p(out, v);
        reply(r, 0, 0, out, 4);
        return true;
    case 0x4004a707:                            /* SIOCATMARK */
        stl_be_p(out, 0);
        reply(r, 0, 0, out, 4);
        return true;
    case 0xc020a70d:                            /* SIOCGIFADDR */
    case 0xc020a70b:                            /* the same, older value */
    case 0xc020a712:
    case 0xc020a70f:
    case 0xc020a711:
    case 0xc020a715:
        if (r->msg->len < 36) {
            reply_err(r, VE_BADLEN);
            return true;
        }
        memcpy(out, b + 4, 16);
        put_ifreq(s, out, cmd);
        reply(r, 0, 0, out, 32);
        return true;
    case 0xc008a714:                            /* SIOCGIFCONF: one interface */
        memset(out, 0x40, 16);
        ebcdic_put(out, "ETH0", 4);
        put_ifreq(s, out, 0xc020a70d);
        reply(r, 0, 0, out, 32);
        return true;
    default:
        reply_err(r, VE_INVAL);
        return true;
    }
}

static void clientid(TcpPath *tp, uint8_t *c)
{
    memset(c, 0, 40);
    stl_be_p(c, VM_AF_INET);
    iucv_userid_from_str(c + 4, s390_iucv_userid());
    memcpy(c + 12, tp->subtask, 8);
}

static bool blank(const uint8_t *p, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (p[i] && p[i] != 0x40) {
            return false;
        }
    }
    return true;
}

static bool c_givesocket(TcpReq *r, TcpSock *so)
{
    Conn *c = so->c;

    if (r->msg->len < 40) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    if (c->type != T_STREAM || c->listening) {
        reply_err(r, c->listening ? VE_BUSY : VE_OPNOTSUPP);
        return true;
    }
    so->given = true;
    so->taken = false;
    memcpy(so->give_to, r->msg->data + 4, 16);
    reply(r, 0, 0, NULL, 0);
    return true;
}

static bool c_takesocket(TcpReq *r)
{
    TcpPath *tp = r->tp, *giver;
    const uint8_t *b = r->msg->data;
    uint32_t his, n;
    uint8_t me[40];
    TcpSock *so;

    if (r->msg->len < 48) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    his = ldl_be_p(b + 40);
    n = ldl_be_p(b + 44);
    if (ldl_be_p(b) != VM_AF_INET) {
        reply_err(r, VE_PFNOSUPPORT);
        return true;
    }
    if (n >= tp->nsock) {
        reply_err(r, VE_RANGE);
        return true;
    }
    if (tp->socks[n]) {
        reply_err(r, VE_INUSE);
        return true;
    }
    clientid(tp, me);
    QTAILQ_FOREACH(giver, &tp->s->paths, next) {
        if (giver->ready && !memcmp(giver->subtask, b + 12, 8)) {
            break;
        }
    }
    so = giver ? sock_get(giver, his) : NULL;
    if (!so || !so->given) {
        reply_err(r, VE_BADF);
        return true;
    }
    if ((!blank(so->give_to, 8) && memcmp(so->give_to, me + 4, 8)) ||
        (!blank(so->give_to + 8, 8) && memcmp(so->give_to + 8, me + 12, 8))) {
        reply_err(r, VE_ACCES);
        return true;
    }
    /* both socket numbers name the connection until the giver closes */
    so->c->refs++;
    sock_new(tp, n, so->c);
    so->given = false;
    so->taken = true;
    reply(r, n, 0, NULL, 0);
    /* the giver's SELECT sees the exception condition */
    kick(tp->s);
    return true;
}

/* SELECT: big-endian fullwords, socket d is bit 1 << (d % 32) of word d / 32 */
static bool fd_isset(const uint8_t *set, uint32_t d)
{
    return ldl_be_p(set + (d / 32) * 4) & (1u << (d % 32));
}

static void fd_set_bit(uint8_t *set, uint32_t d)
{
    stl_be_p(set + (d / 32) * 4, ldl_be_p(set + (d / 32) * 4) |
             (1u << (d % 32)));
}

static bool sock_readable(TcpSock *so)
{
    Conn *c = so->c;

    if (c->listening) {
        return !g_queue_is_empty(&c->pending);
    }
    if (c->type != T_STREAM) {
        return !QSIMPLEQ_EMPTY(&c->dq);
    }
    /* data, end of file, an error, or nothing to wait for */
    return c->rx || c->eof || c->so_error || c->rd_shut ||
           (c->connected && !c->tcp) || (!c->connected && !c->connecting);
}

static bool sock_writable(TcpSock *so)
{
    Conn *c = so->c;

    if (c->type != T_STREAM) {
        return true;
    }
    if (c->listening || c->connecting) {
        return false;
    }
    /* a send reports the error or the missing connection at once */
    return tcp_writable(c) || !c->tcp || c->so_error || c->wr_shut ||
           !c->connected;
}

static void select_timeout(void *opaque)
{
    TcpReq *r = opaque;

    r->timed_out = true;
    kick(r->tp->s);
}

static bool c_select(TcpReq *r)
{
    TcpPath *tp = r->tp;
    const uint8_t *b = r->msg->data;
    uint32_t fdsize = r->lo, nfds, d, found = 0;
    const uint8_t *in[3];
    g_autofree uint8_t *out = NULL;
    bool used[3];
    int k;

    if (r->msg->len < 28 + 3 * fdsize || fdsize % 4) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    nfds = MIN(ldl_be_p(b), fdsize * 8);
    out = g_malloc0(8 + 3 * fdsize);
    for (k = 0; k < 3; k++) {
        used[k] = ldl_be_p(b + 8 + 4 * k);
        in[k] = b + 28 + k * fdsize;
    }
    for (d = 0; d < nfds; d++) {
        TcpSock *so;

        if (!(used[0] && fd_isset(in[0], d)) &&
            !(used[1] && fd_isset(in[1], d)) &&
            !(used[2] && fd_isset(in[2], d))) {
            continue;
        }
        so = sock_get(tp, d);
        if (!so) {
            reply_err(r, VE_BADF);
            return true;
        }
        if (used[0] && fd_isset(in[0], d) && sock_readable(so)) {
            fd_set_bit(out + 8, d);
            found++;
        }
        if (used[1] && fd_isset(in[1], d) && sock_writable(so)) {
            fd_set_bit(out + 8 + fdsize, d);
            found++;
        }
        if (used[2] && fd_isset(in[2], d) && so->taken) {
            fd_set_bit(out + 8 + 2 * fdsize, d);
            found++;
        }
    }
    if (!found && !r->timed_out) {
        if (ldl_be_p(b + 4)) {
            uint64_t ms = (uint64_t)ldl_be_p(b + 20) * 1000 +
                          ldl_be_p(b + 24) / 1000;

            if (!ms) {
                r->timed_out = true;
            } else if (!r->timer) {
                r->timer = timer_new_ms(QEMU_CLOCK_REALTIME, select_timeout,
                                        r);
                timer_mod(r->timer,
                          qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + ms);
            }
        }
        if (!r->timed_out) {
            return false;
        }
    }
    reply(r, found, 0, out, 8 + 3 * fdsize);
    return true;
}

static bool c_shutdown(TcpReq *r, TcpSock *so)
{
    Conn *c = so->c;
    uint32_t how = ldl_be_p(r->msg->data + 4);

    if (how > 2) {
        reply_err(r, VE_INVAL);
        return true;
    }
    if (c->type == T_STREAM && (c->listening || !c->connected)) {
        reply_err(r, VE_NOTCONN);
        return true;
    }
    if (how != 1 && !c->rd_shut) {
        /* received data is dropped from now on; the window stays open */
        c->rd_shut = true;
        if (c->rx) {
            if (c->tcp) {
                tcp_recved(c->tcp, c->rx->tot_len);
            }
            pbuf_free(c->rx);
            c->rx = NULL;
        }
        while (!QSIMPLEQ_EMPTY(&c->dq)) {
            dgram_drop(c);
        }
    }
    if (how != 0 && !c->wr_shut) {
        c->wr_shut = true;
        if (c->tcp) {
            tcp_shutdown(c->tcp, 0, 1);
        }
    }
    reply(r, 0, 0, NULL, 0);
    return true;
}

/* run one request; false: it waits */
static bool tcp_try(TcpReq *r, bool first)
{
    TcpPath *tp = r->tp;
    const uint8_t *b = r->msg->data;
    uint8_t out[48];
    TcpSock *so = NULL;

    switch (r->call) {
    case C_SOCKET:
        return c_socket(r);
    case C_TAKESOCKET:
        return c_takesocket(r);
    case C_SELECT:
        return c_select(r);
    case C_GETHOSTID:
        stl_be_p(out, tp->s->ip);
        stl_be_p(out + 4, 0);
        trace_iucv_tcpip_reply(iucv_path_id(tp->path), r->call, 0, 0, 0);
        iucv_reply(IUCV_SERVICE(tp->s), tp->path, r->msg, out,
                   MIN(8, r->msg->reply_max));
        req_free(r);
        return true;
    case C_GETHOSTNAME: {
        const char *h = tp->s->hostname ? tp->s->hostname : "qemu";
        size_t l = MIN(strlen(h), sizeof(out));

        ebcdic_put(out, h, l);
        reply(r, 0, 0, out, l);
        return true;
    }
    case C_GETCLIENTID:
        clientid(tp, out);
        reply(r, 0, 0, out, 40);
        return true;
    case C_LASTERRNO:
        reply(r, tp->last_rc, tp->last_errno, NULL, 0);
        return true;
    case C_CANCEL: {
        uint32_t call = ldl_be_p(b), id = ldl_be_p(b + 4);
        TcpReq *t;

        QTAILQ_FOREACH(t, &tp->reqs, next) {
            if (t != r && t->msg->id == id && t->call == call) {
                if (call != C_SELECT && call != C_ACCEPT && call != C_READ &&
                    call != C_RECV && call != C_SEND && call != C_SENDTO &&
                    call != C_WRITE && call != C_CONNECT) {
                    reply_err(r, VE_INVAL);
                    return true;
                }
                reply_err(t, VE_CANCELED);
                reply(r, 0, 0, NULL, 0);
                return true;
            }
        }
        reply_err(r, VE_SRCH);
        return true;
    }
    }

    so = sock_get(tp, r->lo);
    if (!so) {
        reply_err(r, r->lo >= tp->nsock ? VE_RANGE : VE_BADF);
        return true;
    }
    switch (r->call) {
    case C_BIND:
        return c_bind(r, so);
    case C_LISTEN:
        return c_listen(r, so);
    case C_ACCEPT:
        return c_accept(r, so, first);
    case C_CONNECT:
        return c_connect(r, so, first);
    case C_READ:
    case C_RECV:
        return c_recv(r, so, first);
    case C_SEND:
    case C_SENDTO:
    case C_WRITE:
        return c_send(r, so, first);
    case C_CLOSE:
        sock_close(tp, r->lo, false);
        reply(r, 0, 0, NULL, 0);
        return true;
    case C_SHUTDOWN:
        return c_shutdown(r, so);
    case C_FCNTL:
        if (ldl_be_p(b) == 4) {
            so->nonblock = ldl_be_p(b + 4) & VM_FNDELAY;
            reply(r, 0, 0, NULL, 0);
        } else if (ldl_be_p(b) == 3) {
            reply(r, so->nonblock ? VM_FNDELAY : 0, 0, NULL, 0);
        } else {
            reply_err(r, VE_INVAL);
        }
        return true;
    case C_GETSOCKNAME:
    case C_GETPEERNAME:
        name_reply(r, so, r->call == C_GETPEERNAME);
        return true;
    case C_GETSOCKOPT:
        /* level and name come in the parameter list, at the same offsets */
        return c_sockopt(r, so, false);
    case C_SETSOCKOPT:
        return c_sockopt(r, so, true);
    case C_IOCTL:
        return c_ioctl(r, so);
    case C_GIVESOCKET:
        return c_givesocket(r, so);
    }
    reply_err(r, VE_BADCALL);
    return true;
}

static bool is_read(uint16_t c)
{
    return c == C_READ || c == C_RECV;
}

static bool is_write(uint16_t c)
{
    return c == C_SEND || c == C_SENDTO || c == C_WRITE;
}

/*
 * Work through the pending requests: per socket, reads, writes and
 * ACCEPTs each in their order; SELECTs are evaluated every time.
 */
static void tcp_pump(TcpPath *tp)
{
    g_autofree uint8_t *rd = g_malloc0(tp->nsock + 1);
    g_autofree uint8_t *wr = g_malloc0(tp->nsock + 1);
    TcpReq *r, *n;
    bool again = true;

    while (again) {
        again = false;
        memset(rd, 0, tp->nsock + 1);
        memset(wr, 0, tp->nsock + 1);
        QTAILQ_FOREACH_SAFE(r, &tp->reqs, next, n) {
            uint8_t *seen = NULL;
            bool first = true;

            if (r->lo < tp->nsock && r->call != C_SELECT &&
                r->call != C_CANCEL) {
                if (is_read(r->call) || r->call == C_ACCEPT) {
                    seen = &rd[r->lo];
                } else if (is_write(r->call) || r->call == C_CONNECT) {
                    seen = &wr[r->lo];
                }
            }
            if (seen) {
                first = !*seen;
                *seen = 1;
            }
            if (tcp_try(r, first)) {
                again = true;
                break;  /* the list changed */
            }
        }
    }
}

/* the initialization message: "IUCVAPI ", maxsock, apitype, subtask */
static void tcp_init(TcpPath *tp, IUCVMessage *msg)
{
    static const uint8_t magic[8] = {
        0xc9, 0xe4, 0xc3, 0xe5, 0xc1, 0xd7, 0xc9, 0x40
    };
    IUCVService *svc = IUCV_SERVICE(tp->s);
    uint8_t user[16], out[8];
    const char *why = NULL;
    uint32_t want;

    if (msg->len != 20) {
        why = "BAD INIT MSG LEN";
    } else if (memcmp(msg->data, magic, 8)) {
        why = "REQUIREDCONSTANT";
    } else if (lduw_be_p(msg->data + 10) != 2 &&
               lduw_be_p(msg->data + 10) != 3) {
        why = lduw_be_p(msg->data + 10) == 4 ? "IPV6 NOT ENABLED" :
                                               "BAD API TYPE";
    }
    if (why) {
        trace_iucv_tcpip_init(iucv_path_id(tp->path), 0, 0, why);
        memset(user, 0x40, 16);
        ebcdic_put(user, why, strlen(why));
        iucv_sever(svc, tp->path, user);
        return;
    }
    want = MAX(lduw_be_p(msg->data + 8), NSOCK_MIN);
    tp->nsock = MIN(want, tp->s->max_sockets);
    tp->apitype = lduw_be_p(msg->data + 10);
    memcpy(tp->subtask, msg->data + 12, 8);
    tp->socks = g_new0(TcpSock *, tp->nsock);
    tp->ready = true;
    trace_iucv_tcpip_init(iucv_path_id(tp->path), tp->apitype, tp->nsock,
                          "ok");
    stl_be_p(out, 0);
    stl_be_p(out + 4, tp->nsock - 1);
    iucv_reply(svc, tp->path, msg, out, MIN(8, msg->reply_max));
}

/* the IUCV side */

static int tcpip_connect(IUCVService *svc, IUCVPath *path,
                         const uint8_t *user)
{
    IUCVTcpip *s = IUCV_TCPIP(svc);
    TcpPath *tp = g_new0(TcpPath, 1);

    tp->s = s;
    tp->path = path;
    QTAILQ_INIT(&tp->reqs);
    QTAILQ_INSERT_TAIL(&s->paths, tp, next);
    iucv_path_set_opaque(path, tp);
    return 0;
}

static void tcpip_message(IUCVService *svc, IUCVPath *path, IUCVMessage *msg)
{
    IUCVTcpip *s = IUCV_TCPIP(svc);
    TcpPath *tp = iucv_path_get_opaque(path);
    TcpReq *r;

    if (!msg->two_way) {
        return;
    }
    if (!tp->ready) {
        tcp_init(tp, msg);
        return;
    }
    if (!msg->prmdata && msg->len < 8) {
        /* every call has at least two words */
        uint8_t out[8];

        stl_be_p(out, -1);
        stl_be_p(out + 4, VE_BADLEN);
        iucv_reply(svc, path, msg, out, MIN(8, msg->reply_max));
        return;
    }
    if (tp->nreqs >= REQS_MAX) {
        tp->last_rc = -1;
        tp->last_errno = VE_NOBUFS;
        iucv_reject(svc, path, msg);
        return;
    }
    r = g_new0(TcpReq, 1);
    r->tp = tp;
    r->msg = msg;
    r->call = msg->trgcls >> 16;
    r->lo = msg->trgcls;
    trace_iucv_tcpip_call(iucv_path_id(path), r->call, r->lo, msg->len,
                          msg->reply_max);
    QTAILQ_INSERT_TAIL(&tp->reqs, r, next);
    tp->nreqs++;
    tcp_pump(tp);
    stack_run(s);
}

/* the guest withdrew a call with PURGE: no reply */
static void tcpip_cancelled(IUCVService *svc, IUCVPath *path,
                            IUCVMessage *msg)
{
    TcpPath *tp = iucv_path_get_opaque(path);
    TcpReq *r;

    QTAILQ_FOREACH(r, &tp->reqs, next) {
        if (r->msg == msg) {
            req_free(r);
            break;
        }
    }
}

static void tcpip_severed(IUCVService *svc, IUCVPath *path)
{
    IUCVTcpip *s = IUCV_TCPIP(svc);
    TcpPath *tp = iucv_path_get_opaque(path);
    TcpReq *r;
    uint32_t i;

    if (!tp) {
        return;
    }
    while ((r = QTAILQ_FIRST(&tp->reqs))) {
        req_free(r);
    }
    for (i = 0; i < tp->nsock; i++) {
        /* connections of the path are reset, as by TCP/IP */
        sock_close(tp, i, true);
    }
    QTAILQ_REMOVE(&s->paths, tp, next);
    g_free(tp->socks);
    g_free(tp);
    iucv_path_set_opaque(path, NULL);
    stack_run(s);
}

/* the network backend */

static bool stack_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t stack_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    IUCVTcpip *s = qemu_get_nic_opaque(nc);
    GByteArray *f;

    if (size < SIZEOF_ETH_HDR || size > 0xffff ||
        g_queue_get_length(&s->rxq) >= RXQ_MAX) {
        return size;
    }
    /* unicast for other stations (a bridge floods unknown ones) */
    if (!(buf[0] & 1) && memcmp(buf, s->conf.macaddr.a, ETH_HWADDR_LEN)) {
        return size;
    }
    f = g_byte_array_sized_new(size);
    g_byte_array_append(f, buf, size);
    g_queue_push_tail(&s->rxq, f);
    kick(s);
    return size;
}

static void stack_link_status_changed(NetClientState *nc)
{
    IUCVTcpip *s = qemu_get_nic_opaque(nc);

    if (nc->link_down) {
        netif_set_link_down(&s->netif);
    } else {
        netif_set_link_up(&s->netif);
    }
    stack_run(s);
}

static NetClientInfo stack_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = stack_can_receive,
    .receive = stack_receive,
    .link_status_changed = stack_link_status_changed,
};

static err_t stack_linkoutput(struct netif *nif, struct pbuf *p)
{
    IUCVTcpip *s = nif->state;
    NetClientState *nc = qemu_get_queue(s->nic);

    if (!p->next) {
        qemu_send_packet(nc, p->payload, p->len);
    } else {
        g_autofree uint8_t *f = g_malloc(p->tot_len);

        pbuf_copy_partial(p, f, p->tot_len, 0);
        qemu_send_packet(nc, f, p->tot_len);
    }
    return ERR_OK;
}

static err_t stack_netif_init(struct netif *nif)
{
    IUCVTcpip *s = nif->state;

    nif->name[0] = 'e';
    nif->name[1] = 'n';
    nif->output = etharp_output;
    nif->linkoutput = stack_linkoutput;
    nif->mtu = s->mtu;
    nif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(nif->hwaddr, s->conf.macaddr.a, ETH_HWADDR_LEN);
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                 NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

/* configuration */

static bool parse_ip(const char *str, uint32_t *ip, uint32_t *mask,
                     Error **errp)
{
    g_autofree char *s = g_strdup(str);
    char *slash = strchr(s, '/');
    struct in_addr a;
    int bits = 32;

    if (slash) {
        if (!mask) {
            error_setg(errp, "'%s' must be an address without a prefix",
                       str);
            return false;
        }
        *slash = 0;
        if (qemu_strtoi(slash + 1, NULL, 10, &bits) || bits < 0 ||
            bits > 32) {
            error_setg(errp, "bad prefix length in '%s'", str);
            return false;
        }
    }
    if (!inet_aton(s, &a)) {
        error_setg(errp, "bad IPv4 address '%s'", str);
        return false;
    }
    *ip = ntohl(a.s_addr);
    if (mask) {
        *mask = bits ? 0xffffffffU << (32 - bits) : 0;
    }
    return true;
}

/* dns=A[;B...]: the name servers the guest is configured with */
static bool parse_dns(IUCVTcpip *s, Error **errp)
{
    g_auto(GStrv) v = NULL;
    int i;

    if (!s->dns_str || !*s->dns_str) {
        return true;
    }
    v = g_strsplit_set(s->dns_str, ";+", -1);
    for (i = 0; v[i]; i++) {
        if (!*v[i]) {
            continue;
        }
        if (s->ndns == DNS_MAX) {
            error_setg(errp, "at most %d name servers", DNS_MAX);
            return false;
        }
        if (!parse_ip(v[i], &s->dns[s->ndns], NULL, errp)) {
            return false;
        }
        s->ndns++;
    }
    return true;
}

static void tcpip_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    IUCVTcpip *s = IUCV_TCPIP(dev);
    DeviceClass *parent = DEVICE_CLASS(object_class_get_parent(
                              object_class_by_name(TYPE_IUCV_TCPIP)));
    ip4_addr_t ip, mask, gw;

    if (the_stack) {
        error_setg(errp, "only one iucv-tcpip device is possible: the "
                   "TCP/IP stack exists once per process");
        return;
    }
    if (!s->conf.peers.ncs[0]) {
        error_setg(errp, "iucv-tcpip needs a network backend (netdev=)");
        return;
    }
    if (!s->ip_str) {
        error_setg(errp, "iucv-tcpip needs ip=<address>/<prefix>");
        return;
    }
    if (!parse_ip(s->ip_str, &s->ip, &s->mask, errp) ||
        (s->gateway_str && !parse_ip(s->gateway_str, &s->gateway, NULL,
                                     errp)) ||
        !parse_dns(s, errp)) {
        return;
    }
    if (!s->ip || (s->ip >> 24) == 127 || (s->ip & 0xf0000000) >= 0xe0000000 ||
        (s->mask < 0xfffffffe &&
         ((s->ip & ~s->mask) == 0 || (s->ip & ~s->mask) == ~s->mask))) {
        error_setg(errp, "'%s' is not a host address", s->ip_str);
        return;
    }
    if (s->gateway && ((s->gateway & s->mask) != (s->ip & s->mask) ||
                       s->gateway == s->ip)) {
        error_setg(errp, "the gateway must be another address in the "
                   "subnet of %s", s->ip_str);
        return;
    }
    if (s->mtu < MTU_MIN || s->mtu > MTU_MAX) {
        error_setg(errp, "mtu must be %d to %d", MTU_MIN, MTU_MAX);
        return;
    }
    if (s->max_sockets < NSOCK_MIN || s->max_sockets > 4096) {
        error_setg(errp, "max-sockets must be %d to 4096", NSOCK_MIN);
        return;
    }
    if (!s->max_open || s->max_open > MEMP_NUM_TCP_PCB) {
        error_setg(errp, "max-open-sockets must be 1 to %d",
                   MEMP_NUM_TCP_PCB);
        return;
    }
    parent->realize(dev, errp);
    if (*errp) {
        return;
    }
    QTAILQ_INIT(&s->paths);
    g_queue_init(&s->rxq);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&stack_net_info, &s->conf,
                          object_get_typename(OBJECT(s)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    s->bh = qemu_bh_new_guarded(stack_bh, s, &dev->mem_reentrancy_guard);
    s->timer = timer_new_ms(QEMU_CLOCK_REALTIME, stack_timer, s);

    if (!lwip_ready) {
        lwip_init();
        lwip_ready = true;
    }
    ip4_addr_set_u32(&ip, htonl(s->ip));
    ip4_addr_set_u32(&mask, htonl(s->mask));
    ip4_addr_set_u32(&gw, htonl(s->gateway));
    netif_add(&s->netif, &ip, &mask, &gw, s, stack_netif_init,
              ethernet_input);
    s->netif_added = true;
    netif_set_default(&s->netif);
    if (!qemu_get_queue(s->nic)->link_down) {
        netif_set_link_up(&s->netif);
    }
    /* announces the address (gratuitous ARP) */
    netif_set_up(&s->netif);
    the_stack = s;
    stack_run(s);
}

static void tcpip_unrealize(DeviceState *dev)
{
    IUCVTcpip *s = IUCV_TCPIP(dev);
    DeviceClass *parent = DEVICE_CLASS(object_class_get_parent(
                              object_class_by_name(TYPE_IUCV_TCPIP)));

    /* severs the paths, which closes their sockets */
    parent->unrealize(dev);
    if (s->netif_added) {
        netif_remove(&s->netif);
        s->netif_added = false;
    }
    if (s->timer) {
        timer_free(s->timer);
        s->timer = NULL;
    }
    if (s->bh) {
        qemu_bh_delete(s->bh);
        s->bh = NULL;
    }
    while (!g_queue_is_empty(&s->rxq)) {
        g_byte_array_free(g_queue_pop_head(&s->rxq), true);
    }
    if (s->nic) {
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
    if (the_stack == s) {
        the_stack = NULL;
    }
}

static const Property tcpip_properties[] = {
    DEFINE_NIC_PROPERTIES(IUCVTcpip, conf),
    DEFINE_PROP_STRING("ip", IUCVTcpip, ip_str),
    DEFINE_PROP_STRING("gateway", IUCVTcpip, gateway_str),
    /* informational: the guest's resolver uses them itself */
    DEFINE_PROP_STRING("dns", IUCVTcpip, dns_str),
    DEFINE_PROP_STRING("hostname", IUCVTcpip, hostname),
    DEFINE_PROP_STRING("domain", IUCVTcpip, domain),
    DEFINE_PROP_UINT32("mtu", IUCVTcpip, mtu, 1500),
    /* socket numbers of one path (its table) */
    DEFINE_PROP_UINT32("max-sockets", IUCVTcpip, max_sockets, 256),
    /* sockets of the stack, all paths together */
    DEFINE_PROP_UINT32("max-open-sockets", IUCVTcpip, max_open, 512),
};

static void tcpip_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    IUCVServiceClass *sc = IUCV_SERVICE_CLASS(oc);

    dc->realize = tcpip_realize;
    dc->unrealize = tcpip_unrealize;
    dc->desc = "IUCV TCP/IP service (socket interface, own TCP/IP stack "
               "on a network backend)";
    dc->hotpluggable = false;
    device_class_set_props(dc, tcpip_properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    sc->max_message = SEND_MAX + 64;
    sc->connect = tcpip_connect;
    sc->message = tcpip_message;
    sc->cancelled = tcpip_cancelled;
    sc->severed = tcpip_severed;
}

static const TypeInfo tcpip_type_info = {
    .name = TYPE_IUCV_TCPIP,
    .parent = TYPE_IUCV_SERVICE,
    .instance_size = sizeof(IUCVTcpip),
    .class_init = tcpip_class_init,
};

static void tcpip_register_types(void)
{
    type_register_static(&tcpip_type_info);
}

type_init(tcpip_register_types)
