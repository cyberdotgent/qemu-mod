#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the zero-data-length ECKD record status test."""

import pathlib
import struct
import subprocess
import sys
import tempfile


HEADS = 15
TRACK_SIZE = 56832
HEADER_SIZE = 512


def create_image(path):
    image = bytearray(HEADER_SIZE + HEADS * TRACK_SIZE)
    image[0:8] = b"CKD_P064"
    struct.pack_into("<II", image, 8, HEADS, TRACK_SIZE)
    image[16] = 0x90
    image[20:32] = b"123456789012"

    for head in range(HEADS):
        offset = HEADER_SIZE + head * TRACK_SIZE
        struct.pack_into(">BHH", image, offset, 0, 0, head)
        end = offset + 5
        if head == 0:
            # CCHHR 0000/0000/00, key length 0, data length 0.
            image[end:end + 8] = bytes(8)
            end += 8
        image[end:end + 8] = bytes((0xff,)) * 8
    path.write_bytes(image)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu, guest = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="qemu-eckd-zero-") as tmp:
        image = pathlib.Path(tmp, "zero.ckd64")
        create_image(image)
        result = subprocess.run([
            qemu,
            "-machine", "s390-ccw-virtio",
            "-cpu", "max",
            "-nodefaults",
            "-display", "none",
            "-monitor", "none",
            "-action", "panic=exit-failure",
            "-dev3390", f"file={image},format=raw,devno=300",
            "-kernel", guest,
        ], timeout=30)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
