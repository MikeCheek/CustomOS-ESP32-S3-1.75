/*
 * hal_spritesheet.h
 * Generic, reusable sprite-sheet loader/renderer. This is the
 * foundational piece for ANY sprite-based game on this hardware, not
 * specific to the ninja/dungeon game - a future game built from a
 * different asset pack reuses this exactly as-is, just pointing it
 * at different .spr files.
 *
 * Why a custom .spr format instead of loading PNG directly: this
 * board has no PNG decode pipeline suited to real-time game use (the
 * media app's JPEG viewer is a one-shot, non-real-time case - a very
 * different workload). Sprites get converted OFFLINE (see
 * tools/convert_sprite.py) into a small binary format: an 8-byte
 * header (frame_w, frame_h, frame_count, reserved) followed by raw
 * RGB565 pixel data, one frame after another. The firmware just reads
 * the header, allocates a PSRAM buffer of the right size, and reads
 * the rest straight in - no parsing, no decode, at runtime.
 *
 * Transparency: source pixels that were transparent in the original
 * PNG become a reserved magenta color-key (0xF81F) during conversion.
 * Blitting uses Arduino_GFX's own draw16bitRGBBitmapWithTranColor(),
 * which already treats a chosen key color as "don't draw this pixel" -
 * measured at ~21-25us per 16x16 sprite on this exact display/bus
 * (see the Sprite Test app), so a full room's worth of actors is
 * nowhere near the bottleneck - the display flush itself dominates.
 */
#pragma once
#include <Arduino_GFX_Library.h>

#define SPRITE_TRANS_KEY ((uint16_t)0xF81F)

struct SpriteSheet {
    uint16_t frame_w = 0, frame_h = 0;
    uint16_t frame_count = 0;
    uint16_t *pixels = nullptr; // PSRAM - frame_count * frame_w * frame_h, RGB565
    bool loaded = false;
};

// Loads a .spr file from the SD card into a PSRAM buffer. Safe to call
// on an already-loaded sheet (frees the old buffer first).
bool spritesheet_load(SpriteSheet *sheet, const char *sd_path);
void spritesheet_free(SpriteSheet *sheet);

// Draws one frame (0-based) at (x,y). Out-of-range frame_idx or an
// unloaded sheet is a silent no-op, not a crash - matches this
// project's general tolerance for "asset missing" over the hardware
// gotchas already found this session (a missing music file, an
// unset watchface, etc. all just no-op rather than fault).
void spritesheet_draw(Arduino_GFX *g, const SpriteSheet *sheet, int frame_idx, int x, int y);

// Horizontally-mirrored draw - lets one direction's frames (e.g.
// "walking right") stand in for the opposite facing without needing
// a separate set of sprites, which most 4-direction sheets in this
// pack don't provide a mirror of anyway.
void spritesheet_draw_flipped(Arduino_GFX *g, const SpriteSheet *sheet, int frame_idx, int x, int y);
