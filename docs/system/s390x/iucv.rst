.. SPDX-License-Identifier: GPL-2.0-or-later

IUCV
====

IUCV (inter-user communication vehicle) is the communication facility that
z/VM offers its guests. QEMU can provide it to a KVM guest itself: it executes
the IUCV instruction (``B2F0``) the way z/VM's CP does and presents the IUCV
external interruptions (code ``4000``). The communication partners of the
guest are services inside QEMU, each with a user ID the guest connects to.

IUCV needs KVM with ``KVM_CAP_S390_USER_OPEREXEC`` (the instruction is handed
to QEMU) and ``KVM_CAP_S390_USER_EXT_IRQ`` (QEMU presents the interruptions).
It blocks live migration and snapshots with RAM.

What the instruction does
-------------------------

Without any IUCV device, the instruction is an operation exception, as on a
machine without a hypervisor.

``-device s390-iucv,available=off``
  The instruction ends with condition code 3 in the supervisor state, as under
  a hypervisor that does not offer IUCV to the guest. Some operating systems
  that check for IUCV at startup expect this. No services can be added.

``-device s390-iucv[,userid=NAME][,maxconn=N]``
  IUCV is provided. It is also created implicitly by the first service.

Services
--------

``-device iucv-net,name=PEER,netdev=ID,ip=A.B.C.D/LEN[,gateway=A.B.C.D][,mtu=N][,mac=MAC]``
  A point-to-point IPv4 link with the protocol of Linux ``netiucv`` (and of
  the IUCV links of VM TCP/IP) on any network backend. Towards the backend the
  service is an Ethernet station with its own MAC address that owns the
  guest's address: it answers ARP for it and resolves the next hop (an
  address in the guest's subnet, or the gateway) itself. ``mtu`` is the MTU
  of the Ethernet side (default 1500): larger packets of the guest are
  fragmented, or, with Don't Fragment set, answered with an ICMP
  "fragmentation needed" message, so path MTU discovery adapts (Linux uses an
  MTU of 9216 on ``netiucv`` links by default). Linux uses IUCV only when it
  finds itself under z/VM: use it with the machine property
  ``zvm-personality=on``. In the guest::

    echo PEER > /sys/bus/iucv/drivers/netiucv/connection
    ip addr add A.B.C.D peer GATEWAY dev iucv0
    ip link set iucv0 up

``-device iucv-tcpip,name=PEER,netdev=ID,ip=A.B.C.D/LEN[,gateway=A.B.C.D][,mac=MAC][,...]``
  The socket interface of a VM TCP/IP virtual machine (``IUCVAPI``, API types
  2 and 3) on a TCP/IP stack of its own (lwIP, built from
  ``subprojects/lwip``). Like the TCPIP virtual machine with its network
  adapter, the service is a host on the network of its backend (for instance
  a tap device on a bridge): it has its own MAC address, owns the address
  ``ip``, answers ARP and ICMP echo for it and terminates TCP and UDP itself.
  The guest's sockets are sockets of that stack; others reach the guest at
  ``ip`` like any host, and the guest reaches the network through
  ``gateway``. Nothing goes through the sockets of the host. A process has
  one such stack, so at most one ``iucv-tcpip`` device.

  - ``dns=A.B.C.D[;A.B.C.D...]`` (up to three), ``hostname`` and ``domain``
    describe the guest's network configuration; ``hostname`` is the answer
    of GETHOSTNAME (default ``qemu``). The interface has no name-resolution
    call: the guest's resolver sends its queries to its name servers itself.
  - ``mtu`` (default 1500) is the MTU of the network side.
  - raw sockets are offered for ICMP (PING); received datagrams carry their
    IPv4 header.
  - ``127.0.0.1`` and the own address reach the stack itself.
  - ``max-sockets`` (default 256) is the size of the socket table of a path,
    ``max-open-sockets`` (default 512) limits the sockets of all paths
    together. The stack has fixed pools for its control blocks, a receive
    window of 32 segments per connection and at most 64 waiting datagrams
    (256 KiB) per datagram socket.

  For example, on a bridge of the host::

    -netdev tap,id=n1,ifname=tap1,script=no,downscript=no
    -device iucv-tcpip,name=TCPIP,netdev=n1,mac=52:54:00:12:34:58,ip=192.0.2.20/24,gateway=192.0.2.1,dns=192.0.2.53,hostname=guest,domain=example.com

Limits for untrusted guests
---------------------------

Messages larger than a service takes (1 MiB by default, 64 KiB for
``iucv-net``) are rejected without being copied, the message limit of a path
is at most 255 and counts the guest's messages until their completion has
been taken, and the data the services hold for a guest is limited to 64 MiB.

Not provided
------------

Control buffers and control paths, paths between two guests, connections a
service opens to the guest, TEST MESSAGE, and the storage-key checks of the
data areas.
