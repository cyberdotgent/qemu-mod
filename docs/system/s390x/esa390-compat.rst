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
   ``target/s390x/kvm/esa390.c``).  QEMU checks for the support when it
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

* A guest cannot be migrated or saved in a snapshot while it is in the mode;
  the attempt fails with a message and the guest keeps running.  Once it has
  switched to z/Architecture it migrates as usual.  Source and destination
  need the same setting of both properties.
* Protected virtualization is not available with the mode.
* SIGP store status at address and set prefix, which QEMU handles itself,
  use the z/Architecture formats also while the guest is in the mode.

SCLP CPU features (experimental)
--------------------------------

``x-sclp-cpu-features=<hex>`` is a test aid.  The bytes given (up to six,
as hexadecimal digits) are ORed into the feature bytes of every CPU entry
that Read SCP Info and Read CPU Info return.  With it, a guest hypervisor
can be shown facilities for interpretive execution that KVM does not
provide, to see how it reacts; a guest that then uses such a facility fails.
The default leaves the entries as they are.  Not for production use::

  -machine czam,x-sclp-cpu-features=060000c0
