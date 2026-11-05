/*
 * app_photo_watchface.h - a photo as the watch face, with depth: the time
 * sits behind the photo's subject (a person, a pet, a building...), as if
 * the picture stood in front of the clock.
 *
 * The companion app does the work: it crops the photo to the screen,
 * finds the subject (ML Kit subject segmentation, on the phone) and
 * draws the digits in its own font, then sends one file, "pf:<name>.pwf",
 * saved as /faces/photos/<name>.pwf. Here the face draws the photo, the time
 * from those digits, then the subject's pixels again over the time.
 *
 * Several photos: a tap on the face shows the next one, and Settings >
 * Display > Photo face (or the app) changes them every hour or every day.
 * One is in memory at a time. The app lists, shows and deletes them over
 * the link ({"t":"pf"}, COMPANION_PROTOCOL.md).
 *
 * A .pwf file, little endian:
 *   "PWF1" or "PWF2"
 *   u16 width, height             466 x 466
 *   u16 time_y                    centre of the time, in pixels from the top
 *   u16 time_color, date_color    RGB565
 *   u16 flags                     bit 0: depth (there's a subject mask)
 *   u16 glyph_h                   height of every digit image
 *   u16 glyph_w[11]               widths of "0123456789:"
 *   (PWF2 only) u16 time_x        centre of the time across (PWF1: the middle)
 *   (PWF2 only) u16 options       bit 0: no date
 *   u16 photo[width * height]     RGB565
 *   u8  glyphs                    11 alpha images (0 = clear .. 255 = solid), one after another
 *   u32 span_count                the subject: runs of pixels on one row
 *   span_count x { u16 y, u16 x, u16 len }
 *   u8  alpha                     the runs' alpha, all of them one after another
 */
#pragma once
#include "ui.h"

extern Screen photo_watchface_screen;

#include <stdint.h>

#define PHOTO_FACE_MAX 20        // photos on the card
#define PHOTO_FACE_NAME_MAX 20   // letters, digits, '_' and '-'

// Look at the card again (and read the current photo again when it's drawn).
void photo_face_reload();
// True when the current photo is there and readable.
bool photo_face_available();

int photo_face_count();
const char *photo_face_name(int i);   // sorted by name
const char *photo_face_current();     // "" with none
bool photo_face_show(const char *name);
void photo_face_next();
bool photo_face_delete(const char *name);
// A "pf:<name>.pwf" transfer: saved and shown. False if it's not a photo
// face, the name isn't allowed, or the card is full.
bool photo_face_save(const char *name, const uint8_t *data, int len);
bool photo_face_name_ok(const char *name);
