/*
 * QTest of IUCV provided by QEMU (s390-iucv and its services)
 *
 * Small guest programs (tests/qtest/s390x-iucv/) issue the IUCV
 * instruction and take the IUCV external interruptions; each stores 0 at
 * 0x2000 when all its steps passed, otherwise the number of the failing
 * step, and ends in a disabled wait. IUCV needs KVM with
 * KVM_CAP_S390_USER_OPEREXEC and KVM_CAP_S390_USER_EXT_IRQ, so the tests
 * are skipped without them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include "libqtest.h"
#include "qemu/bswap.h"
#include "s390x-iucv-guests.h"

/* KVM_CHECK_EXTENSION and the capabilities, without the kernel headers */
#define IUCV_TEST_KVM_CHECK_EXTENSION _IO(0xAE, 0x03)
#define IUCV_TEST_KVM_CAP_S390_USER_OPEREXEC 246
#define IUCV_TEST_KVM_CAP_S390_USER_EXT_IRQ 254

#define G_RESULT    0x2000
#define G_PGM_CODE  0x2010
#define G_ERRNO     0x2014
#define G_PGM_PSW   0x2020  /* unavailable: the program old PSW */
#define G_PARAM     0x2f00
#define G_NOT_DONE  0xffffffffU

#define TIMEOUT_S   30

static bool host_has_cap(int cap)
{
    int fd = open("/dev/kvm", O_RDWR);
    int ret;

    if (fd < 0) {
        return false;
    }
    ret = ioctl(fd, IUCV_TEST_KVM_CHECK_EXTENSION, cap);
    close(fd);
    return ret > 0;
}

static bool skip_without_iucv(bool ext_irq)
{
    if (!qtest_has_accel("kvm") ||
        !host_has_cap(IUCV_TEST_KVM_CAP_S390_USER_OPEREXEC)) {
        g_test_skip("needs KVM with KVM_CAP_S390_USER_OPEREXEC");
        return true;
    }
    if (ext_irq && !host_has_cap(IUCV_TEST_KVM_CAP_S390_USER_EXT_IRQ)) {
        g_test_skip("needs KVM with KVM_CAP_S390_USER_EXT_IRQ");
        return true;
    }
    return false;
}

/* The guest program as a file for -kernel */
static char *write_guest(const unsigned char *elf, unsigned int len)
{
    char *path = NULL;
    int fd = g_file_open_tmp("qemu-s390x-iucv-XXXXXX.elf", &path, NULL);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, elf, len), ==, len);
    close(fd);
    return path;
}

/*
 * Start the guest stopped, so that the test can put data in its memory.
 * The programs end in a disabled wait, which must not end QEMU.
 */
static QTestState *start_guest(const char *path, const char *args)
{
    return qtest_initf("-nodefaults -S -accel kvm -machine s390-ccw-virtio "
                       "-m 256 -action panic=pause -kernel %s %s",
                       path, args);
}

static void run_guest(QTestState *qts)
{
    /* the program sets it too, but only once it runs */
    qtest_writel(qts, G_RESULT, G_NOT_DONE);
    qtest_qmp_assert_success(qts, "{ 'execute': 'cont' }");
}

static uint32_t wait_result(QTestState *qts)
{
    gint64 end = g_get_monotonic_time() + TIMEOUT_S * G_USEC_PER_SEC;
    uint32_t res;

    do {
        res = qtest_readl(qts, G_RESULT);
        if (res != G_NOT_DONE) {
            return res;
        }
        g_usleep(10000);
    } while (g_get_monotonic_time() < end);
    return res;
}

/* Run a program that needs no help from the test */
static void run_simple(const unsigned char *elf, unsigned int len,
                       const char *args, bool ext_irq)
{
    g_autofree char *path = NULL;
    QTestState *qts;
    uint32_t res;

    if (skip_without_iucv(ext_irq)) {
        return;
    }
    path = write_guest(elf, len);
    qts = start_guest(path, args);
    run_guest(qts);
    res = wait_result(qts);
    if (res) {
        g_test_message("failing step %u, program-interruption code 0x%x",
                       res, qtest_readw(qts, G_PGM_CODE + 2));
    }
    g_assert_cmphex(res, ==, 0);
    qtest_quit(qts);
    unlink(path);
}

/* The CP core with the echo service: paths, messages, masks, SEVER */
static void test_cp(void)
{
    run_simple(iucv_guest_cp, iucv_guest_cp_len,
               "-device iucv-echo,name=ECHO", true);
}

/* The registers other than the condition code survive the instruction */
static void test_registers(void)
{
    run_simple(iucv_guest_regs, iucv_guest_regs_len,
               "-device iucv-echo,name=ECHO", true);
}

/* IUCV interruptions go to a vCPU that is enabled for them */
static void test_smp(void)
{
    run_simple(iucv_guest_smp, iucv_guest_smp_len,
               "-smp 2 -device iucv-echo,name=ECHO", true);
}

/*
 * available=off: condition code 3 in the supervisor state, operation
 * exception in the problem state. Needs no external interruptions.
 */
static void test_unavailable(void)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (skip_without_iucv(false)) {
        return;
    }
    path = write_guest(iucv_guest_unavailable, iucv_guest_unavailable_len);
    qts = start_guest(path, "-device s390-iucv,available=off");
    run_guest(qts);
    g_assert_cmphex(wait_result(qts), ==, 0);
    /* the operation exception came from the problem state */
    g_assert_cmphex(qtest_readb(qts, G_PGM_PSW + 1) & 0x01, ==, 0x01);
    qtest_quit(qts);
    unlink(path);
}

/*
 * Without any IUCV device the instruction is an operation exception in
 * the supervisor state too, as on a machine without a hypervisor.
 */
static void test_none(void)
{
    g_autofree char *path = NULL;
    QTestState *qts;

    if (!qtest_has_accel("kvm")) {
        g_test_skip("needs KVM");
        return;
    }
    path = write_guest(iucv_guest_unavailable, iucv_guest_unavailable_len);
    qts = start_guest(path, "");
    run_guest(qts);
    /* the program-interruption handler takes code 1 as a pass ... */
    g_assert_cmphex(wait_result(qts), ==, 0);
    /* ... but here it comes from the first instruction, in supervisor state */
    g_assert_cmphex(qtest_readb(qts, G_PGM_PSW + 1) & 0x01, ==, 0);
    qtest_quit(qts);
    unlink(path);
}

/* The host side of the TCP/IP tests */

typedef struct NetHelper {
    int echo_fd;                /* listening echo server, or -1 */
    uint16_t fwd_port;          /* host port forwarded to the guest */
    const char *send;           /* the client sends this ... */
    const char *expect;         /* ... and expects this answer */
    bool client_ok;
    bool stop;
    unsigned tries;
    char last[64];              /* what the last attempt got */
} NetHelper;

static int listen_loopback(uint16_t *port)
{
    struct sockaddr_in a = { .sin_family = AF_INET };
    socklen_t alen = sizeof(a);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    g_assert_cmpint(fd, >=, 0);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_assert_cmpint(bind(fd, (struct sockaddr *)&a, sizeof(a)), ==, 0);
    g_assert_cmpint(listen(fd, 4), ==, 0);
    g_assert_cmpint(getsockname(fd, (struct sockaddr *)&a, &alen), ==, 0);
    *port = ntohs(a.sin_port);
    return fd;
}

/* A port that is free now, for a forward of QEMU */
static uint16_t free_port(void)
{
    uint16_t port;
    int fd = listen_loopback(&port);

    close(fd);
    return port;
}

/* One attempt of the client: connect, send, read the answer */
static bool client_try(NetHelper *h)
{
    struct sockaddr_in a = { .sin_family = AF_INET };
    char buf[64];
    size_t got = 0, want = strlen(h->expect);
    struct pollfd p;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    bool ok = false;

    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(h->fwd_port);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) ||
        write(fd, h->send, strlen(h->send)) != strlen(h->send)) {
        goto out;
    }
    p.fd = fd;
    p.events = POLLIN;
    while (got < want && poll(&p, 1, 5000) > 0) {
        ssize_t n = read(fd, buf + got, sizeof(buf) - got);

        if (n <= 0) {
            goto out;
        }
        got += n;
    }
    ok = got == want && !memcmp(buf, h->expect, want);
    snprintf(h->last, sizeof(h->last), "%zu bytes", got);
out:
    close(fd);
    return ok;
}

/* The echo server (one connection at a time) and the client */
static gpointer net_helper(gpointer opaque)
{
    NetHelper *h = opaque;
    gint64 next_try = 0;
    int conn = -1;

    while (!h->stop) {
        struct pollfd p[2];
        int n = 0;

        if (h->echo_fd >= 0) {
            p[n].fd = conn >= 0 ? conn : h->echo_fd;
            p[n++].events = POLLIN;
        }
        if (poll(p, n, 20) > 0 && h->echo_fd >= 0) {
            if (conn < 0) {
                conn = accept(h->echo_fd, NULL, NULL);
            } else {
                char buf[256];
                ssize_t len = read(conn, buf, sizeof(buf));

                if (len <= 0 || write(conn, buf, len) != len) {
                    close(conn);
                    conn = -1;
                }
            }
        }
        if (!h->client_ok && h->send && g_get_monotonic_time() >= next_try) {
            h->tries++;
            h->client_ok = client_try(h);
            next_try = g_get_monotonic_time() + 200000;
        }
    }
    if (conn >= 0) {
        close(conn);
    }
    return NULL;
}

/*
 * The TCP/IP service: a connection out to an echo server on the host's
 * loopback (allow-host), one in through a forwarded port, the policy for
 * link-local and denied networks.
 */
static void test_tcpip(void)
{
    NetHelper h = { .send = "ping", .expect = "pong" };
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    uint16_t echo_port;
    QTestState *qts;
    GThread *thread;
    uint32_t res;

    if (skip_without_iucv(true)) {
        return;
    }
    h.echo_fd = listen_loopback(&echo_port);
    h.fwd_port = free_port();
    path = write_guest(iucv_guest_tcpip, iucv_guest_tcpip_len);
    args = g_strdup_printf("-device iucv-tcpip,name=TCPIP,ip=10.0.2.15/24,"
                           "allow-host=on,hostfwd-addr=127.0.0.1,"
                           "hostfwd=tcp:%u-23,deny=192.0.2.0/24",
                           h.fwd_port);
    qts = start_guest(path, args);
    qtest_writew(qts, G_PARAM, echo_port);
    thread = g_thread_new("iucv-net-helper", net_helper, &h);
    run_guest(qts);
    res = wait_result(qts);
    h.stop = true;
    g_thread_join(thread);
    if (res) {
        g_test_message("failing step %u, errno %u", res,
                       qtest_readl(qts, G_ERRNO));
    }
    g_assert_cmphex(res, ==, 0);
    if (!h.client_ok) {
        g_test_message("client: %u attempts, last got %s", h.tries, h.last);
    }
    g_assert_true(h.client_ok);
    qtest_quit(qts);
    close(h.echo_fd);
    unlink(path);
}

/* Without allow-host, the host's loopback is out of reach */
static void test_tcpip_no_host(void)
{
    g_autofree char *path = NULL;
    uint16_t echo_port;
    QTestState *qts;
    int echo_fd;

    if (skip_without_iucv(true)) {
        return;
    }
    echo_fd = listen_loopback(&echo_port);
    path = write_guest(iucv_guest_tcpip, iucv_guest_tcpip_len);
    qts = start_guest(path, "-device iucv-tcpip,name=TCPIP,ip=10.0.2.15/24");
    qtest_writew(qts, G_PARAM, echo_port);
    run_guest(qts);
    /* step 4, CONNECT to 127.0.0.1, fails with ENETUNREACH (VM 51) */
    g_assert_cmphex(wait_result(qts), ==, 4);
    g_assert_cmpuint(qtest_readl(qts, G_ERRNO), ==, 51);
    qtest_quit(qts);
    close(echo_fd);
    unlink(path);
}

/* Two paths: GIVESOCKET/TAKESOCKET, SELECT, CANCEL, PURGE */
static void test_tcpip_two_paths(void)
{
    NetHelper h = { .echo_fd = -1, .send = "data", .expect = "ok" };
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    QTestState *qts;
    GThread *thread;
    uint32_t res;

    if (skip_without_iucv(true)) {
        return;
    }
    h.fwd_port = free_port();
    path = write_guest(iucv_guest_tcpip2, iucv_guest_tcpip2_len);
    args = g_strdup_printf("-device iucv-tcpip,name=TCPIP,ip=10.0.2.15/24,"
                           "hostfwd-addr=127.0.0.1,hostfwd=tcp:%u-21",
                           h.fwd_port);
    qts = start_guest(path, args);
    thread = g_thread_new("iucv-net-helper", net_helper, &h);
    run_guest(qts);
    res = wait_result(qts);
    h.stop = true;
    g_thread_join(thread);
    if (res) {
        g_test_message("failing step %u, errno %u", res,
                       qtest_readl(qts, G_ERRNO));
    }
    g_assert_cmphex(res, ==, 0);
    g_assert_true(h.client_ok);
    qtest_quit(qts);
    unlink(path);
}

/* The netiucv link */

#define NET_GUEST_IP    0x0a000302      /* 10.0.3.2 */
#define NET_GW_IP       0x0a000301      /* 10.0.3.1 */
#define NET_MTU         1500
#define NET_BIG         3000
#define NET_BIG_DF      2000

static const uint8_t net_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t gw_mac[6] = { 0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc };

static uint16_t ip_checksum(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < len; i += 2) {
        sum += lduw_be_p(p + i);
    }
    if (len & 1) {
        sum += p[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}

/* A UDP packet from the guest to the gateway, as a netiucv bundle */
static size_t make_bundle(uint8_t *b, uint16_t len, uint16_t id, bool df)
{
    uint8_t *ip = b + 2;
    int i;

    memset(b, 0, len + 4);
    stw_be_p(b, 2 + len);
    ip[0] = 0x45;
    stw_be_p(ip + 2, len);
    stw_be_p(ip + 4, id);
    stw_be_p(ip + 6, df ? 0x4000 : 0);
    ip[8] = 64;
    ip[9] = IPPROTO_UDP;
    stl_be_p(ip + 12, NET_GUEST_IP);
    stl_be_p(ip + 16, NET_GW_IP);
    stw_be_p(ip + 10, ip_checksum(ip, 20));
    stw_be_p(ip + 20, 4000);
    stw_be_p(ip + 22, 4001);
    stw_be_p(ip + 24, len - 20);
    for (i = 28; i < len; i++) {
        ip[i] = i * 7;
    }
    return 2 + len + 2;         /* and the final halfword 0 */
}

/* Read one frame of -netdev socket (stream: a 4-byte length first) */
static size_t read_frame(int fd, uint8_t *buf, size_t size)
{
    uint8_t lenb[4];
    size_t len = 0, got = 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };

    while (got < 4 + size) {
        ssize_t n;

        if (poll(&p, 1, TIMEOUT_S * 1000) <= 0) {
            return 0;
        }
        n = got < 4 ? read(fd, lenb + got, 4 - got)
                    : read(fd, buf + got - 4, len - (got - 4));
        if (n <= 0) {
            return 0;
        }
        got += n;
        if (got == 4) {
            len = ldl_be_p(lenb);
            g_assert_cmpuint(len, <=, size);
        }
        if (got >= 4 && got == 4 + len) {
            return len;
        }
    }
    return 0;
}

static void write_frame(int fd, const uint8_t *frame, size_t len)
{
    uint8_t lenb[4];

    stl_be_p(lenb, len);
    g_assert_cmpint(write(fd, lenb, 4), ==, 4);
    g_assert_cmpint(write(fd, frame, len), ==, len);
}

/*
 * A packet larger than the MTU leaves as fragments; one with Don't
 * Fragment comes back to the guest as ICMP "fragmentation needed".
 */
static void test_net_mtu(void)
{
    g_autofree char *path = NULL;
    g_autofree char *args = NULL;
    g_autofree uint8_t *bundle = g_malloc(NET_BIG + 4);
    g_autofree uint8_t *data = g_malloc0(NET_BIG);
    uint8_t frame[2048], icmp[4096];
    size_t len, off = 0;
    uint32_t icmp_len;
    QTestState *qts;
    int sv[2];
    bool last = false;

    if (skip_without_iucv(true)) {
        return;
    }
    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
    path = write_guest(iucv_guest_net, iucv_guest_net_len);
    args = g_strdup_printf("-netdev socket,id=n0,fd=%d "
                           "-device iucv-net,name=NET,netdev=n0,"
                           "mac=52:54:00:12:34:56,ip=10.0.3.2/24,"
                           "gateway=10.0.3.1", sv[1]);
    qts = start_guest(path, args);
    close(sv[1]);

    len = make_bundle(bundle, NET_BIG, 0x1234, false);
    qtest_memwrite(qts, 0x80000, bundle, len);
    qtest_writel(qts, G_PARAM, len);
    memcpy(data, bundle + 2, NET_BIG);  /* what must arrive */
    len = make_bundle(bundle, NET_BIG_DF, 0x1235, true);
    qtest_memwrite(qts, 0x90000, bundle, len);
    qtest_writel(qts, G_PARAM + 4, len);
    run_guest(qts);

    /* ARP for the gateway, answered */
    len = read_frame(sv[0], frame, sizeof(frame));
    g_assert_cmpuint(len, >=, 42);
    g_assert_cmphex(lduw_be_p(frame + 12), ==, 0x0806);
    g_assert_cmphex(lduw_be_p(frame + 20), ==, 1);
    g_assert_cmphex(ldl_be_p(frame + 28), ==, NET_GUEST_IP);
    g_assert_cmphex(ldl_be_p(frame + 38), ==, NET_GW_IP);
    memcpy(frame, net_mac, 6);
    memcpy(frame + 6, gw_mac, 6);
    stw_be_p(frame + 20, 2);
    memcpy(frame + 22, gw_mac, 6);
    stl_be_p(frame + 28, NET_GW_IP);
    memcpy(frame + 32, net_mac, 6);
    stl_be_p(frame + 38, NET_GUEST_IP);
    write_frame(sv[0], frame, 42);

    /* the fragments, in order, each within the MTU */
    while (!last) {
        const uint8_t *ip = frame + 14;
        uint16_t flags;
        size_t plen;

        len = read_frame(sv[0], frame, sizeof(frame));
        g_assert_cmpuint(len, >, 14 + 20);
        g_assert_cmpmem(frame, 6, gw_mac, 6);
        g_assert_cmphex(lduw_be_p(frame + 12), ==, 0x0800);
        plen = lduw_be_p(ip + 2);
        g_assert_cmpuint(plen, <=, NET_MTU);
        g_assert_cmphex(lduw_be_p(ip + 4), ==, 0x1234);
        g_assert_cmphex(ip_checksum(ip, 20), ==, 0);
        flags = lduw_be_p(ip + 6);
        g_assert_cmpuint((flags & 0x1fff) * 8, ==, off);
        last = !(flags & 0x2000);
        g_assert_true(last || (plen - 20) % 8 == 0);
        g_assert_cmpmem(ip + 20, plen - 20, data + 20 + off, plen - 20);
        off += plen - 20;
    }
    g_assert_cmpuint(off, ==, NET_BIG - 20);

    /* a paused guest stops its network backend: it ends only now */
    qtest_writeb(qts, G_PARAM + 0x10, 1);
    g_assert_cmphex(wait_result(qts), ==, 0);
    /* the bundle with the ICMP error */
    icmp_len = qtest_readl(qts, G_PARAM + 8);
    g_assert_cmpuint(icmp_len, <=, sizeof(icmp));
    qtest_memread(qts, 0xa0000, icmp, icmp_len);
    g_assert_cmpuint(icmp_len, ==, 2 + 20 + 8 + 28 + 2);
    g_assert_cmpuint(lduw_be_p(icmp), ==, icmp_len - 2);
    g_assert_cmpuint(lduw_be_p(icmp + icmp_len - 2), ==, 0);
    g_assert_cmphex(icmp[2 + 9], ==, IPPROTO_ICMP);
    g_assert_cmphex(ldl_be_p(icmp + 2 + 12), ==, NET_GW_IP);
    g_assert_cmphex(ldl_be_p(icmp + 2 + 16), ==, NET_GUEST_IP);
    g_assert_cmphex(ip_checksum(icmp + 2, 20), ==, 0);
    g_assert_cmphex(icmp[22], ==, 3);           /* unreachable */
    g_assert_cmphex(icmp[23], ==, 4);           /* fragmentation needed */
    g_assert_cmpuint(lduw_be_p(icmp + 28), ==, NET_MTU);
    g_assert_cmphex(ip_checksum(icmp + 22, 8 + 28), ==, 0);
    g_assert_cmpmem(icmp + 30, 28, bundle + 2, 28);

    qtest_quit(qts);
    close(sv[0]);
    unlink(path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/iucv/cp", test_cp);
    qtest_add_func("/iucv/registers", test_registers);
    qtest_add_func("/iucv/smp", test_smp);
    qtest_add_func("/iucv/unavailable", test_unavailable);
    qtest_add_func("/iucv/none", test_none);
    qtest_add_func("/iucv/tcpip/basic", test_tcpip);
    qtest_add_func("/iucv/tcpip/no-host", test_tcpip_no_host);
    qtest_add_func("/iucv/tcpip/two-paths", test_tcpip_two_paths);
    qtest_add_func("/iucv/net/mtu", test_net_mtu);

    return g_test_run();
}
