/*
 * app_charging_anim.cpp
 * "Charging started": a green wave rings out from the centre across the
 * screen that was showing, bending the UI as it passes (each pixel near
 * the wavefront is pushed along its radius), with a few sparks riding
 * the front and the battery level fading in and out. ~1 s, then it pops
 * back to that same screen.
 *
 * Triggered by the .ino on the rising edge of power_is_charging() - this
 * screen just plays once. The screen underneath is rendered once into a
 * PSRAM frame (ui_render_screen_below()); each frame copies it and
 * re-samples only the ring of pixels around the two wavefronts, so the
 * cost is one frame copy plus a thin annulus. Without spare PSRAM it
 * draws the rings on black instead.
 */
#include "app_charging_anim.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "fx3d.h"
#include "hal_power.h"
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#define ANIM_DURATION_MS 1000
#define SPARKS           40

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static uint32_t s_start_ms;
static uint16_t *s_src = nullptr;     // the screen underneath (PSRAM, kept)
static bool s_have_src = false;

struct Spark { float a, speed, size, delay; };
static Spark s_sparks[SPARKS];

static float frand(float lo, float hi) { return lo + (hi - lo) * (float)random(10001) / 10000.0f; }

static void charging_anim_create() {
    if (!s_src) s_src = (uint16_t *)heap_caps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_have_src = s_src && ui_render_screen_below(s_src);
    for (auto &p : s_sparks) p = { frand(0, 6.2831853f), frand(1.05f, 1.35f), frand(1.0f, 2.6f), frand(0.0f, 0.25f) };
    s_start_ms = millis();
}

// Re-samples the annulus |r - R| < W of fb from s_src, displacing each
// pixel by up to `amp` px along its radius (a sine over the band) and
// tinting the crest towards green by `tint` (0..1).
static void wave(uint16_t *fb, float R, float W, float amp, float tint) {
    float ro = R + W, ri = R - W;
    if (ro <= 0) return;
    int y0 = CY - (int)ro, y1 = CY + (int)ro;
    if (y0 < 0) y0 = 0;
    if (y1 > LCD_HEIGHT - 1) y1 = LCD_HEIGHT - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - CY;
        float xo2 = ro * ro - dy * dy;
        if (xo2 <= 0) continue;
        int xo = (int)sqrtf(xo2);
        int xi = (ri > 0 && fabsf(dy) < ri) ? (int)sqrtf(ri * ri - dy * dy) : 0;
        for (int side = 0; side < 2; side++) {
            int xa = side ? CX + xi : CX - xo, xb = side ? CX + xo : CX - xi;
            if (xa < 0) xa = 0;
            if (xb > LCD_WIDTH - 1) xb = LCD_WIDTH - 1;
            uint16_t *row = fb + (size_t)y * LCD_WIDTH;
            for (int x = xa; x <= xb; x++) {
                float dx = (float)x + 0.5f - CX;
                float r = sqrtf(dx * dx + dy * dy);
                float u = (r - R) / W;                 // -1..1 across the band
                if (u <= -1.0f || u >= 1.0f || r < 1.0f) continue;
                float env = 1.0f - u * u;
                float s = sinf(u * 3.14159265f);
                float rs = r - amp * s * env;           // sample from nearer/farther in
                int sx = CX + (int)(dx * rs / r), sy = CY + (int)(dy * rs / r);
                if (sx < 0 || sx >= LCD_WIDTH || sy < 0 || sy >= LCD_HEIGHT) continue;
                uint16_t c = s_have_src ? s_src[(size_t)sy * LCD_WIDTH + sx] : COLOR_BG;
                float k = tint * env * (0.35f + 0.65f * (s > 0 ? s : 0));
                if (k > 0.02f) c = fx_lerp565(c, COLOR_GOOD, k > 1 ? 1 : k);
                row[x] = c;
            }
        }
    }
}

static void charging_anim_draw() {
    Arduino_GFX *g = ui_gfx();
    uint16_t *fb = ui_framebuffer();
    if (!g || !fb) return;
    float t = (float)(millis() - s_start_ms) / ANIM_DURATION_MS;
    if (t > 1.0f) t = 1.0f;

    if (s_have_src) memcpy(fb, s_src, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);

    // Two fronts: the main one, and a softer echo a beat behind.
    const float RMAX = 360.0f;
    float R1 = RMAX * fx_ease_out_cubic(t);
    wave(fb, R1, 30.0f, 12.0f * (1.0f - t), 0.55f * (1.0f - t));
    float t2 = (t - 0.22f) / 0.78f;
    if (t2 > 0) wave(fb, RMAX * fx_ease_out_cubic(t2), 22.0f, 6.0f * (1.0f - t2), 0.3f * (1.0f - t2));

    // Sparks riding just ahead of the front.
    for (const Spark &p : s_sparks) {
        float tp = (t - p.delay) / (1.0f - p.delay);
        if (tp <= 0 || tp >= 1) continue;
        float r = RMAX * fx_ease_out_cubic(tp) * p.speed;
        int x = CX + (int)(cosf(p.a) * r), y = CY + (int)(sinf(p.a) * r);
        uint16_t c = fx_scale565(fx_lerp565(COLOR_GOOD, COLOR_TEXT, tp), (uint32_t)(32 * (1.0f - tp)));
        g->fillCircle(x, y, (int)(p.size * (1.0f - 0.5f * tp) + 0.5f), c);
    }

    // The level, in and out.
    float lt = fx_smooth(t / 0.25f) * (1.0f - fx_smooth((t - 0.7f) / 0.3f));
    if (lt > 0.03f) {
        int pct = power_get_battery_percent();
        char b[20];
        if (pct >= 0) snprintf(b, sizeof(b), "Charging  %d%%", pct);
        else snprintf(b, sizeof(b), "Charging");
        int w = ui_text_width(b, 2) + 32;
        const int py = 78;   // up top, clear of the clock
        g->fillRoundRect(CX - w / 2, py - 20, w, 40, 20, fx_scale565(COLOR_PANEL, (uint32_t)(32 * lt)));
        ui_text_center(CX, py, fx_scale565(COLOR_GOOD, (uint32_t)(32 * lt)), b, 2);
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
    false, // no_drag_back
    true,  // hide_status - the frame underneath already has it
};
