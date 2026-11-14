QETH network adapters
=====================

``qeth-ccw`` provides an emulated OSA-Express (OSD) network adapter.  It
connects the s390x channel subsystem's CCW and QDIO interfaces to a normal
QEMU network backend.  The guest chooses layer-2 or layer-3 operation when it
starts the adapter, so the same device serves Linux in either mode and z/OS
Communications Server, which uses layer 3.

The adapter occupies three consecutive CCW device numbers.  ``devno`` selects
the first device; the guest assigns the read, write and QDIO data roles when
it activates the adapter.  If ``devno`` is omitted, automatic allocation
begins at ``0400``.  For example::

  -netdev user,id=qnet \
  -device qeth-ccw,netdev=qnet,devno=fe.0.0600,mac=52:54:00:12:34:60

This creates devices ``0600``, ``0601``, and ``0602``.  The three subchannels
report control-unit type/model ``1731/01``, device type/model ``1732/01``,
and an OSD channel path.  The channel path ID defaults to the high byte of the
device number; ``chpid`` selects another one.

The ``-qeth`` shorthand creates the adapter together with its backend.
``hostdev`` attaches it to an existing host tap interface, without running
any configuration scripts::

  -qeth devno=0400,hostdev=tap0

The tap must already exist and be usable by the user running QEMU, for
example a persistent tap created with ``ip tuntap add dev tap0 mode tap user
$USER`` or a ``systemd-networkd`` ``.netdev`` unit with ``Kind=tap`` and
``User=``. Alternatively, ``netdev=ID`` attaches it to a backend defined with
``-netdev``; ``mac`` sets the adapter MAC address and ``chpid`` the channel
path::

  -netdev user,id=qnet -qeth devno=0400,netdev=qnet,chpid=0xf0

Layer 2 and layer 3
-------------------

In layer-2 mode the guest exchanges Ethernet frames with the backend.

In layer-3 mode the guest exchanges IP packets, and the adapter performs the
link-layer work of a real OSA on the guest's behalf: it registers the IP
addresses the guest sets (``SETIP``), announces them with a gratuitous ARP,
answers ARP requests for them, resolves next hops through its own ARP cache,
maps broadcast and multicast destinations, and adds or removes the Ethernet
header.  The adapter's MAC address is the one the network sees.  Unicast IPv6
destinations are not resolved in layer-3 mode (IPv6 multicast is passed).

Linux configuration
-------------------

The Linux ``qeth`` driver does not automatically group an unconfigured CCW
triplet.  With ``s390-tools`` installed, configure and online the adapter
with either ``znetconf`` or distribution-specific network configuration.
For example::

  znetconf -a 0.0.0600,0.0.0601,0.0.0602
  ip link set dev enc0600 up

The ``layer2`` attribute of the group device selects the mode before the
device is set online.  The interface name depends on the guest's
predictable-interface-name policy.  DHCP can then be run in the usual way.

z/OS configuration
------------------

z/OS uses the adapter through a VTAM TRLE and a TCP/IP ``MPCIPA`` device
(``IPAQENET`` link or ``INTERFACE`` statement).  The TRLE's ``READ``,
``WRITE`` and ``DATAPATH`` device numbers must match the adapter, for example
a TRLE with ``READ=(0400),WRITE=(0401),DATAPATH=(0402)`` and::

  -qeth devno=0400,hostdev=tap0

Give the host side of the tap an address in the z/OS interface's subnet (or
the z/OS default gateway) to reach the z/OS TCP/IP stack, for example TN3270
on port 23.

Tracing
-------

The ``qeth_*`` and ``qdio_*`` trace events cover channel commands, the IDX,
MPC and IPA control exchanges (``qeth_ctrl_data`` dumps the raw control
data), unhandled requests, SIGA, queue establishment, transmitted and
received packets, and the layer-3 address and ARP handling.

Implementation scope
--------------------

The model implements IDX, the MPC CM/ULP/DM exchanges, the IPA commands used
by Linux and z/OS (assist negotiation, address registration, adapter
parameters), self-description (Read Configuration Data, Read Node
Identifier), format-0 QDIO queues, SIGA, SSQD, conventional
program-controlled interrupts, and Ethernet or IP transport.  It does not
currently implement QDIO thin interrupts, QEBSM, VLANs, checksum or
segmentation offloads, IPv6 neighbour discovery in layer-3 mode, multiple
data devices per adapter, or migration of an active adapter.
