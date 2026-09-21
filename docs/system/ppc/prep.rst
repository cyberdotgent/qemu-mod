Prep machine (``40p``)
==================================================================

Use the executable ``qemu-system-ppc`` to simulate a complete 40P (PREP)

Supported devices
-----------------

QEMU emulates the following 40P (PREP) peripherals:

 *  PCI Bridge
 *  PCI VGA compatible card with VESA Bochs Extensions
 *  2 IDE interfaces with hard disk and CD-ROM support
 *  Floppy disk
 *  PCnet network adapters
 *  Serial port
 *  PREP Non Volatile RAM
 *  PC compatible keyboard and mouse.

Persistent NVRAM
----------------

By default the contents of the M48T59 NVRAM are lost when QEMU exits. To
keep the firmware settings across runs, create an 8 KiB raw image and attach
it as the first ``pflash`` drive::

  qemu-img create -f raw 40p-nvram.img 8k
  qemu-system-ppc -M 40p -drive if=pflash,format=raw,file=40p-nvram.img ...

Every guest write to the general-purpose NVRAM area is written through to
the image immediately. The clock, alarm, watchdog and control registers at
the top of the chip are derived from the host clock at runtime and are not
persisted.
