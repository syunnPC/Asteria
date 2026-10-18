#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate original project cursor art and ACSR v1, using only Python's library.
desktop session personalization. Coordinates/colors come from Source.json.
"""
import argparse
import json
from pathlib import Path
import struct


def render(shapes, size):
    pixels = bytearray(size * size * 4)
    for shape in shapes:
        kind = shape[0]
        color = bytes(shape[1])
        if kind == 'polygon':
            points = shape[2]
            # Sample pixel centers; even/odd crossings give deterministic fill.
            for y in range(size):
                for x in range(size):
                    inside = False
                    prior = points[-1]
                    for point in points:
                        ax, ay = prior
                        bx, by = point
                        if (ay > y +.5) != (by > y +.5):
                            cross = ax + (y +.5 - ay) * (bx - ax) / (by - ay)
                            if x +.5 < cross:
                                inside = not inside
                        prior = point
                    if inside:
                        offset = (y * size + x) * 4
                        pixels[offset:offset + 4] = color
        elif kind == 'rect':
            x0, y0, x1, y1 = shape[2]
            for y in range(max(0, y0), min(size, y1)):
                for x in range(max(0, x0), min(size, x1)):
                    offset = (y * size + x) * 4
                    pixels[offset:offset + 4] = color
        elif kind == 'ring':
            cx, cy, outer, inner = shape[2]
            for y in range(size):
                for x in range(size):
                    radius = (x - cx) ** 2 + (y - cy) ** 2
                    if inner ** 2 <= radius <= outer ** 2:
                        offset = (y * size + x) * 4
                        pixels[offset:offset + 4] = color
        else:
            raise ValueError(kind)
    return pixels


def generate(source):
    assert source['version'] == 1
    roles = source['roles']
    assert len(roles) == 10
    data = bytearray(struct.pack('<4sHHII', b'ACSR', 1, len(roles), 0, 0))
    for role_id, role in enumerate(roles):
        frames = role['frames']
        data += struct.pack('<HH', role_id, len(frames))
        for frame in frames:
            pixels = render(frame['shapes'], source['size'])
            data += struct.pack('<HHHHIII', source['size'], source['size'],
                                *role['hotspot'], frame['delay_ms'], 0, len(pixels))
            data += pixels
    struct.pack_into('<I', data, 8, len(data))
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--check', type=Path)
    args = parser.parse_args()
    data = generate(json.loads(args.source.read_text(encoding='utf-8')))
    if args.check and args.check.read_bytes() != data:
        raise SystemExit('default cursor data differs from its source')
    if args.output:
        args.output.write_bytes(data)


if __name__ == '__main__':
    main()
