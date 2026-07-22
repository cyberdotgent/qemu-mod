Emulated FBA disks
==================

QEMU can expose a block backend as a channel-attached fixed-block architecture
(FBA) DASD.  The ``fba-ccw`` device identifies itself as an IBM 9336 model 20
on a 6310 control unit and uses 512-byte blocks.  Its reported capacity is
normally derived from the block backend, so raw, qcow2, compressed, sparse,
and other images supported by QEMU's block layer can be used.

The complete block and device syntax is::

  -blockdev driver=qcow2,node-name=sysres,file.driver=file,file.filename=sysres.qcow2 \
  -device fba-ccw,drive=sysres,devno=fe.0.0200

The equivalent shorthand for an image file is::

  -dev9336 file=sysres.raw,devno=200

``-dev9336`` accepts either ``file=PATH`` or ``drive=NODE``.  For ``file``,
``format=FORMAT`` (``raw`` by default) and ``readonly=on`` are also accepted.
``blocks=N`` can expose a capacity smaller than the backend, and ``id=ID``
sets the QEMU device ID.  Three-digit S/370 and four-digit ESA/390 hexadecimal
device numbers are both accepted in the default ``fe.0`` channel subsystem;
for example, ``200`` and ``0200`` both become ``fe.0.0200``.  The option is
repeatable, for example::

  -dev9336 file=sysres.raw,devno=200 \
  -dev9336 file=work01.qcow2,format=qcow2,devno=201 \
  -dev9336 drive=shared-node,devno=202

The implementation provides Read IPL, NOP, Sense, Unconditional Reserve,
Write, Read, Locate, Define Extent, Read Device Characteristics, Release,
Read and Reset Buffered Log, Reserve, and Sense ID.  Reserve state is local to
one QEMU device and is migrated, but reservations are not coordinated between
separate QEMU processes sharing storage.

IPL
---

Use the operator-style ``-ipl`` option to select an FBA device.  The device
number may be short when it uniquely identifies a configured device::

  qemu-system-s390x -M s390-ccw-virtio \
    -dev9336 file=sysres.raw,devno=200 -ipl 200

The s390-ccw firmware performs the architectural FBA Read IPL operation and
runs the channel program loaded from block zero before entering the loaded
IPL PSW.  ``-loadparm`` can be used alongside ``-ipl`` when the guest consumes
a load parameter.
