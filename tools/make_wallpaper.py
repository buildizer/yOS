#!/usr/bin/env python3
"""Convert a PNG/JPG into yOS wallpaper files.

    python3 tools/make_wallpaper.py photo.png [-o outdir] [-n name]

Writes <name>.rgb565 (480x480 RGB565 little-endian, 460800 bytes) and <name>.thumb
(96x96, 18432 bytes). The image is centre-cropped to a square, resized, then ordered
(Bayer 4x4) dithered to 5/6/5 bits exactly like renderWallpaper() in src/os/os.cpp.
Upload both files at http://<device>/wallpapers, or copy them to /wallpapers on the SD card.

Needs Pillow:  pip install pillow
"""
import argparse
import os
import re
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def to_rgb565(img, size):
    s = min(img.size)
    left, top = (img.width - s) // 2, (img.height - s) // 2
    img = img.crop((left, top, left + s, top + s)).resize((size, size), Image.LANCZOS)
    px = img.convert("RGB").load()
    out = bytearray(size * size * 2)
    i = 0
    for y in range(size):
        row = BAYER[y & 3]
        for x in range(size):
            r, g, b = px[x, y]
            d = (row[x & 3] + 0.5) / 16.0  # dither before truncating to 5/6/5 bits
            c = (min(31, int(r / 8 + d)) << 11) | (min(63, int(g / 4 + d)) << 5) | min(31, int(b / 8 + d))
            struct.pack_into("<H", out, i, c)
            i += 2
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("-o", "--outdir", default=".")
    ap.add_argument("-n", "--name", help="wallpaper name (letters, digits, _ and -); default: from the file name")
    a = ap.parse_args()
    name = a.name or re.sub(r"[^A-Za-z0-9_-]+", "_", os.path.splitext(os.path.basename(a.image))[0]).strip("_")[:32]
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", name or ""):
        sys.exit("Bad name; use -n with letters, digits, _ and - (max 32)")
    img = Image.open(a.image)
    img.load()
    os.makedirs(a.outdir, exist_ok=True)
    for ext, size in (("rgb565", 480), ("thumb", 96)):
        path = os.path.join(a.outdir, f"{name}.{ext}")
        data = to_rgb565(img, size)
        assert len(data) == size * size * 2
        with open(path, "wb") as f:
            f.write(data)
        print(f"{path}  {len(data)} bytes")


if __name__ == "__main__":
    main()
