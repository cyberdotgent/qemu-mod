QETH network adapters
=====================

``qeth-ccw`` provides an emulated OSA-Express (OSD) network adapter.  It
connects the s390x channel subsystem's CCW and QDIO interfaces to a normal
QEMU network backend.  The guest chooses layer-2 or layer-3 operation when it
starts the adapter, so the same device serves guests using either mode.

The adapter occupies consecutive CCW device numbers: a read and a write
device and, by default, one QDIO data device.  ``devno`` selects the first
device; the guest assigns the read, write and data roles when it activates
the adapter.  If ``devno`` is omitted, automatic allocation begins at
``0400``.  For example::

  -netdev user,id=qnet \
  -device qeth-ccw,netdev=qnet,devno=fe.0.0600,mac=52:54:00:12:34:60

This creates devices ``0600``, ``0601``, and ``0602``.  The subchannels
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

Multiple data devices
---------------------

A guest can run several interfaces over one adapter, each set up as its own
ULP connection with its own data device and QDIO queues while sharing the
read and write devices and the network backend; some guests use separate
interfaces, and therefore separate data devices, for IPv4 and IPv6.  The
``data-devices`` property (``datadevs`` with ``-qeth``, at most 8) adds data
devices after the write device::

  -qeth devno=0400,hostdev=tap0,datadevs=2

creates devices ``0400`` to ``0403``.  Received packets go to the connection
that registered the destination address or multicast group; broadcasts go to
every connection with IPv4 addresses.

Layer 2 and layer 3
-------------------

In layer-2 mode the guest exchanges Ethernet frames with the backend.

In layer-3 mode the guest exchanges IP packets, and the adapter performs the
link-layer work of a real OSA on the guest's behalf: it registers the IP
addresses the guest sets (``SETIP``), announces them with a gratuitous ARP,
answers ARP requests for them, resolves next hops through its own ARP cache,
maps broadcast and multicast destinations, and adds or removes the Ethernet
header.  The adapter's MAC address is the one the network sees.

For IPv6 in layer-3 mode a guest either resolves neighbours itself and hands
the adapter complete Ethernet frames (passthru, as Linux does), or hands it
plain IPv6 packets.  For the latter the adapter keeps a neighbour cache: it
solicits unknown next hops, learns from neighbour solicitations and
advertisements, answers solicitations for the guest's registered addresses
and announces newly registered addresses.  Solicited-node and all-nodes
multicast for the registered addresses is accepted without explicit
registration.

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

Other guests
------------

Guests that define the adapter by its read, write and data device numbers
must use the adapter's device numbers: with ``devno=0400`` the read device is
``0400``, the write device ``0401`` and the data devices start at ``0402``.  Give the
host side of the tap an address in the guest interface's subnet (or the
guest's default gateway) to reach the guest's TCP/IP stack.

Tracing
-------

The ``qeth_*`` and ``qdio_*`` trace events cover channel commands, the IDX,
MPC and IPA control exchanges (``qeth_ctrl_data`` dumps the raw control
data), unhandled requests, SIGA, queue establishment, transmitted and
received packets, and the layer-3 address and ARP handling.

Implementation scope
--------------------

The model implements IDX, the MPC CM/ULP/DM exchanges, the IPA commands used
by the supported guests (assist negotiation, address registration, adapter
parameters), self-description (Read Configuration Data, Read Node
Identifier), format-0 QDIO queues, SIGA, SSQD, conventional
program-controlled interrupts, and Ethernet or IP transport.  It does not
currently implement QDIO thin interrupts, QEBSM, VLANs, or checksum or
segmentation offloads.

Migration
---------

An active adapter migrates (live migration, snapshots with RAM state): the
peer subchannels with their outstanding control READs and the active-queues
channel program of each data subchannel, the IDX roles, the MPC tokens and
sequence numbers, every ULP connection with its layer-2/3 mode, LAN state,
assists and QDIO queue descriptors and cursors, the layer-3 address and
multicast tables, the ARP and neighbour caches, and queued control
responses.  The QDIO buffers and their states (SLSBs) are guest storage and
migrate with it.  When the VM starts running on the destination, the adapter
transmits what the guest primed meanwhile, accepts packets queued by the
backend and answers an outstanding READ, so the guest continues without
re-initialising the interface.  Frames held back in layer-3 mode while the
adapter resolves their next hop are dropped; the guest retransmits them.
The destination must use the same ``data-devices`` value.
