/*
 * IUCV point-to-point IP link (the protocol of Linux netiucv and of the
 * IUCV links of z/VM TCP/IP) on a QEMU network backend
 *
 * The guest connects to the service's user ID and sends one-way messages
 * that each carry one or more IPv4 packets: a halfword with the offset of
 * the next header, the packet, ..., and a halfword 0. There is no link
 * layer on the IUCV side. Towards the network backend the service is an
 * Ethernet station with its own MAC address that owns the guest's IPv4
 * address: it answers ARP for it and resolves the next hop (an address on
 * the guest's subnet, or the gateway) itself.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/sockets.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/s390x/iucv.h"
#include "net/net.h"
#include "net/eth.h"
#include "trace.h"

#define TYPE_IUCV_NET "iucv-net"
OBJECT_DECLARE_SIMPLE_TYPE(IUCVNet, IUCV_NET)

#define ETH_HDR         14
#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806
#define ARP_LEN         28
#define NET_MSGLIM      50
#define NET_BUNDLE_MAX  32768   /* bytes per message to the guest */
#define NET_QUEUE_MAX   512     /* packets waiting for the guest */
#define NET_ARP_ENTRIES 16
#define NET_ARP_PENDING 32

typedef struct ArpEntry {
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    int64_t used;
} ArpEntry;

struct IUCVNet {
    IUCVService parent_obj;

    NICConf conf;
    NICState *nic;
    char *ip_str;
    char *gateway_str;

    uint32_t ip;
    uint32_t mask;
    uint32_t gateway;
    IUCVPath *path;
    GQueue to_guest;            /* IP packets (GByteArray) */
    GQueue arp_wait;            /* Ethernet frames without a destination */
    ArpEntry arp[NET_ARP_ENTRIES];
    int64_t arp_last;
};

static const uint8_t bcast_mac[ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

/* user data the Linux driver connects with: EBCDIC "0       0       " */
static const uint8_t iucv_magic[16] = {
    0xf0, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0xf0, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40
};

/* towards the guest */

static void net_flush(IUCVNet *n)
{
    IUCVService *svc = IUCV_SERVICE(n);

    while (n->path && !g_queue_is_empty(&n->to_guest) &&
           iucv_send_outstanding(n->path) < iucv_path_msglim(n->path)) {
        g_autoptr(GByteArray) b = g_byte_array_new();
        uint8_t hdr[2];

        while (!g_queue_is_empty(&n->to_guest)) {
            GByteArray *p = g_queue_peek_head(&n->to_guest);

            if (b->len && b->len + 2 + p->len + 2 > NET_BUNDLE_MAX) {
                break;
            }
            g_queue_pop_head(&n->to_guest);
            stw_be_p(hdr, b->len + 2 + p->len);
            g_byte_array_append(b, hdr, 2);
            g_byte_array_append(b, p->data, p->len);
            g_byte_array_free(p, true);
        }
        stw_be_p(hdr, 0);
        g_byte_array_append(b, hdr, 2);
        if (!iucv_send(svc, n->path, 0, b->data, b->len, false, 0)) {
            break;
        }
    }
}

static void net_to_guest(IUCVNet *n, const uint8_t *ip, size_t len)
{
    GByteArray *p;

    if (!n->path || len < 20 || g_queue_get_length(&n->to_guest) >=
                                NET_QUEUE_MAX) {
        return;
    }
    p = g_byte_array_sized_new(len);
    g_byte_array_append(p, ip, len);
    g_queue_push_tail(&n->to_guest, p);
    net_flush(n);
}

/* ARP on the guest's behalf */

static ArpEntry *arp_find(IUCVNet *n, uint32_t ip)
{
    int i;

    for (i = 0; i < NET_ARP_ENTRIES; i++) {
        if (n->arp[i].used && n->arp[i].ip == ip) {
            return &n->arp[i];
        }
    }
    return NULL;
}

static void send_arp(IUCVNet *n, uint16_t op, uint32_t tip,
                     const uint8_t *tmac)
{
    uint8_t f[ETH_HDR + ARP_LEN] = {};
    uint8_t *a = f + ETH_HDR;

    memcpy(f, op == 2 ? tmac : bcast_mac, ETH_ALEN);
    memcpy(f + 6, n->conf.macaddr.a, ETH_ALEN);
    stw_be_p(f + 12, ETHERTYPE_ARP);
    stw_be_p(a, 1);                     /* Ethernet */
    stw_be_p(a + 2, ETHERTYPE_IP);
    a[4] = ETH_ALEN;
    a[5] = 4;
    stw_be_p(a + 6, op);
    memcpy(a + 8, n->conf.macaddr.a, ETH_ALEN);
    stl_be_p(a + 14, n->ip);
    if (op == 2) {
        memcpy(a + 18, tmac, ETH_ALEN);
    }
    stl_be_p(a + 24, tip);
    qemu_send_packet(qemu_get_queue(n->nic), f, sizeof(f));
}

static void arp_learn(IUCVNet *n, uint32_t ip, const uint8_t *mac)
{
    ArpEntry *e = arp_find(n, ip);
    GList *l;
    int i;

    if (!e) {
        e = &n->arp[0];
        for (i = 1; i < NET_ARP_ENTRIES; i++) {
            if (n->arp[i].used < e->used) {
                e = &n->arp[i];
            }
        }
        e->ip = ip;
    }
    memcpy(e->mac, mac, ETH_ALEN);
    e->used = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    /* frames that waited for this address */
    l = n->arp_wait.head;
    while (l) {
        GList *next = l->next;
        GByteArray *f = l->data;

        if (ldl_p(f->data + f->len - 4) == ip) {
            memcpy(f->data, mac, ETH_ALEN);
            qemu_send_packet(qemu_get_queue(n->nic), f->data, f->len - 4);
            g_queue_delete_link(&n->arp_wait, l);
            g_byte_array_free(f, true);
        }
        l = next;
    }
}

/* towards the network: one IPv4 packet of the guest */
static void net_from_guest(IUCVNet *n, const uint8_t *ip, size_t len)
{
    uint32_t dst, hop;
    GByteArray *f;
    ArpEntry *e;
    uint8_t hdr[ETH_HDR];

    if (len < 20 || (ip[0] >> 4) != 4) {
        return;
    }
    dst = ldl_be_p(ip + 16);
    memcpy(hdr + 6, n->conf.macaddr.a, ETH_ALEN);
    stw_be_p(hdr + 12, ETHERTYPE_IP);
    if (dst == 0xffffffff || (n->mask && dst == (n->ip | ~n->mask))) {
        memcpy(hdr, bcast_mac, ETH_ALEN);
    } else if ((dst & 0xf0000000) == 0xe0000000) {
        hdr[0] = 0x01;
        hdr[1] = 0x00;
        hdr[2] = 0x5e;
        hdr[3] = (dst >> 16) & 0x7f;
        hdr[4] = dst >> 8;
        hdr[5] = dst;
    } else {
        hop = (dst & n->mask) == (n->ip & n->mask) ? dst : n->gateway;
        if (!hop) {
            return;
        }
        e = arp_find(n, hop);
        if (!e) {
            int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

            if (g_queue_get_length(&n->arp_wait) < NET_ARP_PENDING) {
                uint8_t h[4];

                /* the next hop goes after the frame until it is resolved */
                f = g_byte_array_sized_new(ETH_HDR + len + 4);
                g_byte_array_append(f, hdr, ETH_HDR);
                g_byte_array_append(f, ip, len);
                stl_p(h, hop);
                g_byte_array_append(f, h, 4);
                g_queue_push_tail(&n->arp_wait, f);
            }
            if (now - n->arp_last > 200) {
                n->arp_last = now;
                send_arp(n, 1, hop, NULL);
            }
            return;
        }
        memcpy(hdr, e->mac, ETH_ALEN);
    }
    f = g_byte_array_sized_new(ETH_HDR + len);
    g_byte_array_append(f, hdr, ETH_HDR);
    g_byte_array_append(f, ip, len);
    qemu_send_packet(qemu_get_queue(n->nic), f->data, f->len);
    g_byte_array_free(f, true);
}

/* the network backend */

static bool net_can_receive(NetClientState *nc)
{
    return true;
}

static ssize_t net_receive(NetClientState *nc, const uint8_t *buf,
                           size_t size)
{
    IUCVNet *n = qemu_get_nic_opaque(nc);
    const uint8_t *a = buf + ETH_HDR;

    if (size < ETH_HDR) {
        return size;
    }
    if (memcmp(buf, n->conf.macaddr.a, ETH_ALEN) && !(buf[0] & 1)) {
        return size;
    }
    switch (lduw_be_p(buf + 12)) {
    case ETHERTYPE_ARP:
        if (size < ETH_HDR + ARP_LEN || lduw_be_p(a + 2) != ETHERTYPE_IP) {
            break;
        }
        if (ldl_be_p(a + 14)) {
            arp_learn(n, ldl_be_p(a + 14), a + 8);
        }
        if (lduw_be_p(a + 6) == 1 && n->ip && ldl_be_p(a + 24) == n->ip) {
            send_arp(n, 2, ldl_be_p(a + 14), a + 8);
        }
        break;
    case ETHERTYPE_IP: {
        uint32_t dst = size >= ETH_HDR + 20 ? ldl_be_p(a + 16) : 0;
        size_t len = size - ETH_HDR;

        if (size >= ETH_HDR + 20 && lduw_be_p(a + 2) < len) {
            len = lduw_be_p(a + 2);     /* minimum frame padding */
        }
        if (dst == n->ip || (buf[0] & 1)) {
            net_to_guest(n, a, len);
        }
        break;
    }
    }
    return size;
}

static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = net_can_receive,
    .receive = net_receive,
};

/* the IUCV side */

static int net_connect(IUCVService *svc, IUCVPath *path, const uint8_t *user)
{
    IUCVNet *n = IUCV_NET(svc);

    if (n->path) {
        return IUCV_RC_NO_PATHS;
    }
    if (memcmp(user, iucv_magic, 16)) {
        warn_report_once("%s: connection with unexpected user data",
                         svc->name);
    }
    iucv_path_set_msglim(path, NET_MSGLIM);
    iucv_path_set_user(path, iucv_magic);
    n->path = path;
    trace_iucv_net_path(svc->name, iucv_path_id(path), "up");
    return 0;
}

static void net_message(IUCVService *svc, IUCVPath *path, IUCVMessage *msg)
{
    IUCVNet *n = IUCV_NET(svc);
    uint32_t off = 0;

    if (msg->two_way) {
        iucv_reject(svc, path, msg);
        return;
    }
    /* [next][packet][next][packet]...[0] */
    while (off + 2 <= msg->len) {
        uint16_t next = lduw_be_p(msg->data + off);

        if (!next || next <= off + 2 || next > msg->len) {
            break;
        }
        net_from_guest(n, msg->data + off + 2, next - off - 2);
        off = next;
    }
}

static void net_received(IUCVService *svc, IUCVPath *path, uint32_t msgid)
{
    net_flush(IUCV_NET(svc));
}

static void net_quiesced(IUCVService *svc, IUCVPath *path, bool quiesced)
{
    if (!quiesced) {
        net_flush(IUCV_NET(svc));
    }
}

static void net_drop_queues(IUCVNet *n)
{
    GByteArray *p;

    while ((p = g_queue_pop_head(&n->to_guest))) {
        g_byte_array_free(p, true);
    }
}

static void net_severed(IUCVService *svc, IUCVPath *path)
{
    IUCVNet *n = IUCV_NET(svc);

    if (n->path == path) {
        n->path = NULL;
        net_drop_queues(n);
        trace_iucv_net_path(svc->name, iucv_path_id(path), "down");
    }
}

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

static void net_realize(DeviceState *dev, Error **errp)
{
    IUCVNet *n = IUCV_NET(dev);
    DeviceClass *parent = DEVICE_CLASS(object_class_get_parent(
                              object_class_by_name(TYPE_IUCV_NET)));

    if (!n->ip_str) {
        error_setg(errp, "iucv-net needs ip=<address>/<prefix>");
        return;
    }
    if (!parse_ip(n->ip_str, &n->ip, &n->mask, errp) ||
        (n->gateway_str && !parse_ip(n->gateway_str, &n->gateway, NULL,
                                     errp))) {
        return;
    }
    parent->realize(dev, errp);
    if (*errp) {
        return;
    }
    qemu_macaddr_default_if_unset(&n->conf.macaddr);
    n->nic = qemu_new_nic(&net_info, &n->conf, object_get_typename(OBJECT(n)),
                          dev->id, &dev->mem_reentrancy_guard, n);
    qemu_format_nic_info_str(qemu_get_queue(n->nic), n->conf.macaddr.a);
    g_queue_init(&n->to_guest);
    g_queue_init(&n->arp_wait);
}

static void net_reset(DeviceState *dev)
{
    IUCVNet *n = IUCV_NET(dev);
    GByteArray *f;

    net_drop_queues(n);
    while ((f = g_queue_pop_head(&n->arp_wait))) {
        g_byte_array_free(f, true);
    }
    memset(n->arp, 0, sizeof(n->arp));
}

static const Property net_properties[] = {
    DEFINE_NIC_PROPERTIES(IUCVNet, conf),
    DEFINE_PROP_STRING("ip", IUCVNet, ip_str),
    DEFINE_PROP_STRING("gateway", IUCVNet, gateway_str),
};

static void net_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    IUCVServiceClass *sc = IUCV_SERVICE_CLASS(oc);

    dc->realize = net_realize;
    device_class_set_legacy_reset(dc, net_reset);
    device_class_set_props(dc, net_properties);
    dc->desc = "IUCV point-to-point IP link (netiucv)";
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    sc->connect = net_connect;
    sc->message = net_message;
    sc->received = net_received;
    sc->quiesced = net_quiesced;
    sc->severed = net_severed;
}

static const TypeInfo net_type_info = {
    .name = TYPE_IUCV_NET,
    .parent = TYPE_IUCV_SERVICE,
    .instance_size = sizeof(IUCVNet),
    .class_init = net_class_init,
};

static void net_register_types(void)
{
    type_register_static(&net_type_info);
}

type_init(net_register_types)
