#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the tape-write guest and verify its final AWS serialization."""

import pathlib
import struct
import subprocess
import sys
import tempfile


def parse_aws(path):
    data = pathlib.Path(path).read_bytes()
    offset = 0
    previous = 0
    records = []
    current = bytearray()
    while offset < len(data):
        if offset + 6 > len(data):
            raise RuntimeError("truncated AWS header")
        length, back, flags, reserved = struct.unpack_from("<HHBB", data,
                                                           offset)
        if back != previous or reserved:
            raise RuntimeError("invalid AWS backward link")
        offset += 6
        chunk = data[offset:offset + length]
        if len(chunk) != length:
            raise RuntimeError("truncated AWS chunk")
        offset += length
        if flags & 0x40:
            if length or current:
                raise RuntimeError("invalid AWS tape mark")
            records.append(None)
        else:
            if bool(flags & 0x80) != (not current):
                raise RuntimeError("invalid AWS record start")
            current.extend(chunk)
            if flags & 0x20:
                records.append(bytes(current))
                current.clear()
        previous = length
    if current:
        raise RuntimeError("AWS image ends within a record")
    return records


def run_guest(qemu, guest, image, readonly):
    result = subprocess.run([
        qemu,
        "-machine", "s390-ccw-virtio",
        "-cpu", "max",
        "-nodefaults",
        "-display", "none",
        "-monitor", "none",
        "-action", "panic=exit-failure",
        "-dev3590", f"file={image},readonly={readonly},ident=3490,devno=580",
        "-kernel", guest,
    ], timeout=30)
    return result.returncode


def main():
    args = sys.argv[1:]
    protect = False
    if args and args[0] == "--protect":
        protect = True
        args = args[1:]
    if len(args) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} [--protect] QEMU GUEST")
    qemu, guest = args
    expected = [b"first-record", b"replacement", None, b"final-record"]

    with tempfile.TemporaryDirectory(prefix="qemu-tape-write-") as tmp:
        image = pathlib.Path(tmp, "write.aws")
        image.touch()
        if protect:
            # The guest only checks that writes to a read-only tape are
            # rejected with the expected unit check, so an empty image
            # is sufficient.
            return run_guest(qemu, guest, image, "on")

        ret = run_guest(qemu, guest, image, "off")
        if ret:
            return ret
        actual = parse_aws(image)
        if actual != expected:
            raise RuntimeError(f"unexpected AWS contents: {actual!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
