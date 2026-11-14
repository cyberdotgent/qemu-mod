#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the explicit ECKD topology test under KVM."""

import pathlib
import struct
import subprocess
import sys
import tempfile


def create_image(path):
    heads = 15
    cylinders = 1
    track_size = 56832
    image = bytearray(512 + cylinders * heads * track_size)
    image[0:8] = b"CKD_P064"
    struct.pack_into("<II", image, 8, heads, track_size)
    image[16] = 0x90
    image[20:32] = b"123456789012"
    for head in range(heads):
        offset = 512 + head * track_size
        struct.pack_into(">BHH", image, offset, 0, 0, head)
        image[offset + 5:offset + 13] = b"\xff" * 8
    path.write_bytes(image)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu, guest = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="qemu-eckd-topology-") as tmp:
        image = pathlib.Path(tmp, "topology.ckd64")
        create_image(image)
        result = subprocess.run([
            qemu, "-machine", "s390-ccw-virtio", "-accel", "kvm",
            "-cpu", "host", "-nodefaults", "-display", "none",
            "-monitor", "none", "-action", "panic=exit-failure",
            "-dev3390",
            f"file={image},format=raw,devno=0A85,unit-address=5,"
            "unit-address-limit=0x24,subsystem-id=3,interface-id=0,"
            "lcu-queue-number=1,"
            "path-cu-number=2,fla=0x1234",
            "-kernel", guest,
        ], timeout=30)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
