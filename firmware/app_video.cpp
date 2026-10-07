#include "app_video.h"
#include "video_container.h"
#include "config.h"
#include "board_pins.h"
#include "hal_audio.h"
#include "hal_sd.h"
#include "hal_touch.h"
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <minimp3.h>            // declarations only - the implementation lives in hal_audio.cpp
#include <AACDecoderHelix.h>    // libhelix (Library Manager: "libhelix") - AAC / HE-AAC
#include <ESP_H264_Decoder.h>   // Espressif tinyH264, packaged as an Arduino library
#include <math.h>
#include <string.h>

#define PS_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PS_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static char s_err[56] = "";

// =============================================================================
//  H.264 (tinyH264)
// =============================================================================

struct H264Dec {
    h264bsd_hd_t hd = nullptr;
    uint8_t *buf = nullptr;      // Annex-B working buffer
    size_t cap = 0;
    const uint8_t *pic = nullptr;   // last decoded picture (I420), owned by the decoder
    uint32_t pw = 0, ph = 0;        // its coded size

    bool open() {
        h264bsd_cfg_t cfg = H264BSD_CFG_DEFAULT();
        hd = h264bsdAlloc(&cfg);
        return hd != nullptr;
    }
    void close() {
        if (hd) { h264bsdShutdown(hd); h264bsdFree(hd); hd = nullptr; }
        free(buf); buf = nullptr; cap = 0; pic = nullptr;
    }
    bool reserve(size_t n) {
        if (n <= cap) return true;
        uint8_t *nb = (uint8_t *)PS_REALLOC(buf, n + 1024);
        if (!nb) return false;
        buf = nb; cap = n + 1024;
        return true;
    }
    // Feeds an Annex-B buffer; true if a picture came out.
    bool decode(uint8_t *data, size_t len) {
        bool got = false;
        uint8_t *p = data;
        uint32_t left = (uint32_t)len;
        for (int guard = 0; left > 0 && guard < 64; guard++) {
            u32 l = left;                    // tinyH264's own u32 (unsigned int), not uint32_t
            u8 *out = nullptr;
            u32 w = 0, h = 0;
            u32 rc = h264bsdDecode(hd, p, &l, &out, &w, &h);
            uint32_t used = left - l;
            if (rc == H264BSD_PIC_RDY && out) { pic = out; pw = w; ph = h; got = true; }
            if (rc == H264BSD_MEMALLOC_ERROR) break;
            // HDRS_RDY: stream headers activated, nothing consumed - the
            // same data has to be fed again to decode the picture.
            if (used == 0 && rc != H264BSD_HDRS_RDY) break;
            p += used; left = l;
        }
        return got;
    }
};

// I420 -> RGB565 (BT.601, limited range), cropped to w x h.
static void i420_to_rgb565(const uint8_t *pic, uint32_t pw, uint32_t ph, int w, int h, uint16_t *dst, int dst_stride) {
    const uint8_t *Y = pic, *U = pic + pw * ph, *V = U + (pw / 2) * (ph / 2);
    if (w > (int)pw) w = pw;
    if (h > (int)ph) h = ph;
    for (int y = 0; y < h; y++) {
        const uint8_t *yr = Y + (size_t)y * pw, *ur = U + (size_t)(y / 2) * (pw / 2), *vr = V + (size_t)(y / 2) * (pw / 2);
        uint16_t *o = dst + (size_t)y * dst_stride;
        for (int x = 0; x < w; x++) {
            int c = (yr[x] - 16) * 298, d = ur[x >> 1] - 128, e = vr[x >> 1] - 128;
            int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            o[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}

// Reads frame `idx` of an H.264 track into dec.buf as Annex-B. Returns length.
static size_t h264_read_sample(File &f, const VidInfo &vi, int idx, H264Dec &dec, uint8_t **scratch, size_t *scratch_cap) {
    uint32_t sz = vi.vsize.v[idx];
    if (!sz) return 0;
    if (vi.nal_len_size == 0) {                       // AVI: already Annex-B
        if (!dec.reserve(sz)) return 0;
        f.seek(vi.voff.v[idx]);
        return (uint32_t)f.read(dec.buf, sz) == sz ? sz : 0;
    }
    if (sz > *scratch_cap) {
        uint8_t *nb = (uint8_t *)PS_REALLOC(*scratch, sz + 1024);
        if (!nb) return 0;
        *scratch = nb; *scratch_cap = sz + 1024;
    }
    f.seek(vi.voff.v[idx]);
    if ((uint32_t)f.read(*scratch, sz) != sz) return 0;
    size_t need = sz + 64 * 4;                        // start codes can be longer than 1-3 byte lengths
    if (!dec.reserve(need)) return 0;
    return avcc_to_annexb(*scratch, sz, vi.nal_len_size, dec.buf, dec.cap);
}

static bool h264_feed_header(H264Dec &dec, const VidInfo &vi) {
    if (!vi.avc_hdr_len || !dec.reserve(vi.avc_hdr_len)) return true;
    memcpy(dec.buf, vi.avc_hdr, vi.avc_hdr_len);
    dec.decode(dec.buf, vi.avc_hdr_len);
    return true;
}

// =============================================================================
//  Posters (Gallery thumbnails)
// =============================================================================

bool video_poster(const char *path, int max_w, int max_h, bool cover, DecodedImage *out) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    VidInfo vi;
    char err[48];
    bool ok = false;
    if (video_parse(f, path, vi, true, err, sizeof(err))) {
        if (vi.vcodec == VC_MJPEG) {
            for (int i = 0; i < vi.voff.n && i < 8 && !ok; i++) {
                uint32_t sz = vi.vsize.v[i];
                if (!sz || sz > 2 * 1024 * 1024) continue;
                uint8_t *buf = (uint8_t *)PS_ALLOC(sz);
                if (!buf) break;
                f.seek(vi.voff.v[i]);
                if ((uint32_t)f.read(buf, sz) == sz) ok = img_decode_jpeg_mem(buf, sz, max_w, max_h, cover, out);
                free(buf);
            }
        } else if (vi.vcodec == VC_H264) {
            H264Dec dec;
            uint8_t *scratch = nullptr; size_t scap = 0;
            if (dec.open()) {
                h264_feed_header(dec, vi);
                int k = vi.vkey.n ? (int)vi.vkey.v[0] : 0;
                for (int i = k; i < vi.voff.n && i < k + 4 && !dec.pic; i++) {
                    size_t n = h264_read_sample(f, vi, i, dec, &scratch, &scap);
                    if (n) dec.decode(dec.buf, n);
                }
                if (dec.pic) {
                    uint16_t *rgb = (uint16_t *)PS_ALLOC((size_t)vi.w * vi.h * 2);
                    if (rgb) {
                        i420_to_rgb565(dec.pic, dec.pw, dec.ph, vi.w, vi.h, rgb, vi.w);
                        ok = img_from_rgb565(rgb, vi.w, vi.h, vi.w, max_w, max_h, cover, out);
                        free(rgb);
                    }
                }
                dec.close();
            }
            free(scratch);
        }
    }
    vi.clear();
    f.close();
    return ok;
}

// =============================================================================
//  Audio: ring buffer (UI task writes) -> audio task (decodes, plays, counts)
// =============================================================================

static const uint32_t RING_SIZE = 192 * 1024;
static uint8_t *s_ring = nullptr;
static volatile uint32_t s_rh = 0, s_rt = 0;         // totals written / read
static volatile bool s_a_run = false;                // task should be playing
static volatile bool s_a_flush = false;              // task: drop everything, reset
static volatile bool s_a_eos = false;                // reader has queued the last packet
static volatile bool s_a_drained = false;            // task played everything after eos
static volatile uint64_t s_a_frames = 0;             // per-channel samples played
static volatile uint32_t s_a_hz = 0;                 // actual output-side rate
static TaskHandle_t s_a_task = nullptr;
static volatile bool s_a_task_quit = false;
static VidInfo s_vi;                                 // read-only for the task while playing

static uint32_t ring_take(uint8_t *dst, uint32_t n) {
    uint32_t avail = s_rh - s_rt;
    if (n > avail) n = avail;
    uint32_t t = s_rt % RING_SIZE, first = RING_SIZE - t;
    if (first > n) first = n;
    memcpy(dst, s_ring + t, first);
    if (n > first) memcpy(dst + first, s_ring, n - first);
    s_rt += n;
    return n;
}

static void audio_task(void *) {
    const uint32_t TMP = 8192, INB = 16384;
    uint8_t *tmp = (uint8_t *)malloc(TMP);
    int16_t *pcm = (int16_t *)PS_ALLOC(4096 * 2);          // MP3 1152x2, AAC(+SBR) 2048x2
    uint8_t *inb = (uint8_t *)PS_ALLOC(INB);
    mp3dec_t *mp3 = nullptr;
    HAACDecoder aac = nullptr;
    int inlen = 0;
    uint32_t cfg_hz = 0; int cfg_ch = 0;
    auto configure = [&](uint32_t hz, int ch) {
        if (cfg_hz != hz || cfg_ch != ch) { audio_pcm_configure(hz, ch); cfg_hz = hz; cfg_ch = ch; s_a_hz = hz; }
    };

    while (!s_a_task_quit) {
        if (s_a_flush) {
            s_rt = s_rh;
            inlen = 0;
            if (mp3) mp3dec_init(mp3);
            if (aac) AACFlushCodec(aac);
            s_a_frames = 0;
            s_a_drained = false;
            cfg_hz = 0;
            s_a_flush = false;
        }
        if (!s_a_run || !tmp || !pcm || !inb) { vTaskDelay(pdMS_TO_TICKS(4)); continue; }
        ACodec ac = s_vi.acodec;

        if (ac == AC_PCM16LE || ac == AC_PCM16BE || ac == AC_PCM8) {
            int bps = ac == AC_PCM8 ? 1 : 2, frame = bps * s_vi.ach;
            uint32_t avail = s_rh - s_rt;
            if (avail < (uint32_t)frame) {
                if (s_a_eos) s_a_drained = true;
                vTaskDelay(pdMS_TO_TICKS(3));
                continue;
            }
            uint32_t want = 1024 * frame;                       // ~50 ms at 22 kHz mono
            if (want > avail) want = avail - avail % frame;
            if (bps == 1 && want > TMP / 2) want = (TMP / 2) - (TMP / 2) % frame;
            configure(s_vi.arate, s_vi.ach);
            ring_take(tmp, want);
            int16_t *s16 = (int16_t *)tmp;
            if (ac == AC_PCM8) for (int i = (int)want - 1; i >= 0; i--) s16[i] = (int16_t)((tmp[i] - 128) << 8);
            else if (ac == AC_PCM16BE) for (uint32_t i = 0; i < want / 2; i++) s16[i] = (int16_t)__builtin_bswap16((uint16_t)s16[i]);
            uint32_t samples = bps == 1 ? want : want / 2;
            audio_pcm_write(s16, samples);
            s_a_frames += want / frame;
            continue;
        }

        // compressed: keep the input buffer topped up
        if (inlen < (int)INB / 2) inlen += ring_take(inb + inlen, INB - inlen);
        if (inlen == 0) {
            if (s_a_eos) s_a_drained = true;
            vTaskDelay(pdMS_TO_TICKS(3));
            continue;
        }
        if (ac == AC_MP3) {
            if (!mp3) { mp3 = (mp3dec_t *)PS_ALLOC(sizeof(mp3dec_t)); if (!mp3) { vTaskDelay(50); continue; } mp3dec_init(mp3); }
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(mp3, inb, inlen, pcm, &info);
            if (info.frame_bytes > 0) { memmove(inb, inb + info.frame_bytes, inlen - info.frame_bytes); inlen -= info.frame_bytes; }
            else if (inlen >= (int)INB / 2 || s_a_eos) { inlen = 0; continue; }
            else { vTaskDelay(pdMS_TO_TICKS(3)); continue; }
            if (samples > 0) {
                configure(info.hz, info.channels);
                audio_pcm_write(pcm, (size_t)samples * info.channels);
                s_a_frames += samples;
            }
        } else if (ac == AC_AAC) {
            if (!aac) { aac = AACInitDecoder(); if (!aac) { vTaskDelay(50); continue; } }
            int sync = AACFindSyncWord(inb, inlen);
            if (sync < 0) { inlen = inlen > 1 ? 1 : inlen; if (inlen) inb[0] = inb[inlen - 1]; continue; }
            if (sync > 0) { memmove(inb, inb + sync, inlen - sync); inlen -= sync; }
            unsigned char *p = inb;
            int left = inlen;
            int err = AACDecode(aac, &p, &left, pcm);
            int used = inlen - left;
            if (err == ERR_AAC_INDATA_UNDERFLOW && !s_a_eos) { vTaskDelay(pdMS_TO_TICKS(3)); continue; }
            if (err) { if (used <= 0) used = 1; }                 // skip this sync and resync
            if (used > 0) { memmove(inb, inb + used, inlen - used); inlen -= used; }
            if (!err) {
                AACFrameInfo fi;
                AACGetLastFrameInfo(aac, &fi);
                if (fi.outputSamps > 0 && fi.nChans > 0) {
                    configure(fi.sampRateOut, fi.nChans);
                    audio_pcm_write(pcm, fi.outputSamps);
                    s_a_frames += fi.outputSamps / fi.nChans;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (aac) AACFreeDecoder(aac);
    free(tmp); free(pcm); free(inb); free(mp3);
    s_a_task = nullptr;
    vTaskDelete(nullptr);
}

static void audio_flush_wait() {
    s_a_run = false;
    if (!s_a_task) { s_rt = s_rh; s_a_frames = 0; return; }
    s_a_flush = true;
    uint32_t t0 = millis();
    while (s_a_flush && millis() - t0 < 600) vTaskDelay(1);
}

// =============================================================================
//  Player state
// =============================================================================

static char      s_path[160];
static File      s_vf;
static bool      s_loaded = false;
static uint8_t  *s_cbuf = nullptr;      // compressed frame / scratch
static size_t    s_cbuf_sz = 0;
static uint16_t *s_frame = nullptr;     // decoded frame (RGB565)
static int       s_fw = 0, s_fh = 0, s_shift = 0;
static int       s_shown = -1;          // frame index in s_frame
static H264Dec   s_h264;
static int       s_dec_next = 0;        // H.264: next frame index the decoder expects
enum PState : uint8_t { PS_PAUSED, PS_PLAYING, PS_ENDED };
static PState    s_state = PS_PAUSED;
static uint32_t  s_pos_ms = 0;
static uint32_t  s_wall0 = 0;
static bool      s_use_audio_clock = false;
static uint32_t  s_a_next = 0, s_a_next_off = 0;   // reader position in audio packets
static bool      s_a_hdr_done = false;             // AAC: ADTS header of the current packet queued
static const uint32_t AUDIO_LATENCY_MS = 230;     // I2S DMA (4096 frames @16 kHz) minus a bit
static bool      s_fill = false;
static uint32_t  s_overlay_until = 0;
static int       s_dropped = 0;

static uint32_t duration_ms() { return s_vi.duration_ms(); }

static uint32_t now_ms() {
    if (s_state != PS_PLAYING) return s_pos_ms;
    if (s_use_audio_clock) {
        uint32_t hz = s_a_hz ? s_a_hz : s_vi.arate;
        uint32_t played = hz ? (uint32_t)(s_a_frames * 1000ULL / hz) : 0;
        uint32_t t = s_pos_ms + (played > AUDIO_LATENCY_MS ? played - AUDIO_LATENCY_MS : 0);
        if (s_a_drained) {          // soundtrack over (video may be longer): wall clock from here
            s_pos_ms = t;
            s_wall0 = millis();
            s_use_audio_clock = false;
        }
        return t;
    }
    return s_pos_ms + (millis() - s_wall0);
}

static void audio_seek_reader(uint32_t ms) {
    s_a_next = 0; s_a_next_off = 0; s_a_hdr_done = false;
    if (!s_vi.has_audio || s_vi.ams.n == 0) return;
    int lo = 0, hi = s_vi.ams.n - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (s_vi.ams.v[mid] <= ms) lo = mid; else hi = mid - 1; }
    s_a_next = lo;
    bool pcm = s_vi.acodec == AC_PCM16LE || s_vi.acodec == AC_PCM16BE || s_vi.acodec == AC_PCM8;
    if (pcm && s_vi.abytes_per_sec && ms > s_vi.ams.v[lo]) {
        int frame = (s_vi.acodec == AC_PCM8 ? 1 : 2) * s_vi.ach;
        uint32_t inside = (uint32_t)((uint64_t)(ms - s_vi.ams.v[lo]) * s_vi.abytes_per_sec / 1000);
        inside -= inside % frame;
        if (inside < s_vi.asize.v[lo]) s_a_next_off = inside;
    }
}

static uint32_t ring_put(const uint8_t *src, uint32_t n) {
    uint32_t space = RING_SIZE - (s_rh - s_rt);
    if (n > space) n = space;
    uint32_t h = s_rh % RING_SIZE, first = RING_SIZE - h;
    if (first > n) first = n;
    memcpy(s_ring + h, src, first);
    if (n > first) memcpy(s_ring, src + first, n - first);
    s_rh += n;
    return n;
}

// Keeps ~0.8 s of audio queued. Bounded work per call.
static void audio_fill() {
    if (!s_vi.has_audio || !s_ring || s_a_eos) return;
    uint32_t bps = s_vi.abytes_per_sec ? s_vi.abytes_per_sec : 24000;
    uint32_t want = bps * 8 / 10;
    if (want < 16384) want = 16384;
    if (want > RING_SIZE - 8192) want = RING_SIZE - 8192;
    uint32_t budget = 48 * 1024;
    static uint8_t *chunk = (uint8_t *)heap_caps_malloc(4096, (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); // PSRAM: keep internal RAM for radios
    if (!chunk) return;
    while ((s_rh - s_rt) < want && budget) {
        if (s_a_next >= (uint32_t)s_vi.aoff.n) { s_a_eos = true; break; }
        uint32_t psize = s_vi.asize.v[s_a_next];
        if (s_vi.acodec == AC_AAC && !s_a_hdr_done) {
            if (RING_SIZE - (s_rh - s_rt) < 7) break;
            uint8_t h[7];
            adts_header(s_vi, psize, h);
            ring_put(h, 7);
            s_a_hdr_done = true;
        }
        uint32_t left = psize - s_a_next_off;
        uint32_t space = RING_SIZE - (s_rh - s_rt);
        uint32_t n = left < sizeof(chunk) ? left : sizeof(chunk);
        if (n > space) n = space;
        if (n > budget) n = budget;
        if (n == 0 && left) break;
        if (n) {
            s_vf.seek(s_vi.aoff.v[s_a_next] + s_a_next_off);
            int got = s_vf.read(chunk, n);
            if (got <= 0) { s_a_eos = true; break; }
            ring_put(chunk, got);
            budget -= got;
            s_a_next_off += got;
        }
        if (s_a_next_off >= psize) { s_a_next++; s_a_next_off = 0; s_a_hdr_done = false; }
    }
}

// ---- video frames -------------------------------------------------------------

static bool mjpeg_decode(int idx) {
    while (idx > 0 && s_vi.vsize.v[idx] == 0) idx--;          // empty chunk = repeat previous
    uint32_t sz = s_vi.vsize.v[idx];
    if (!sz) return false;
    if (sz > s_cbuf_sz) {
        uint8_t *nb = (uint8_t *)PS_REALLOC(s_cbuf, sz + 1024);
        if (!nb) return false;
        s_cbuf = nb; s_cbuf_sz = sz + 1024;
    }
    s_vf.seek(s_vi.voff.v[idx]);
    if ((uint32_t)s_vf.read(s_cbuf, sz) != sz) return false;
    return img_decode_jpeg_into(s_cbuf, sz, s_shift, s_frame, s_fw, s_fh);
}

// Decodes forward (from a keyframe if needed) until frame `idx` is in
// s_frame, or the time budget runs out. Returns the frame now shown.
static int h264_advance_to(int idx, uint32_t budget_ms) {
    uint32_t t0 = millis();
    if (idx < s_dec_next - 1 || idx < 0) {
        // backwards (or a seek): restart at the keyframe before idx
        s_dec_next = s_vi.keyframe_at_or_before(idx);
        h264_feed_header(s_h264, s_vi);
    } else {
        // far behind: jump to a later keyframe instead of decoding the gap
        int k = s_vi.keyframe_at_or_before(idx);
        if (k > s_dec_next) { s_dropped += k - s_dec_next; s_dec_next = k; h264_feed_header(s_h264, s_vi); }
    }
    bool got = false;
    while (s_dec_next <= idx) {
        size_t n = h264_read_sample(s_vf, s_vi, s_dec_next, s_h264, &s_cbuf, &s_cbuf_sz);
        s_dec_next++;
        if (n && s_h264.decode(s_h264.buf, n)) got = true;
        if (millis() - t0 > budget_ms) break;
    }
    if (got && s_h264.pic) {
        i420_to_rgb565(s_h264.pic, s_h264.pw, s_h264.ph, s_fw, s_fh, s_frame, s_fw);
        return s_dec_next - 1;
    }
    return s_shown;
}

static void show_frame(int idx, uint32_t budget_ms) {
    if (idx < 0 || idx >= s_vi.voff.n || !s_frame) return;
    if (s_vi.vcodec == VC_H264) s_shown = h264_advance_to(idx, budget_ms);
    else if (idx != s_shown && mjpeg_decode(idx)) s_shown = idx;
}

static void start_playing(uint32_t from_ms) {
    if (s_vi.raw_done || !s_vi.raw_mjpeg) { if (from_ms >= duration_ms()) from_ms = 0; }
    audio_flush_wait();
    // H.264 can only (re)start at a keyframe - unless the decoder is
    // already right there (plain resume after pause).
    if (s_vi.vcodec == VC_H264) {
        int idx = (int)((uint64_t)from_ms * 1000 / s_vi.frame_us);
        bool continuous = s_shown >= 0 && idx >= s_shown && idx <= s_dec_next;
        if (!continuous) {
            int k = s_vi.keyframe_at_or_before(idx);
            from_ms = (uint32_t)((uint64_t)k * s_vi.frame_us / 1000);
            s_dec_next = k;
            h264_feed_header(s_h264, s_vi);
        }
    }
    s_pos_ms = from_ms;
    s_use_audio_clock = false;
    if (s_vi.has_audio && s_ring && from_ms < s_vi.audio_ms) {
        s_rh = 0;
        s_rt = 0;
        s_a_eos = false; s_a_drained = false;
        audio_seek_reader(from_ms);
        audio_fill();
        s_use_audio_clock = true;
        s_a_run = true;
    }
    s_wall0 = millis();
    s_state = PS_PLAYING;
}

static void pause_playing() {
    if (s_state != PS_PLAYING) return;
    s_pos_ms = now_ms();
    s_state = PS_PAUSED;
    audio_flush_wait();
}

static void seek_preview(uint32_t ms) {
    uint32_t d = duration_ms();
    if (ms > d) ms = d;
    s_pos_ms = ms;
    int idx = (int)((uint64_t)ms * 1000 / s_vi.frame_us);
    if (idx >= s_vi.voff.n) idx = s_vi.voff.n - 1;
    if (s_vi.vcodec == VC_H264) idx = s_vi.keyframe_at_or_before(idx);   // keyframes decode instantly
    show_frame(idx, 120);
}

static void unload() {
    audio_flush_wait();
    if (s_a_task) {
        s_a_task_quit = true;
        uint32_t t0 = millis();
        while (s_a_task && millis() - t0 < 500) vTaskDelay(2);
    }
    audio_pcm_restore_default();
    s_h264.close();
    if (s_vf) s_vf.close();
    s_vi.clear();
    free(s_cbuf); s_cbuf = nullptr; s_cbuf_sz = 0;
    free(s_frame); s_frame = nullptr;
    free(s_ring); s_ring = nullptr;
    s_loaded = false;
}

static bool load(const char *path) {
    s_err[0] = 0;
    if (!sd_is_mounted()) { strcpy(s_err, "No SD card"); return false; }
    s_vf = SD.open(path, FILE_READ);
    if (!s_vf) { strcpy(s_err, "Can't open file"); return false; }
    if (!video_parse(s_vf, path, s_vi, false, s_err, sizeof(s_err))) return false;

    if (s_vi.vcodec == VC_H264) {
        if (!s_h264.open()) { strcpy(s_err, "Not enough memory for H.264"); return false; }
        s_shift = 0;
        s_fw = s_vi.w; s_fh = s_vi.h;
        s_frame = (uint16_t *)PS_ALLOC((size_t)s_fw * s_fh * 2);
    } else {
        // MJPEG: smallest power-of-two decode that still covers the screen width
        s_shift = 0;
        while (s_shift < 3 && (s_vi.w >> (s_shift + 1)) >= LCD_WIDTH && (s_vi.h >> (s_shift + 1)) >= 120) s_shift++;
        int fw = (s_vi.w + (1 << s_shift) - 1) >> s_shift, fh = (s_vi.h + (1 << s_shift) - 1) >> s_shift;
        s_fw = (fw + 15) & ~15; s_fh = (fh + 15) & ~15;      // JPEGDEC writes whole MCUs
        s_frame = (uint16_t *)PS_ALLOC((size_t)s_fw * s_fh * 2);
    }
    if (!s_frame) { strcpy(s_err, "Video too large"); return false; }
    memset(s_frame, 0, (size_t)s_fw * s_fh * 2);

    if (s_vi.has_audio) {
        s_ring = (uint8_t *)PS_ALLOC(RING_SIZE);
        if (s_ring && !s_a_task) {
            s_a_task_quit = false;
            // minimp3/helix keep big scratch buffers on the stack: 32 KB stack
            // in PSRAM (as hal_audio.cpp's mp3_task), TCB internal. Core 0,
            // next to the display sender; it mostly blocks on I2S.
            static StaticTask_t s_tcb;
            static StackType_t *s_stack = nullptr;
            if (!s_stack) s_stack = (StackType_t *)PS_ALLOC(32768);
            if (s_stack)
                s_a_task = xTaskCreateStaticPinnedToCore(audio_task, "vid_audio", 32768, nullptr, 6, s_stack, &s_tcb, 0);
        }
        if (!s_ring || !s_a_task) { s_vi.has_audio = false; free(s_ring); s_ring = nullptr; }
    }
    s_loaded = true;
    return true;
}

// =============================================================================
//  Screen
// =============================================================================

static const float RING_START = 220, RING_SWEEP = 280;   // progress ring, clock degrees
static bool s_prev = false, s_scrub = false, s_btn = false;
static uint32_t s_last_tap = 0;
static bool s_was_playing_before_scrub = false;

static void fmt_time(uint32_t ms, char *b, size_t n) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void vp_create() {
    audio_stop_playback();         // the music player shares the speaker
    s_state = PS_PAUSED;
    s_pos_ms = 0; s_shown = -1; s_dec_next = 0; s_dropped = 0;
    s_prev = s_scrub = s_btn = false;
    s_overlay_until = millis() + 2500;
    s_fill = false;
    if (load(s_path)) {
        show_frame(0, 400);
        start_playing(0);
    }
}

static void vp_destroy() { unload(); }

static void vp_tick() {
    if (!s_loaded) return;
    if (s_vi.raw_mjpeg && !s_vi.raw_done) video_index_more(s_vf, s_vi, 48 * 1024);   // keep indexing in the background
    if (s_state != PS_PLAYING) return;
    audio_fill();
    uint32_t t = now_ms();
    int idx = (int)((uint64_t)t * 1000 / s_vi.frame_us);
    if (idx >= s_vi.voff.n) {
        bool more_coming = s_vi.raw_mjpeg && !s_vi.raw_done;
        if (!more_coming && (!s_use_audio_clock || s_a_drained)) {
            s_state = PS_ENDED;
            s_pos_ms = duration_ms();
            audio_flush_wait();
            s_overlay_until = 0;
        }
        idx = s_vi.voff.n - 1;
    }
    if (s_vi.vcodec == VC_H264 || idx != s_shown) show_frame(idx, 45);
}

// Blit the decoded frame scaled into the circle.
static void draw_frame(Arduino_GFX *g) {
    if (!s_frame || s_shown < 0) return;
    float a = (float)s_vi.w / s_vi.h;
    float dw, dh;
    if (s_fill) { dw = LCD_WIDTH; dh = LCD_WIDTH / a; if (dh > LCD_HEIGHT) { dh = LCD_HEIGHT; dw = dh * a; } }
    else { float d = sqrtf(a * a + 1); dw = LCD_WIDTH * a / d; dh = LCD_WIDTH / d; }   // inscribed in the circle
    int w = (int)dw, h = (int)dh;
    if (w < 2 || h < 2) return;
    int x0 = (LCD_WIDTH - w) / 2, y0 = (LCD_HEIGHT - h) / 2;
    int sw = s_vi.w >> s_shift, sh = s_vi.h >> s_shift;     // pixels actually holding the picture
    static uint16_t row[LCD_WIDTH];
    static int16_t xmap[LCD_WIDTH];
    for (int x = 0; x < w; x++) xmap[x] = (int16_t)((int64_t)x * sw / w);
    for (int y = 0; y < h; y++) {
        int sy = (int)((int64_t)y * sh / h);
        const uint16_t *src = s_frame + (size_t)sy * s_fw;
        for (int x = 0; x < w; x++) row[x] = src[xmap[x]];
        g->draw16bitRGBBitmap(x0, y0 + y, row, w, 1);
    }
}

static const char *codec_label() {
    static char b[32];
    const char *v = s_vi.vcodec == VC_H264 ? "H.264" : "MJPEG";
    const char *a = !s_vi.has_audio ? "no sound" :
                    s_vi.acodec == AC_AAC ? "AAC" : s_vi.acodec == AC_MP3 ? "MP3" : "PCM";
    snprintf(b, sizeof(b), "%s  %s", v, a);
    return b;
}

static void vp_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (!s_loaded) {
        ui_text_center(CX, CY - 40, COLOR_BAD, "Can't play video", 2);
        ui_text_center(CX, CY - 8, COLOR_TEXT_DIM, s_err[0] ? s_err : "Unknown error", 1);
        ui_text_center(CX, CY + 20, COLOR_TEXT_DIM, "Plays: MJPEG (.avi .mov .mp4 .mjpeg)", 1);
        ui_text_center(CX, CY + 34, COLOR_TEXT_DIM, "H.264 baseline up to 640x480", 1);
        ui_text_center(CX, CY + 48, COLOR_TEXT_DIM, "sound: AAC, MP3, PCM", 1);
        return;
    }
    draw_frame(g);

    uint32_t now = millis();
    bool show = s_state != PS_PLAYING || now < s_overlay_until || s_scrub;
    uint32_t t = now_ms(), d = duration_ms();
    float f = d ? (float)t / d : 0;
    if (f > 1) f = 1;
    int thick = show ? 10 : 4;
    ui_edge_ring(RING_START, RING_SWEEP, ui_dim(COLOR_TEXT, 0.25f), thick, 4);
    if (f > 0.002f) ui_edge_ring(RING_START, RING_SWEEP * f, COLOR_ACCENT, thick, 4);
    if (!show) return;

    int kx, ky;
    ui_polar(RING_START + RING_SWEEP * f, ui_screen_radius() - 9, &kx, &ky);
    g->fillCircle(kx, ky, s_scrub ? 14 : 10, COLOR_TEXT);
    g->fillCircle(kx, ky, s_scrub ? 7 : 5, COLOR_ACCENT);

    const char *name = strrchr(s_path, '/');
    name = name ? name + 1 : s_path;
    char title[21];                   // ~250 px at size 2: fits the top of the circle
    strncpy(title, name, sizeof(title) - 1); title[sizeof(title) - 1] = 0;
    if (strlen(name) > sizeof(title) - 1) strcpy(title + sizeof(title) - 4, "...");
    ui_text_center(CX, 44, COLOR_TEXT, title, 2);

    bool playing = s_state == PS_PLAYING;
    g->fillCircle(CX, CY, 46, s_btn ? COLOR_ACCENT : COLOR_PANEL);
    if (playing) {
        g->fillRoundRect(CX - 17, CY - 20, 11, 40, 3, COLOR_TEXT);
        g->fillRoundRect(CX + 6, CY - 20, 11, 40, 3, COLOR_TEXT);
    } else if (s_state == PS_ENDED) {
        ui_arc(CX, CY, 22, 6, 60, 280, COLOR_TEXT);              // replay
        g->fillTriangle(CX + 10, CY - 26, CX + 26, CY - 22, CX + 16, CY - 8, COLOR_TEXT);
    } else {
        g->fillTriangle(CX - 13, CY - 22, CX - 13, CY + 22, CX + 24, CY, COLOR_TEXT);
    }

    char a[16], b[16], line[48];
    fmt_time(t, a, sizeof(a)); fmt_time(d, b, sizeof(b));
    bool growing = s_vi.raw_mjpeg && !s_vi.raw_done;       // raw MJPEG still being indexed
    snprintf(line, sizeof(line), "%s / %s%s", a, b, growing ? "+" : "");
    ui_text_center(CX, LCD_HEIGHT - 66, COLOR_TEXT, line, 2);
    snprintf(line, sizeof(line), "%dx%d  %s%s", s_vi.w, s_vi.h, codec_label(), s_fill ? "  fill" : "");
    ui_text_center(CX, LCD_HEIGHT - 44, COLOR_TEXT_DIM, line, 1);
}

static void scrub_to(int x, int y) {
    float a = ui_angle_of(x, y);
    float dd = a - RING_START; if (dd < 0) dd += 360;
    float f = dd / RING_SWEEP;
    if (f > 1) f = (dd - RING_SWEEP < (360 - RING_SWEEP) / 2) ? 1 : 0;   // in the gap: nearest end
    seek_preview((uint32_t)(f * duration_ms()));
}

static void vp_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_prev;
    s_prev = pressed;
    if (!s_loaded) { if (edge) ui_pop_screen(); return; }
    uint32_t now = millis();
    bool controls = s_state != PS_PLAYING || now < s_overlay_until;

    if (edge) {
        int r = ui_radius_of(x, y);
        int dx = x - CX, dy = y - CY;
        if (controls && r >= ui_screen_radius() - 48 && ui_angle_in_arc(ui_angle_of(x, y), RING_START, RING_SWEEP)) {
            s_scrub = true;
            s_was_playing_before_scrub = s_state == PS_PLAYING;
            if (s_state == PS_PLAYING) pause_playing();
            scrub_to(x, y);
            touch_cancel_swipes();
            return;
        }
        if (controls && dx * dx + dy * dy <= 56 * 56) { s_btn = true; return; }
        if (now - s_last_tap < 320) { s_fill = !s_fill; s_last_tap = 0; return; }   // double tap: fit <-> fill
        s_last_tap = now;
        s_overlay_until = controls && s_state == PS_PLAYING ? 0 : now + 3000;
        return;
    }
    if (pressed) {
        if (s_scrub) { scrub_to(x, y); touch_cancel_swipes(); s_overlay_until = now + 3000; }
        return;
    }
    if (s_scrub) {
        s_scrub = false;
        touch_cancel_swipes();
        if (s_was_playing_before_scrub || s_state == PS_ENDED) start_playing(s_pos_ms);
        s_overlay_until = now + 2500;
        return;
    }
    if (s_btn) {
        s_btn = false;
        if (s_state == PS_PLAYING) pause_playing();
        else start_playing(s_state == PS_ENDED ? 0 : s_pos_ms);
        s_overlay_until = now + 2500;
    }
}

static void vp_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_UP) ui_pop_screen();
}

Screen video_player_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    vp_create, vp_draw, vp_touch, vp_tick, vp_destroy, vp_gesture,
    0,
    true,    // suppress_idle: don't dim/sleep mid-film
    false, false,
    true,    // no_drag_back: the left edge is part of the seek ring
    true,    // hide_status
};

void video_open(const char *path) {
    strncpy(s_path, path, sizeof(s_path) - 1);
    s_path[sizeof(s_path) - 1] = 0;
    ui_push(&video_player_screen);
}
