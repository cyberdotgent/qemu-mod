The virtual channel subsystem
=============================

QEMU implements a virtual channel subsystem with subchannels, (mostly
functionless) channel paths, and channel devices (virtio-ccw, 3270, and
devices passed via vfio-ccw). It supports multiple subchannel sets (MSS) and
multiple channel subsystems extended (MCSS-E).

All channel devices support the ``devno`` property, which takes a parameter
in the form ``<cssid>.<ssid>.<device number>``.

The default channel subsystem image id (``<cssid>``) is ``0xfe``. Devices in
there will show up in channel subsystem image ``0`` to guests that do not
enable MCSS-E. Note that devices with a different cssid will not be visible
if the guest OS does not enable MCSS-E (which is true for all supported guest
operating systems today).

Supported values for the subchannel set id (``<ssid>``) range from ``0-3``.
Devices with a ssid that is not ``0`` will not be visible if the guest OS
does not enable MSS (any Linux version that supports virtio also enables MSS).
Any device may be put into any subchannel set, there is no restriction by
device type.

The device number can range from ``0-0xffff``.

If the ``devno`` property is not specified for a device, QEMU will choose the
next free device number in subchannel set 0, skipping to the next subchannel
set if no more device numbers are free. Some emulated device models use a
model-specific starting number: FBA disks start at ``0200``, ECKD disks at
``0300``, QETH adapters at ``0400``, tape drives at ``0580``, and 3270
terminals at ``0700``.

QEMU places a device at the first free subchannel in the specified subchannel
set. If a device is hotunplugged and later replugged, it may appear at a
different subchannel. (This is similar to how z/VM works.)

Configuration token
-------------------

The optional ``chsc-config-token`` machine property supplies the 64-byte
current configuration token returned by CHSC Store Configuration Information.
Its value is exactly 128 hexadecimal digits.  When present, QEMU also returns
the same value as the target configuration token and marks the configuration
and current token valid.  The value should remain stable while the virtual I/O
configuration is unchanged.  For example::

    -machine s390-ccw-virtio,chsc-config-token=<128 hexadecimal digits>

If the property is omitted, QEMU retains its legacy basic-mode response
without a configuration token.

.. _s390x-css-measurement:

Channel-subsystem monitoring
----------------------------

The channel subsystem maintains measurement blocks for subchannels that the
guest enables for it (SET CHANNEL MONITOR, measurement-block-update mode).
The subchannels have no timing facility, so the only measurement data is the
count of START SUBCHANNEL and RESUME SUBCHANNEL operations.  It is stored
when the start function completes at the device or is suspended, as the
architecture describes, and not for an operation that is cleared, cancelled
or ends with a subchannel logout.

Both block formats are available by default: format 0 (an entry in the
measurement-block area) and format 1 (a block of its own per subchannel, the
extended-I/O-measurement-block facility).  The machine property
``ext-measurement-block=off`` withholds the facility: CHSC store
channel-subsystem characteristics does not report it (general
characteristic 48), MODIFY SUBCHANNEL rejects the measurement-block-format
control, and only format-0 blocks are used.  QEMU does not provide the
extended-measurement-word facility that real machines have together with
the first one, and an operating system that takes one for the other stops
during its initialisation; for such a system turn the property off::

    -machine s390-ccw-virtio,ext-measurement-block=off

The default is ``on`` for all machine types except ``czam-compat``.  The
property adds no migration state; source and destination need the same
setting.  Trace event: ``css_chnmon_update``.

Examples
--------

* a virtio-net device, cssid/ssid/devno automatically assigned::

    -device virtio-net-ccw

  In a Linux guest (without default devices and no other devices specified
  prior to this one), this will show up as ``0.0.0000`` under subchannel
  ``0.0.0000``.

  The auto-assigned-properties in QEMU (as seen via e.g. ``info qtree``)
  would be ``dev_id = "fe.0.0000"`` and ``subch_id = "fe.0.0000"``.

* a virtio-rng device in subchannel set ``0``::

    -device virtio-rng-ccw,devno=fe.0.0042

  If added to the same Linux guest as above, it would show up as ``0.0.0042``
  under subchannel ``0.0.0001``.

  The properties for the device would be ``dev_id = "fe.0.0042"`` and
  ``subch_id = "fe.0.0001"``.

* a virtio-gpu device in subchannel set ``2``::

    -device virtio-gpu-ccw,devno=fe.2.1111

  If added to the same Linux guest as above, it would show up as ``0.2.1111``
  under subchannel ``0.2.0000``.

  The properties for the device would be ``dev_id = "fe.2.1111"`` and
  ``subch_id = "fe.2.0000"``.

* a virtio-mouse device in a non-standard channel subsystem image::

    -device virtio-mouse-ccw,devno=2.0.2222

  This would not show up in a standard Linux guest.

  The properties for the device would be ``dev_id = "2.0.2222"`` and
  ``subch_id = "2.0.0000"``.

* a virtio-keyboard device in another non-standard channel subsystem image::

    -device virtio-keyboard-ccw,devno=0.0.1234

  This would not show up in a standard Linux guest, either, as ``0`` is not
  the standard channel subsystem image id.

  The properties for the device would be ``dev_id = "0.0.1234"`` and
  ``subch_id = "0.0.0000"``.
