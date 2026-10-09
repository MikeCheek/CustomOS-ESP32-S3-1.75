/*
 * phone_images.h
 * App icons and album art from the phone.
 *
 * Notifications carry "ic" (a hash of the app) and now-playing carries
 * "ah" (a hash of the artwork). When something wants to draw one the
 * watch doesn't have, it asks the phone ({"e":"img","k":"i"|"a","h":..})
 * and the phone sends the JPEG as binary frames on the link
 * characteristic:
 *
 *   0x03 kind(1: 'i'/'a') hash(4 LE) total(4 LE) offset(4 LE) data...
 *
 * Decoded images stay in PSRAM: up to 40 icons (40x40) and the last two
 * album covers (up to 128x128), least recently used dropped first.
 */
#pragma once
#include <stdint.h>

struct Arduino_GFX;

#define PHONE_ICON_SIZE 40
#define PHONE_ART_SIZE  128

// The icon / cover for this hash, or nullptr (then it's been requested
// and a later frame will have it). hash 0 = none.
const uint16_t *phone_icon(uint32_t hash);
const uint16_t *phone_art(uint32_t hash, int *w, int *h);

// An image clipped to a circle of radius r centred on (cx, cy), scaled
// to cover it (nearest neighbour). dim: brightness 0..32 (32 = as is).
void phone_image_draw_round(Arduino_GFX *g, const uint16_t *px, int w, int h, int cx, int cy, int r,
                            uint8_t dim = 32);

// BLE task: one 0x03 frame (copies only).
void phone_images_on_frame(const uint8_t *d, int len);

// Main loop: decodes what arrived.
void phone_images_update();
