#include "video_container.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef VIDEO_HOST_TEST
#define VC_REALLOC(p, n) realloc((p), (n))
#define VC_ALLOC(n) malloc(n)
#else
#include <esp_heap_caps.h>
#define VC_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define VC_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif

static const int MAX_FRAMES = 200000;          // ~2 h at 24 fps
static const int MAX_AUDIO_PACKETS = 600000;   // ~3.7 h of 44.1 kHz AAC
static const int H264_MAX_PIXELS = 640 * 480;  // tinyH264 manages ~10 fps here on the S3

bool U32Vec::push(uint32_t x) {
    if (n == cap) {
        int nc = cap ? cap * 2 : 1024;
        uint32_t *nv = (uint32_t *)VC_REALLOC(v, (size_t)nc * 4);
        if (!nv) return false;
        v = nv; cap = nc;
    }
    v[n++] = x;
    return true;
}
void U32Vec::clear() { free(v); v = nullptr; n = cap = 0; }

void VidInfo::clear() {
    voff.clear(); vsize.clear(); vkey.clear();
    aoff.clear(); asize.clear(); ams.clear();
    free(avc_hdr); avc_hdr = nullptr; avc_hdr_len = 0;
    *this = VidInfo();
}

int VidInfo::keyframe_at_or_before(int idx) const {
    if (vkey.n == 0) return idx < 0 ? 0 : idx;
    int lo = 0, hi = vkey.n - 1, best = (int)vkey.v[0];
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if ((int)vkey.v[mid] <= idx) { best = (int)vkey.v[mid]; lo = mid + 1; } else hi = mid - 1;
    }
    return best;
}

static uint32_t le32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t le16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static uint32_t be32(const uint8_t *b) { return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
static uint16_t be16(const uint8_t *b) { return (b[0] << 8) | b[1]; }

static void set_err(char *e, size_t n, const char *m) { if (e && n) { strncpy(e, m, n - 1); e[n - 1] = 0; } }

static const uint32_t AAC_RATES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

// AudioSpecificConfig -> ADTS fields. HE-AAC (SBR, AOT 5/29) is signalled
// implicitly: ADTS carries the core AAC-LC config and the decoder finds the
// SBR extension in the stream.
static void parse_asc(VidInfo &vi, const uint8_t *a, int n) {
    if (n < 2) return;
    int aot = a[0] >> 3;
    int fi = ((a[0] & 7) << 1) | (a[1] >> 7);
    int ch = (a[1] >> 3) & 15;
    if (aot == 5 || aot == 29) aot = 2;      // SBR/PS on top of AAC-LC
    if (aot < 1 || aot > 4) aot = 2;
    vi.aac_aot = (uint8_t)aot;
    if (fi < 13) { vi.aac_freq_idx = (uint8_t)fi; if (!vi.arate) vi.arate = AAC_RATES[fi]; }
    if (ch >= 1 && ch <= 7) { vi.aac_chan_cfg = (uint8_t)ch; if (!vi.ach) vi.ach = ch > 2 ? 2 : ch; }
}

void adts_header(const VidInfo &vi, size_t payload, uint8_t h[7]) {
    size_t len = payload + 7;
    int prof = vi.aac_aot - 1;
    h[0] = 0xFF;
    h[1] = 0xF1;                                            // MPEG-4, no CRC
    h[2] = (uint8_t)((prof << 6) | (vi.aac_freq_idx << 2) | ((vi.aac_chan_cfg >> 2) & 1));
    h[3] = (uint8_t)(((vi.aac_chan_cfg & 3) << 6) | ((len >> 11) & 3));
    h[4] = (uint8_t)((len >> 3) & 0xFF);
    h[5] = (uint8_t)(((len & 7) << 5) | 0x1F);
    h[6] = 0xFC;
}

size_t avcc_to_annexb(const uint8_t *in, size_t len, int L, uint8_t *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i + L <= len) {
        uint32_t n = 0;
        for (int k = 0; k < L; k++) n = (n << 8) | in[i + k];
        i += L;
        if (n == 0 || i + n > len || o + 4 + n > cap) return o;
        out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 1;
        memcpy(out + o, in + i, n);
        o += n; i += n;
    }
    return o;
}

// Peek at the start of an Annex-B chunk: does it contain an IDR/SPS NAL?
static bool annexb_is_key(File &f, uint32_t off, uint32_t size) {
    uint8_t b[96];
    uint32_t n = size < sizeof(b) ? size : sizeof(b);
    f.seek(off);
    if ((uint32_t)f.read(b, n) != n) return false;
    for (uint32_t i = 0; i + 3 < n; i++) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            int t = b[i + 3] & 31;
            if (t == 5 || t == 7) return true;
            if (t == 1) return false;
        }
    }
    return false;
}

static bool is_h264_fourcc(const char *u) {   // u = upper-cased fourcc
    return !strcmp(u, "H264") || !strcmp(u, "X264") || !strcmp(u, "AVC1") || !strcmp(u, "DAVC") || !strcmp(u, "VSSH");
}
static bool is_mjpeg_fourcc(const char *u) {
    return !strcmp(u, "MJPG") || !strcmp(u, "JPEG") || !strcmp(u, "AVRN") || !strcmp(u, "DMB1") ||
           !strcmp(u, "MJPA") || !strcmp(u, "AVDJ");
}

// =============================================================================
//  AVI
// =============================================================================

static bool avi_parse(File &f, VidInfo &vi, bool poster_only, char *err, size_t el) {
    uint8_t h[12];
    f.seek(0);
    if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) { set_err(err, el, "Not an AVI file"); return false; }
    uint32_t fsize = f.size(), riff_end = 8 + le32(h + 4);
    if (riff_end > fsize || riff_end < 12) riff_end = fsize;

    uint32_t movi_fcc = 0, movi_end = 0, idx1_pos = 0, idx1_size = 0, avih_us = 0;
    int vs = -1, as = -1, stream = -1;
    uint32_t v_scale = 0, v_rate = 0, a_avg = 0;
    uint16_t afmt = 0; int abits = 16;
    char vfcc[5] = {0};

    uint32_t pos = 12;
    while (pos + 8 <= riff_end) {
        f.seek(pos);
        uint8_t c[12];
        if (f.read(c, 8) != 8) break;
        uint32_t sz = le32(c + 4);
        if (!memcmp(c, "LIST", 4)) {
            f.read(c + 8, 4);
            if (!memcmp(c + 8, "hdrl", 4)) {
                uint32_t p = pos + 12, end = pos + 8 + sz;
                while (p + 8 <= end) {
                    f.seek(p);
                    uint8_t s[8];
                    if (f.read(s, 8) != 8) break;
                    uint32_t ssz = le32(s + 4);
                    if (!memcmp(s, "avih", 4)) {
                        uint8_t a[40];
                        if (f.read(a, 40) == 40) { avih_us = le32(a); vi.w = le32(a + 32); vi.h = le32(a + 36); }
                    } else if (!memcmp(s, "LIST", 4)) {
                        uint8_t t[4];
                        f.read(t, 4);
                        if (!memcmp(t, "strl", 4)) {
                            stream++;
                            uint32_t q = p + 12, qend = p + 8 + ssz;
                            char type[5] = {0};
                            uint32_t scale = 0, rate = 0;
                            while (q + 8 <= qend) {
                                f.seek(q);
                                uint8_t k[8];
                                if (f.read(k, 8) != 8) break;
                                uint32_t ksz = le32(k + 4);
                                if (!memcmp(k, "strh", 4)) {
                                    uint8_t sh[28];
                                    if (f.read(sh, 28) == 28) { memcpy(type, sh, 4); scale = le32(sh + 20); rate = le32(sh + 24); }
                                } else if (!memcmp(k, "strf", 4)) {
                                    if (!memcmp(type, "vids", 4) && vs < 0) {
                                        uint8_t bi[20];
                                        if (f.read(bi, 20) == 20) {
                                            vs = stream; v_scale = scale; v_rate = rate;
                                            vi.w = (int)le32(bi + 4);
                                            int32_t hh = (int32_t)le32(bi + 8);
                                            vi.h = hh < 0 ? -hh : hh;
                                            memcpy(vfcc, bi + 16, 4);
                                        }
                                    } else if (!memcmp(type, "auds", 4) && as < 0) {
                                        uint8_t wf[64];
                                        int got = f.read(wf, ksz < 64 ? ksz : 64);
                                        if (got >= 16) {
                                            afmt = le16(wf);
                                            vi.ach = le16(wf + 2);
                                            vi.arate = le32(wf + 4);
                                            a_avg = le32(wf + 8);
                                            abits = le16(wf + 14);
                                            bool ok = false;
                                            if (afmt == 1 && (abits == 16 || abits == 8)) { vi.acodec = abits == 16 ? AC_PCM16LE : AC_PCM8; ok = true; }
                                            else if (afmt == 0x55) { vi.acodec = AC_MP3; ok = true; }
                                            else if (afmt == 0xFF && got >= 20) {            // raw AAC + ASC in cbSize extra
                                                int cb = le16(wf + 16);
                                                if (cb >= 2 && 18 + cb <= got) { parse_asc(vi, wf + 18, cb); vi.acodec = AC_AAC; ok = true; }
                                            }
                                            if (ok && vi.ach >= 1 && vi.arate >= 8000) {
                                                as = stream;
                                                if (vi.ach > 2) vi.ach = 2;
                                                if (vi.acodec == AC_PCM16LE || vi.acodec == AC_PCM8)
                                                    vi.abytes_per_sec = vi.arate * le16(wf + 2) * (abits / 8);
                                            } else vi.acodec = AC_NONE;
                                        }
                                    }
                                }
                                q += 8 + ksz + (ksz & 1);
                            }
                        }
                    }
                    p += 8 + ssz + (ssz & 1);
                }
            } else if (!memcmp(c + 8, "movi", 4)) {
                movi_fcc = pos + 8;
                movi_end = pos + 8 + sz;
                if (movi_end > riff_end) movi_end = riff_end;
                if (poster_only) break;
            }
        } else if (!memcmp(c, "idx1", 4)) {
            idx1_pos = pos + 8;
            idx1_size = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (vs < 0 || !movi_fcc || vi.w <= 0 || vi.h <= 0) { set_err(err, el, "No video stream"); return false; }
    memcpy(vi.fourcc, vfcc, 4); vi.fourcc[4] = 0;
    char u[5]; for (int i = 0; i < 4; i++) u[i] = (char)toupper((unsigned char)vfcc[i]); u[4] = 0;
    if (is_mjpeg_fourcc(u)) vi.vcodec = VC_MJPEG;
    else if (is_h264_fourcc(u)) vi.vcodec = VC_H264;
    else { char m[48]; snprintf(m, sizeof(m), "Video codec %.4s not supported", vfcc); set_err(err, el, m); return false; }
    if (vi.vcodec == VC_H264 && vi.w * vi.h > H264_MAX_PIXELS) { set_err(err, el, "H.264 too large (max 640x480)"); return false; }

    if (v_scale && v_rate) vi.frame_us = (uint32_t)((uint64_t)v_scale * 1000000ULL / v_rate);
    else if (avih_us) vi.frame_us = avih_us;
    if (vi.frame_us < 8000 || vi.frame_us > 1000000) vi.frame_us = 41667;
    vi.has_audio = as >= 0 && !poster_only;

    char vt0 = '0' + vs / 10, vt1 = '0' + vs % 10;
    char at0 = '0' + (as < 0 ? 9 : as / 10), at1 = '0' + (as < 0 ? 9 : as % 10);
    uint64_t abytes = 0;
    uint32_t a_rate_bytes = vi.abytes_per_sec ? vi.abytes_per_sec : (a_avg ? a_avg : 16000);
    auto add_audio = [&](uint32_t off, uint32_t sz) {
        if (vi.aoff.n >= MAX_AUDIO_PACKETS) return;
        vi.aoff.push(off); vi.asize.push(sz);
        vi.ams.push((uint32_t)(abytes * 1000 / a_rate_bytes));
        abytes += sz;
    };

    bool indexed = false;
    if (idx1_pos && idx1_size >= 16 && !poster_only) {
        uint8_t *buf = (uint8_t *)malloc(16 * 128);
        if (buf) {
            uint32_t base = 0;
            bool base_known = false;
            uint32_t n = idx1_size / 16;
            for (uint32_t i = 0; i < n; i += 128) {
                uint32_t cnt = n - i < 128 ? n - i : 128;
                f.seek(idx1_pos + i * 16);
                if ((uint32_t)f.read(buf, cnt * 16) != cnt * 16) break;
                for (uint32_t k = 0; k < cnt; k++) {
                    const uint8_t *e = buf + k * 16;
                    uint32_t flags = le32(e + 4), off = le32(e + 8), sz = le32(e + 12);
                    bool isv = e[0] == vt0 && e[1] == vt1 && e[2] == 'd' && (e[3] == 'c' || e[3] == 'b');
                    bool isa = vi.has_audio && e[0] == at0 && e[1] == at1 && e[2] == 'w' && e[3] == 'b';
                    if (!isv && !isa) continue;
                    if (!base_known) {
                        uint8_t t[4];
                        uint32_t save = f.position();
                        f.seek(movi_fcc + off); f.read(t, 4);
                        if (!memcmp(t, e, 4)) base = movi_fcc;
                        else { f.seek(off); f.read(t, 4); base = !memcmp(t, e, 4) ? 0 : movi_fcc; }
                        f.seek(save);
                        base_known = true;
                    }
                    if (isv) {
                        if (vi.voff.n >= MAX_FRAMES) continue;
                        if (vi.vcodec == VC_H264 && (flags & 0x10) && sz) vi.vkey.push(vi.voff.n);
                        vi.voff.push(base + off + 8); vi.vsize.push(sz);
                    } else add_audio(base + off + 8, sz);
                }
            }
            free(buf);
            indexed = vi.voff.n > 0;
        }
    }
    if (!indexed) {
        vi.voff.clear(); vi.vsize.clear(); vi.vkey.clear(); vi.aoff.clear(); vi.asize.clear(); vi.ams.clear();
        abytes = 0;
        uint32_t p = movi_fcc + 4;
        while (p + 8 <= movi_end) {
            f.seek(p);
            uint8_t c[12];
            if (f.read(c, 8) != 8) break;
            uint32_t sz = le32(c + 4);
            if (!memcmp(c, "LIST", 4)) { p += 12; continue; }       // 'rec ' groups
            if (c[0] == vt0 && c[1] == vt1 && c[2] == 'd' && (c[3] == 'c' || c[3] == 'b')) {
                if (vi.vcodec == VC_H264 && sz && annexb_is_key(f, p + 8, sz)) vi.vkey.push(vi.voff.n);
                vi.voff.push(p + 8); vi.vsize.push(sz);
                if (poster_only && sz && (vi.vcodec == VC_MJPEG || vi.vkey.n)) break;
                if (vi.voff.n >= MAX_FRAMES) break;
            } else if (vi.has_audio && c[0] == at0 && c[1] == at1 && c[2] == 'w' && c[3] == 'b') {
                add_audio(p + 8, sz);
            }
            p += 8 + sz + (sz & 1);
        }
    }
    if (vi.has_audio && vi.aoff.n == 0) vi.has_audio = false;
    if (vi.has_audio) vi.audio_ms = (uint32_t)(abytes * 1000 / a_rate_bytes);
    if (vi.voff.n == 0) { set_err(err, el, "No video frames"); return false; }
    if (vi.vcodec == VC_H264 && vi.vkey.n == 0) vi.vkey.push(0);
    vi.ok = true;
    return true;
}

// =============================================================================
//  MP4 / MOV
// =============================================================================

struct Box { uint64_t s = 0, e = 0; };   // content start/end

static bool box_child(File &f, Box parent, const char *name, Box *out) {
    uint64_t p = parent.s;
    while (p + 8 <= parent.e) {
        f.seek((uint32_t)p);
        uint8_t h[16];
        if (f.read(h, 8) != 8) return false;
        uint64_t sz = be32(h), hdr = 8;
        if (sz == 1) { if (f.read(h + 8, 8) != 8) return false; sz = ((uint64_t)be32(h + 8) << 32) | be32(h + 12); hdr = 16; }
        else if (sz == 0) sz = parent.e - p;
        if (sz < hdr) return false;
        if (!memcmp(h + 4, name, 4)) { out->s = p + hdr; out->e = p + sz; if (out->e > parent.e) out->e = parent.e; return true; }
        p += sz;
    }
    return false;
}

static bool box_path(File &f, Box from, const char *const *names, int n, Box *out) {
    Box b = from;
    for (int i = 0; i < n; i++) if (!box_child(f, b, names[i], &b)) return false;
    *out = b;
    return true;
}

struct TrackTables {
    Box stbl;
    uint32_t timescale = 0;
    uint8_t entry[128];        // first sample description entry (head)
    int entry_len = 0;
    Box entry_box;             // whole entry, for child boxes
};

static bool read_track(File &f, Box trak, char handler[5], TrackTables &t) {
    static const char *const MDIA_HDLR[] = {"mdia", "hdlr"};
    Box hd;
    if (!box_path(f, trak, MDIA_HDLR, 2, &hd)) return false;
    uint8_t b[12];
    f.seek((uint32_t)hd.s); f.read(b, 12);
    memcpy(handler, b + 8, 4); handler[4] = 0;
    static const char *const MDIA_MDHD[] = {"mdia", "mdhd"};
    Box md;
    if (box_path(f, trak, MDIA_MDHD, 2, &md)) {
        uint8_t m[24]; f.seek((uint32_t)md.s); f.read(m, 24);
        t.timescale = m[0] == 1 ? be32(m + 20) : be32(m + 12);
    }
    static const char *const STBL[] = {"mdia", "minf", "stbl"};
    if (!box_path(f, trak, STBL, 3, &t.stbl)) return false;
    Box sd;
    if (!box_child(f, t.stbl, "stsd", &sd)) return false;
    // stsd: version/flags(4) count(4) then entries
    uint8_t eh[8];
    f.seek((uint32_t)sd.s + 8);
    if (f.read(eh, 8) != 8) return false;
    uint32_t esz = be32(eh);
    t.entry_box.s = sd.s + 8; t.entry_box.e = sd.s + 8 + esz;
    if (t.entry_box.e > sd.e) t.entry_box.e = sd.e;
    f.seek((uint32_t)t.entry_box.s);
    t.entry_len = f.read(t.entry, esz < sizeof(t.entry) ? esz : sizeof(t.entry));
    return t.entry_len >= 16;
}

// Sample offsets/sizes from stsz/stz2 + stco/co64 + stsc. max = cap on samples.
static bool build_samples(File &f, Box stbl, U32Vec &off, U32Vec &size, uint32_t max) {
    Box z, c, sc;
    if (!box_child(f, stbl, "stsz", &z)) return false;
    bool co64 = false;
    if (!box_child(f, stbl, "stco", &c)) { if (!box_child(f, stbl, "co64", &c)) return false; co64 = true; }
    if (!box_child(f, stbl, "stsc", &sc)) return false;
    uint8_t b[12];
    f.seek((uint32_t)z.s); f.read(b, 12);
    uint32_t fixed = be32(b + 4), count = be32(b + 8);
    if (count > max) count = max;
    U32Vec sizes, chunks, scf, scn;
    uint8_t blk[1024];
    if (fixed) { for (uint32_t i = 0; i < count; i++) if (!sizes.push(fixed)) break; }
    else for (uint32_t i = 0; i < count; i += 256) {
        uint32_t n = count - i < 256 ? count - i : 256;
        f.seek((uint32_t)(z.s + 12 + (uint64_t)i * 4)); f.read(blk, n * 4);
        for (uint32_t k = 0; k < n; k++) sizes.push(be32(blk + k * 4));
    }
    f.seek((uint32_t)c.s); f.read(b, 8);
    uint32_t nch = be32(b + 4);
    int es = co64 ? 8 : 4;
    for (uint32_t i = 0; i < nch; i += 128) {
        uint32_t n = nch - i < 128 ? nch - i : 128;
        f.seek((uint32_t)(c.s + 8 + (uint64_t)i * es)); f.read(blk, n * es);
        for (uint32_t k = 0; k < n; k++) chunks.push(co64 ? be32(blk + k * 8 + 4) : be32(blk + k * 4));
    }
    f.seek((uint32_t)sc.s); f.read(b, 8);
    uint32_t nsc = be32(b + 4);
    for (uint32_t i = 0; i < nsc; i++) { uint8_t e[12]; if (f.read(e, 12) != 12) break; scf.push(be32(e)); scn.push(be32(e + 4)); }
    uint32_t si = 0;
    int e = 0;
    for (int ch = 0; ch < chunks.n && si < (uint32_t)sizes.n; ch++) {
        while (e + 1 < scf.n && (uint32_t)(ch + 1) >= scf.v[e + 1]) e++;
        uint32_t per = scn.n ? scn.v[e] : 1, o = chunks.v[ch];
        for (uint32_t k = 0; k < per && si < (uint32_t)sizes.n; k++, si++) {
            off.push(o); size.push(sizes.v[si]); o += sizes.v[si];
        }
    }
    sizes.clear(); chunks.clear(); scf.clear(); scn.clear();
    return off.n > 0;
}

// Per-sample start times from stts (in timescale units -> ms).
static void sample_times(File &f, Box stbl, uint32_t timescale, int count, U32Vec &ms, uint64_t *total_ticks) {
    Box t;
    uint64_t tick = 0;
    if (box_child(f, stbl, "stts", &t) && timescale) {
        uint8_t b[8];
        f.seek((uint32_t)t.s); f.read(b, 8);
        uint32_t n = be32(b + 4);
        uint8_t blk[1024];
        for (uint32_t i = 0; i < n && ms.n < count; i += 128) {
            uint32_t m = n - i < 128 ? n - i : 128;
            f.seek((uint32_t)(t.s + 8 + (uint64_t)i * 8)); f.read(blk, m * 8);
            for (uint32_t k = 0; k < m && ms.n < count; k++) {
                uint32_t c = be32(blk + k * 8), d = be32(blk + k * 8 + 4);
                for (uint32_t j = 0; j < c && ms.n < count; j++) { ms.push((uint32_t)(tick * 1000 / timescale)); tick += d; }
            }
        }
    }
    while (ms.n < count) ms.push(ms.n ? ms.v[ms.n - 1] : 0);
    if (total_ticks) *total_ticks = tick;
}

// Finds a child box anywhere directly inside `b` after `skip` bytes (sample entries).
static bool entry_child(File &f, Box entry, uint32_t skip, const char *name, Box *out) {
    Box b; b.s = entry.s + skip; b.e = entry.e;
    if (b.s >= b.e) return false;
    return box_child(f, b, name, out);
}

// Walks an esds box: 03 ES_Descriptor -> 04 DecoderConfig (object type
// indication) -> 05 DecoderSpecificInfo (AAC AudioSpecificConfig, parsed
// into `vi` when given). Returns the OTI (0x40 AAC, 0x6B MP3, 0x6C JPEG...).
static int esds_oti(File &f, Box es, VidInfo *vi) {
    uint8_t d[128];
    f.seek((uint32_t)es.s);
    int len = f.read(d, (es.e - es.s) < sizeof(d) ? (int)(es.e - es.s) : (int)sizeof(d));
    int p = 4, oti = 0;    // skip version/flags
    while (p < len) {
        int tag = d[p++];
        int sz = 0;
        for (int k = 0; k < 4 && p < len; k++) { int b = d[p++]; sz = (sz << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
        if (tag == 3) { p += 2; if (p >= len) break; int fl = d[p++]; if (fl & 0x80) p += 2; if ((fl & 0x40) && p < len) p += 1 + d[p]; if (fl & 0x20) p += 2; continue; }
        if (tag == 4) { if (p < len) oti = d[p]; p += 13; continue; }
        if (tag == 5) { if (vi && p + sz <= len) parse_asc(*vi, d + p, sz); break; }
        p += sz;
    }
    return oti;
}

static bool mp4_video(File &f, VidInfo &vi, TrackTables &t, bool poster_only, char *err, size_t el) {
    memcpy(vi.fourcc, t.entry + 4, 4); vi.fourcc[4] = 0;
    vi.w = be16(t.entry + 32); vi.h = be16(t.entry + 34);
    char u[5]; for (int i = 0; i < 4; i++) u[i] = (char)toupper((unsigned char)vi.fourcc[i]); u[4] = 0;
    if (!strcmp(vi.fourcc, "mp4v")) {                  // generic MPEG-4 visual: codec is in esds
        Box es;
        int oti = entry_child(f, t.entry_box, 86, "esds", &es) ? esds_oti(f, es, nullptr) : 0;
        if (oti == 0x6C) { vi.vcodec = VC_MJPEG; strcpy(vi.fourcc, "jpeg"); }
        else { set_err(err, el, "MPEG-4 Part 2 video not supported"); return false; }
    } else if (is_mjpeg_fourcc(u)) vi.vcodec = VC_MJPEG;
    else if (!strcmp(vi.fourcc, "avc1") || !strcmp(vi.fourcc, "avc3")) {
        vi.vcodec = VC_H264;
        Box ac;
        if (!entry_child(f, t.entry_box, 86, "avcC", &ac)) { set_err(err, el, "H.264 without avcC"); return false; }
        uint32_t n = (uint32_t)(ac.e - ac.s);
        if (n < 7 || n > 4096) { set_err(err, el, "Bad avcC"); return false; }
        uint8_t *a = (uint8_t *)malloc(n);
        if (!a) return false;
        f.seek((uint32_t)ac.s); f.read(a, n);
        vi.h264_profile = a[1];
        vi.nal_len_size = (a[4] & 3) + 1;
        // SPS/PPS -> Annex-B
        vi.avc_hdr = (uint8_t *)malloc(n + 64);
        int o = 0;
        uint32_t p = 5;
        int nsps = a[p++] & 31;
        for (int i = 0; i < nsps && p + 2 <= n; i++) {
            uint32_t L = be16(a + p); p += 2;
            if (p + L > n || !vi.avc_hdr) break;
            vi.avc_hdr[o++] = 0; vi.avc_hdr[o++] = 0; vi.avc_hdr[o++] = 0; vi.avc_hdr[o++] = 1;
            memcpy(vi.avc_hdr + o, a + p, L); o += L; p += L;
        }
        if (p < n) {
            int npps = a[p++];
            for (int i = 0; i < npps && p + 2 <= n; i++) {
                uint32_t L = be16(a + p); p += 2;
                if (p + L > n || !vi.avc_hdr) break;
                vi.avc_hdr[o++] = 0; vi.avc_hdr[o++] = 0; vi.avc_hdr[o++] = 0; vi.avc_hdr[o++] = 1;
                memcpy(vi.avc_hdr + o, a + p, L); o += L; p += L;
            }
        }
        vi.avc_hdr_len = o;
        free(a);
        if (vi.h264_profile != 66) {
            char m[48];
            snprintf(m, sizeof(m), "H.264 %s profile not supported", vi.h264_profile == 77 ? "Main" : vi.h264_profile == 100 ? "High" : "this");
            set_err(err, el, m);
            return false;
        }
        if (vi.w * vi.h > H264_MAX_PIXELS) { set_err(err, el, "H.264 too large (max 640x480)"); return false; }
    } else if (!strcmp(vi.fourcc, "hvc1") || !strcmp(vi.fourcc, "hev1")) { set_err(err, el, "HEVC can't be decoded"); return false; }
    else { char m[48]; snprintf(m, sizeof(m), "Video codec %s not supported", vi.fourcc); set_err(err, el, m); return false; }

    if (!build_samples(f, t.stbl, vi.voff, vi.vsize, poster_only ? 1 : MAX_FRAMES)) { set_err(err, el, "MP4 index missing"); return false; }
    if (!poster_only) {
        U32Vec ms;
        uint64_t ticks = 0;
        sample_times(f, t.stbl, t.timescale, vi.voff.n, ms, &ticks);
        if (ticks && t.timescale && vi.voff.n) vi.frame_us = (uint32_t)(ticks * 1000000ULL / t.timescale / vi.voff.n);
        ms.clear();
        if (vi.vcodec == VC_H264) {
            Box ss;
            if (box_child(f, t.stbl, "stss", &ss)) {
                uint8_t b[8]; f.seek((uint32_t)ss.s); f.read(b, 8);
                uint32_t n = be32(b + 4);
                uint8_t blk[512];
                for (uint32_t i = 0; i < n; i += 128) {
                    uint32_t m = n - i < 128 ? n - i : 128;
                    f.seek((uint32_t)(ss.s + 8 + (uint64_t)i * 4)); f.read(blk, m * 4);
                    for (uint32_t k = 0; k < m; k++) { uint32_t s = be32(blk + k * 4); if (s >= 1 && (int)s <= vi.voff.n) vi.vkey.push(s - 1); }
                }
            }
        }
    }
    if (vi.vcodec == VC_H264 && vi.vkey.n == 0) vi.vkey.push(0);   // no stss = every sample is a sync sample; start is enough
    if (vi.frame_us < 8000 || vi.frame_us > 1000000) vi.frame_us = 41667;
    return true;
}

static void mp4_audio(File &f, VidInfo &vi, TrackTables &t) {
    char fcc[5]; memcpy(fcc, t.entry + 4, 4); fcc[4] = 0;
    int ver = be16(t.entry + 16);
    int ch = be16(t.entry + 24), bits = be16(t.entry + 26);
    uint32_t rate = be32(t.entry + 32) >> 16;
    uint32_t skip = 36 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
    vi.ach = ch > 2 ? 2 : (ch < 1 ? 1 : ch);
    vi.arate = rate;
    ACodec ac = AC_NONE;
    if (!strcmp(fcc, "mp4a")) {
        Box es;
        bool found = entry_child(f, t.entry_box, skip, "esds", &es);
        if (!found) { Box w; if (entry_child(f, t.entry_box, skip, "wave", &w)) found = box_child(f, w, "esds", &es); }
        int oti = 0;
        if (found) oti = esds_oti(f, es, &vi);
        if (found) {
            if (oti == 0x40 || oti == 0x66 || oti == 0x67 || oti == 0x68) ac = AC_AAC;
            else if (oti == 0x6B || oti == 0x69) ac = AC_MP3;
        }
    } else if (!strcmp(fcc, ".mp3") || !strcmp(fcc, "mp3 ")) ac = AC_MP3;
    else if (!strcmp(fcc, "sowt")) ac = bits == 8 ? AC_PCM8 : AC_PCM16LE;
    else if (!strcmp(fcc, "twos")) ac = bits == 8 ? AC_PCM8 : AC_PCM16BE;
    else if (!strcmp(fcc, "raw ")) ac = AC_PCM8;
    if (ac == AC_NONE || !vi.arate) return;
    vi.acodec = ac;

    if (ac == AC_AAC || ac == AC_MP3) {
        if (!build_samples(f, t.stbl, vi.aoff, vi.asize, MAX_AUDIO_PACKETS)) return;
        uint64_t ticks = 0;
        sample_times(f, t.stbl, t.timescale, vi.aoff.n, vi.ams, &ticks);
        vi.audio_ms = t.timescale ? (uint32_t)(ticks * 1000 / t.timescale) : 0;
    } else {
        // PCM: one entry per chunk (thousands of 1-frame "samples" otherwise)
        int bpf = (bits == 8 ? 1 : 2) * (ch < 1 ? 1 : ch);
        Box c, sc;
        bool co64 = false;
        if (!box_child(f, t.stbl, "stco", &c)) { if (!box_child(f, t.stbl, "co64", &c)) return; co64 = true; }
        if (!box_child(f, t.stbl, "stsc", &sc)) return;
        uint8_t b[12];
        f.seek((uint32_t)sc.s); f.read(b, 8);
        uint32_t nsc = be32(b + 4);
        U32Vec scf, scn;
        for (uint32_t i = 0; i < nsc; i++) { uint8_t e[12]; if (f.read(e, 12) != 12) break; scf.push(be32(e)); scn.push(be32(e + 4)); }
        f.seek((uint32_t)c.s); f.read(b, 8);
        uint32_t nch = be32(b + 4);
        uint64_t frames = 0;
        int e = 0;
        int es = co64 ? 8 : 4;
        for (uint32_t i = 0; i < nch; i++) {
            uint8_t o[8];
            f.seek((uint32_t)(c.s + 8 + (uint64_t)i * es)); f.read(o, es);
            while (e + 1 < scf.n && i + 1 >= scf.v[e + 1]) e++;
            uint32_t spc = scn.n ? scn.v[e] : 1;
            vi.aoff.push(co64 ? be32(o + 4) : be32(o));
            vi.asize.push(spc * bpf);
            vi.ams.push((uint32_t)(frames * 1000 / vi.arate));
            frames += spc;
        }
        scf.clear(); scn.clear();
        vi.abytes_per_sec = vi.arate * bpf;
        vi.audio_ms = (uint32_t)(frames * 1000 / vi.arate);
    }
    vi.has_audio = vi.aoff.n > 0;
}

static bool mp4_parse(File &f, VidInfo &vi, bool poster_only, char *err, size_t el) {
    Box file; file.s = 0; file.e = f.size();
    Box moov;
    if (!box_child(f, file, "moov", &moov)) { set_err(err, el, "MP4 without index (moov)"); return false; }
    bool have_video = false, have_audio = false;
    uint64_t p = moov.s;
    char vreason[48] = "";
    while (p < moov.e) {
        Box from; from.s = p; from.e = moov.e;
        Box trak;
        if (!box_child(f, from, "trak", &trak)) break;
        p = trak.e;
        TrackTables t;
        char hdl[5];
        if (!read_track(f, trak, hdl, t)) continue;
        if (!strcmp(hdl, "vide") && !have_video) {
            if (mp4_video(f, vi, t, poster_only, vreason, sizeof(vreason))) have_video = true;
            else { vi.voff.clear(); vi.vsize.clear(); vi.vkey.clear(); free(vi.avc_hdr); vi.avc_hdr = nullptr; }
        } else if (!strcmp(hdl, "soun") && !have_audio && !poster_only) {
            mp4_audio(f, vi, t);
            have_audio = vi.has_audio;
        }
    }
    if (!have_video) { set_err(err, el, vreason[0] ? vreason : "No video track"); return false; }
    vi.ok = vi.voff.n > 0;
    if (!vi.ok) set_err(err, el, "No video frames");
    return vi.ok;
}

// =============================================================================
//  Raw MJPEG
// =============================================================================

static uint32_t fps_from_name(const char *name) {
    const char *p = name;
    while ((p = strstr(p, "fps")) || (p = strstr(name, "FPS"))) {
        const char *q = p;
        while (q > name && isdigit((unsigned char)q[-1])) q--;
        if (q < p) { int v = atoi(q); if (v >= 1 && v <= 120) return (uint32_t)v; }
        p += 3;
        if (p >= name + strlen(name)) break;
    }
    return 24;
}

bool video_index_more(File &f, VidInfo &vi, uint32_t max_bytes) {
    if (!vi.raw_mjpeg || vi.raw_done) return true;
    // Heap, not stack: this runs on Arduino's loopTask (8 KB stack) under
    // the UI's own frames - a 4 KB local here overflowed it.
    static uint8_t *buf = nullptr;
    if (!buf) buf = (uint8_t *)malloc(4096 + 2);
    if (!buf) return vi.raw_done;
    uint32_t done = 0;
    while (done < max_bytes) {
        if (vi.raw_pos >= vi.raw_size) {
            if (vi.raw_cur != 0xFFFFFFFF && vi.raw_size > vi.raw_cur) { vi.voff.push(vi.raw_cur); vi.vsize.push(vi.raw_size - vi.raw_cur); }
            vi.raw_cur = 0xFFFFFFFF;
            vi.raw_done = true;
            return true;
        }
        f.seek(vi.raw_pos);
        int n = f.read(buf, 4096 + 2);
        if (n < 3) { vi.raw_pos = vi.raw_size; continue; }
        int scan = n - 2;
        for (int i = 0; i < scan; i++) {
            if (buf[i] == 0xFF && buf[i + 1] == 0xD8 && buf[i + 2] == 0xFF) {
                uint32_t at = vi.raw_pos + i;
                if (vi.raw_cur != 0xFFFFFFFF && vi.voff.n < MAX_FRAMES) { vi.voff.push(vi.raw_cur); vi.vsize.push(at - vi.raw_cur); }
                vi.raw_cur = at;
                i += 2;
            }
        }
        vi.raw_pos += scan;
        done += scan;
        if (n < 4096 + 2) vi.raw_pos = vi.raw_size;
    }
    return vi.raw_done;
}

static bool raw_mjpeg_parse(File &f, const char *name, VidInfo &vi, bool poster_only, char *err, size_t el) {
    vi.raw_mjpeg = true;
    vi.vcodec = VC_MJPEG;
    strcpy(vi.fourcc, "MJPG");
    vi.raw_size = f.size();
    vi.frame_us = 1000000 / fps_from_name(name);
    // Index enough to start (the rest happens while playing).
    for (int i = 0; i < 64 && vi.voff.n < (poster_only ? 1 : 3) && !vi.raw_done; i++) video_index_more(f, vi, 64 * 1024);
    if (vi.voff.n == 0) { set_err(err, el, "No JPEG frames found"); return false; }
    // dimensions from the first frame's SOF marker
    const int BL = 4096;
    uint8_t *b = (uint8_t *)malloc(BL);            // heap: see video_index_more()
    if (!b) { set_err(err, el, "Out of memory"); return false; }
    f.seek(vi.voff.v[0]);
    int n = f.read(b, BL);
    for (int i = 2; i + 9 < n;) {
        if (b[i] != 0xFF) { i++; continue; }
        uint8_t m = b[i + 1];
        if (m >= 0xC0 && m <= 0xC3) { vi.h = be16(b + i + 5); vi.w = be16(b + i + 7); break; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        i += 2 + be16(b + i + 2);
    }
    free(b);
    if (vi.w <= 0 || vi.h <= 0) { set_err(err, el, "Bad MJPEG frame"); return false; }
    vi.ok = true;
    return true;
}

// =============================================================================

static bool ends_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (b > a) return false;
    for (size_t i = 0; i < b; i++) if (tolower((unsigned char)s[a - b + i]) != suf[i]) return false;
    return true;
}

bool video_parse(File &f, const char *name, VidInfo &vi, bool poster_only, char *err, size_t el) {
    uint8_t m[12];
    f.seek(0);
    if (f.read(m, 12) < 8) { set_err(err, el, "Empty file"); return false; }
    bool ok;
    if (!memcmp(m, "RIFF", 4)) ok = avi_parse(f, vi, poster_only, err, el);
    else if (!memcmp(m + 4, "ftyp", 4) || !memcmp(m + 4, "moov", 4) || !memcmp(m + 4, "mdat", 4) ||
             !memcmp(m + 4, "wide", 4) || !memcmp(m + 4, "free", 4) || !memcmp(m + 4, "skip", 4)) ok = mp4_parse(f, vi, poster_only, err, el);
    else if (m[0] == 0xFF && m[1] == 0xD8) ok = raw_mjpeg_parse(f, name, vi, poster_only, err, el);
    else if (m[0] == 0x1A && m[1] == 0x45) { set_err(err, el, "MKV/WebM not supported"); ok = false; }
    else if (ends_ci(name, ".mjpeg") || ends_ci(name, ".mjpg")) ok = raw_mjpeg_parse(f, name, vi, poster_only, err, el);
    else { set_err(err, el, "Unknown video format"); ok = false; }
    if (ok && vi.has_audio && vi.acodec == AC_AAC && vi.ach < 1) vi.ach = 2;
    return ok;
}
