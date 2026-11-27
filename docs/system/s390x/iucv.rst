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

``-device iucv-tcpip,name=PEER,ip=A.B.C.D/LEN[,...]``
  The socket interface of a VM TCP/IP virtual machine (``IUCVAPI``, API types
  2 and 3), run on sockets of the host with a policy:

  - outgoing connections and datagrams leave from the host like NAT; the guest
    sees the address ``ip``;
  - the host's loopback and own addresses are refused unless
    ``allow-host=on``; link-local, multicast, reserved and broadcast
    destinations (including the broadcast and network addresses of the host's
    subnets) are always refused, and so are the networks in
    ``deny=CIDR[;CIDR...]``;
  - the guest can accept connections only on ports forwarded to it with
    ``hostfwd=tcp:HOSTPORT-GUESTPORT[;udp:HOSTPORT-GUESTPORT...]``; the host
    port is opened when the guest binds the guest port, on
    ``hostfwd-addr`` (default all addresses);
  - ``dns=A.B.C.D`` is an address the guest can use as its name server; its
    DNS traffic goes to the first name server of the host;
  - raw sockets are limited to ICMP echo through unprivileged ICMP sockets,
    with ``raw=on``;
  - ``max-sockets`` (default 256) is the size of the socket table of a path,
    ``max-host-sockets`` (default 512) limits the host sockets of all paths.

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
