Boot devices on s390x
=====================

Selecting an IPL device by address
----------------------------------

The ``-ipl`` option provides an operator-style way to select one CCW device by
its device number, without adding a ``bootindex`` property to the device. For
example::

 qemu-system-s390x -drive if=none,id=dr1,file=guest.qcow2 \
                   -device virtio-blk-ccw,drive=dr1,devno=fe.0.1000 \
                   -ipl 1000 -loadparm 2

Device numbers are hexadecimal. A short address containing one to four digits
is accepted when it uniquely identifies a device. The full QEMU CCW address
form, such as ``fe.0.1000``, can be used to distinguish devices with the same
device number in different channel-subsystem images or subchannel sets.

``-loadparm`` is a convenience spelling of the machine ``loadparm`` property.
It accepts the same value and validation rules. ``-ipl`` cannot be combined
with ``-kernel`` or with device ``bootindex`` properties. It selects one device
only, so there is no fallback boot chain.

The selected device must implement direct CCW IPL. QEMU currently supports
direct selection of virtio block, virtio network, and assigned vfio-ccw
devices. A future emulated DASD, FBA, tape, or card-reader device can opt in by
providing the CCW IPL-parameter-block operation; the command-line syntax and
selection code do not need device-specific changes.

HMC list-directed loads
-----------------------

On s390x, ``-kernel`` also accepts an HMC list-directed-load control file whose
name ends in ``.INS`` (case-insensitive). Each non-comment line contains a
component filename and its hexadecimal load address. Component filenames are
resolved relative to the control file. For example::

 * Install image
 NUCLEUS 0x00000000
 RAMDISK 0x01800000

QEMU loads every component as raw data, then obtains the initial address and
24-bit or 31-bit addressing mode from the short IPL PSW at address zero. This
matches the loading part of the HMC ``Load from Removable Media or Server``
operation and avoids a separate ``loader`` device for every component::

 qemu-system-s390x -m 2G -kernel media/BOOT.INS -loadparm CONS0009

The INS control file only describes the initial memory image. Any later files
that the installer retrieves from the HMC media still require an emulated
device or another QEMU service that implements that media protocol.

Booting with bootindex parameter
--------------------------------

For classical mainframe guests (i.e. LPAR or z/VM installations), you always
have to explicitly specify the disk where you want to boot from (or "IPL" from,
in s390x-speak -- IPL means "Initial Program Load").

So for booting an s390x guest in QEMU, you should always mark the
device where you want to boot from with the ``bootindex`` property, for
example::

 qemu-system-s390x -drive if=none,id=dr1,file=guest.qcow2 \
                   -device virtio-blk,drive=dr1,bootindex=1

Multiple devices may have a bootindex. The lowest bootindex is assigned to the
device to IPL first.  If the IPL fails for the first, the device with the second
lowest bootindex will be tried and so on until IPL is successful or there are no
remaining boot devices to try.

For booting from a CD-ROM ISO image (which needs to include El-Torito boot
information in order to be bootable), it is recommended to specify a ``scsi-cd``
device, for example like this::

 qemu-system-s390x -blockdev file,node-name=c1,filename=... \
                   -device virtio-scsi \
                   -device scsi-cd,drive=c1,bootindex=1

Note that you really have to use the ``bootindex`` property to select the
boot device. The old-fashioned ``-boot order=...`` command of QEMU (and
also ``-boot once=...``) is not supported on s390x.


Booting without bootindex parameter
-----------------------------------

The QEMU guest firmware (the so-called s390-ccw bios) has also some rudimentary
support for scanning through the available block devices. So in case you did
not specify a boot device with the ``bootindex`` property, there is still a
chance that it finds a bootable device on its own and starts a guest operating
system from it. However, this scanning algorithm is still very rough and may
be incomplete, so that it might fail to detect a bootable device in many cases.
It is really recommended to always specify the boot device with the
``bootindex`` property instead.

This also means that you should avoid the classical short-cut commands like
``-hda``, ``-cdrom`` or ``-drive if=virtio``, since it is not possible to
specify the ``bootindex`` with these commands. Note that the convenience
``-cdrom`` option even does not give you a real (virtio-scsi) CD-ROM device on
s390x. Due to technical limitations in the QEMU code base, you will get a
virtio-blk device with this parameter instead, which might not be the right
device type for installing a Linux distribution via ISO image. It is
recommended to specify a CD-ROM device via ``-device scsi-cd`` (as mentioned
above) instead.


Selecting kernels with the ``loadparm`` property
------------------------------------------------

The ``s390-ccw-virtio`` machine supports the so-called ``loadparm`` parameter
which can be used to select the kernel on the disk of the guest that the
s390-ccw bios should boot. When starting QEMU, it can be specified like this::

 qemu-system-s390x -machine s390-ccw-virtio,loadparm=<string>

The equivalent short form is ``-loadparm <string>``.

The first way to use this parameter is to use the word ``PROMPT`` as the
``<string>`` here. In that case the s390-ccw bios will show a list of
installed kernels on the disk of the guest and ask the user to enter a number
to chose which kernel should be booted -- similar to what can be achieved by
specifying the ``-boot menu=on`` option when starting QEMU. Note that the menu
list will only show the names of the installed kernels when using a DASD-like
disk image with 4k byte sectors. On normal SCSI-style disks with 512-byte
sectors, there is not enough space for the zipl loader on the disk to store
the kernel names, so you only get a list without names here.

The second way to use this parameter is to use a number in the range from 0
to 31. The numbers that can be used here correspond to the numbers that are
shown when using the ``PROMPT`` option, and the s390-ccw bios will then try
to automatically boot the kernel that is associated with the given number.
Note that ``0`` can be used to boot the default entry. If the machine
``loadparm`` is not assigned a value, then the default entry is used.

By default, the machine ``loadparm`` applies to all boot devices. If multiple
devices are assigned a ``bootindex`` and the ``loadparm`` is to be different
between them, an independent ``loadparm`` may be assigned on a per-device basis.

An example guest using per-device ``loadparm``::

  qemu-system-s390x -drive if=none,id=dr1,file=primary.qcow2 \
                   -device virtio-blk,drive=dr1,bootindex=1 \
                   -drive if=none,id=dr2,file=secondary.qcow2 \
                   -device virtio-blk,drive=dr2,bootindex=2,loadparm=3

In this case, the primary boot device will attempt to IPL using the default
entry (because no ``loadparm`` is specified for this device or for the
machine). If that device fails to boot, the secondary device will attempt to
IPL using entry number 3.

If a ``loadparm`` is specified on both the machine and a device, the per-device
value will superseded the machine value.  Per-device ``loadparm`` values are
only used for devices with an assigned ``bootindex``. The machine ``loadparm``
is used when attempting to boot without a ``bootindex``.


Booting from a network device
-----------------------------

The firmware that ships with QEMU includes a small TFTP network bootloader
for virtio-net-ccw devices.  The ``bootindex`` property is especially
important for booting via the network. If you don't specify the ``bootindex``
property here, the network bootloader won't be taken into consideration and
the network boot will fail. For a successful network boot, try something
like this::

 qemu-system-s390x -netdev user,id=n1,tftp=...,bootfile=... \
                   -device virtio-net-ccw,netdev=n1,bootindex=1

The network bootloader also has basic support for pxelinux.cfg-style
configuration files. See the `PXELINUX Configuration page
<https://wiki.syslinux.org/wiki/index.php?title=PXELINUX#Configuration>`__
for details how to set up the configuration file on your TFTP server.
The supported configuration file entries are ``DEFAULT``, ``LABEL``,
``KERNEL``, ``INITRD`` and ``APPEND`` (see the `Syslinux Config file syntax
<https://wiki.syslinux.org/wiki/index.php?title=Config>`__ for more
information).
