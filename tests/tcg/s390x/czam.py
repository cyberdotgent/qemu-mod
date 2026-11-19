#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the czam (v1) host-mirrored LPAR personality under KVM.

The guest prints what it sees through STIDP, STSI and SCLP Read SCP Info.
This wrapper compares that with the host's /proc/sysinfo and memory block
size.  On a host that is not a 3907, the test is skipped (exit 77).
"""

import pathlib
import re
import subprocess
import sys

MEMORY_MB = 8192
SKIP = 77


def ebcdic_hex(text, length):
    return text.ljust(length).encode("cp037").hex()


def host_sysinfo():
    values = {}
    for line in pathlib.Path("/proc/sysinfo").read_text().splitlines():
        key, sep, value = line.partition(":")
        if sep:
            values.setdefault(key.strip(), value.strip())
    return values


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu, guest = sys.argv[1:]

    try:
        host = host_sysinfo()
    except OSError:
        return SKIP
    if host.get("Type") != "3907":
        return SKIP
    block = int(pathlib.Path(
        "/sys/devices/system/memory/block_size_bytes").read_text(), 16)
    serial = host["Sequence Code"][-6:]

    result = subprocess.run([
        qemu,
        "-machine", "czam",
        "-accel", "kvm",
        "-cpu", "host",
        "-m", f"{MEMORY_MB}M",
        "-smp", "2,maxcpus=4",
        "-nodefaults",
        "-display", "none",
        # czam: serial #0 is the line-mode console, #1 the ASCII
        # console that the test guest prints to.
        "-serial", "null",
        "-serial", "stdio",
        "-action", "panic=exit-failure",
        "-kernel", guest,
    ], capture_output=True, text=True, timeout=30)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if result.returncode:
        return result.returncode
    seen = dict(re.findall(r"^([\w-]+)=(\S+)$", result.stdout, re.M))

    expected = {
        "stsi111-cc": "0",
        "stsi111-type": ebcdic_hex("3907", 4),
        "stsi222-cc": "0",
        "stsi222-lpar-number": "1",
        "stsi222-lpar-name": ebcdic_hex("QEMUKVM", 8),
        # total/configured/standby/reserved for -smp 2,maxcpus=4
        "stsi222-cpus": "4/2/0/2",
        "stsi222-dedicated-shared": "2/0",
        "increment-mb": str(block >> 20),
        "increments": str(MEMORY_MB // (block >> 20)),
    }
    failures = [f"{key}: expected {value}, got {seen.get(key)}"
                for key, value in expected.items() if seen.get(key) != value]

    # STIDP: CPU identification (host serial) and machine type 3907.
    stidp = int(seen.get("stidp", "0"), 16)
    if (stidp >> 16) & 0xffff != 0x3907:
        failures.append(f"stidp type: {stidp:016x}")
    if f"{(stidp >> 32) & 0xffffff:06X}" != serial.upper():
        failures.append(f"stidp serial: {stidp:016x}, host {serial}")
    if not seen.get("stsi111-sequence", "").endswith(
            ebcdic_hex(serial.upper(), 6)):
        failures.append("stsi111 sequence does not end in host serial "
                        f"{serial}: {seen.get('stsi111-sequence')}")

    for failure in failures:
        print("FAIL:", failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
