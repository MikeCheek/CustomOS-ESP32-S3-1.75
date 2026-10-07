#include "media_decode.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#ifdef MEDIA_HOST_TEST
// Host unit-test build: stdio files, plain malloc, a bundled miniz.
#include "miniz_host.h"
#define MD_ALLOC(n) malloc(n)
struct MFile {
    FILE *f = nullptr;
    bool open(const char *p) { f = fopen(p, "rb"); return f != nullptr; }
    void close() { if (f) fclose(f); f = nullptr; }
    int read(void *b, int n) { return (int)fread(b, 1, n, f); }
    bool seek(uint32_t p) { return fseek(f, p, SEEK_SET) == 0; }
    uint32_t size() { long c = ftell(f); fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, c, SEEK_SET); return (uint32_t)s; }
};
#else
#include <Arduino.h>
#include "config.h"
#include <SD.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include "miniz.h"          // ESP32-S3 ROM inflate (tinfl), esp_rom/include
#define MD_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
struct MFile {
    File f;
    bool open(const char *p) { f = SD.open(p, FILE_READ); return (bool)f; }
    void close() { if (f) f.close(); }
    int read(void *b, int n) { return (int)f.read((uint8_t *)b, n); }
    bool seek(uint32_t p) { return f.seek(p); }
    uint32_t size() { return (uint32_t)f.size(); }
};
#endif

static void set_err(char *err, size_t n, const char *msg) {
    if (err && n) { strncpy(err, msg, n - 1); err[n - 1] = 0; }
}

// ---- file kinds ---------------------------------------------------------------

static bool ends_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (b > a) return false;
    for (size_t i = 0; i < b; i++)
        if (tolower((unsigned char)s[a - b + i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}

MediaKind media_kind_of(const char *n) {
    if (ends_ci(n, ".jpg") || ends_ci(n, ".jpeg")) return MEDIA_JPEG;
    if (ends_ci(n, ".png")) return MEDIA_PNG;
    if (ends_ci(n, ".bmp")) return MEDIA_BMP;
    if (ends_ci(n, ".avi") || ends_ci(n, ".mjpeg") || ends_ci(n, ".mjpg") ||
        ends_ci(n, ".mp4") || ends_ci(n, ".mov")) return MEDIA_VIDEO;
    return MEDIA_NONE;
}
bool media_is_image(const char *n) { MediaKind k = media_kind_of(n); return k == MEDIA_JPEG || k == MEDIA_PNG || k == MEDIA_BMP; }
bool media_is_video(const char *n) { return media_kind_of(n) == MEDIA_VIDEO; }

void img_free(DecodedImage *img) {
    if (img && img->px) { free(img->px); img->px = nullptr; }
    if (img) { img->w = img->h = 0; }
}

// =============================================================================
//  Geometry + streaming box-filter resampler
// =============================================================================

struct Plan {
    int sw, sh;                 // source
    int cx, cy, cw, ch;         // crop rectangle in source pixels
    int dw, dh;                 // output
};

static bool make_plan(int sw, int sh, int max_w, int max_h, bool cover, Plan *p) {
    if (sw <= 0 || sh <= 0 || max_w <= 0 || max_h <= 0) return false;
    p->sw = sw; p->sh = sh;
    if (cover) {
        // crop the source to the target aspect, centred
        if ((int64_t)sw * max_h > (int64_t)sh * max_w) {     // source wider
            p->ch = sh; p->cw = (int)((int64_t)sh * max_w / max_h);
        } else {
            p->cw = sw; p->ch = (int)((int64_t)sw * max_h / max_w);
        }
        if (p->cw < 1) p->cw = 1;
        if (p->ch < 1) p->ch = 1;
        p->cx = (sw - p->cw) / 2; p->cy = (sh - p->ch) / 2;
        p->dw = max_w < p->cw ? max_w : p->cw;
        p->dh = max_h < p->ch ? max_h : p->ch;
    } else {
        p->cx = p->cy = 0; p->cw = sw; p->ch = sh;
        if (sw <= max_w && sh <= max_h) { p->dw = sw; p->dh = sh; }
        else if ((int64_t)sw * max_h > (int64_t)sh * max_w) {
            p->dw = max_w; p->dh = (int)((int64_t)sh * max_w / sw);
        } else {
            p->dh = max_h; p->dw = (int)((int64_t)sw * max_h / sh);
        }
        if (p->dw < 1) p->dw = 1;
        if (p->dh < 1) p->dh = 1;
    }
    return true;
}

// Rows arrive top to bottom as RGB888; each output pixel is the average of
// the source pixels that map onto it.
struct Sink {
    Plan p;
    uint16_t *dst = nullptr;
    uint32_t *acc = nullptr;    // dw * 4: r, g, b, n
    int cur_oy = -1;
    bool ok = false;

    bool begin(const Plan &plan) {
        p = plan;
        dst = (uint16_t *)MD_ALLOC((size_t)p.dw * p.dh * 2);
        acc = (uint32_t *)malloc((size_t)p.dw * 16);
        if (!dst || !acc) { end_fail(); return false; }
        memset(dst, 0, (size_t)p.dw * p.dh * 2);
        memset(acc, 0, (size_t)p.dw * 16);
        cur_oy = -1;
        ok = true;
        return true;
    }
    void flush() {
        if (cur_oy < 0 || cur_oy >= p.dh) return;
        uint16_t *row = dst + (size_t)cur_oy * p.dw;
        for (int x = 0; x < p.dw; x++) {
            uint32_t *a = acc + x * 4;
            if (!a[3]) { if (x) row[x] = row[x - 1]; continue; }
            uint32_t r = a[0] / a[3], g = a[1] / a[3], b = a[2] / a[3];
            row[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
        memset(acc, 0, (size_t)p.dw * 16);
    }
    // Is source row y needed at all? (lets decoders skip work)
    bool wants(int y) const { return y >= p.cy && y < p.cy + p.ch; }
    void push(int y, const uint8_t *rgb) {
        if (!ok || !wants(y)) return;
        int oy = (int)((int64_t)(y - p.cy) * p.dh / p.ch);
        if (oy != cur_oy) {
            flush();
            // rows skipped entirely (upscaling isn't done, so only at the end)
            cur_oy = oy;
        }
        const uint8_t *s = rgb + p.cx * 3;
        // step through source columns of the crop
        int dw = p.dw, cw = p.cw;
        for (int x = 0; x < cw; x++, s += 3) {
            int ox = (int)((int64_t)x * dw / cw);
            uint32_t *a = acc + ox * 4;
            a[0] += s[0]; a[1] += s[1]; a[2] += s[2]; a[3]++;
        }
    }
    bool finish(DecodedImage *out) {
        if (!ok) return false;
        flush();
        free(acc); acc = nullptr;
        // Any output rows never reached (corrupt/short file): copy last good row.
        out->px = dst; out->w = p.dw; out->h = p.dh; out->src_w = p.sw; out->src_h = p.sh;
        dst = nullptr;
        ok = false;
        return true;
    }
    void end_fail() {
        if (dst) free(dst);
        if (acc) free(acc);
        dst = nullptr; acc = nullptr; ok = false;
    }
};

// =============================================================================
//  PNG
// =============================================================================

static uint32_t be32(const uint8_t *b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }

struct PngState {
    MFile *f;
    uint32_t chunk_left = 0;    // bytes of the current IDAT still unread
    bool idat_done = false;
    // header
    int w, h, depth, ctype, channels;
    uint8_t pal[256][3];
    uint8_t pal_a[256];
    int pal_n = 0;
    bool has_trns_grey = false; uint16_t trns_grey = 0;
    bool has_trns_rgb = false;  uint16_t trns_r = 0, trns_g = 0, trns_b = 0;
    // scanline assembly
    int stride, bpp;
    uint8_t *cur = nullptr, *prev = nullptr;
    int fill = 0;               // bytes in cur (incl. filter byte)
    int row = 0;
    uint8_t *rgb = nullptr;
    Sink *sink;
    bool bad = false;
};

// Reads more IDAT payload, crossing chunk boundaries. Returns bytes read.
static int png_read_idat(PngState *s, uint8_t *buf, int n) {
    int got = 0;
    while (got < n && !s->idat_done) {
        if (s->chunk_left == 0) {
            uint8_t hdr[12];
            // skip CRC of the previous IDAT, read next chunk header
            if (s->f->read(hdr, 4) != 4) { s->idat_done = true; break; }       // crc
            if (s->f->read(hdr, 8) != 8) { s->idat_done = true; break; }
            if (memcmp(hdr + 4, "IDAT", 4) != 0) { s->idat_done = true; break; }
            s->chunk_left = be32(hdr);
            continue;
        }
        int want = n - got;
        if ((uint32_t)want > s->chunk_left) want = (int)s->chunk_left;
        int r = s->f->read(buf + got, want);
        if (r <= 0) { s->idat_done = true; break; }
        got += r;
        s->chunk_left -= r;
    }
    return got;
}

static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    return (uint8_t)(pb <= pc ? b : c);
}

static void png_emit_row(PngState *s) {
    uint8_t ft = s->cur[0];
    uint8_t *x = s->cur + 1, *pv = s->prev;
    int n = s->stride, bpp = s->bpp;
    switch (ft) {
    case 0: break;
    case 1: for (int i = bpp; i < n; i++) x[i] += x[i - bpp]; break;
    case 2: for (int i = 0; i < n; i++) x[i] += pv[i]; break;
    case 3: for (int i = 0; i < n; i++) x[i] += (uint8_t)(((i >= bpp ? x[i - bpp] : 0) + pv[i]) >> 1); break;
    case 4: for (int i = 0; i < n; i++) x[i] += paeth(i >= bpp ? x[i - bpp] : 0, pv[i], i >= bpp ? pv[i - bpp] : 0); break;
    default: s->bad = true; return;
    }
    int y = s->row;
    if (s->sink->wants(y)) {
        uint8_t *o = s->rgb;
        int w = s->w, d = s->depth;
        for (int i = 0; i < w; i++, o += 3) {
            int r, g, b, a = 255;
            switch (s->ctype) {
            case 0: {   // grey
                int v;
                if (d == 16) { v = x[i * 2]; if (s->has_trns_grey && ((x[i*2] << 8) | x[i*2+1]) == s->trns_grey) a = 0; }
                else if (d == 8) { v = x[i]; if (s->has_trns_grey && v == s->trns_grey) a = 0; }
                else {
                    int per = 8 / d, sh = 8 - d * (1 + i % per);
                    int raw = (x[i / per] >> sh) & ((1 << d) - 1);
                    if (s->has_trns_grey && raw == s->trns_grey) a = 0;
                    v = raw * 255 / ((1 << d) - 1);
                }
                r = g = b = v;
                break;
            }
            case 2:
                if (d == 16) {
                    r = x[i * 6]; g = x[i * 6 + 2]; b = x[i * 6 + 4];
                    if (s->has_trns_rgb && ((x[i*6] << 8) | x[i*6+1]) == s->trns_r &&
                        ((x[i*6+2] << 8) | x[i*6+3]) == s->trns_g && ((x[i*6+4] << 8) | x[i*6+5]) == s->trns_b) a = 0;
                } else {
                    r = x[i * 3]; g = x[i * 3 + 1]; b = x[i * 3 + 2];
                    if (s->has_trns_rgb && r == s->trns_r && g == s->trns_g && b == s->trns_b) a = 0;
                }
                break;
            case 3: {
                int idx;
                if (d == 8) idx = x[i];
                else { int per = 8 / d, sh = 8 - d * (1 + i % per); idx = (x[i / per] >> sh) & ((1 << d) - 1); }
                if (idx >= s->pal_n) idx = 0;
                r = s->pal[idx][0]; g = s->pal[idx][1]; b = s->pal[idx][2]; a = s->pal_a[idx];
                break;
            }
            case 4:
                if (d == 16) { r = g = b = x[i * 4]; a = x[i * 4 + 2]; }
                else { r = g = b = x[i * 2]; a = x[i * 2 + 1]; }
                break;
            default: // 6
                if (d == 16) { r = x[i * 8]; g = x[i * 8 + 2]; b = x[i * 8 + 4]; a = x[i * 8 + 6]; }
                else { r = x[i * 4]; g = x[i * 4 + 1]; b = x[i * 4 + 2]; a = x[i * 4 + 3]; }
                break;
            }
            if (a != 255) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }   // over black
            o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b;
        }
        s->sink->push(y, s->rgb);
    }
    memcpy(s->prev, x, n);      // the unfiltered row is the next row's "previous"
    s->row++;
}

// Consumes inflated bytes into scanlines.
static void png_consume(PngState *s, const uint8_t *data, size_t n) {
    int line = s->stride + 1;
    while (n && !s->bad && s->row < s->h) {
        int take = line - s->fill;
        if ((size_t)take > n) take = (int)n;
        memcpy(s->cur + s->fill, data, take);
        s->fill += take; data += take; n -= take;
        if (s->fill == line) {
            png_emit_row(s);
            s->fill = 0;
        }
    }
}

static bool png_decode(MFile &f, int max_w, int max_h, bool cover, DecodedImage *out, char *err, size_t errlen) {
    uint8_t sig[8];
    static const uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (f.read(sig, 8) != 8 || memcmp(sig, PNG_SIG, 8) != 0) { set_err(err, errlen, "Not a PNG"); return false; }

    PngState *s = (PngState *)calloc(1, sizeof(PngState));
    if (!s) { set_err(err, errlen, "Out of memory"); return false; }
    s->f = &f;
    for (int i = 0; i < 256; i++) s->pal_a[i] = 255;
    bool have_hdr = false, ok = false;
    Sink sink;
    tinfl_decompressor *inf = nullptr;
    uint8_t *dict = nullptr, *inbuf = nullptr;

    // ---- chunks up to the first IDAT ----
    for (;;) {
        uint8_t hdr[8];
        if (f.read(hdr, 8) != 8) { set_err(err, errlen, "Truncated PNG"); goto done; }
        uint32_t len = be32(hdr);
        if (!memcmp(hdr + 4, "IHDR", 4)) {
            uint8_t b[13];
            if (len < 13 || f.read(b, 13) != 13) { set_err(err, errlen, "Bad PNG header"); goto done; }
            s->w = (int)be32(b); s->h = (int)be32(b + 4); s->depth = b[8]; s->ctype = b[9];
            if (b[12] != 0) { set_err(err, errlen, "Interlaced PNG not supported"); goto done; }
            static const int CH[7] = {1, 0, 3, 1, 2, 0, 4};
            if (s->ctype > 6 || CH[s->ctype] == 0) { set_err(err, errlen, "Bad PNG colour type"); goto done; }
            s->channels = CH[s->ctype];
            if (s->w <= 0 || s->h <= 0 || s->w > 16384 || s->h > 16384) { set_err(err, errlen, "PNG too large"); goto done; }
            have_hdr = true;
            // skip rest of chunk + CRC
            { uint8_t skip[4]; for (uint32_t k = 13; k < len; k++) f.read(skip, 1); f.read(skip, 4); }
        } else if (!memcmp(hdr + 4, "PLTE", 4)) {
            int n = (int)(len / 3); if (n > 256) n = 256;
            for (int i = 0; i < n; i++) f.read(s->pal[i], 3);
            s->pal_n = n;
            uint8_t skip[4]; for (uint32_t k = n * 3; k < len; k++) f.read(skip, 1); f.read(skip, 4);
        } else if (!memcmp(hdr + 4, "tRNS", 4)) {
            uint8_t b[256]; int n = len > 256 ? 256 : (int)len;
            f.read(b, n);
            if (s->ctype == 3) for (int i = 0; i < n; i++) s->pal_a[i] = b[i];
            else if (s->ctype == 0 && n >= 2) { s->has_trns_grey = true; s->trns_grey = (b[0] << 8) | b[1]; }
            else if (s->ctype == 2 && n >= 6) { s->has_trns_rgb = true; s->trns_r = (b[0] << 8) | b[1]; s->trns_g = (b[2] << 8) | b[3]; s->trns_b = (b[4] << 8) | b[5]; }
            uint8_t skip[4]; for (uint32_t k = n; k < len; k++) f.read(skip, 1); f.read(skip, 4);
        } else if (!memcmp(hdr + 4, "IDAT", 4)) {
            s->chunk_left = len;
            break;
        } else if (!memcmp(hdr + 4, "IEND", 4)) {
            set_err(err, errlen, "PNG has no image data"); goto done;
        } else {
            // skip: seek is cheap on SD
            uint32_t pos_skip = len + 4;
            uint8_t tmp[64];
            while (pos_skip) { int r = f.read(tmp, pos_skip > 64 ? 64 : (int)pos_skip); if (r <= 0) break; pos_skip -= r; }
        }
    }
    if (!have_hdr) { set_err(err, errlen, "Bad PNG"); goto done; }
    if (!(s->depth == 1 || s->depth == 2 || s->depth == 4 || s->depth == 8 || s->depth == 16) ||
        ((s->ctype == 2 || s->ctype == 4 || s->ctype == 6) && s->depth < 8) || (s->ctype == 3 && s->depth == 16)) {
        set_err(err, errlen, "Unsupported PNG bit depth"); goto done;
    }

    {
        Plan plan;
        if (!make_plan(s->w, s->h, max_w, max_h, cover, &plan) || !sink.begin(plan)) { set_err(err, errlen, "Out of memory"); goto done; }
        s->sink = &sink;
        s->stride = (s->w * s->channels * s->depth + 7) / 8;
        s->bpp = (s->channels * s->depth + 7) / 8;
        s->cur = (uint8_t *)MD_ALLOC(s->stride + 1);
        s->prev = (uint8_t *)MD_ALLOC(s->stride);
        s->rgb = (uint8_t *)MD_ALLOC((size_t)s->w * 3);
        inf = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
        dict = (uint8_t *)MD_ALLOC(TINFL_LZ_DICT_SIZE);
        inbuf = (uint8_t *)malloc(4096);
        if (!s->cur || !s->prev || !s->rgb || !inf || !dict || !inbuf) { set_err(err, errlen, "Out of memory"); goto done; }
        memset(s->prev, 0, s->stride);
        tinfl_init(inf);

        size_t dict_ofs = 0, in_avail = 0;
        const uint8_t *in_ptr = inbuf;
        for (;;) {
            if (in_avail == 0 && !s->idat_done) {
                in_avail = png_read_idat(s, inbuf, 4096);
                in_ptr = inbuf;
            }
            size_t in_bytes = in_avail, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
            // More input may follow until the last IDAT has been read.
            int flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (s->idat_done ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
            tinfl_status st = tinfl_decompress(inf, in_ptr, &in_bytes, dict, dict + dict_ofs, &out_bytes, flags);
            in_ptr += in_bytes; in_avail -= in_bytes;
            if (out_bytes) png_consume(s, dict + dict_ofs, out_bytes);
            dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
            if (s->bad) { set_err(err, errlen, "Corrupt PNG"); goto done; }
            if (s->row >= s->h) break;
            if (st == TINFL_STATUS_DONE) break;
            if (st < 0) { set_err(err, errlen, "Corrupt PNG data"); break; }   // keep what decoded
            if (st == TINFL_STATUS_NEEDS_MORE_INPUT && s->idat_done && in_avail == 0) break;
        }
        ok = s->row > 0 && sink.finish(out);
        if (!ok) set_err(err, errlen, "Corrupt PNG");
    }
done:
    if (!ok) sink.end_fail();
    if (s) { free(s->cur); free(s->prev); free(s->rgb); free(s); }
    free(inf); free(dict); free(inbuf);
    return ok;
}

// =============================================================================
//  BMP
// =============================================================================

static uint32_t le32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t le16(const uint8_t *b) { return b[0] | (b[1] << 8); }

static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(uint32_t m) { int n = 0; while (m) { n += m & 1; m >>= 1; } return n; }
static uint8_t mask_get(uint32_t px, uint32_t m) {
    if (!m) return 0;
    uint32_t v = (px & m) >> mask_shift(m);
    int bits = mask_bits(m);
    if (bits >= 8) return (uint8_t)(v >> (bits - 8));
    return (uint8_t)(v * 255 / ((1u << bits) - 1));
}

static bool bmp_decode(MFile &f, int max_w, int max_h, bool cover, DecodedImage *out, char *err, size_t errlen) {
    uint8_t h[54 + 16];
    if (f.read(h, 54) != 54 || h[0] != 'B' || h[1] != 'M') { set_err(err, errlen, "Not a BMP"); return false; }
    uint32_t data_off = le32(h + 10), dib = le32(h + 14);
    int32_t w = (int32_t)le32(h + 18), hh = (int32_t)le32(h + 22);
    int bpp = le16(h + 28);
    uint32_t comp = le32(h + 30);
    uint32_t colors = le32(h + 46);
    bool top_down = hh < 0;
    if (hh < 0) hh = -hh;
    if (w <= 0 || hh <= 0 || w > 16384 || hh > 16384) { set_err(err, errlen, "Bad BMP size"); return false; }
    if (!(comp == 0 || comp == 3 || comp == 6)) { set_err(err, errlen, "Compressed BMP not supported"); return false; }
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) { set_err(err, errlen, "Unsupported BMP depth"); return false; }

    uint32_t rm = 0, gm = 0, bm = 0;
    if (bpp == 16) { rm = 0x7C00; gm = 0x03E0; bm = 0x001F; }
    if (bpp == 32) { rm = 0x00FF0000; gm = 0x0000FF00; bm = 0x000000FF; }
    if (comp == 3 || comp == 6) {
        uint8_t mk[12];
        f.seek(14 + 40);      // right after the 40-byte core header (BITFIELDS or V4/V5)
        f.read(mk, 12);
        rm = le32(mk); gm = le32(mk + 4); bm = le32(mk + 8);
    }
    uint8_t pal[256][3];
    if (bpp <= 8) {
        int n = colors ? (int)colors : (1 << bpp);
        if (n > 256) n = 256;
        f.seek(14 + dib);
        for (int i = 0; i < n; i++) { uint8_t q[4]; f.read(q, 4); pal[i][0] = q[2]; pal[i][1] = q[1]; pal[i][2] = q[0]; }
    }
    Plan plan;
    Sink sink;
    if (!make_plan(w, hh, max_w, max_h, cover, &plan) || !sink.begin(plan)) { set_err(err, errlen, "Out of memory"); return false; }
    uint32_t stride = ((uint32_t)w * bpp + 31) / 32 * 4;
    uint8_t *row = (uint8_t *)MD_ALLOC(stride);
    uint8_t *rgb = (uint8_t *)MD_ALLOC((size_t)w * 3);
    if (!row || !rgb) { free(row); free(rgb); sink.end_fail(); set_err(err, errlen, "Out of memory"); return false; }

    // Only rows inside the crop are read; when shrinking a lot, every row
    // still contributes (box filter) but reading stays sequential-ish.
    for (int y = 0; y < hh; y++) {
        if (!sink.wants(y)) continue;
        int fr = top_down ? y : (hh - 1 - y);
        if (!f.seek(data_off + (uint32_t)fr * stride) || f.read(row, (int)stride) != (int)stride) break;
        uint8_t *o = rgb;
        for (int x = 0; x < w; x++, o += 3) {
            switch (bpp) {
            case 24: o[0] = row[x * 3 + 2]; o[1] = row[x * 3 + 1]; o[2] = row[x * 3]; break;
            case 32: { uint32_t px = le32(row + x * 4); o[0] = mask_get(px, rm); o[1] = mask_get(px, gm); o[2] = mask_get(px, bm); break; }
            case 16: { uint32_t px = le16(row + x * 2); o[0] = mask_get(px, rm); o[1] = mask_get(px, gm); o[2] = mask_get(px, bm); break; }
            default: {
                int per = 8 / bpp, sh = 8 - bpp * (1 + x % per);
                int idx = (row[x / per] >> sh) & ((1 << bpp) - 1);
                o[0] = pal[idx][0]; o[1] = pal[idx][1]; o[2] = pal[idx][2];
            }
            }
        }
        sink.push(y, rgb);
    }
    free(row); free(rgb);
    return sink.finish(out);
}

// =============================================================================
//  JPEG (JPEGDEC)
// =============================================================================
#ifndef MEDIA_HOST_TEST

struct JpgCtx { uint16_t *buf; int w, h; };

static int jpg_to_buf(JPEGDRAW *d) {
    JpgCtx *c = (JpgCtx *)d->pUser;
    if (!c || !c->buf) return 0;
    for (int r = 0; r < d->iHeight; r++) {
        int y = d->y + r;
        if (y < 0 || y >= c->h) continue;
        int n = d->iWidth;
        if (d->x + n > c->w) n = c->w - d->x;
        if (n <= 0) continue;
        memcpy(c->buf + (size_t)y * c->w + d->x, d->pPixels + (size_t)r * d->iWidth, (size_t)n * 2);
    }
    return 1;
}

// One decoder object, in PSRAM (it's ~18 KB) and shared - all decoding
// happens on the UI task.
static JPEGDEC *jpegdec() {
    static JPEGDEC *s = nullptr;
    if (!s) {
        void *m = MD_ALLOC(sizeof(JPEGDEC));
        if (m) s = new (m) JPEGDEC();
    }
    return s;
}

// Decodes (already opened) into a temp at a power-of-two scale, then
// box-resamples into the plan.
static bool jpeg_finish(JPEGDEC *j, int max_w, int max_h, bool cover, DecodedImage *out, char *err, size_t errlen) {
    int sw = j->getWidth(), sh = j->getHeight();
    bool use_thumb = false;
    // Small targets (grid thumbnails): use the EXIF thumbnail when it's big enough.
    if (max_w <= 160 && max_h <= 160 && j->hasThumb() &&
        j->getThumbWidth() >= max_w && j->getThumbHeight() >= max_h) use_thumb = true;
    int full_w = use_thumb ? j->getThumbWidth() : sw;
    int full_h = use_thumb ? j->getThumbHeight() : sh;

    Plan plan;
    if (!make_plan(full_w, full_h, max_w, max_h, cover, &plan)) { set_err(err, errlen, "Bad JPEG"); j->close(); return false; }
    int shift = 0;
    if (!use_thumb) {
        while (shift < 3 && (plan.cw >> (shift + 1)) >= plan.dw && (plan.ch >> (shift + 1)) >= plan.dh) shift++;
        // memory guard for the intermediate buffer (~6 MB max)
        while (shift < 3 && (size_t)(full_w >> shift) * (full_h >> shift) * 2 > 6u * 1024 * 1024) shift++;
    }
    JpgCtx c;
    c.w = full_w >> shift; c.h = full_h >> shift;
    if (c.w < 1 || c.h < 1) { j->close(); return false; }
    c.buf = (uint16_t *)MD_ALLOC((size_t)c.w * c.h * 2);
    if (!c.buf) { set_err(err, errlen, "Image too large"); j->close(); return false; }
    memset(c.buf, 0, (size_t)c.w * c.h * 2);
    j->setUserPointer(&c);
    j->setPixelType(RGB565_LITTLE_ENDIAN);
    int opts = use_thumb ? JPEG_EXIF_THUMBNAIL : (shift == 1 ? JPEG_SCALE_HALF : shift == 2 ? JPEG_SCALE_QUARTER : shift == 3 ? JPEG_SCALE_EIGHTH : 0);
    int rc = j->decode(0, 0, opts);
    j->close();
    if (!rc) { free(c.buf); set_err(err, errlen, "JPEG decode failed"); return false; }

    // resample the scaled decode
    Plan p2;
    Plan scaled = plan;
    scaled.sw = c.w; scaled.sh = c.h;
    scaled.cx = plan.cx >> shift; scaled.cy = plan.cy >> shift;
    scaled.cw = plan.cw >> shift; scaled.ch = plan.ch >> shift;
    if (scaled.cw < 1) scaled.cw = 1;
    if (scaled.ch < 1) scaled.ch = 1;
    if (scaled.dw > scaled.cw) scaled.dw = scaled.cw;
    if (scaled.dh > scaled.ch) scaled.dh = scaled.ch;
    p2 = scaled;
    Sink sink;
    if (!sink.begin(p2)) { free(c.buf); set_err(err, errlen, "Out of memory"); return false; }
    uint8_t *rgb = (uint8_t *)MD_ALLOC((size_t)c.w * 3);
    if (!rgb) { free(c.buf); sink.end_fail(); return false; }
    for (int y = 0; y < c.h; y++) {
        if (!sink.wants(y)) continue;
        const uint16_t *s = c.buf + (size_t)y * c.w;
        for (int x = 0; x < c.w; x++) {
            uint16_t px = s[x];
            rgb[x * 3]     = (uint8_t)(((px >> 11) & 31) * 255 / 31);
            rgb[x * 3 + 1] = (uint8_t)(((px >> 5) & 63) * 255 / 63);
            rgb[x * 3 + 2] = (uint8_t)((px & 31) * 255 / 31);
        }
        sink.push(y, rgb);
    }
    free(rgb);
    free(c.buf);
    bool ok = sink.finish(out);
    if (ok) { out->src_w = sw; out->src_h = sh; }
    return ok;
}

static bool jpeg_decode_file(const char *path, int max_w, int max_h, bool cover, DecodedImage *out, char *err, size_t errlen) {
    JPEGDEC *j = jpegdec();
    if (!j) { set_err(err, errlen, "Out of memory"); return false; }
    File f = SD.open(path, FILE_READ);
    if (!f) { set_err(err, errlen, "Can't open file"); return false; }
    if (!j->open(f, jpg_to_buf)) { f.close(); set_err(err, errlen, "Not a valid JPEG"); return false; }
    bool ok = jpeg_finish(j, max_w, max_h, cover, out, err, errlen);
    f.close();
    return ok;
}

bool img_decode_jpeg_mem(const uint8_t *data, size_t len, int max_w, int max_h, bool cover, DecodedImage *out) {
    JPEGDEC *j = jpegdec();
    if (!j || !j->openRAM((uint8_t *)data, (int)len, jpg_to_buf)) return false;
    return jpeg_finish(j, max_w, max_h, cover, out, nullptr, 0);
}

bool img_decode_jpeg_into(const uint8_t *data, size_t len, int shift, uint16_t *dst, int dst_w, int dst_h) {
    JPEGDEC *j = jpegdec();
    if (!j || !dst || !j->openRAM((uint8_t *)data, (int)len, jpg_to_buf)) return false;
    JpgCtx c = { dst, dst_w, dst_h };
    j->setUserPointer(&c);
    j->setPixelType(RGB565_LITTLE_ENDIAN);
    int opts = shift == 1 ? JPEG_SCALE_HALF : shift == 2 ? JPEG_SCALE_QUARTER : shift == 3 ? JPEG_SCALE_EIGHTH : 0;
    int rc = j->decode(0, 0, opts);
    j->close();
    return rc != 0;
}
#endif

// =============================================================================

bool img_decode_file(const char *path, int max_w, int max_h, bool cover, DecodedImage *out, char *err, size_t errlen) {
    if (!path || !out) return false;
    out->px = nullptr; out->w = out->h = 0;
    MediaKind k = media_kind_of(path);
#ifndef MEDIA_HOST_TEST
    if (k == MEDIA_JPEG) return jpeg_decode_file(path, max_w, max_h, cover, out, err, errlen);
#endif
    MFile f;
    if (!f.open(path)) { set_err(err, errlen, "Can't open file"); return false; }
    bool ok = false;
    if (k == MEDIA_PNG) ok = png_decode(f, max_w, max_h, cover, out, err, errlen);
    else if (k == MEDIA_BMP) ok = bmp_decode(f, max_w, max_h, cover, out, err, errlen);
    else set_err(err, errlen, "Not an image");
    f.close();
    return ok;
}

bool img_from_rgb565(const uint16_t *src, int w, int h, int stride, int max_w, int max_h, bool cover, DecodedImage *out) {
    Plan plan;
    Sink sink;
    if (!src || !make_plan(w, h, max_w, max_h, cover, &plan) || !sink.begin(plan)) return false;
    uint8_t *rgb = (uint8_t *)MD_ALLOC((size_t)w * 3);
    if (!rgb) { sink.end_fail(); return false; }
    for (int y = 0; y < h; y++) {
        if (!sink.wants(y)) continue;
        const uint16_t *s = src + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint16_t px = s[x];
            rgb[x * 3]     = (uint8_t)(((px >> 11) & 31) * 255 / 31);
            rgb[x * 3 + 1] = (uint8_t)(((px >> 5) & 63) * 255 / 63);
            rgb[x * 3 + 2] = (uint8_t)((px & 31) * 255 / 31);
        }
        sink.push(y, rgb);
    }
    free(rgb);
    return sink.finish(out);
}
