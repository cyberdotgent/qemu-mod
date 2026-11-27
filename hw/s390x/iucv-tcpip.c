/*
 * IUCV TCP/IP service: the socket interface of a VM TCP/IP virtual
 * machine over IUCV, on host sockets
 *
 * Guests written for VM TCP/IP connect an IUCV path to the TCPIP user ID,
 * send an initialization message and then issue each socket call as a
 * two-way IUCV message: the target class holds the call number and the
 * socket number, the reply holds the return value, the error number and
 * the data. Calls that cannot complete at once are answered later.
 *
 * The service runs the calls on non-blocking host sockets with a policy:
 * outgoing traffic is translated like NAT (the guest sees the address it
 * is configured with), connections to the host itself, link-local and
 * configured addresses are refused unless allowed, and the guest can only
 * accept connections on ports that are forwarded to it explicitly.
 * Broadcasts are not offered: SO_BROADCAST is refused, and so are the
 * network and broadcast addresses of the host's own subnets. Raw sockets
 * are limited to ICMP echo through unprivileged ICMP sockets, and only
 * when allowed. The host sockets of all paths together are limited
 * (max-host-sockets).
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
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/s390x/iucv.h"
#include "hw/s390x/ebcdic.h"
#include "trace.h"
#include "qemu/bswap.h"
#include <ifaddrs.h>
#include <poll.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <netinet/ip_icmp.h>
#include <sys/ioctl.h>

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
#define VE_FAULT        14
#define VE_BUSY         16
#define VE_INVAL        22
#define VE_MFILE        24
#define VE_WOULDBLOCK   35
#define VE_INPROGRESS   36
#define VE_ALREADY      37
#define VE_NOTSOCK      38
#define VE_MSGSIZE      40
#define VE_NOPROTOOPT   42
#define VE_PROTONOSUPPORT 43
#define VE_SOCKTNOSUPPORT 44
#define VE_OPNOTSUPP    45
#define VE_AFNOSUPPORT  47
#define VE_ADDRNOTAVAIL 49
#define VE_NETUNREACH   51
#define VE_ISCONN       56
#define VE_NOTCONN      57
#define VE_CONNREFUSED  61
#define VE_HOSTUNREACH  65
#define VE_BADCALL      1000
#define VE_BADLEN       1001
#define VE_RANGE        1002
#define VE_INUSE        1003
#define VE_CONFLICT     1008
#define VE_CANCELED     1009

#define VM_AF_INET      2
#define VM_SOL_SOCKET   0xffff
#define VM_FNDELAY      4
#define SADDR           16      /* AF_INET socket address */
#define RECV_MAX        65536
#define SEND_MAX        (1024 * 1024)
#define REQS_MAX        256
#define NSOCK_MIN       50

typedef struct TcpPath TcpPath;

/* a pending call */
typedef struct TcpReq {
    QTAILQ_ENTRY(TcpReq) next;
    TcpPath *tp;
    IUCVMessage *msg;
    uint16_t call;
    uint16_t lo;
    QEMUTimer *timer;
    bool timed_out;
} TcpReq;

typedef struct TcpSock {
    int fd;                     /* -1: no host socket (listening, unmapped) */
    int type;                   /* 1 stream, 2 datagram, 3 raw */
    bool nonblock;
    bool listening;
    bool connecting;
    bool icmp;                  /* raw ICMP on an ICMP datagram socket */
    uint16_t port;              /* local port the guest sees */
    uint32_t dns_peer;          /* datagrams of the resolver come from here */
    /* GIVESOCKET */
    bool given;
    bool taken;
    uint8_t give_to[16];        /* user ID and subtask, blank = anyone */
    int so_error;
} TcpSock;

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

/* the host port is bound when the guest binds the guest port */
typedef struct HostFwd {
    bool udp;
    uint16_t host_port;
    uint16_t guest_port;
} HostFwd;

struct IUCVTcpip {
    IUCVService parent_obj;

    char *ip_str;
    char *hostname;
    char *hostfwd;
    char *fwd_addr_str;
    char *dns_str;
    char *deny;
    bool allow_host;
    bool raw;
    uint32_t max_sockets;
    uint32_t max_host_sockets;

    uint32_t nfds;              /* host sockets open */
    uint32_t ip;
    uint32_t mask;
    uint32_t dns;
    struct in_addr fwd_addr;
    struct sockaddr_in resolver;
    HostFwd *fwds;
    int nfwds;
    uint32_t *deny_net;
    uint32_t *deny_mask;
    int ndeny;
    QTAILQ_HEAD(, TcpPath) paths;
};

static void tcp_pump(TcpPath *tp);

/* error numbers: Linux to VM */
static int vm_errno(int e)
{
    switch (e) {
    case EAGAIN:
        return VE_WOULDBLOCK;
    case EINPROGRESS:
        return VE_INPROGRESS;
    case EALREADY:
        return VE_ALREADY;
    case ENOTSOCK:
        return VE_NOTSOCK;
    case EDESTADDRREQ:
        return 39;
    case EMSGSIZE:
        return VE_MSGSIZE;
    case EPROTOTYPE:
        return 41;
    case ENOPROTOOPT:
        return VE_NOPROTOOPT;
    case EPROTONOSUPPORT:
        return VE_PROTONOSUPPORT;
    case ESOCKTNOSUPPORT:
        return VE_SOCKTNOSUPPORT;
    case EOPNOTSUPP:
        return VE_OPNOTSUPP;
    case EPFNOSUPPORT:
        return 46;
    case EAFNOSUPPORT:
        return VE_AFNOSUPPORT;
    case EADDRINUSE:
        return 48;
    case EADDRNOTAVAIL:
        return VE_ADDRNOTAVAIL;
    case ENETDOWN:
        return 50;
    case ENETUNREACH:
        return VE_NETUNREACH;
    case ENETRESET:
        return 52;
    case ECONNABORTED:
        return 53;
    case ECONNRESET:
        return 54;
    case ENOBUFS:
        return 55;
    case EISCONN:
        return VE_ISCONN;
    case ENOTCONN:
        return VE_NOTCONN;
    case ESHUTDOWN:
        return 58;
    case ETIMEDOUT:
        return 60;
    case ECONNREFUSED:
        return VE_CONNREFUSED;
    case EHOSTDOWN:
        return 64;
    case EHOSTUNREACH:
        return VE_HOSTUNREACH;
    default:
        return e > 0 && e <= 34 ? e : VE_INVAL;
    }
}

/* the policy for destinations of the guest */

static bool dst_allowed(IUCVTcpip *s, uint32_t ip)
{
    struct ifaddrs *ifa, *i;
    bool host = false;
    int n;

    if ((ip >> 24) == 0 || (ip >> 24) == 127 ||
        (ip & 0xffff0000) == 0xa9fe0000 ||      /* 169.254/16 */
        (ip & 0xf0000000) >= 0xe0000000) {      /* multicast, reserved */
        /* the deny list does not apply to the host's loopback */
        return s->allow_host && (ip >> 24) == 127;
    }
    for (n = 0; n < s->ndeny; n++) {
        if ((ip & s->deny_mask[n]) == s->deny_net[n]) {
            return false;
        }
    }
    if (!getifaddrs(&ifa)) {
        for (i = ifa; i && !host; i = i->ifa_next) {
            uint32_t a, m;

            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
            m = i->ifa_netmask ?
                ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr)
                : 0xffffffff;
            /* the host itself, or a broadcast to one of its subnets */
            if ((ip == a && !s->allow_host) ||
                (m != 0xffffffff && m != 0xfffffffe &&
                 (ip & m) == (a & m) &&
                 ((ip & ~m) == 0 || (ip & ~m) == ~m))) {
                host = true;
            }
        }
        freeifaddrs(ifa);
    }
    return !host;
}

/* guest socket address (16 bytes) to a host one; false: refused */
static bool to_host_addr(IUCVTcpip *s, const uint8_t *g,
                         struct sockaddr_in *h, int *err)
{
    uint32_t ip = ldl_be_p(g + 4);
    uint16_t port = lduw_be_p(g + 2);

    if (lduw_be_p(g) != VM_AF_INET) {
        *err = VE_AFNOSUPPORT;
        return false;
    }
    memset(h, 0, sizeof(*h));
    h->sin_family = AF_INET;
    h->sin_port = htons(port);
    if (s->dns && ip == s->dns && port == 53 && s->resolver.sin_family) {
        *h = s->resolver;
        return true;
    }
    if (!dst_allowed(s, ip)) {
        *err = VE_NETUNREACH;
        return false;
    }
    h->sin_addr.s_addr = htonl(ip);
    return true;
}

static void put_saddr(uint8_t *g, uint32_t ip, uint16_t port)
{
    memset(g, 0, SADDR);
    stw_be_p(g, VM_AF_INET);
    stw_be_p(g + 2, port);
    stl_be_p(g + 4, ip);
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

/* sockets */

static TcpSock *sock_get(TcpPath *tp, uint32_t n)
{
    return n < tp->nsock ? tp->socks[n] : NULL;
}

static void sock_update_handlers(TcpPath *tp);

static void sock_close(TcpPath *tp, uint32_t n, bool reset)
{
    TcpSock *so = tp->socks[n];

    if (!so) {
        return;
    }
    if (so->fd >= 0) {
        qemu_set_fd_handler(so->fd, NULL, NULL, NULL);
        if (reset && so->type == 1) {
            struct linger l = { 1, 0 };

            setsockopt(so->fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
        }
        close(so->fd);
        tp->s->nfds--;
    }
    g_free(so);
    tp->socks[n] = NULL;
}

static uint16_t local_port(TcpSock *so)
{
    struct sockaddr_in a;
    socklen_t l = sizeof(a);

    if (so->port || so->fd < 0) {
        return so->port;
    }
    if (!getsockname(so->fd, (struct sockaddr *)&a, &l)) {
        return ntohs(a.sin_port);
    }
    return 0;
}

/* a forwarded port the guest listens on */
static HostFwd *fwd_find(IUCVTcpip *s, bool udp, uint16_t guest_port)
{
    int i;

    for (i = 0; i < s->nfwds; i++) {
        if (s->fwds[i].udp == udp && s->fwds[i].guest_port == guest_port) {
            return &s->fwds[i];
        }
    }
    return NULL;
}

/* readiness of a host socket, without waiting */
static short sock_poll(TcpSock *so)
{
    struct pollfd p = { .fd = so->fd, .events = POLLIN | POLLOUT | POLLPRI };

    if (so->fd < 0) {
        return 0;
    }
    return poll(&p, 1, 0) > 0 ? p.revents : 0;
}

/* the calls; each returns true when it answered the request */

static bool c_socket(TcpReq *r)
{
    TcpPath *tp = r->tp;
    const uint8_t *b = r->msg->data;
    uint32_t dom, type, proto, n;
    TcpSock *so;
    int fd;

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
    if (tp->s->nfds >= tp->s->max_host_sockets) {
        reply_err(r, VE_MFILE);
        return true;
    }
    so = g_new0(TcpSock, 1);
    so->type = type;
    switch (type) {
    case 1:
        if (proto && proto != IPPROTO_TCP) {
            g_free(so);
            reply_err(r, VE_PROTONOSUPPORT);
            return true;
        }
        fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
        break;
    case 2:
        if (proto && proto != IPPROTO_UDP) {
            g_free(so);
            reply_err(r, VE_PROTONOSUPPORT);
            return true;
        }
        fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
        break;
    case 3:
        if (!tp->s->raw || proto != IPPROTO_ICMP) {
            g_free(so);
            reply_err(r, VE_ACCES);
            return true;
        }
        fd = qemu_socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        so->icmp = true;
        break;
    default:
        g_free(so);
        reply_err(r, VE_SOCKTNOSUPPORT);
        return true;
    }
    if (fd < 0) {
        g_free(so);
        reply_err(r, vm_errno(errno));
        return true;
    }
    qemu_set_blocking(fd, false, NULL);
    so->fd = fd;
    tp->s->nfds++;
    tp->socks[n] = so;
    reply(r, n, 0, NULL, 0);
    return true;
}

static bool c_bind(TcpReq *r, TcpSock *so)
{
    IUCVTcpip *s = r->tp->s;
    const uint8_t *g = r->msg->data;
    uint32_t ip;
    uint16_t port;
    HostFwd *f;

    if (r->msg->len < SADDR) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    ip = ldl_be_p(g + 4);
    port = lduw_be_p(g + 2);
    if (lduw_be_p(g) != VM_AF_INET) {
        reply_err(r, VE_AFNOSUPPORT);
        return true;
    }
    if (ip && ip != s->ip) {
        reply_err(r, VE_ADDRNOTAVAIL);
        return true;
    }
    if (so->port) {
        reply_err(r, VE_INVAL);
        return true;
    }
    f = port ? fwd_find(s, so->type == 2, port) : NULL;
    if (f) {
        /* the forwarded host port becomes this socket */
        struct sockaddr_in a = {
            .sin_family = AF_INET, .sin_port = htons(f->host_port),
            .sin_addr = s->fwd_addr,
        };
        int one = 1;

        setsockopt(so->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(so->fd, (struct sockaddr *)&a, sizeof(a))) {
            reply_err(r, vm_errno(errno));
            return true;
        }
    }
    so->port = port;
    reply(r, 0, 0, NULL, 0);
    return true;
}

static bool c_listen(TcpReq *r, TcpSock *so)
{
    uint32_t backlog = ldl_be_p(r->msg->data + 4);

    if (so->type != 1) {
        reply_err(r, VE_OPNOTSUPP);
        return true;
    }
    if (!fwd_find(r->tp->s, false, so->port)) {
        /* nothing can reach it: no host socket listens */
        if (so->fd >= 0) {
            qemu_set_fd_handler(so->fd, NULL, NULL, NULL);
            close(so->fd);
            so->fd = -1;
            r->tp->s->nfds--;
        }
    } else if (listen(so->fd, MAX(1, MIN(backlog, 64)))) {
        reply_err(r, vm_errno(errno));
        return true;
    }
    so->listening = true;
    reply(r, 0, 0, NULL, 0);
    return true;
}

static bool c_accept(TcpReq *r, TcpSock *so, bool first)
{
    TcpPath *tp = r->tp;
    uint32_t n = ldl_be_p(r->msg->data + 4);
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    uint8_t g[SADDR];
    TcpSock *ns;
    int fd;

    if (!so->listening) {
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
    if (tp->s->nfds >= tp->s->max_host_sockets) {
        reply_err(r, VE_MFILE);
        return true;
    }
    fd = so->fd < 0 ? -1 : accept(so->fd, (struct sockaddr *)&a, &l);
    if (fd < 0) {
        if (so->fd >= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            reply_err(r, vm_errno(errno));
            return true;
        }
        if (so->nonblock) {
            reply_err(r, VE_WOULDBLOCK);
            return true;
        }
        return false;
    }
    qemu_set_blocking(fd, false, NULL);
    tp->s->nfds++;
    ns = g_new0(TcpSock, 1);
    ns->fd = fd;
    ns->type = 1;
    ns->nonblock = so->nonblock;
    ns->port = so->port;
    tp->socks[n] = ns;
    put_saddr(g, ntohl(a.sin_addr.s_addr), ntohs(a.sin_port));
    reply(r, n, 0, g, SADDR);
    return true;
}

static bool c_connect(TcpReq *r, TcpSock *so, bool first)
{
    struct sockaddr_in a;
    int err, e;
    socklen_t l = sizeof(e);

    if (so->connecting || so->so_error) {
        if (!first) {
            return false;
        }
        if (so->connecting &&
            !(sock_poll(so) & (POLLOUT | POLLERR | POLLHUP))) {
            if (so->nonblock) {
                reply_err(r, VE_ALREADY);
                return true;
            }
            return false;
        }
        so->connecting = false;
        getsockopt(so->fd, SOL_SOCKET, SO_ERROR, &e, &l);
        e = e ? e : so->so_error;
        so->so_error = 0;
        if (e) {
            reply_err(r, vm_errno(e));
        } else {
            reply(r, 0, 0, NULL, 0);
        }
        return true;
    }
    if (r->msg->len < SADDR) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    if (so->fd < 0 || so->listening) {
        reply_err(r, VE_OPNOTSUPP);
        return true;
    }
    if (!to_host_addr(r->tp->s, r->msg->data, &a, &err)) {
        trace_iucv_tcpip_refused(ldl_be_p(r->msg->data + 4));
        reply_err(r, err);
        return true;
    }
    if (so->type == 2 &&
        ntohl(a.sin_addr.s_addr) != ldl_be_p(r->msg->data + 4)) {
        so->dns_peer = ldl_be_p(r->msg->data + 4);
    }
    if (!connect(so->fd, (struct sockaddr *)&a, sizeof(a))) {
        reply(r, 0, 0, NULL, 0);
        return true;
    }
    if (errno != EINPROGRESS) {
        reply_err(r, vm_errno(errno));
        return true;
    }
    so->connecting = true;
    if (so->nonblock) {
        reply_err(r, VE_INPROGRESS);
        return true;
    }
    return false;
}

/* READ (14) and RECV (16): reply cc, errno, source address, data */
static bool c_recv(TcpReq *r, TcpSock *so, bool first)
{
    uint32_t want = r->msg->reply_max > 24 ? r->msg->reply_max - 24 : 0;
    uint32_t flags = r->call == C_RECV ? ldl_be_p(r->msg->data + 4) : 0;
    g_autofree uint8_t *buf = NULL;
    struct sockaddr_in a = {};
    socklen_t l = sizeof(a);
    int hflags = MSG_DONTWAIT;
    ssize_t n;
    uint32_t src;

    if (!first) {
        return false;
    }
    if (so->fd < 0 || so->listening) {
        reply_err(r, VE_NOTCONN);
        return true;
    }
    want = MIN(want, RECV_MAX);
    buf = g_malloc(16 + MAX(want, 1) + 20);
    if (flags & 1) {
        hflags |= MSG_OOB;
    }
    if (flags & 2) {
        hflags |= MSG_PEEK;
    }
    if (so->icmp) {
        /* raw ICMP: the guest gets an IPv4 header in front */
        n = recvfrom(so->fd, buf + 16 + 20, want > 20 ? want - 20 : 0, hflags,
                     (struct sockaddr *)&a, &l);
        if (n >= 0) {
            uint8_t *ip = buf + 16;

            memset(ip, 0, 20);
            ip[0] = 0x45;
            stw_be_p(ip + 2, n + 20);
            ip[8] = 64;
            ip[9] = IPPROTO_ICMP;
            stl_be_p(ip + 12, ntohl(a.sin_addr.s_addr));
            stl_be_p(ip + 16, r->tp->s->ip);
            n += 20;
        }
    } else {
        n = recvfrom(so->fd, buf + 16, want, hflags, (struct sockaddr *)&a,
                     &l);
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (so->nonblock) {
                reply_err(r, VE_WOULDBLOCK);
                return true;
            }
            return false;
        }
        reply_err(r, vm_errno(errno));
        return true;
    }
    src = ntohl(a.sin_addr.s_addr);
    if (so->dns_peer && a.sin_port == r->tp->s->resolver.sin_port &&
        a.sin_addr.s_addr == r->tp->s->resolver.sin_addr.s_addr) {
        src = so->dns_peer;
    }
    put_saddr(buf, src, ntohs(a.sin_port));
    reply(r, n, 0, buf, 16 + n);
    return true;
}

/* SEND (20), SENDTO (22), WRITE (26): +0 flags, +4 address, +20 data */
static bool c_send(TcpReq *r, TcpSock *so, bool first)
{
    const uint8_t *b = r->msg->data;
    uint32_t len = r->msg->len;
    uint32_t flags;
    struct sockaddr_in a;
    int err, hflags = MSG_DONTWAIT | MSG_NOSIGNAL;
    ssize_t n;

    if (!first) {
        return false;
    }
    if (len < 20) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    if (so->fd < 0 || so->listening) {
        reply_err(r, VE_NOTCONN);
        return true;
    }
    flags = r->call == C_WRITE ? 0 : ldl_be_p(b);
    if (flags & 1) {
        hflags |= MSG_OOB;
    }
    if (flags & 4) {
        hflags |= MSG_DONTROUTE;
    }
    len = MIN(len - 20, SEND_MAX);
    if (r->call == C_SENDTO) {
        if (!to_host_addr(r->tp->s, b + 4, &a, &err)) {
            trace_iucv_tcpip_refused(ldl_be_p(b + 8));
            reply_err(r, err);
            return true;
        }
        if (ntohl(a.sin_addr.s_addr) != ldl_be_p(b + 8)) {
            so->dns_peer = ldl_be_p(b + 8);
        }
        if (so->icmp && len >= 20 && (b[20] >> 4) == 4) {
            /* raw ICMP with an IP header from the guest */
            uint32_t hl = (b[20] & 0xf) * 4;

            n = hl <= len ? sendto(so->fd, b + 20 + hl, len - hl, hflags,
                                   (struct sockaddr *)&a, sizeof(a)) : -1;
            if (n >= 0) {
                n += hl;
            }
        } else {
            n = sendto(so->fd, b + 20, len, hflags, (struct sockaddr *)&a,
                       sizeof(a));
        }
    } else {
        n = send(so->fd, b + 20, len, hflags);
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (so->nonblock) {
                reply_err(r, VE_WOULDBLOCK);
                return true;
            }
            return false;
        }
        reply_err(r, vm_errno(errno));
        return true;
    }
    reply(r, n, 0, NULL, 0);
    return true;
}

static void name_reply(TcpReq *r, TcpSock *so, bool peer)
{
    IUCVTcpip *s = r->tp->s;
    struct sockaddr_in a = {};
    socklen_t l = sizeof(a);
    uint8_t g[SADDR];

    if (!peer) {
        put_saddr(g, so->fd >= 0 || so->port ? s->ip : 0, local_port(so));
        reply(r, 0, 0, g, SADDR);
        return;
    }
    if (so->fd < 0 || getpeername(so->fd, (struct sockaddr *)&a, &l)) {
        reply_err(r, VE_NOTCONN);
        return;
    }
    put_saddr(g, so->dns_peer && a.sin_addr.s_addr ==
                 s->resolver.sin_addr.s_addr ? so->dns_peer :
                 ntohl(a.sin_addr.s_addr), ntohs(a.sin_port));
    reply(r, 0, 0, g, SADDR);
}

/* socket options: VM numbering to Linux */
static bool sockopt_map(uint32_t level, uint32_t name, int *hl, int *hn)
{
    if (level == VM_SOL_SOCKET || level == 0xffffffff) {
        *hl = SOL_SOCKET;
        switch (name) {
        case 0x0001:
            *hn = SO_DEBUG;
            return true;
        case 0x0004:
            *hn = SO_REUSEADDR;
            return true;
        case 0x0008:
            *hn = SO_KEEPALIVE;
            return true;
        case 0x0010:
            *hn = SO_DONTROUTE;
            return true;
        case 0x0020:            /* SO_BROADCAST: not offered */
            return false;
        case 0x0080:
            *hn = SO_LINGER;
            return true;
        case 0x0100:
            *hn = SO_OOBINLINE;
            return true;
        case 0x1001:
            *hn = SO_SNDBUF;
            return true;
        case 0x1002:
            *hn = SO_RCVBUF;
            return true;
        case 0x1007:
            *hn = SO_ERROR;
            return true;
        case 0x1008:
            *hn = SO_TYPE;
            return true;
        }
        return false;
    }
    if (level == IPPROTO_TCP && name == 1) {
        *hl = IPPROTO_TCP;
        *hn = TCP_NODELAY;
        return true;
    }
    return false;
}

static bool c_sockopt(TcpReq *r, TcpSock *so, bool set)
{
    const uint8_t *b = r->msg->data;
    uint32_t level, name;
    int hl, hn, v = 0;
    struct linger lg = {};
    socklen_t l;
    uint8_t out[8];

    if (r->msg->len < 8) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    level = ldl_be_p(b);
    name = ldl_be_p(b + 4);
    if (!sockopt_map(level, name, &hl, &hn)) {
        reply_err(r, VE_NOPROTOOPT);
        return true;
    }
    if (so->fd < 0) {
        if (set) {
            reply(r, 0, 0, NULL, 0);
        } else {
            stl_be_p(out, hn == SO_TYPE ? so->type : 0);
            reply(r, 0, 0, out, 4);
        }
        return true;
    }
    if (set) {
        if (r->msg->len < (hn == SO_LINGER ? 16 : 12) ||
            hn == SO_ERROR || hn == SO_TYPE) {
            reply_err(r, VE_INVAL);
            return true;
        }
        if (hn == SO_LINGER) {
            lg.l_onoff = ldl_be_p(b + 8);
            lg.l_linger = ldl_be_p(b + 12);
            v = setsockopt(so->fd, hl, hn, &lg, sizeof(lg));
        } else {
            v = ldl_be_p(b + 8);
            v = setsockopt(so->fd, hl, hn, &v, sizeof(v));
        }
        if (v) {
            reply_err(r, vm_errno(errno));
        } else {
            reply(r, 0, 0, NULL, 0);
        }
        return true;
    }
    if (hn == SO_LINGER) {
        l = sizeof(lg);
        getsockopt(so->fd, hl, hn, &lg, &l);
        stl_be_p(out, lg.l_onoff);
        stl_be_p(out + 4, lg.l_linger);
        reply(r, 0, 0, out, 8);
        return true;
    }
    l = sizeof(v);
    if (getsockopt(so->fd, hl, hn, &v, &l)) {
        reply_err(r, vm_errno(errno));
        return true;
    }
    if (hn == SO_ERROR) {
        /* a failed connect that SELECT has seen */
        v = v ? v : so->so_error;
        so->so_error = 0;
        v = v ? vm_errno(v) : 0;
    } else if (hn == SO_TYPE) {
        v = so->type;
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
    const uint8_t *b = r->msg->data;
    uint32_t cmd = r->msg->len >= 4 ? ldl_be_p(b) : 0;
    uint8_t out[32];
    int v = 0;

    switch (cmd) {
    case 0x8004a77e:                            /* FIONBIO */
        so->nonblock = r->msg->len >= 8 && ldl_be_p(b + 4);
        reply(r, 0, 0, NULL, 0);
        return true;
    case 0x4004a77f:                            /* FIONREAD */
        if (so->fd >= 0) {
            ioctl(so->fd, FIONREAD, &v);
        }
        stl_be_p(out, v);
        reply(r, 0, 0, out, 4);
        return true;
    case 0x4004a707:                            /* SIOCATMARK */
        stl_be_p(out, so->fd >= 0 ? MAX(sockatmark(so->fd), 0) : 0);
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
    if (r->msg->len < 40) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    if (so->type != 1 || so->listening) {
        reply_err(r, so->listening ? VE_BUSY : VE_OPNOTSUPP);
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
    TcpSock *so, *ns;
    int fd;

    if (r->msg->len < 48) {
        reply_err(r, VE_BADLEN);
        return true;
    }
    his = ldl_be_p(b + 40);
    n = ldl_be_p(b + 44);
    if (ldl_be_p(b) != VM_AF_INET) {
        reply_err(r, 46);
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
    fd = tp->s->nfds < tp->s->max_host_sockets ? dup(so->fd) : -1;
    if (fd < 0) {
        reply_err(r, VE_MFILE);
        return true;
    }
    qemu_set_blocking(fd, false, NULL);
    tp->s->nfds++;
    ns = g_new0(TcpSock, 1);
    ns->fd = fd;
    ns->type = 1;
    ns->port = so->port;
    tp->socks[n] = ns;
    so->given = false;
    so->taken = true;
    reply(r, n, 0, NULL, 0);
    /* the giver's SELECT sees the exception condition */
    if (giver != tp) {
        tcp_pump(giver);
    }
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

static void select_timeout(void *opaque)
{
    TcpReq *r = opaque;

    r->timed_out = true;
    tcp_pump(r->tp);
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
        short ev;

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
        ev = sock_poll(so);
        if (so->connecting && (ev & POLLOUT)) {
            int e = 0;
            socklen_t l = sizeof(e);

            getsockopt(so->fd, SOL_SOCKET, SO_ERROR, &e, &l);
            so->connecting = false;
            so->so_error = e;
        }
        if (used[0] && fd_isset(in[0], d) && (ev & (POLLIN | POLLHUP |
                                                    POLLERR))) {
            fd_set_bit(out + 8, d);
            found++;
        }
        if (used[1] && fd_isset(in[1], d) && !so->listening &&
            !so->connecting && (ev & (POLLOUT | POLLERR | POLLHUP))) {
            fd_set_bit(out + 8 + fdsize, d);
            found++;
        }
        if (used[2] && fd_isset(in[2], d) && ((ev & POLLPRI) || so->taken)) {
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
        if (so->fd >= 0 && shutdown(so->fd, ldl_be_p(b + 4) & 3)) {
            reply_err(r, vm_errno(errno));
        } else {
            reply(r, 0, 0, NULL, 0);
        }
        return true;
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
    sock_update_handlers(tp);
}

static void sock_event(void *opaque)
{
    tcp_pump(opaque);
}

/* host fd handlers for the sockets that have waiting requests */
static void sock_update_handlers(TcpPath *tp)
{
    g_autofree uint8_t *want = g_malloc0(tp->nsock);
    TcpReq *r;
    uint32_t i, d;

    QTAILQ_FOREACH(r, &tp->reqs, next) {
        if (r->call == C_SELECT) {
            const uint8_t *b = r->msg->data;
            uint32_t fdsize = r->lo;

            for (d = 0; d < MIN(tp->nsock, fdsize * 8); d++) {
                if (ldl_be_p(b + 8) && fd_isset(b + 28, d)) {
                    want[d] |= 1;
                }
                if (ldl_be_p(b + 12) && fd_isset(b + 28 + fdsize, d)) {
                    want[d] |= 2;
                }
                /*
                 * Exceptions (out-of-band data, a taken socket) are not
                 * watched: read interest would fire on ordinary data and
                 * spin. They are seen when the request runs again.
                 */
            }
        } else if (r->lo < tp->nsock) {
            if (is_read(r->call) || r->call == C_ACCEPT) {
                want[r->lo] |= 1;
            } else if (is_write(r->call) || r->call == C_CONNECT) {
                want[r->lo] |= 2;
            }
        }
    }
    for (i = 0; i < tp->nsock; i++) {
        TcpSock *so = tp->socks[i];

        if (so && so->fd >= 0) {
            qemu_set_fd_handler(so->fd, want[i] & 1 ? sock_event : NULL,
                                want[i] & 2 ? sock_event : NULL, tp);
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
        tp->last_errno = 55;
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
    sock_update_handlers(tp);
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

/* hostfwd=tcp:HOSTPORT-GUESTPORT;udp:...  (';' or '+' separated) */
static bool parse_fwds(IUCVTcpip *s, Error **errp)
{
    g_auto(GStrv) v = NULL;
    int i;

    if (!s->hostfwd || !*s->hostfwd) {
        return true;
    }
    v = g_strsplit_set(s->hostfwd, ";+", -1);
    s->fwds = g_new0(HostFwd, g_strv_length(v));
    for (i = 0; v[i]; i++) {
        HostFwd *f = &s->fwds[s->nfwds];
        unsigned hp, gp;
        char proto[4];

        if (!*v[i]) {
            continue;
        }
        if (sscanf(v[i], "%3[a-z]:%u-%u", proto, &hp, &gp) != 3 ||
            (strcmp(proto, "tcp") && strcmp(proto, "udp")) ||
            !hp || hp > 65535 || !gp || gp > 65535) {
            error_setg(errp, "bad hostfwd entry '%s' (want tcp:HOST-GUEST)",
                       v[i]);
            return false;
        }
        f->udp = !strcmp(proto, "udp");
        f->host_port = hp;
        f->guest_port = gp;
        s->nfwds++;
    }
    return true;
}

static bool parse_deny(IUCVTcpip *s, Error **errp)
{
    g_auto(GStrv) v = NULL;
    int i;

    if (!s->deny || !*s->deny) {
        return true;
    }
    v = g_strsplit_set(s->deny, ";+", -1);
    s->deny_net = g_new0(uint32_t, g_strv_length(v));
    s->deny_mask = g_new0(uint32_t, g_strv_length(v));
    for (i = 0; v[i]; i++) {
        if (!*v[i]) {
            continue;
        }
        if (!parse_ip(v[i], &s->deny_net[s->ndeny], &s->deny_mask[s->ndeny],
                      errp)) {
            return false;
        }
        s->deny_net[s->ndeny] &= s->deny_mask[s->ndeny];
        s->ndeny++;
    }
    return true;
}

/* the first name server of the host serves the guest's DNS address */
static void find_resolver(IUCVTcpip *s)
{
    g_autofree char *conf = NULL;
    g_auto(GStrv) lines = NULL;
    struct in_addr a;
    int i;

    if (!g_file_get_contents("/etc/resolv.conf", &conf, NULL, NULL)) {
        return;
    }
    lines = g_strsplit(conf, "\n", -1);
    for (i = 0; lines[i]; i++) {
        char addr[64];

        if (sscanf(lines[i], " nameserver %63s", addr) == 1 &&
            inet_aton(addr, &a)) {
            s->resolver.sin_family = AF_INET;
            s->resolver.sin_port = htons(53);
            s->resolver.sin_addr = a;
            return;
        }
    }
}

static void tcpip_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    IUCVTcpip *s = IUCV_TCPIP(dev);
    DeviceClass *parent = DEVICE_CLASS(object_class_get_parent(
                              object_class_by_name(TYPE_IUCV_TCPIP)));

    if (!s->ip_str) {
        error_setg(errp, "iucv-tcpip needs ip=<address>/<prefix>");
        return;
    }
    if (!parse_ip(s->ip_str, &s->ip, &s->mask, errp) ||
        (s->dns_str && !parse_ip(s->dns_str, &s->dns, NULL, errp)) ||
        !parse_fwds(s, errp) || !parse_deny(s, errp)) {
        return;
    }
    if (!s->fwd_addr_str) {
        s->fwd_addr.s_addr = htonl(INADDR_ANY);
    } else if (!inet_aton(s->fwd_addr_str, &s->fwd_addr)) {
        error_setg(errp, "bad hostfwd-addr '%s'", s->fwd_addr_str);
        return;
    }
    if (s->max_sockets < NSOCK_MIN || s->max_sockets > 4096) {
        error_setg(errp, "max-sockets must be %d to 4096", NSOCK_MIN);
        return;
    }
    if (!s->max_host_sockets || s->max_host_sockets > 65536) {
        error_setg(errp, "max-host-sockets must be 1 to 65536");
        return;
    }
    if (s->dns) {
        find_resolver(s);
    }
    parent->realize(dev, errp);
    if (*errp) {
        return;
    }
    QTAILQ_INIT(&s->paths);
}

static const Property tcpip_properties[] = {
    DEFINE_PROP_STRING("ip", IUCVTcpip, ip_str),
    DEFINE_PROP_STRING("hostname", IUCVTcpip, hostname),
    DEFINE_PROP_STRING("hostfwd", IUCVTcpip, hostfwd),
    DEFINE_PROP_STRING("hostfwd-addr", IUCVTcpip, fwd_addr_str),
    DEFINE_PROP_STRING("dns", IUCVTcpip, dns_str),
    DEFINE_PROP_STRING("deny", IUCVTcpip, deny),
    DEFINE_PROP_BOOL("allow-host", IUCVTcpip, allow_host, false),
    DEFINE_PROP_BOOL("raw", IUCVTcpip, raw, false),
    /* socket numbers of one path (its table) */
    DEFINE_PROP_UINT32("max-sockets", IUCVTcpip, max_sockets, 256),
    /* host sockets of all paths together */
    DEFINE_PROP_UINT32("max-host-sockets", IUCVTcpip, max_host_sockets, 512),
};

static void tcpip_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    IUCVServiceClass *sc = IUCV_SERVICE_CLASS(oc);

    dc->realize = tcpip_realize;
    dc->desc = "IUCV TCP/IP service (socket interface on host sockets)";
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
