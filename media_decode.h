/*
 * media_decode.h
 * Still-image decoding for the Gallery: JPEG (JPEGDEC), PNG (the
 * ESP32-S3 ROM's inflate - no extra library) and BMP, straight from the
 * SD card into an RGB565 buffer in PSRAM, scaled down on the way in with
 * a box filter (so a 12 MP photo becomes a clean 466 px image, not an
 * aliased one).
 *
 *   fit   - whole image inside max_w x max_h, aspect kept, never enlarged
 *   cover - fills exactly max_w x max_h, centre-cropped (grid thumbnails)
 *
 * PNG: 1/2/4/8/16-bit grey, RGB, palette (+tRNS), grey+alpha, RGBA; alpha
 * is composited over black. Interlaced (Adam7) PNGs are not supported.
 * BMP: 1/4/8-bit palette, 16 (555/565), 24, 32 bit; bottom-up or top-down.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

struct DecodedImage {
    uint16_t *px = nullptr;   // RGB565, native byte order (what the canvas uses)
    int w = 0, h = 0;         // decoded size
    int src_w = 0, src_h = 0; // original size
};

enum MediaKind : uint8_t { MEDIA_NONE, MEDIA_JPEG, MEDIA_PNG, MEDIA_BMP, MEDIA_VIDEO };
MediaKind media_kind_of(const char *name);   // by extension
bool media_is_image(const char *name);
bool media_is_video(const char *name);

bool img_decode_file(const char *path, int max_w, int max_h, bool cover, DecodedImage *out,
                     char *err = nullptr, size_t errlen = 0);
// A JPEG already in memory (video frames, for thumbnails / posters).
bool img_decode_jpeg_mem(const uint8_t *data, size_t len, int max_w, int max_h, bool cover,
                         DecodedImage *out);
void img_free(DecodedImage *img);

// Fast path for video frames: decode a JPEG into an existing buffer of
// exactly (src_w >> scale_shift) x (src_h >> scale_shift) pixels.
// scale_shift 0..3. Returns false on a corrupt frame.
bool img_decode_jpeg_into(const uint8_t *data, size_t len, int scale_shift, uint16_t *dst, int dst_w, int dst_h);

// Scale an RGB565 buffer (e.g. a decoded H.264 frame) into a new image.
bool img_from_rgb565(const uint16_t *src, int w, int h, int stride, int max_w, int max_h, bool cover, DecodedImage *out);
