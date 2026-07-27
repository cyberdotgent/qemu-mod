#!/usr/bin/env python3
"""Run the disconnected 3270 residual-count guest."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu, guest = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="qemu-3270-residual-") as tmp:
        terminal_path = pathlib.Path(tmp, "terminal.sock")
        return subprocess.run([
            qemu,
            "-machine", "s390-ccw-virtio",
            "-cpu", "max",
            "-nodefaults",
            "-display", "none",
            "-monitor", "none",
            "-action", "panic=exit-failure",
            "-chardev",
            (f"socket,id=tn,path={terminal_path},server=on,"
             "wait=off,tn3270=on"),
            "-device", "x-terminal3270,chardev=tn,devno=fe.0.0301",
            "-kernel", guest,
        ], timeout=10).returncode


if __name__ == "__main__":
    sys.exit(main())
