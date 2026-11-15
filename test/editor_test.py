#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Matteo Scalia

"""ctrl+g external-editor tests for the native cform prompt UI."""

import fcntl
import os
import pty
import select
import shutil
import struct
import sys
import tempfile
import termios
import time


SCHEMA = (
    '{"type":"object","required":["name"],'
    '"properties":{"name":{"type":"string"}}}'
)
CTRL_G = b"\x07"
KITTY_CTRL_G = b"\x1b[103;5u"
XTERM_CTRL_G = b"\x1b[27;5;103~"


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


def run(binary, editor, key):
    pid, fd = pty.fork()
    if pid == 0:
        environment = dict(os.environ)
        environment["EDITOR"] = editor
        environment["VISUAL"] = editor
        os.execve(binary, [binary, "--prompt"], environment)
    output = bytearray()
    try:
        set_size(fd)
        os.write(fd, SCHEMA.encode() + b"\n\x04")
        drain(fd, output, 0.8)
        os.write(fd, b"seed")
        drain(fd, output, 0.3)
        os.write(fd, key)
        drain(fd, output, 1.2)
        os.write(fd, b"\r")
        drain(fd, output, 0.4)
        os.write(fd, b"q")
        drain(fd, output, 0.6)
        text = output.decode("utf-8", "replace")
        assert '"name": "seed plus\\nsecond line"' in text, (
            "the edited multiline value was not accepted"
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
        print(f"editor test: {binary} not found, skipping")
        return 0

    directory = tempfile.mkdtemp(prefix="cform-editor-test-")
    try:
        editor = os.path.join(directory, "editor.sh")
        with open(editor, "w", encoding="utf-8") as script:
            script.write(
                "#!/bin/sh\n"
                "if [ \"$(cat \"$1\")\" != \"seed\" ]; then\n"
                "  printf 'wrong initial value' > \"$1\"\n"
                "  exit 1\n"
                "fi\n"
                "printf 'seed plus\\nsecond line\\n' > \"$1\"\n"
            )
        os.chmod(editor, 0o755)

        run(binary, editor, CTRL_G)
        run(binary, editor, KITTY_CTRL_G)
        run(binary, editor, XTERM_CTRL_G)
    finally:
        shutil.rmtree(directory, ignore_errors=True)

    print("ok - editor: ctrl+g opens the editor for the current value")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL - editor: {error}")
        sys.exit(1)
