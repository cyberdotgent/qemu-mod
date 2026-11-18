#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the blank CKD volume test and check the written track slot."""

import os
import pathlib
import struct
import subprocess
import sys
import tempfile


HEADS = 15
TRACK_SIZE = 56832
HEADER_SIZE = 512
CYLINDER_SIZE = HEADS * TRACK_SIZE
BLANK_CYLINDERS = 2
# Trailing slack after the volume: not a multiple of a track or 512 bytes.
SLACK = 4 * 1024 * 1024 + 123
TEST_CYL, TEST_HEAD = 1, 14


def header(high_cylinder):
    image = bytearray(HEADER_SIZE)
    image[0:8] = b"CKD_P064"
    struct.pack_into("<II", image, 8, HEADS, TRACK_SIZE)
    image[16] = 0x90
    struct.pack_into("<H", image, 18, high_cylinder)
    image[20:32] = b"123456789012"
    return image


def create_blank(path):
    """Header only; every track slot and the slack stay sparse zeros."""
    with open(path, "wb") as f:
        f.write(header(BLANK_CYLINDERS))
        f.truncate(HEADER_SIZE + BLANK_CYLINDERS * CYLINDER_SIZE + SLACK)


def create_garbage(path):
    """Legacy one-cylinder image (high_cylinder 0) with a garbage track 0/1."""
    with open(path, "wb") as f:
        f.write(header(0))
        f.seek(HEADER_SIZE + TRACK_SIZE)
        f.write(bytes(range(1, 65)))
        f.truncate(HEADER_SIZE + CYLINDER_SIZE)


def slot(image, cylinder, head):
    offset = HEADER_SIZE + (cylinder * HEADS + head) * TRACK_SIZE
    return image[offset:offset + TRACK_SIZE]


def check_blank(path):
    image = path.read_bytes()
    expected_size = HEADER_SIZE + BLANK_CYLINDERS * CYLINDER_SIZE + SLACK
    if len(image) != expected_size:
        return f"image size changed: {len(image)} != {expected_size}"
    if image[:HEADER_SIZE] != header(BLANK_CYLINDERS):
        return "header changed"

    track = bytearray(TRACK_SIZE)
    struct.pack_into(">BHH", track, 0, 0, TEST_CYL, TEST_HEAD)
    struct.pack_into(">HHBBH", track, 5, TEST_CYL, TEST_HEAD, 0, 0, 8)
    struct.pack_into(">HHBBH", track, 21, TEST_CYL, TEST_HEAD, 1, 0, 16)
    track[29:45] = b"QEMU-ECKD-BLANK!"
    track[45:53] = b"\xff" * 8
    if slot(image, TEST_CYL, TEST_HEAD) != track:
        return "written track slot differs"

    for cylinder in range(BLANK_CYLINDERS):
        for head in range(HEADS):
            if (cylinder, head) == (TEST_CYL, TEST_HEAD):
                continue
            if any(slot(image, cylinder, head)):
                return f"track {cylinder}/{head} was written"
    if any(image[HEADER_SIZE + BLANK_CYLINDERS * CYLINDER_SIZE:]):
        return "slack was written"
    return None


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu, guest = sys.argv[1:]
    accel = os.environ.get("ECKD_TEST_ACCEL", "tcg")

    with tempfile.TemporaryDirectory(prefix="qemu-eckd-blank-") as tmp:
        blank = pathlib.Path(tmp, "blank.ckd64")
        garbage = pathlib.Path(tmp, "garbage.ckd64")
        create_blank(blank)
        create_garbage(garbage)
        result = subprocess.run([
            qemu,
            "-machine", "s390-ccw-virtio",
            "-accel", accel,
            "-nodefaults",
            "-display", "none",
            "-monitor", "none",
            "-action", "panic=exit-failure",
            "-dev3390", f"file={blank},format=raw,devno=0200",
            "-dev3390", f"file={garbage},format=raw,devno=0201",
            "-kernel", guest,
        ], timeout=60)
        if result.returncode:
            return result.returncode
        error = check_blank(blank)
        if error:
            print(f"eckd-blank: {error}", file=sys.stderr)
            return 1
        return 0


if __name__ == "__main__":
    sys.exit(main())
