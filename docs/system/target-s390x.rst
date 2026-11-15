.. _s390x-System-emulator:

s390x System emulator
---------------------

QEMU can emulate z/Architecture (in particular, 64 bit) s390x systems
via the ``qemu-system-s390x`` binary. Only one machine type,
``s390-ccw-virtio``, is supported (with versioning for compatibility
handling).

When using KVM as accelerator, QEMU can emulate CPUs up to the generation
of the host. When using the default cpu model with TCG as accelerator,
QEMU will emulate a subset of z13 cpu features that should be enough to run
distributions built for the z13.

Device support
==============

z/OS compatibility handshake under KVM
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``mtm-3907`` machine automatically enables a z/OS control-block
adjustment at KVM STSI exits.  This reproduces
the result of the compatibility handshake which some emulators perform
at ``STNSM 0x950,0xff``; it is not an architectural instruction side effect.
Other machine types do not perform this adjustment.  No environment variable
is required; ``QEMU_S390_ZOS_COMPAT`` is no longer used.

The hook reads the CVT pointer at guest address 0x10, the ECVT pointer at
CVT+0x8c, and the level halfword at ECVT+0x1dc.  For levels at least
0x0102 it clears the byte at SVT+0x560, where the SVT pointer is at guest
address 0xb4c.  Null, out-of-range, or inaccessible pointers cause no
adjustment.  Accesses use the current guest address translation and do
not inject guest exceptions.  Each STSI exit retries the check, allowing
initialization to publish the pointers after earlier STSI calls.

For example::

  qemu-system-s390x -machine mtm-3907 -accel kvm -cpu host ...

This hook does not intercept STNSM itself.  It requires an STSI exit after
the guest initializes the byte and before the guest consumes it; validate
that ordering for the guest release being used.  A successful change is
reported in QEMU's diagnostic output.

QEMU will not emulate most of the traditional devices found under LPAR or
z/VM; virtio devices (especially using virtio-ccw) make up the bulk of
the available devices. Passthrough of host devices via vfio-pci, vfio-ccw,
or vfio-ap is also available.

.. toctree::
   s390x/vfio-ap
   s390x/css
   s390x/3215
   s390x/3270
   s390x/fba
   s390x/3590
   s390x/3390
   s390x/qeth
   s390x/config
   s390x/vfio-ccw
   s390x/pcidevices

Architectural features
======================

.. toctree::
   s390x/bootdevices
   s390x/protvirt
   s390x/cpu-topology
   s390x/secure-ipl
   s390x/cpacf
