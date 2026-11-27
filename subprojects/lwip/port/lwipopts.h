/*
 * lwIP options for QEMU
 *
 * One stack per process, driven from QEMU's main loop (NO_SYS: no threads,
 * no sequential or socket API; the raw API with callbacks). IPv4 only, on
 * one Ethernet interface (a QEMU network backend). The heap is the C
 * library's; the protocol control blocks come from fixed pools, which
 * bound the number of connections, segments and queued packets.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef QEMU_LWIPOPTS_H
#define QEMU_LWIPOPTS_H

/* the environment */
#define NO_SYS                          1
#define LWIP_TIMERS                     1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_CALLBACK_API               1
#define LWIP_EVENT_API                  0
#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS 1
#define LWIP_RAND()                     qemu_lwip_rand()
/* initial sequence numbers that cannot be predicted */
#define LWIP_HOOK_TCP_ISN(lip, lport, rip, rport) qemu_lwip_rand()

/* memory: the heap is malloc(), control blocks come from pools */
#define MEM_LIBC_MALLOC                 1
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   8
#define MEMP_NUM_RAW_PCB                64
#define MEMP_NUM_UDP_PCB                512
#define MEMP_NUM_TCP_PCB                1024
#define MEMP_NUM_TCP_PCB_LISTEN         256
#define MEMP_NUM_TCP_SEG                8192
#define MEMP_NUM_REASSDATA              32
#define MEMP_NUM_FRAG_PBUF              64
#define MEMP_NUM_ARP_QUEUE              128
#define MEMP_NUM_PBUF                   256
#define PBUF_POOL_SIZE                  64
#define PBUF_POOL_BUFSIZE               1536
#define MEMP_OVERFLOW_CHECK             0
#define MEMP_SANITY_CHECK               0

/* the protocols */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       0
#define LWIP_AUTOIP                     0
#define LWIP_ACD                        0
#define LWIP_IGMP                       0
#define LWIP_DNS                        0
#define LWIP_BROADCAST_PING             0
#define LWIP_MULTICAST_PING             0
#define IP_FORWARD                      0
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              128
#define IP_DEFAULT_TTL                  64
#define IP_SOF_BROADCAST                1
#define IP_SOF_BROADCAST_RECV           1
#define SO_REUSE                        1
#define LWIP_SINGLE_NETIF               0
/* the guest reaches its own address and 127.0.0.1 (no threads: polled) */
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_HAVE_LOOPIF                1
#define LWIP_LOOPBACK_MAX_PBUFS         64
#define LWIP_NETIF_LOOPBACK_MULTITHREADING 0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_TX_SINGLE_PBUF       1
#define LWIP_CHECKSUM_ON_COPY           0
#define ETH_PAD_SIZE                    0
#define ARP_TABLE_SIZE                  64
#define ARP_QUEUEING                    1
#define ARP_QUEUE_LEN                   16
#define ETHARP_SUPPORT_STATIC_ENTRIES   0
#define ETHARP_TABLE_MATCH_NETIF        0

/* TCP (the C library's netinet/tcp.h has a TCP_MSS of its own) */
#undef TCP_MSS
#define TCP_MSS                         1460
#define TCP_WND                         (32 * TCP_MSS)
#define TCP_SND_BUF                     (32 * TCP_MSS)
#define TCP_SND_QUEUELEN                (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_SNDLOWAT                    (TCP_SND_BUF / 2)
#define TCP_QUEUE_OOSEQ                 1
#define TCP_OOSEQ_MAX_BYTES             TCP_WND
#define TCP_OOSEQ_MAX_PBUFS             64
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      64
#define TCP_SYNMAXRTX                   5
#define TCP_MAXRTX                      12
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_TCP_TIMESTAMPS             0
#define LWIP_TCP_SACK_OUT               0
#define LWIP_WND_SCALE                  0
#define TCP_CALCULATE_EFF_SEND_MSS      1

/* no statistics, no debug output */
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

#endif
