.. _s390x-esa390-compat:

ESA/390-compatibility mode
==========================

Machines since the z14 generation start in z/Architecture mode and can no
longer be switched to it (the configuration-z/Architecture-architectural-mode
facility).  Programs that expect to be loaded in ESA/390 mode and to switch
to z/Architecture themselves with SIGP set-architecture, such as older
operating systems and stand-alone utilities, do not start on them.  Real
machines offer such programs the ESA/390-compatibility mode in a virtual
machine.  The properties below make KVM guests of the ``s390-ccw-virtio``
and ``czam`` machine types start in that mode.  The machine type
``czam-compat`` turns them on by default.

.. note::

   These properties are experimental.  They need support in the host
   kernel's KVM that is not part of upstream Linux, and the interface to it
   is provisional (see the ``PROVISIONAL`` blocks in ``linux-headers/`` and
   ``target/s390x/kvm/esa390.c``).  QEMU speaks the interface of the kernel
   series for Linux 7.3; that kernel offers the mode only when its ``kvm``
   module is loaded with ``esa390_cm=1``, and ``esa390-dat`` additionally
   needs ``esa390_lab_dat=1``.  QEMU checks for the support when it
   starts and ends with an error message naming the property if the kernel
   does not have it, or if the accelerator is not KVM.  With the properties
   off, which is the default of every machine type but ``czam-compat``,
   nothing changes.

``esa390-cm=on|off``
  The configuration starts in the ESA/390-compatibility mode, and is put
  back into it by every reset that belongs to a load or that clears the
  configuration (not by a CPU reset).  The guest leaves the mode with SIGP
  set-architecture.  The mode has no dynamic address translation.

``esa390-dat=on|off``
  Needs ``esa390-cm=on``.  Lets the guest turn on ESA/390 dynamic address
  translation while it is in the mode, which the mode itself does not
  provide and an older generation of initial program load code relies on.
  The topmost megabyte of the guest's storage is set aside for the tables
  the kernel keeps for this and is not reported to the guest as storage.
  The machine needs at least 16 MiB.

For example::

  qemu-system-s390x -machine czam,esa390-cm=on -accel kvm -cpu host ...

Limitations:

* With ``esa390-dat=on`` a guest cannot be migrated or saved in a snapshot
  while it is in the mode; the attempt fails with a message and the guest
  keeps running.  Once it has switched to z/Architecture it migrates as
  usual.  Source and destination need the same setting of both properties.
* Protected virtualization is not available with the mode.

SIGP set-architecture is executed by QEMU: the kernel changes the format
in which it runs the CPUs, QEMU the PSW, control registers and prefix as
the order defines, including the state captured for code 2.  While the
guest is in the mode the SIGP orders that QEMU handles use the ESA/390
formats: store status and store status at address, a 4K prefix area for
set prefix, store extended status at address (order 0x11) instead of store
additional status at address (order 0x17).

Nested guests in ESA/390 mode
-----------------------------

This is independent of the properties above and concerns a hypervisor that
runs *inside* the guest, for example z/VM, or Linux with KVM.  Such a
hypervisor may start its own guests in ESA/390 mode.  Linux 7.1 and later
can pass these guests on to the machine, but only when QEMU asks for it;
older kernels end them with a validity interception.

``nested-esa390=auto|on|off``
  ``auto``, the default of all machine types, asks the host kernel whenever
  it has the capability (``KVM_CAP_S390_VSIE_ESAMODE``).  ``on`` requires
  it: QEMU ends with an error message on a host without it, also as the
  destination of a migration.  ``off`` never asks, which is how QEMU
  behaved before the property existed.

The setting is not part of the migration stream and the guest hypervisor
has no indication of it.  With ``auto``, a guest that moves from a host
with the capability to a host without it keeps running, but the nested
ESA/390-mode guests of its hypervisor fail from then on; use ``on`` where
hosts with older kernels may be migration targets.  The guest itself always
runs in z/Architecture mode unless ``esa390-cm`` is set.

SCLP CPU features (experimental)
--------------------------------

``x-sclp-cpu-features=<hex>`` is a test aid.  The bytes given (up to six,
as hexadecimal digits) are ORed into the feature bytes of every CPU entry
that Read SCP Info and Read CPU Info return.  With it, a guest hypervisor
can be shown facilities for interpretive execution that KVM does not
provide, to see how it reacts; a guest that then uses such a facility fails.
The default leaves the entries as they are.  Not for production use::

  -machine czam,x-sclp-cpu-features=060000c0
