#!/usr/bin/env python3
"""Writes a test photo watch face (firmware/app_photo_watchface.h's format)
for emu_host_test: a blue gradient "photo" with a red disc as the subject
right where the time is, white digits. The test checks the disc stays in
front of the time and the digits show around it.

    python3 make_test_photoface.py <out.pwf> [green] [v2]
      green: a green subject; v2: the PWF2 format - the time left of
      centre (x 150) and no date
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

W = H = 466
TIME_Y = 200
CX, CY, R = 233, 200, 60          # the subject


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main(out, green=False, v2=False):
    subject = rgb565(0, 255, 0) if green else rgb565(255, 0, 0)
    photo = bytearray()
    for y in range(H):
        for x in range(W):
            inside = (x - CX) ** 2 + (y - CY) ** 2 <= R * R
            c = subject if inside else rgb565(0, 40 + y * 120 // H, 120 + x * 100 // W)
            photo += struct.pack("<H", c)

    try:
        font = ImageFont.load_default(size=110)
    except TypeError:                    # Pillow < 10.1
        font = ImageFont.load_default()
    chars = "0123456789:"
    boxes = [font.getbbox(c) for c in chars]
    top = min(b[1] for b in boxes)           # cropped to the ink, like the app does
    glyph_h = max(b[3] for b in boxes) - top + 1
    glyphs, widths = bytearray(), []
    for c, b in zip(chars, boxes):
        w = max(1, b[2] + 6)
        im = Image.new("L", (w, glyph_h), 0)
        ImageDraw.Draw(im).text((3, -top), c, fill=255, font=font)
        glyphs += im.tobytes()
        widths.append(w)

    spans, alpha = bytearray(), bytearray()
    count = 0
    for y in range(CY - R, CY + R + 1):
        xs = [x for x in range(CX - R, CX + R + 1) if (x - CX) ** 2 + (y - CY) ** 2 <= R * R]
        if not xs:
            continue
        spans += struct.pack("<HHH", y, xs[0], len(xs))
        alpha += bytes([255]) * len(xs)
        count += 1

    head = (b"PWF2" if v2 else b"PWF1") + struct.pack("<HHHHHHH", W, H, TIME_Y, 0xFFFF, 0xFFFF, 1, glyph_h)
    head += struct.pack("<11H", *widths)
    if v2:
        head += struct.pack("<HH", 150, 1)   # time_x, options bit 0: no date
    assert len(head) == (44 if v2 else 40)
    with open(out, "wb") as f:
        f.write(head + photo + glyphs + struct.pack("<I", count) + spans + alpha)


if __name__ == "__main__":
    main(sys.argv[1], "green" in sys.argv[2:], "v2" in sys.argv[2:])
