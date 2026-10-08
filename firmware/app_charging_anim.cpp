/*
 * app_charging_anim.cpp
 * "Charging started" animation: a solid 3D lightning bolt spins in and
 * settles facing you, while sparks spiral up into it along a helix and a
 * tilted ring of light expands around it; then the battery level fades in.
 * ~1.6 s, then it pops back to whatever was showing underneath.
 *
 * Triggered by the .ino on the rising edge of power_is_charging() - this
 * screen just plays once. The bolt is a flat polygon extruded in depth,
 * flat-shaded per face (fx3d.h camera) and drawn back to front.
 */
#include "app_charging_anim.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "fx3d.h"
#include "hal_power.h"
#include <Arduino_GFX_Library.h>
#include <math.h>

#define ANIM_DURATION_MS 1600

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2 - 16;
static uint32_t s_start_ms;

// Bolt outline (x right, y down, unit size) and its triangulation.
static const float BOLT[6][2] = {
    { 0.18f, -1.00f }, { -0.58f, 0.14f }, { -0.06f, 0.14f },
    { -0.22f, 1.00f }, { 0.58f, -0.16f }, { 0.06f, -0.16f },
};
static const uint8_t BOLT_TRI[4][3] = { {0, 1, 2}, {0, 2, 5}, {2, 3, 4}, {2, 4, 5} };

struct V3 { float x, y, z; };
struct Face { int16_t x[4], y[4]; uint8_t n; float z; uint16_t c; };

static void charging_anim_create() { s_start_ms = millis(); }

static V3 xform(V3 p, float cy_, float sy_, float cp, float sp) {
    float x1 = p.x * cy_ + p.z * sy_, z1 = -p.x * sy_ + p.z * cy_;
    float y2 = p.y * cp - z1 * sp, z2 = p.y * sp + z1 * cp;
    return { x1, y2, z2 };
}

static void project(V3 p, int16_t &sx, int16_t &sy) {
    float sc = FX_FOCAL / (FX_FOCAL + p.z);
    sx = (int16_t)(CX + p.x * sc);
    sy = (int16_t)(CY + p.y * sc);
}

static uint16_t shade(uint16_t base, V3 n) {
    // towards the light: upper left, in front of the screen (-z)
    const float lx = -0.45f, ly = -0.55f, lz = -0.70f;
    float d = n.x * lx + n.y * ly + n.z * lz;
    float k = 0.28f + 0.72f * (d > 0 ? d : 0);
    return fx_scale565(base, (uint32_t)(32 * (k > 1 ? 1 : k)));
}

static void draw_sparks(Arduino_GFX *g, float t, bool front) {
    // 36 sparks on a 3-turn helix, rising and closing in on the bolt.
    for (int i = 0; i < 36; i++) {
        float ph = fmodf(t * 1.6f + i / 36.0f, 1.0f);   // each spark's own 0..1 trip
        float a = ph * 6.2831853f * 3.0f + i * 0.9f;
        float r = 150.0f * (1.0f - ph) + 18.0f;
        float y = 150.0f - ph * 260.0f;
        float z = sinf(a) * r;
        if ((z < 0) != front) continue;
        float x = cosf(a) * r;
        float sc = FX_FOCAL / (FX_FOCAL + z);
        float b = (1.0f - ph) * (ph < 0.1f ? ph * 10 : 1) * (t < 0.85f ? 1.0f : (1.0f - t) / 0.15f);
        uint16_t c = fx_scale565(fx_lerp565(COLOR_GOOD, COLOR_TEXT, ph), (uint32_t)(32 * (b < 0 ? 0 : b)));
        int s = z < 0 ? 2 : 1;
        g->fillCircle((int)(CX + x * sc), (int)(CY + y * sc), s, c);
    }
}

static void charging_anim_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    float t = (float)(millis() - s_start_ms) / ANIM_DURATION_MS;
    if (t > 1.0f) t = 1.0f;

    // Tilted ring of light expanding from the bolt.
    for (int k = 0; k < 2; k++) {
        float rt = t * 1.3f - k * 0.25f;
        if (rt <= 0 || rt >= 1) continue;
        float rr = 40 + 190 * fx_smooth(rt);
        uint16_t c = fx_scale565(COLOR_GOOD, (uint32_t)(28 * (1 - rt)));
        int16_t px = 0, py = 0;
        for (int i = 0; i <= 64; i++) {
            float a = i * 6.2831853f / 64;
            V3 p = xform({ cosf(a) * rr, 0, sinf(a) * rr }, 1, 0, cosf(1.2f), sinf(1.2f));
            p.y += 60;
            int16_t sx, sy;
            project(p, sx, sy);
            if (i) g->drawLine(px, py, sx, sy, c);
            px = sx;
            py = sy;
        }
    }

    draw_sparks(g, t, false);

    // The bolt: spins in (2.5 turns) and settles facing front with a wobble.
    float s = fx_ease_out_back(fx_smooth(t / 0.7f) * 0.98f + 0.02f);
    float yaw = (1.0f - s) * 6.2831853f * 2.5f + 0.38f + 0.12f * sinf(t * 5.0f);   // rests turned a little
    float pitch = 0.22f * (1.0f - fx_smooth(t / 0.8f));
    float size = 74.0f * (0.35f + 0.65f * fx_smooth(t / 0.35f)), depth = 16.0f;
    float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);

    V3 v[12];
    for (int i = 0; i < 6; i++) {
        v[i] = xform({ BOLT[i][0] * size, BOLT[i][1] * size, -depth }, cyw, syw, cp, sp);
        v[i + 6] = xform({ BOLT[i][0] * size, BOLT[i][1] * size, depth }, cyw, syw, cp, sp);
    }
    int16_t sx[12], sy[12];
    for (int i = 0; i < 12; i++) project(v[i], sx[i], sy[i]);

    Face faces[14];
    int nf = 0;
    const uint16_t bolt_c = COLOR_WARN;
    // caps
    for (int cap = 0; cap < 2; cap++) {
        V3 n = xform({ 0, 0, cap ? 1.0f : -1.0f }, cyw, syw, cp, sp);
        for (int k = 0; k < 4; k++) {
            Face &f = faces[nf++];
            f.n = 3;
            float z = 0;
            for (int j = 0; j < 3; j++) {
                int idx = BOLT_TRI[k][j] + cap * 6;
                f.x[j] = sx[idx];
                f.y[j] = sy[idx];
                z += v[idx].z;
            }
            f.z = z / 3;
            f.c = n.z < 0 ? shade(bolt_c, n) : 0;   // back-facing: skip
        }
    }
    // sides
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        float ex = BOLT[j][0] - BOLT[i][0], ey = BOLT[j][1] - BOLT[i][1];
        float len = sqrtf(ex * ex + ey * ey);
        V3 n = xform({ -ey / len, ex / len, 0 }, cyw, syw, cp, sp);   // outward (outline runs anticlockwise on screen)
        Face &f = faces[nf++];
        f.n = 4;
        int q[4] = { i, j, j + 6, i + 6 };
        float z = 0;
        for (int k = 0; k < 4; k++) {
            f.x[k] = sx[q[k]];
            f.y[k] = sy[q[k]];
            z += v[q[k]].z;
        }
        f.z = z / 4;
        f.c = shade(fx_lerp565(bolt_c, COLOR_ACCENT3, 0.35f), n);
    }
    // back to front
    for (int a = 1; a < nf; a++)
        for (int b = a; b > 0 && faces[b - 1].z < faces[b].z; b--) {
            Face tmp = faces[b];
            faces[b] = faces[b - 1];
            faces[b - 1] = tmp;
        }
    for (int i = 0; i < nf; i++) {
        Face &f = faces[i];
        if (!f.c) continue;
        g->fillTriangle(f.x[0], f.y[0], f.x[1], f.y[1], f.x[2], f.y[2], f.c);
        if (f.n == 4) g->fillTriangle(f.x[0], f.y[0], f.x[2], f.y[2], f.x[3], f.y[3], f.c);
    }

    draw_sparks(g, t, true);

    // Label + level fade in once the bolt has settled.
    float lt = fx_smooth((t - 0.45f) / 0.3f);
    if (lt > 0.02f) {
        uint32_t k = (uint32_t)(32 * lt);
        ui_text_center(LCD_WIDTH / 2, CY + 118, fx_scale565(COLOR_TEXT, k), "Charging", 2);
        int pct = power_get_battery_percent();
        if (pct >= 0) {
            char b[8];
            snprintf(b, sizeof(b), "%d%%", pct);
            ui_text_center(LCD_WIDTH / 2, CY + 152, fx_scale565(COLOR_GOOD, k), b, 2);
        }
    }
}

static void charging_anim_tick() {
    if (millis() - s_start_ms >= ANIM_DURATION_MS) {
        ui_pop_screen();
    }
}

Screen charging_anim_screen = {
    nullptr, GESTURE_MODE_FREE,
    16,   // 60 fps while it plays
    charging_anim_create, charging_anim_draw, nullptr, charging_anim_tick, nullptr, nullptr,
    0, false, false,
    true, // no_transition - an overlay with its own animation
};
