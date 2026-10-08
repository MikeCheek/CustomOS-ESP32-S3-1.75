/*
 * fx3d.h
 * Small 3D effects shared by the boot / lock / unlock animations
 * (anim_lock.cpp) and the music players' visualizer.
 *
 * Everything is software: a perspective camera (focal length FX_FOCAL px
 * looking down -z at the screen centre), drawn with Arduino_GFX
 * primitives or written straight into the RGB565 frame.
 */
#pragma once
#include <stdint.h>

struct Arduino_GFX;

#define FX_FOCAL 520.0f

// RGB565 colour scaled by k/32 (k = 0..32) - three fields in one multiply.
static inline uint16_t fx_scale565(uint16_t c, uint32_t k) {
    uint32_t x = (c | ((uint32_t)c << 16)) & 0x07E0F81Fu;
    x = ((x * k) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(x | (x >> 16));
}
uint16_t fx_lerp565(uint16_t a, uint16_t b, float t);

// Easing
float fx_smooth(float t);         // smoothstep
float fx_ease_out_back(float t);  // overshoots a little, then settles
float fx_ease_in_cubic(float t);

// ---- Card: a whole frame as a flat plane in 3D -------------------------------
// Draws `src` (a full LCD_WIDTH x LCD_HEIGHT frame) into `dst` as a card
// tilted `pitch` radians about the horizontal axis (positive = top edge
// away from the viewer), scaled by `scale`, pushed `z` px into the screen,
// at brightness 0..1 (rows further away are a little darker). Every row of
// `dst` is written (black where the card isn't). Row by row, so it's cheap:
// each output row maps to one source row with a constant horizontal step.
// `squash` (0..1] flattens the card vertically about its centre (the CRT
// line in the lock/unlock animations).
void fx_card(uint16_t *dst, const uint16_t *src, float pitch, float z, float scale, float bright,
             float squash = 1.0f);

// The rim of the round screen on that same card, as a glowing outline -
// makes the tilt readable on a mostly black screen. Same pose arguments.
void fx_card_rim(Arduino_GFX *g, float pitch, float z, float scale, float squash, uint16_t color, float bright);

// ---- Beat: a level/beat signal for visualizers ------------------------------------
// Fed the real output level (0-100) when the watch plays the audio, or -1
// when it can't hear it (music playing on the phone) - then it makes up a
// steady 120 BPM pulse so the visual still moves with "a" rhythm.
struct FxBeat {
    float bands[8];   // 0..1, low to high
    float energy;     // 0..1 overall
    float kick;       // 0..1, jumps on a beat and decays
    float avg;        // running average level (beat detection)
    uint32_t last_ms, last_beat_ms;
};
void fx_beat_reset(FxBeat &b);
void fx_beat_update(FxBeat &b, int level, bool playing, uint32_t now_ms);

// ---- Spectrum ring: 3D equalizer around a round control -------------------------
// Spikes standing out from a ring that lies on a tilted, slowly turning
// plane - a Saturn-style ring round the control at (cx, cy). Their length
// follows the FxBeat bands (bass front and back, treble at the sides), the
// ring breathes with the kick, peaks get a bright cap. Draw it in two
// passes around the control: pass 0 (far half) before it, pass 1 (near
// half) after it, so the control sits inside the ring. FX_EQ_INNER(r)
// is the inner radius that keeps the near half clear of a control of
// radius r.
#define FX_EQ_INNER(r) ((r) / 0.62f + 6.0f)
void fx_eq_ring_draw(Arduino_GFX *g, int cx, int cy, float inner_r, float max_len, const FxBeat &b, uint32_t now_ms,
                     uint16_t cold, uint16_t hot, int pass);
