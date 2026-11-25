SCLP file access
================

The ``sclpfileaccess`` device serves the files below one host directory to
the guest through the service-call interface (SCLP event type 0x07).  On a
real machine this is the path by which a system that was loaded from
removable media or from a server reads further files from that location:
installers use it after a list-directed load (see the INS files in
:ref:`s390x-bootdevices`), and Linux knows it as the HMC drive (module
``hmcdrv``).  For example::

  -kernel media/BOOT.INS \
  -device sclpfileaccess,path=media

Properties:

``path``
  The host directory.  It can be set and changed while the machine runs
  (``qom-set``); an empty value serves nothing and every request is answered
  as not permitted.

``readonly``
  ``on`` (default) refuses put, append and delete.  With ``off`` the guest
  can create, overwrite, extend and delete files in the directory, and by
  that use as much host disk space as the file system allows.

The guest can list directories and transfer parts of files by path, or load
a file with an eight-character name at a storage address.

Names are resolved below the directory only (``openat2()`` with
``RESOLVE_BENEATH``): a path, ``..`` component or symbolic link that leads
out of it is refused, a symbolic link that stays inside is followed.  Only
regular files are read or written.  When a name does not exist as given, it
is looked up without regard to case, one path component at a time, because
systems name the files of their medium in upper case and a medium copied to
a host directory often has them in lower case.  A component that matches
more than one entry that way is not found.

One request is in flight at a time and runs in its own thread.  When the
machine is stopped for a migration or a snapshot, the request completes
first; the time that takes is part of the downtime.  The directory is host
configuration and is not migrated: give the destination the same content.

The device needs a Linux host.  Trace events: ``sclp_file_*``.
