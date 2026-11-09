#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Matteo Scalia

"""Independent width/height terminal-resize tests for the native cform CLI.

The test drives cform through a pty and resizes it without sending a keypress.
It checks that the artwork is redrawn (and, for a width-only resize, rescaled)
instead of disappearing.
"""

import fcntl
import os
import pty
import re
import select
import shutil
import struct
import sys
import tempfile
import termios
import time
import zlib


def write_png(path, width, height):
    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type: none
        for x in range(width):
            raw += bytes(
                (
                    (x * 255) // max(1, width - 1),
                    (y * 255) // max(1, height - 1),
                    128,
                )
            )
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(
        b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    )
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


def set_size(fd, columns, rows):
    fcntl.ioctl(
        fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0)
    )


def ansi_blocks(data):
    return re.findall(r"\x1b\[\?2026h(.*?)\x1b\[\?2026l", data, re.S)


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


def run_case(binary, image, schema, initial, resize):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(binary, [binary, "--image", image])

    output = bytearray()
    try:
        set_size(fd, *initial)
        os.write(fd, schema.encode() + b"\n\x04")
        drain(fd, output, 1.0)
        before = len(output)
        before_blocks = ansi_blocks(output.decode("utf-8", "replace"))
        before_colors = before_blocks[-1].count("48;2;") if before_blocks else 0
        set_size(fd, *resize)
        drain(fd, output, 1.0)
        after = len(output)
        data = output.decode("utf-8", "replace")
        blocks = ansi_blocks(data)
        assert after > before, f"no redraw for {initial} -> {resize}"
        assert blocks, "no synchronized frame was rendered"
        last = blocks[-1]
        assert "38;2;" in last or "48;2;" in last, (
            f"artwork disappeared for {initial} -> {resize}"
        )
        return len(last), before_colors, last.count("48;2;")
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
        print(f"resize test: {binary} not found, skipping")
        return 0
    directory = tempfile.mkdtemp(prefix="cform-resize-")
    portrait = os.path.join(directory, "portrait.png")
    landscape = os.path.join(directory, "landscape.png")
    write_png(portrait, 100, 160)
    write_png(landscape, 160, 100)

    short = '{"type":"object","properties":{"a":{"type":"string"}}}'
    long_name = "n" * 35
    long_row = (
        '{"type":"object","properties":{"' + long_name + '":{"type":"string"}}}'
    )

    try:
        checks = []
        # Height-only resize with a portrait image and a row that fits at 24
        # rows but not at 40 rows. This used to hide the artwork.
        checks.append(
            ("height-only, long row", portrait, long_row, (80, 24), (80, 40))
        )
        # Height-only on a short tree: the image must still be re-rendered.
        checks.append(
            ("height-only, short tree", portrait, short, (80, 24), (80, 40))
        )
        # Width-only resize with a landscape image: the image must be rescaled.
        checks.append(
            (
                "width-only, landscape",
                landscape,
                short,
                (80, 24),
                (120, 24),
                True,
            )
        )

        for name, image, schema, initial, resize, *rest in checks:
            size, before_colors, after_colors = run_case(
                binary, image, schema, initial, resize
            )
            if rest and rest[0]:
                assert after_colors > before_colors, (
                    f"artwork was not rescaled for {initial} -> {resize} "
                    f"({before_colors} -> {after_colors} cells)"
                )
            print(
                f"ok - resize: {name} ({size} bytes, "
                f"{before_colors} -> {after_colors} image cells)"
            )
        return 0
    finally:
        shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        print(f"FAIL - resize: {error}")
        sys.exit(1)
