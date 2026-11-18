#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the persistent 3270 guest under KVM, its TN3270 clients coming and going."""

import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


IAC = 0xFF
EOR = 0xEF
AID_CLEAR = 0x6D
AID_ENTER = 0x7D
TN3270_OFFER = bytes((
    IAC, 0xFD, 0x19, IAC, 0xFB, 0x19,
    IAC, 0xFD, 0x00, IAC, 0xFB, 0x00,
    IAC, 0xFD, 0x18,
    IAC, 0xFA, 0x18, 0x01, IAC, 0xF0,
))
TN3270_ANSWER = bytes((
    IAC, 0xFB, 0x19, IAC, 0xFD, 0x19,
    IAC, 0xFB, 0x00, IAC, 0xFD, 0x00,
    IAC, 0xFB, 0x18,
    IAC, 0xFA, 0x18, 0x00,
)) + b"IBM-3278-2-E" + bytes((IAC, 0xF0))


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("unexpected TN3270 disconnect")
        data.extend(chunk)
    return bytes(data)


def recv_record(sock):
    data = bytearray()
    while len(data) < 65537:
        data.extend(recv_exact(sock, 1))
        if data[-2:] == bytes((IAC, EOR)):
            return bytes(data[:-2])
    raise RuntimeError("oversized TN3270 record")


def expect_read_modified(sock, which):
    request = recv_record(sock)
    if request != bytes((0xF6,)):
        raise RuntimeError(
            f"expected {which} READ MODIFIED, received {request.hex()}"
        )


def connect_unix(path):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            sock.settimeout(10)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            time.sleep(0.01)
    raise RuntimeError(f"timed out connecting to {path}")


def qmp_command(stream, command):
    stream.write(json.dumps({"execute": command}).encode() + b"\n")
    stream.flush()
    while True:
        reply = json.loads(stream.readline())
        if "return" in reply:
            return
        if "error" in reply:
            raise RuntimeError(f"QMP {command} failed: {reply['error']}")


def attach(path):
    """Connect and negotiate a client; return the socket."""
    terminal = connect_unix(path)
    if recv_exact(terminal, len(TN3270_OFFER)) != TN3270_OFFER:
        raise RuntimeError("unexpected TN3270 negotiation offer")
    terminal.sendall(TN3270_ANSWER)
    return terminal


def expect_screen(terminal, records):
    """The replayed screen: records (command, trailing text) in order."""
    for command, text in records:
        record = recv_record(terminal)
        if record[0] != command or not record.endswith(text):
            raise RuntimeError(f"expected replayed {command:02x} record "
                               f"ending {text.hex()}, received {record.hex()}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} QEMU GUEST")
    qemu, guest = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="qemu-3270-detach-") as tmp:
        terminal_path = str(pathlib.Path(tmp, "terminal.sock"))
        qmp_path = str(pathlib.Path(tmp, "qmp.sock"))
        proc = subprocess.Popen([
            qemu,
            "-machine", "s390-ccw-virtio",
            "-accel", "kvm",
            "-cpu", "host",
            "-nodefaults",
            "-display", "none",
            "-monitor", "none",
            "-action", "panic=exit-failure",
            "-chardev", (f"socket,id=tn,path={terminal_path},server=on,"
                         "wait=off,tn3270=on"),
            "-device", "x-terminal3270,chardev=tn,devno=fe.0.0301",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
            "-S",
            "-kernel", guest,
        ])
        screen = [(0xF5, bytes((0xC1, 0xC1, 0xC1)))]
        try:
            terminal = attach(terminal_path)
            with connect_unix(qmp_path) as qmp:
                stream = qmp.makefile("rwb", buffering=0)
                if "QMP" not in json.loads(stream.readline()):
                    raise RuntimeError("missing QMP greeting")
                qmp_command(stream, "qmp_capabilities")
                recv_record(terminal)  # QEMU connection banner
                qmp_command(stream, "cont")

            # First client: the screen and the read, then it goes away.
            expect_screen(terminal, screen)
            expect_read_modified(terminal, "first")
            terminal.close()
            time.sleep(0.5)

            # Second client: the screen again (no banner) and the read.
            terminal = attach(terminal_path)
            expect_screen(terminal, screen)
            expect_read_modified(terminal, "replayed")
            terminal.sendall(bytes((AID_ENTER, 0x40, 0x40, IAC, EOR)))
            terminal.close()
            time.sleep(0.5)

            # Third client: the screen including the write it missed.
            screen.append((0xF1, bytes((0xC2, 0xC2, 0xC2))))
            terminal = attach(terminal_path)
            expect_screen(terminal, screen)
            expect_read_modified(terminal, "second")
            terminal.sendall(bytes((AID_ENTER, 0x40, 0x40, IAC, EOR)))
            rc = proc.wait(timeout=10)
            terminal.close()
            return rc
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    sys.exit(main())
