#!/usr/bin/env python3
"""Turns the simulator's raw RGB565 frames into round PNGs (transparent
outside the 466 px circle, like the real panel)."""
import sys, glob, os
from PIL import Image, ImageDraw

W = H = 466
SS = 4  # supersampling for a smooth circle edge

def convert(src, dst):
    raw = open(src, 'rb').read()
    px = bytearray(W * H * 3)
    for i in range(W * H):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[3 * i] = (r * 527 + 23) >> 6
        px[3 * i + 1] = (g * 259 + 33) >> 6
        px[3 * i + 2] = (b * 527 + 23) >> 6
    img = Image.frombytes('RGB', (W, H), bytes(px)).convert('RGBA')
    mask = Image.new('L', (W * SS, H * SS), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, W * SS - 1, H * SS - 1), fill=255)
    img.putalpha(mask.resize((W, H), Image.LANCZOS))
    img.save(dst, optimize=True)

# README banner: a few screens side by side on a transparent strip.
HERO = ['watchface', 'notifications', 'quick_panel', 'music', 'navigation']

def hero(dst_dir):
    shots = [os.path.join(dst_dir, n + '.png') for n in HERO]
    if not all(os.path.exists(f) for f in shots):
        return
    size, gap = 300, 24
    strip = Image.new('RGBA', (len(shots) * size + (len(shots) - 1) * gap, size), (0, 0, 0, 0))
    for i, f in enumerate(shots):
        im = Image.open(f).resize((size, size), Image.LANCZOS)
        strip.alpha_composite(im, (i * (size + gap), 0))
    strip.save(os.path.join(dst_dir, 'hero.png'), optimize=True)

if __name__ == '__main__':
    src_dir, dst_dir = sys.argv[1], sys.argv[2]
    os.makedirs(dst_dir, exist_ok=True)
    for f in sorted(glob.glob(os.path.join(src_dir, '*.rgb565'))):
        name = os.path.splitext(os.path.basename(f))[0]
        convert(f, os.path.join(dst_dir, name + '.png'))
        print(name)
    hero(dst_dir)
