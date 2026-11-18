#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the DASD flush test and check the flush counters of each drive."""

import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time


HEADS = 15
TRACK_SIZE = 56832
HEADER_SIZE = 512

# drive id -> (flush_operations, failed_flush_operations) after the guest
EXPECTED = {
    "eckdro": (0, 0),
    "eckdrw": (3, 0),
    "fbaro": (0, 0),
    "fbarw": (2, 0),
    "eckderr": (0, 1),
    "fbaerr": (0, 1),
}


def create_ckd(path):
    """A one-cylinder CKD64 volume of which only the header is written."""
    header = bytearray(HEADER_SIZE)
    header[0:8] = b"CKD_P064"
    struct.pack_into("<II", header, 8, HEADS, TRACK_SIZE)
    header[16] = 0x90
    struct.pack_into("<H", header, 18, 1)
    with open(path, "wb") as f:
        f.write(header)
        f.truncate(HEADER_SIZE + HEADS * TRACK_SIZE)


def create_fba(path):
    with open(path, "wb") as f:
        f.truncate(64 * 512)


class Qmp:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.file = self.sock.makefile("rw")
        self.read()
        self.command("qmp_capabilities")

    def read(self):
        line = self.file.readline()
        if not line:
            raise EOFError("QMP connection closed")
        return json.loads(line)

    def command(self, name, **arguments):
        self.file.write(json.dumps({"execute": name,
                                    "arguments": arguments}) + "\n")
        self.file.flush()
        while True:
            reply = self.read()
            if "return" in reply:
                return reply["return"]
            if "error" in reply:
                raise RuntimeError(f"{name}: {reply['error']}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu, guest = sys.argv[1:]
    accel = os.environ.get("DASD_TEST_ACCEL", "tcg")

    with tempfile.TemporaryDirectory(prefix="qemu-dasd-flush-") as tmp:
        images = {}
        for name in EXPECTED:
            path = pathlib.Path(tmp, name)
            if name.startswith("eckd"):
                create_ckd(path)
            else:
                create_fba(path)
            images[name] = path

        args = [
            qemu,
            "-machine", "s390-ccw-virtio",
            "-accel", accel,
            "-nodefaults",
            "-display", "none",
            # A successful guest stops in the special wait PSW, which
            # requests a shutdown; a failing one panics.  Pause in both
            # cases to read the counters.
            "-action", "panic=pause",
            "-action", "shutdown=pause",
            "-qmp", f"unix:{tmp}/qmp,server=on,wait=off",
        ]
        for devno, name in enumerate(EXPECTED, 0x200):
            drive = f"if=none,id={name},format=raw,cache=writeback"
            if name.endswith("err"):
                # Every flush of the image fails with EIO.
                drive += (f",file.driver=blkdebug"
                          f",file.image.filename={images[name]}"
                          ",file.inject-error.0.event=flush_to_os"
                          ",file.inject-error.0.iotype=flush"
                          ",file.inject-error.0.errno=5")
            else:
                drive += f",file={images[name]}"
            device = "eckd-ccw" if name.startswith("eckd") else "fba-ccw"
            args += ["-drive", drive,
                     "-device", f"{device},drive={name},devno=fe.0.{devno:04x}"]
        args += ["-kernel", guest]

        proc = subprocess.Popen(args)
        try:
            for _ in range(100):
                if os.path.exists(f"{tmp}/qmp"):
                    break
                time.sleep(0.1)
            qmp = Qmp(f"{tmp}/qmp")
            deadline = time.monotonic() + 60
            while True:
                status = qmp.command("query-status")["status"]
                if status in ("shutdown", "guest-panicked"):
                    break
                if time.monotonic() > deadline:
                    status = "timeout"
                    break
                time.sleep(0.1)
            stats = {s["device"]: s["stats"]
                     for s in qmp.command("query-blockstats")}
            qmp.command("quit")
        except BaseException:
            proc.kill()
            raise
        finally:
            if proc.wait(timeout=30):
                print(f"dasd-flush: QEMU exited with {proc.returncode}",
                      file=sys.stderr)
                return 1
        if status != "shutdown":
            print(f"dasd-flush: guest failed ({status})", file=sys.stderr)
            return 1

        failed = False
        for name, expected in EXPECTED.items():
            got = (stats[name]["flush_operations"],
                   stats[name]["failed_flush_operations"])
            if got != expected:
                print(f"dasd-flush: {name}: flush/failed {got}, "
                      f"expected {expected}", file=sys.stderr)
                failed = True
        return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
