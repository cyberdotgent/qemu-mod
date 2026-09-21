QETH network adapters
=====================

``qeth-ccw`` provides an emulated OSA-Express layer-2 network adapter.  It
connects the s390x channel subsystem's CCW and QDIO interfaces to a normal
QEMU network backend.

The adapter occupies three consecutive CCW device numbers.  ``devno`` selects
the first (read) device; the write and QDIO data devices use the next two
numbers.  If ``devno`` is omitted, automatic allocation begins at ``0400``.
For example::

  -netdev user,id=qnet \
  -device qeth-ccw,netdev=qnet,devno=fe.0.0600,mac=52:54:00:12:34:60

This creates devices ``0600``, ``0601``, and ``0602``.  The three subchannels
report control-unit type/model ``1731/01``, device type/model ``1732/01``,
and an OSD channel path.

Linux configuration
-------------------

The Linux ``qeth`` driver does not automatically group an unconfigured CCW
triplet.  With ``s390-tools`` installed, configure and online the adapter
with either ``znetconf`` or distribution-specific network configuration.
For example::

  znetconf -a 0.0.0600,0.0.0601,0.0.0602
  ip link set dev enc0600 up

The interface name depends on the guest's predictable-interface-name policy.
DHCP can then be run in the usual way.

Implementation scope
--------------------

The model implements the Linux OSD layer-2 initialization path, including
IDX, MPC and IPA control messages, format-0 QDIO queues, SIGA, SSQD,
conventional program-controlled interrupts, and Ethernet frame transport.
It does not currently implement layer-3 QETH, multicast/VLAN offloads,
QDIO thin interrupts, QEBSM, or migration of an active adapter.
