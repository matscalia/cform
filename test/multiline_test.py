#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Matteo Scalia

"""Multiline-description tests for the native cform prompt UI."""

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
    '{"type":"object","required":["name"],"properties":{'
    '"name":{"type":"string",'
    '"description":"first line\\nsecond line\\n\\nfourth line"}}}'
)
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


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
        os.execv(binary, [binary, "--prompt"])
    output = bytearray()
    try:
        set_size(fd)
        os.write(fd, SCHEMA.encode() + b"\n\x04")
        drain(fd, output, 1.0)
        frame = ANSI.sub("", last_frame(output.decode("utf-8", "replace")))
        lines = frame.split("\r\n")
        first = next(
            (index for index, line in enumerate(lines) if "first line" in line),
            None,
        )
        assert first is not None, "first description line is missing"
        assert "second line" in lines[first + 1], (
            "the description line break was not preserved"
        )
        assert lines[first + 2].strip() == "", (
            "the blank description line was not preserved"
        )
        assert "fourth line" in lines[first + 3], (
            "the line after the blank line is missing"
        )
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
        print(f"multiline test: {binary} not found, skipping")
        return 0
    run(binary)
    print("ok - multiline: description line breaks are preserved")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL - multiline: {error}")
        sys.exit(1)
