/*
 * app_photo_watchface.h - a photo as the watch face, with depth: the time
 * sits behind the photo's subject (a person, a pet, a building...), as if
 * the picture stood in front of the clock.
 *
 * The companion app does the work: it crops the photo to the screen,
 * finds the subject (ML Kit subject segmentation, on the phone) and
 * draws the digits in its own font, then sends one file, "pf:photo.pwf",
 * saved as /faces/photo.pwf. Here the face draws the photo, the time from
 * those digits, then the subject's pixels again over the time.
 *
 * /faces/photo.pwf, little endian:
 *   "PWF1"
 *   u16 width, height             466 x 466
 *   u16 time_y                    centre of the time, in pixels from the top
 *   u16 time_color, date_color    RGB565
 *   u16 flags                     bit 0: depth (there's a subject mask)
 *   u16 glyph_h                   height of every digit image
 *   u16 glyph_w[11]               widths of "0123456789:"
 *   u16 photo[width * height]     RGB565
 *   u8  glyphs                    11 alpha images (0 = clear .. 255 = solid), one after another
 *   u32 span_count                the subject: runs of pixels on one row
 *   span_count x { u16 y, u16 x, u16 len }
 *   u8  alpha                     the runs' alpha, all of them one after another
 */
#pragma once
#include "ui.h"

extern Screen photo_watchface_screen;

// A new /faces/photo.pwf was written: read it again next time it's drawn.
void photo_face_reload();
// True when /faces/photo.pwf is there and readable.
bool photo_face_available();
