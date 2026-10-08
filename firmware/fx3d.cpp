#include "fx3d.h"
#include "config.h"
#include "board_pins.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

uint16_t fx_lerp565(uint16_t a, uint16_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    int r = ar + (int)((br - ar) * t), g = ag + (int)((bg - ag) * t), bl = ab + (int)((bb - ab) * t);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

float fx_smooth(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

float fx_ease_out_back(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c1 = 1.4f, c3 = c1 + 1.0f;
    float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

float fx_ease_in_cubic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * t;
}

// ---- Card ----------------------------------------------------------------------------

void fx_card(uint16_t *dst, const uint16_t *src, float pitch, float z, float scale, float bright, float squash) {
    const int W = LCD_WIDTH, H = LCD_HEIGHT;
    const float cx = W * 0.5f, cy = H * 0.5f, f = FX_FOCAL;
    const float cp = cosf(pitch), sp = sinf(pitch);
    const float sv = scale * squash;   // vertical scale of the card
    if (bright > 1.0f) bright = 1.0f;
    if (scale < 0.001f || sv < 0.0005f || bright <= 0.0f) {
        memset(dst, 0, (size_t)W * H * 2);
        return;
    }
    for (int y = 0; y < H; y++) {
        uint16_t *out = dst + (size_t)y * W;
        float yp = (float)y + 0.5f - cy;
        float den = f * cp - yp * sp;
        float v = 0.0f, w = 0.0f;
        bool hit = false;
        if (den > 1e-3f) {
            v = yp * (f + z) / (sv * den);       // card row (centred, source px)
            w = f + z + sv * v * sp;             // depth of that row
            hit = w > 1.0f && v >= -cy && v < cy;
        }
        if (!hit) { memset(out, 0, (size_t)W * 2); continue; }
        const uint16_t *row = src + (size_t)(int)(v + cy) * W;

        // Horizontal: src col = cx + (x - cx + 0.5) * k, k constant along the row.
        float k = w / (f * scale);
        float u0 = cx + (0.5f - cx) * k;       // at x = 0
        int x0 = 0, x1 = W - 1;
        if (u0 < 0.0f) x0 = (int)ceilf(-u0 / k);
        float ulast = (W - 0.05f - u0) / k;
        if (ulast < x1) x1 = (int)floorf(ulast);

        // Brightness: further rows a bit darker, on top of `bright`.
        float depth = f / w;
        if (depth > 1.0f) depth = 1.0f;
        uint32_t kb = (uint32_t)(32.0f * bright * (0.55f + 0.45f * depth) + 0.5f);
        if (kb > 32) kb = 32;

        if (x0 > x1) { memset(out, 0, (size_t)W * 2); continue; }
        if (x0 > 0) memset(out, 0, (size_t)x0 * 2);
        int32_t uf = (int32_t)((u0 + x0 * k) * 65536.0f), step = (int32_t)(k * 65536.0f);
        if (uf < 0) uf = 0;
        if (kb >= 32) {
            for (int x = x0; x <= x1; x++, uf += step) out[x] = row[uf >> 16];
        } else {
            for (int x = x0; x <= x1; x++, uf += step) out[x] = fx_scale565(row[uf >> 16], kb);
        }
        if (x1 < W - 1) memset(out + x1 + 1, 0, (size_t)(W - 1 - x1) * 2);
    }
}

void fx_card_rim(Arduino_GFX *g, float pitch, float z, float scale, float squash, uint16_t color, float bright) {
    if (!g || bright <= 0.0f || scale <= 0.0f) return;
    const float cx = LCD_WIDTH * 0.5f, cy = LCD_HEIGHT * 0.5f, f = FX_FOCAL, R = LCD_WIDTH * 0.5f - 6.0f;
    const float cp = cosf(pitch), sp = sinf(pitch), sv = scale * squash;
    const int N = 72;
    int px = 0, py = 0;
    for (int i = 0; i <= N; i++) {
        float a = i * (6.2831853f / N);
        float u = cosf(a) * R, v = sinf(a) * R;
        float X = scale * u, Y = sv * v * cp, Z = z + sv * v * sp;
        float w = f + Z;
        if (w < 1.0f) return;
        int x = (int)(cx + f * X / w), y = (int)(cy + f * Y / w);
        if (i) {
            // nearer parts of the rim brighter
            float near = f / w;
            uint32_t k = (uint32_t)(32.0f * bright * (near > 1.0f ? 1.0f : near * near));
            uint16_t c = fx_scale565(color, k > 32 ? 32 : k);
            g->drawLine(px, py, x, y, c);
            g->drawLine(px, py + 1, x, y + 1, fx_scale565(c, 16));
        }
        px = x;
        py = y;
    }
}

// ---- Beat ------------------------------------------------------------------------------

void fx_beat_reset(FxBeat &b) { memset(&b, 0, sizeof(b)); }

void fx_beat_update(FxBeat &b, int level, bool playing, uint32_t now) {
    uint32_t dt = b.last_ms ? now - b.last_ms : 16;
    if (dt > 100) dt = 100;
    uint32_t prev = b.last_ms;
    b.last_ms = now;
    float decay = expf(-(float)dt / 180.0f);
    b.kick *= decay;
    if (!playing) {
        b.energy *= decay;
        for (int i = 0; i < 8; i++) b.bands[i] *= decay;
        return;
    }

    float lvl;
    if (level < 0) {
        // Not our audio: a made-up 120 BPM pulse (kick every 500 ms, a
        // softer off-beat, a little jitter) so the visual has a rhythm.
        uint32_t ph = now % 500;
        if (prev && now / 500 != prev / 500) b.kick = 1.0f;
        float env = expf(-(float)ph / 90.0f) + 0.35f * expf(-(float)((now + 250) % 500) / 70.0f);
        uint32_t h = (now / 70) * 2654435761u;
        lvl = 0.30f + 0.45f * env + 0.12f * (float)((h >> 24) & 0xFF) / 255.0f;
    } else {
        lvl = (float)level / 100.0f;
        b.avg = b.avg * 0.96f + lvl * 0.04f;
        if (lvl > b.avg * 1.25f + 0.05f && now - b.last_beat_ms > 220) {
            b.kick = 1.0f;
            b.last_beat_ms = now;
        }
    }
    if (lvl > 1.0f) lvl = 1.0f;
    b.energy += (lvl - b.energy) * (lvl > b.energy ? 0.5f : 0.15f);

    // Spread the energy over 8 "bands": each wobbles at its own rate, the
    // low ones also jump with the kick. (The watch has the level, not an
    // FFT - this looks like a spectrum without pretending to be one.)
    float t = (float)now * 0.001f;
    for (int i = 0; i < 8; i++) {
        float osc = 0.5f + 0.5f * sinf(t * (2.1f + i * 1.37f) + i * 1.9f) * sinf(t * (0.7f + i * 0.31f) + i);
        float target = b.energy * (0.25f + 0.75f * osc) * 1.6f;
        if (i < 3) target *= 0.6f + 0.9f * b.kick;
        if (target > 1.0f) target = 1.0f;
        b.bands[i] += (target - b.bands[i]) * (target > b.bands[i] ? 0.55f : 0.18f);
    }
}

// ---- Spectrum ring ----------------------------------------------------------------------

#define EQ_SPIKES 56

void fx_eq_ring_draw(Arduino_GFX *g, int cx, int cy, float inner_r, float max_len, const FxBeat &b, uint32_t now,
                     uint16_t cold, uint16_t hot, int pass) {
    if (!g) return;
    const float f = FX_FOCAL, cam = f;
    float t = (float)now * 0.001f;
    float yaw = t * 0.35f, tilt = 0.90f + 0.04f * sinf(t * 0.5f);   // plane seen from ~38 deg above
    float cyw = cosf(yaw), syw = sinf(yaw), ct = cosf(tilt), st = sinf(tilt);
    float r_in = inner_r * (1.0f + 0.05f * b.kick);

    for (int i = 0; i < EQ_SPIKES; i++) {
        float a = i * (6.2831853f / EQ_SPIKES);
        // Bands mirrored round the ring: bass at front/back, treble at the sides.
        int band = (int)(fabsf(sinf(a)) * 7.99f);
        float e = b.bands[band];
        float len = 3.0f + max_len * (1.1f * e + 0.25f * b.kick);
        if (len > max_len * 1.25f) len = max_len * 1.25f;

        float ca = cosf(a + yaw), sa = sinf(a + yaw);
        float pts[2][3];
        for (int k = 0; k < 2; k++) {
            float r = k ? r_in + len : r_in;
            float x = ca * r, z = sa * r;                     // on the ring's plane
            float y2 = -z * ct, z2 = z * st;                  // plane tilted toward the viewer
            float sc = f / (cam + z2);
            pts[k][0] = cx + x * sc;
            pts[k][1] = cy + y2 * sc;
            pts[k][2] = z2;
        }
        bool back = pts[0][2] > 0;
        if (back != (pass == 0)) continue;
        float near = 0.5f - pts[0][2] / (2.0f * r_in);
        if (near < 0) near = 0;
        if (near > 1) near = 1;
        uint32_t kb = (uint32_t)(14 + 18 * near);
        uint16_t base = fx_scale565(cold, kb);
        uint16_t tip = fx_scale565(fx_lerp565(cold, hot, 0.2f + 0.8f * e), kb);
        int x0 = (int)pts[0][0], y0 = (int)pts[0][1], x1 = (int)pts[1][0], y1 = (int)pts[1][1];
        int xm = (x0 + x1) / 2, ym = (y0 + y1) / 2;
        // two-tone spike, 2-3 px wide (wider in front)
        for (int w = 0; w < (near > 0.55f ? 3 : 2); w++) {
            int dx = (w == 1), dy = (w == 2);
            g->drawLine(x0 + dx, y0 + dy, xm + dx, ym + dy, base);
            g->drawLine(xm + dx, ym + dy, x1 + dx, y1 + dy, tip);
        }
        if (e > 0.5f) g->fillCircle(x1, y1, near > 0.5f ? 2 : 1, fx_scale565(COLOR_TEXT, kb));   // peak cap
    }

    // A thin orbit of dots just inside the spikes, pulsing with the kick.
    float ro = r_in - 7.0f;
    for (int i = 0; i < 48; i++) {
        float a = i * (6.2831853f / 48) - yaw * 1.6f;
        float x = cosf(a) * ro, z = sinf(a) * ro;
        float y2 = -z * ct, z2 = z * st;
        if ((z2 > 0) != (pass == 0)) continue;
        float sc = f / (cam + z2);
        float near = 0.5f - z2 / (2.0f * ro);
        uint32_t kb = (uint32_t)(6 + 14 * near + 12 * b.kick);
        g->fillRect((int)(cx + x * sc), (int)(cy + y2 * sc), 2, 2, fx_scale565(hot, kb > 32 ? 32 : kb));
    }
}
