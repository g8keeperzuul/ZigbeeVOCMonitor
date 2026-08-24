#!/usr/bin/env python3
"""Convert the harness's raw PBM into a PNG showing what the panel displays.

The framebuffer is in the panel's native portrait orientation (122 x 250). The
firmware draws through Paint's ROTATE_90, so to see what a viewer sees the
bitmap is rotated back to 250 x 122 here. Nothing but zlib from the stdlib.
"""
import struct
import sys
import zlib

PANEL_W, PANEL_H = 122, 250


def read_pbm(path):
    data = open(path, 'rb').read()
    end_of_dims = data.index(b'\n', data.index(b'\n') + 1) + 1
    bits = data[end_of_dims:]
    stride = (PANEL_W + 7) // 8
    return [[(bits[y * stride + x // 8] >> (7 - (x % 8))) & 1 for x in range(PANEL_W)]
            for y in range(PANEL_H)]


def to_landscape(native):
    """Undo ROTATE_90: native[x][PANEL_W-1-y] -> view[y][x], giving 250 x 122."""
    return [[native[x][PANEL_W - 1 - y] for x in range(PANEL_H)] for y in range(PANEL_W)]


def write_png(path, rows, scale=3):
    """Greyscale PNG. Scaled up because 250 x 122 is tiny on a modern screen."""
    big = [[p for p in row for _ in range(scale)] for row in rows for _ in range(scale)]
    height, width = len(big), len(big[0])
    raw = b''.join(b'\x00' + bytes(0 if p else 255 for p in row) for row in big)

    def chunk(tag, payload):
        body = tag + payload
        return struct.pack('>I', len(payload)) + body + struct.pack('>I', zlib.crc32(body))

    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 9))
        + chunk(b'IEND', b''))


if __name__ == '__main__':
    write_png(sys.argv[2], to_landscape(read_pbm(sys.argv[1])))
    print('wrote', sys.argv[2])
