#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native channel-program IPL from an emulated 3390 under KVM.

Builds a CKD volume whose IPL1 record (cylinder 0, head 0, record 1) holds
an ESA/390 IPL PSW and a CCW reading record 2 to 0x1000.  Record 2 is a
small program that checks the architected IPL state (subchannel
identification at absolute 184, GPR 15 cleared because no firmware ran)
and loads the success or failure wait PSW.  QEMU is started with
`-ipl 0A80`, so no firmware is loaded.  The guest binary argument is not
used: the program under test is on the volume.
"""

import pathlib
import struct
import subprocess
import sys
import tempfile

HEADS = 15
CYLINDERS = 1
TRACK_SIZE = 56832
HEADER_SIZE = 512

IPL_PSW = bytes.fromhex("0008000080001000")     # ESA/390, 31-bit, 0x1000
READ_PROGRAM = bytes.fromhex("0600100020000100")  # Read Data -> 0x1000, SLI

PROGRAM = bytes.fromhex(
    "0dc0"          # 1000 BASR  12,0          r12 = 1002
    "581000b8"      # 1002 L     1,184         subchannel identification
    "5510c07e"      # 1006 CL    1,X'7E'(12)   == expected word at 1080?
    "a774000a"      # 100a JNE   101e
    "b90200ff"      # 100e LTGR  15,15         no firmware residue
    "a7740006"      # 1012 JNE   101e
    "b2b2c08e"      # 1016 LPSWE X'8E'(12)     success PSW at 1090
    "07070707"      # 101a padding
    "b2b2c09e"      # 101e LPSWE X'9E'(12)     failure PSW at 10a0
)
EXPECTED_SCHID = bytes.fromhex("00010000")
SUCCESS_PSW = bytes.fromhex("0002000180000000" "0000000000000fff")
FAILURE_PSW = bytes.fromhex("0002000180000000" "0000000000000000")


def program_record():
    data = bytearray(256)
    data[0:len(PROGRAM)] = PROGRAM
    data[0x80:0x84] = EXPECTED_SCHID
    data[0x90:0xa0] = SUCCESS_PSW
    data[0xa0:0xb0] = FAILURE_PSW
    return bytes(data)


def create_image(path):
    image = bytearray(HEADER_SIZE + CYLINDERS * HEADS * TRACK_SIZE)
    image[0:8] = b"CKD_P064"
    struct.pack_into("<II", image, 8, HEADS, TRACK_SIZE)
    image[16] = 0x90
    image[20:32] = b"123456789012"
    records = [
        (0, b""),
        (1, IPL_PSW + READ_PROGRAM + bytes(8)),
        (2, program_record()),
    ]
    for head in range(HEADS):
        offset = HEADER_SIZE + head * TRACK_SIZE
        struct.pack_into(">BHH", image, offset, 0, 0, head)
        end = offset + 5
        if head == 0:
            for number, data in records:
                struct.pack_into(">HHBBH", image, end, 0, 0, number, 0,
                                 len(data))
                end += 8
                image[end:end + len(data)] = data
                end += len(data)
        image[end:end + 8] = b"\xff" * 8
    path.write_bytes(image)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu = sys.argv[1]

    with tempfile.TemporaryDirectory(prefix="qemu-native-ipl-") as tmp:
        image = pathlib.Path(tmp, "ipl.ckd64")
        create_image(image)
        result = subprocess.run([
            qemu,
            "-machine", "s390-ccw-virtio",
            "-accel", "kvm",
            "-cpu", "host",
            "-nodefaults",
            "-display", "none",
            "-monitor", "none",
            "-action", "panic=exit-failure",
            "-dev3390", f"file={image},format=raw,devno=0A80",
            "-ipl", "0A80",
        ], timeout=30)
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
