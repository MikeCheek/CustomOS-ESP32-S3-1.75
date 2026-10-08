#!/usr/bin/env python3
"""Turns the simulator's raw RGB565 frames into round PNGs (transparent
outside the 466 px circle, like the real panel)."""
import sys, glob, os, re
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

def load(src):
    raw = open(src, 'rb').read()
    px = bytearray(W * H * 3)
    for i in range(W * H):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        px[3 * i] = (((v >> 11) & 0x1F) * 527 + 23) >> 6
        px[3 * i + 1] = (((v >> 5) & 0x3F) * 259 + 33) >> 6
        px[3 * i + 2] = ((v & 0x1F) * 527 + 23) >> 6
    return Image.frombytes('RGB', (W, H), bytes(px))

# Animations: <name>_000, <name>_001... become <name>.gif (240 px, round).
GIF_SIZE = 240

def gif(name, frames, dst_dir, step_ms):
    mask = Image.new('L', (GIF_SIZE * SS, GIF_SIZE * SS), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, GIF_SIZE * SS - 1, GIF_SIZE * SS - 1), fill=255)
    mask = mask.resize((GIF_SIZE, GIF_SIZE), Image.LANCZOS).point(lambda a: 255 if a > 127 else 0)
    out = []
    for f in frames:
        im = load(f).resize((GIF_SIZE, GIF_SIZE), Image.LANCZOS)
        bg = Image.new('RGB', (GIF_SIZE, GIF_SIZE), (255, 0, 255))
        bg.paste(im, (0, 0), mask)
        p = bg.convert('P', palette=Image.ADAPTIVE, colors=255)
        # the key colour becomes the transparent index
        key = p.getpixel((0, 0))
        p.info['transparency'] = key
        out.append(p)
    out[0].save(os.path.join(dst_dir, name + '.gif'), save_all=True, append_images=out[1:],
                duration=step_ms, loop=0, disposal=2, transparency=out[0].info['transparency'], optimize=False)

STEP_MS = {'anim_boot': 40, 'anim_lock': 30, 'anim_unlock': 30, 'anim_music': 50, 'anim_charging': 40}

if __name__ == '__main__':
    src_dir, dst_dir = sys.argv[1], sys.argv[2]
    os.makedirs(dst_dir, exist_ok=True)
    groups = {}
    for f in sorted(glob.glob(os.path.join(src_dir, '*.rgb565'))):
        name = os.path.splitext(os.path.basename(f))[0]
        m = re.match(r'^(.*)_(\d{3})$', name)
        if m:
            groups.setdefault(m.group(1), []).append(f)
            continue
        convert(f, os.path.join(dst_dir, name + '.png'))
        print(name)
    for name, frames in sorted(groups.items()):
        gif(name, frames, dst_dir, STEP_MS.get(name, 40))
        print(name + '.gif')
    hero(dst_dir)
