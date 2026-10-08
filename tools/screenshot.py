#!/usr/bin/env python3
"""Takes a screenshot of a device over the serial console and saves it as PNG.

    python tools/screenshot.py COMx out.png

Needs the development console (CONFIG_HC_DEV_CONSOLE) and pyserial (part of the ESP-IDF Python
environment). The board is not reset: the port is opened with DTR and RTS released.

The device sends run-length encoded RGB565 (pairs of count and pixel) in base64 between
SCREENSHOT-BEGIN and SCREENSHOT-END <width> <height> <bytes>, see components/uidebug.
"""

import base64
import struct
import sys
import time
import zlib

import serial


def read_screenshot(port: str, timeout: float = 60.0) -> tuple[int, int, bytes]:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    with s:
        s.reset_input_buffer()
        s.write(b"screenshot\r\n")
        lines = []
        inside = False
        buffer = b""
        end = time.time() + timeout
        while time.time() < end:
            buffer += s.read(4096)
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                text = line.decode("ascii", errors="replace").strip()
                if text.endswith("SCREENSHOT-BEGIN"):
                    inside = True
                    lines = []
                elif text.startswith("SCREENSHOT-END") and inside:
                    _, width, height, size = text.split()
                    data = base64.b64decode("".join(lines))
                    if len(data) != int(size):
                        raise RuntimeError(f"received {len(data)} of {size} bytes")
                    return int(width), int(height), data
                elif inside:
                    lines.append(text)
        raise RuntimeError("no screenshot received")


def decode(width: int, height: int, data: bytes) -> bytes:
    pixels = bytearray()
    for i in range(0, len(data), 3):
        count = data[i]
        value = data[i + 1] | (data[i + 2] << 8)
        r = (value >> 11) & 0x1F
        g = (value >> 5) & 0x3F
        b = value & 0x1F
        pixels += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * count
    if len(pixels) != width * height * 3:
        raise RuntimeError(f"{len(pixels) // 3} pixels for {width}x{height}")
    return bytes(pixels)


def write_png(path: str, width: int, height: int, rgb: bytes) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main() -> None:
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    started = time.time()
    width, height, data = read_screenshot(sys.argv[1])
    write_png(sys.argv[2], width, height, decode(width, height, data))
    print(f"{sys.argv[2]}: {width}x{height}, {len(data)} bytes encoded, {time.time() - started:.1f} s")


if __name__ == "__main__":
    main()
