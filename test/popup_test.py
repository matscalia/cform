#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Matteo Scalia

"""Description-popup tests for the native cform tree."""

import fcntl
import os
import pty
import re
import select
import struct
import sys
import termios
import time


SCHEMA = (
    '{"type":"object","properties":{'
    '"a":{"type":"string","description":"alpha description"},'
    '"b":{"type":"string","description":"beta description"}},'
    '"required":["a","b"]}'
)


def set_size(fd, columns=80, rows=24):
    fcntl.ioctl(
        fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0)
    )


def drain(fd, output, seconds):
    end = time.time() + seconds
    while time.time() < end:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return
        if not chunk:
            return
        output.extend(chunk)


def last_frame(data):
    blocks = re.findall(r"\x1b\[\?2026h(.*?)\x1b\[\?2026l", data, re.S)
    return blocks[-1] if blocks else ""


def run(binary):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(binary, [binary])
    output = bytearray()
    try:
        set_size(fd)
        os.write(fd, SCHEMA.encode() + b"\n\x04")
        drain(fd, output, 1.0)

        os.write(fd, b"j")  # root -> a
        drain(fd, output, 0.3)
        os.write(fd, b" ")  # show a's description
        drain(fd, output, 0.6)
        shown = last_frame(output.decode("utf-8", "replace"))
        assert "┌" in shown and "alpha description" in shown, (
            "space did not show the field description popup"
        )

        os.write(fd, b" ")  # hide popup
        drain(fd, output, 0.4)
        hidden = last_frame(output.decode("utf-8", "replace"))
        assert "┌" not in hidden, "space did not close the popup"

        os.write(fd, b"j")  # a -> b
        drain(fd, output, 0.3)
        os.write(fd, b" ")  # show b's description
        drain(fd, output, 0.6)
        moved = last_frame(output.decode("utf-8", "replace"))
        assert "┌" in moved and "beta description" in moved, (
            "popup did not follow the selected row"
        )
        os.write(fd, b"q")
        drain(fd, output, 0.4)
        return 0
    finally:
        try:
            os.write(fd, b"\x03")
            drain(fd, output, 0.1)
        except OSError:
            pass
        try:
            os.close(fd)
        except OSError:
            pass
        os.waitpid(pid, 0)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/cform"
    if not os.path.exists(binary):
        print(f"popup test: {binary} not found, skipping")
        return 0
    run(binary)
    print("ok - popup: description toggles and follows the cursor")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL - popup: {error}")
        sys.exit(1)
