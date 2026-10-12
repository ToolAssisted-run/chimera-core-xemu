#!/usr/bin/env python3
"""Are two screenshots the same picture, drawn by two renderers?

    check-same-picture.py A.tga B.tga [most-different-pixels-per-thousand]

Two renderers do not round every edge alike, so a few pixels along the edges
of what is drawn may differ; a picture that is wrong differs in far more. A
pixel counts as different when any of its colours is more than 8 levels (of
255) away. Exit 0 when at most the given share differs (default 5 per
thousand), 1 when more does or the sizes differ, 2 on a file that is missing
or is not an uncompressed 32-bit TGA - which is what the harness writes."""
import struct
import sys


def read(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        # 2, not 1: a picture that is not there is not "a different picture"
        print(f"{path}: {e.strerror}")
        sys.exit(2)
    if len(data) < 18 or data[2] != 2 or data[16] != 32:
        print(f"{path}: not an uncompressed 32-bit TGA")
        sys.exit(2)
    width, height = struct.unpack_from("<HH", data, 12)
    start = 18 + data[0]
    pixels = data[start:start + width * height * 4]
    if len(pixels) != width * height * 4:
        print(f"{path}: {len(pixels)} bytes of pixels for {width}x{height}")
        sys.exit(2)
    return width, height, pixels


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    limit = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
    wa, ha, a = read(sys.argv[1])
    wb, hb, b = read(sys.argv[2])
    if (wa, ha) != (wb, hb):
        print(f"the pictures are {wa}x{ha} and {wb}x{hb}")
        return 1
    lit = sum(1 for i in range(0, len(a), 4) if a[i] > 8 or a[i + 1] > 8 or a[i + 2] > 8)
    different = 0
    for i in range(0, len(a), 4):
        if abs(a[i] - b[i]) > 8 or abs(a[i + 1] - b[i + 1]) > 8 or abs(a[i + 2] - b[i + 2]) > 8:
            different += 1
    share = 1000.0 * different / (wa * ha)
    print(f"{different} of {wa * ha} pixels differ ({share:.2f} per thousand; {lit} are not black)")
    if lit == 0:
        print("the first picture is all black: nothing was compared")
        return 1
    return 0 if share <= limit else 1


if __name__ == "__main__":
    sys.exit(main())
