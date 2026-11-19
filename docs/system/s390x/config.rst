Configuration files
===================

A guest with many devices is easier to describe in a configuration file read
with ``-readconfig`` than on the command line.  Besides the standard groups
(``machine``, ``accel``, ``memory``, ``name``, ``drive``, ``netdev``,
``device`` and so on), the s390x device shorthands are accepted as sections
with the same keys as the command-line options: ``dev3215``, ``dev3270``,
``sclp3270``, ``dev3390``, ``dev3590``, ``dev9336`` and ``qeth``.  A section
is equivalent to the option with the same keys::

  [dev3390 "res"]
    file = "/images/0A80-RES001.qcow2"
    format = "qcow2"
    devno = "0A80"

is the same as ``-dev3390 id=res,file=/images/0A80-RES001.qcow2,format=qcow2,devno=0A80``.
The section name supplies ``id``; leave it out for shorthands that do not
take one (``dev3270``, ``sclp3270`` and ``qeth``).  Values are strings, as
everywhere in QEMU configuration files.

A complete example::

  [name]
    guest = "example"

  [machine]
    type = "czam"
    ipl = "0A80"
    loadparm = "0A82CS"

  [accel]
    accel = "kvm"

  [memory]
    size = "32G"

  [qeth]
    devno = "0400"
    hostdev = "tap0"
    mac = "52:54:00:12:34:56"

  [sclp3270]
    port = "3905"

  [dev3270]
    port = "3900"
    devno = "0700"

  [dev3390 "res"]
    file = "/images/0A80-RES001.qcow2"
    format = "qcow2"
    devno = "0A80"

Start it with ``qemu-system-s390x -readconfig example.conf -nographic``;
options without a configuration group, such as ``-nographic`` and
``-snapshot``, stay on the command line.  ``-snapshot`` also applies to
disks defined in the file.
