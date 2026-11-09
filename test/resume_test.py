#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Matteo Scalia

"""Exact ctrl+o prompt-resume tests for the native cform CLI."""

import fcntl
import os
import pty
import select
import struct
import sys
import termios
import time


SCHEMA = (
    '{"type":"object","properties":'
    '{"a":{"type":"string"},"b":{"type":"string"}},"required":["a","b"]}'
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


def run(binary, script):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(binary, [binary])
    output = bytearray()
    try:
        set_size(fd)
        os.write(fd, SCHEMA.encode() + b"\n\x04")
        drain(fd, output, 1.0)
        for delay, data in script:
            time.sleep(delay)
            os.write(fd, data)
        drain(fd, output, 1.0)
        return output.decode("utf-8", "replace")
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
        print(f"resume test: {binary} not found, skipping")
        return 0

    # Interrupt while typing "hello", then resume and append "!" to prove the
    # exact typed state was restored.
    text = run(
        binary,
        [
            (0.0, b"\r"),        # root wizard
            (0.4, b"hello"),     # typed but not accepted
            (0.3, b"\x06"),      # ctrl+f -> tree
            (0.5, b"\x0f"),      # ctrl+o -> exact prompt
            (0.6, b"!"),         # append to the restored "hello"
            (0.3, b"\r"),        # accept "hello!"
            (0.4, b"world\r"),   # second field
            (0.5, b"q"),         # finish tree
        ],
    )
    assert '"a": "hello!"' in text, "ctrl+f/ctrl+o did not restore the typed text"
    assert '"b": "world"' in text, "second field was not completed"
    print("ok - resume: exact typed prompt state")

    # Ctrl+o after a completed wizard reopens the last prompt just before its
    # accepting key, preserving its value.
    text = run(
        binary,
        [
            (0.0, b"\r"),
            (0.4, b"hello\r"),
            (0.4, b"world\r"),
            (0.6, b"\x0f"),      # ctrl+o from the tree
            (0.6, b"!"),         # append to the restored "world"
            (0.3, b"\r"),
            (0.5, b"q"),
        ],
    )
    assert '"a": "hello"' in text, "first field changed unexpectedly"
    assert '"b": "world!"' in text, "completed last prompt was not restored"
    print("ok - resume: completed last prompt")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL - resume: {error}")
        sys.exit(1)
