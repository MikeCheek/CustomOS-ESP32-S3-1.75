/*
 * anim_lock.cpp
 * The power-on, lock and unlock transitions (see anim_lock.h), all in
 * software 3D (fx3d.h).
 *
 * Lock and unlock work on the real screen: lock takes the last frame that
 * went to the panel, unlock and boot draw the screen underneath into a
 * PSRAM frame (ui_render_screen_below()). That frame is then shown as a
 * card in perspective - tilting away / swinging in - with fx_card(),
 * which maps each output row to one source row, so a full-screen warp is
 * cheap enough for 60 fps. The CRT line and dot, and the boot particles,
 * are drawn on top with ordinary GFX calls.
 *
 * Without spare PSRAM for the frame the card parts are skipped (the line
 * and particles still play).
 *
 * An ordinary Screen (see ui.h) driven by the normal render loop; it pops
 * itself when done.
 */
#include "anim_lock.h"
#include "config.h"
#include "board_pins.h"
#include "hal_sleep.h"
#include "hal_display.h"
#include "fx3d.h"
#include "diag.h"
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

enum AnimKind : uint8_t { ANIM_WAKE, ANIM_LOCK, ANIM_BOOT };

static const uint32_t DUR_WAKE = 560, DUR_LOCK = 620, DUR_BOOT = 2100;
static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

static AnimKind s_kind = ANIM_WAKE;
static bool s_sleep_after = false;
static uint32_t s_start_ms;
static uint16_t *s_frame = nullptr;     // the screen being animated (PSRAM, kept)
static bool s_have_frame = false;

void lock_anim_set_mode(bool waking, bool sleep_after) {
    s_kind = waking ? ANIM_WAKE : ANIM_LOCK;
    s_sleep_after = sleep_after;
}

void lock_anim_set_boot() {
    s_kind = ANIM_BOOT;
    s_sleep_after = false;
}

static uint32_t duration() { return s_kind == ANIM_BOOT ? DUR_BOOT : s_kind == ANIM_LOCK ? DUR_LOCK : DUR_WAKE; }

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// A glowing horizontal line (the CRT trace) of half-width `hw`.
static void crt_line(Arduino_GFX *g, int hw, float bright) {
    if (hw <= 0 || bright <= 0.0f) return;
    uint32_t k = (uint32_t)(32 * clamp01(bright));
    g->fillRect(CX - hw - 10, CY - 5, 2 * hw + 20, 11, fx_scale565(COLOR_ACCENT, k / 4));
    g->fillRect(CX - hw - 4, CY - 2, 2 * hw + 8, 5, fx_scale565(0xC6FF, k / 2));
    g->fillRect(CX - hw, CY - 1, 2 * hw, 2, fx_scale565(COLOR_TEXT, k));
}

static void crt_dot(Arduino_GFX *g, float r, float bright) {
    if (r <= 0.5f || bright <= 0.0f) return;
    uint32_t k = (uint32_t)(32 * clamp01(bright));
    g->fillCircle(CX, CY, (int)(r * 3), fx_scale565(COLOR_ACCENT, k / 5));
    g->fillCircle(CX, CY, (int)(r * 1.8f), fx_scale565(0xC6FF, k / 2));
    g->fillCircle(CX, CY, (int)r, fx_scale565(COLOR_TEXT, k));
}

// ---- Boot particles -------------------------------------------------------------------

#define BOOT_N 220
struct BootP {
    float sx, sy, sz;      // start: far away, all around
    float px, py, pz;      // on the sphere
    float rx, ry, rz;      // on the ring
    uint16_t color;
    uint8_t size;
};
static BootP *s_boot = nullptr;   // PSRAM

static float frand(float lo, float hi) { return lo + (hi - lo) * (float)random(10001) / 10000.0f; }

static void boot_init() {
    if (!s_boot) s_boot = (BootP *)heap_caps_malloc(sizeof(BootP) * BOOT_N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_boot) return;
    static const uint16_t pal[4] = { COLOR_ACCENT, 0xC6FF /* lilac */, COLOR_ACCENT2, COLOR_TEXT };
    const float ga = 3.14159265f * (3.0f - sqrtf(5.0f));
    for (int i = 0; i < BOOT_N; i++) {
        BootP &p = s_boot[i];
        // start on a big random shell, mostly behind the screen
        float th = frand(0, 6.2831853f), u = frand(-1, 1), s = sqrtf(1 - u * u), R = frand(420, 700);
        p.sx = cosf(th) * s * R;
        p.sy = u * R;
        p.sz = fabsf(sinf(th) * s * R) + 200;
        // Fibonacci sphere, radius 110
        float y = 1.0f - 2.0f * (i + 0.5f) / BOOT_N, r = sqrtf(1 - y * y);
        p.px = cosf(ga * i) * r * 110;
        p.py = y * 110;
        p.pz = sinf(ga * i) * r * 110;
        // ring in the x/z plane, radius 150, a little thickness
        float a = i * (6.2831853f / BOOT_N);
        float rr = 150 + frand(-8, 8);
        p.rx = cosf(a) * rr;
        p.ry = frand(-5, 5);
        p.rz = sinf(a) * rr;
        p.color = pal[i % 7 == 0 ? 3 : i % 3];
        p.size = random(100) < 80 ? 1 : 2;
    }
}

static void boot_draw(Arduino_GFX *g, uint16_t *fb, uint32_t el) {
    float tf = (float)el;
    float warp = clamp01((tf - 1350.0f) / 750.0f);   // 0..1 in the last phase

    // The watch face flies in from the distance under the warp.
    if (warp > 0.0f && s_have_frame && fb) {
        float e = fx_smooth(warp);
        float pitch = 0.55f * (1 - e), z = 700.0f * (1 - e), scale = 0.55f + 0.45f * e;
        fx_card(fb, s_frame, pitch, z, scale, e);
        fx_card_rim(g, pitch, z, scale, 1.0f, 0xC6FF, fx_smooth(warp * 2.0f) * (1.0f - fx_smooth((warp - 0.6f) / 0.4f)));
    }
    if (!s_boot) return;

    float form = fx_smooth(tf / 750.0f);                   // stream in -> sphere
    float fold = fx_smooth((tf - 700.0f) / 500.0f);        // sphere -> ring
    float yaw = tf * 0.0032f, pitch = 0.18f + 0.95f * fold; // the ring tilts toward us
    float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    float spread = 1.0f + 5.0f * fx_ease_in_cubic(warp);   // ring blasts outward
    float spread_prev = 1.0f + 5.0f * fx_ease_in_cubic(clamp01((tf - 1350.0f - 40.0f) / 750.0f));
    float fade = 1.0f - fx_smooth(warp * 1.2f);
    const float f = FX_FOCAL, cam = 600.0f;

    for (int i = 0; i < BOOT_N; i++) {
        BootP &p = s_boot[i];
        float x = p.sx + (p.px - p.sx) * form, y = p.sy + (p.py - p.sy) * form, z = p.sz + (p.pz - p.sz) * form;
        x += (p.rx - x) * fold;
        y += (p.ry - y) * fold;
        z += (p.rz - z) * fold;
        float pts[2][2];
        float depth = 0;
        bool visible = true;
        for (int k = 0; k < 2 && visible; k++) {
            float m = k ? spread : spread_prev;
            float xs = x * m, ys = y, zs = z * m;
            float x1 = xs * cyw - zs * syw, z1 = xs * syw + zs * cyw;
            float y2 = ys * cp - z1 * sp, z2 = ys * sp + z1 * cp;
            if (cam + z2 < 40) { visible = false; break; }   // passed the camera
            float sc = f / (cam + z2);
            pts[k][0] = CX + x1 * sc;
            pts[k][1] = CY + y2 * sc;
            depth = z2;
        }
        if (!visible) continue;
        float near = clamp01(0.5f - depth / 400.0f);
        float b = (0.5f + 0.5f * near) * (0.45f + 0.55f * fx_smooth(tf / 200.0f)) * fade;
        uint16_t c = fx_scale565(p.color, (uint32_t)(32 * clamp01(b)));
        if (warp > 0.02f) {
            g->drawLine((int)pts[0][0], (int)pts[0][1], (int)pts[1][0], (int)pts[1][1], c);   // light-speed streak
        } else {
            int r = p.size + (near > 0.7f ? 1 : 0) + (fold > 0.5f ? 1 : 0);
            g->fillCircle((int)pts[1][0], (int)pts[1][1], r, c);
        }
    }

    // Name in the middle of the ring.
    float title = fx_smooth((tf - 850.0f) / 400.0f) * (1.0f - fx_smooth((tf - 1300.0f) / 250.0f));
    if (title > 0.02f) {
        uint32_t k = (uint32_t)(32 * title);
        ui_text_center(CX, CY - 12, fx_scale565(COLOR_TEXT, k), "AmoledWatch", 3);
        char v[24];
        snprintf(v, sizeof(v), "OS %s", diag_fw_version());
        ui_text_center(CX, CY + 24, fx_scale565(0xC6FF, k), v, 1);
    }
}

// ---- Screen ------------------------------------------------------------------------------

static void anim_create() {
    if (!s_frame) s_frame = (uint16_t *)heap_caps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_have_frame = false;
    if (s_frame) {
        if (s_kind == ANIM_LOCK) {
            const uint16_t *last = display_fb_last_submitted();
            if (last) {
                memcpy(s_frame, last, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
                s_have_frame = true;
            }
        } else {
            s_have_frame = ui_render_screen_below(s_frame);
        }
    }
    if (s_kind == ANIM_BOOT) boot_init();
    s_start_ms = millis();
}

static void anim_draw() {
    Arduino_GFX *g = ui_gfx();
    uint16_t *fb = ui_framebuffer();
    if (!g) return;
    uint32_t el = millis() - s_start_ms;
    if (el > duration()) el = duration();

    if (s_kind == ANIM_BOOT) {
        boot_draw(g, fb, el);
        return;
    }

    if (s_kind == ANIM_LOCK) {
        // 0..0.6: the screen tilts back and recedes; 0.6..1: CRT off.
        float t = (float)el / DUR_LOCK;
        float a = clamp01(t / 0.6f), e = fx_ease_in_cubic(a) * 0.6f + fx_smooth(a) * 0.4f;
        float q = clamp01((t - 0.55f) / 0.45f);
        float squash = 1.0f - 0.97f * fx_smooth(q * 1.4f);
        float pitch = 1.3f * e, z = 300.0f * e, scale = 1.0f - 0.25f * e;
        if (s_have_frame && fb && q < 0.75f)
            fx_card(fb, s_frame, pitch, z, scale, (1.0f - 0.6f * e) * (1.0f - q), squash);
        if (q < 0.6f) fx_card_rim(g, pitch, z, scale, squash, 0xC6FF, fx_smooth(a * 3.0f) * (1.0f - q / 0.6f));
        if (q > 0.0f) {
            crt_line(g, (int)(200 * (1.0f - fx_smooth(q * 1.25f))), fx_smooth(q * 3.0f));
            crt_dot(g, 5.0f * (1.0f - fx_smooth((q - 0.65f) / 0.35f)), q > 0.6f ? 1.0f : 0.0f);
        }
        return;
    }

    // Unlock: 0..0.18 a dot stretches into the CRT line; then the screen
    // opens out of the line and swings up from tilted-back to flat, with a
    // little overshoot.
    float t = (float)el / DUR_WAKE;
    float q = clamp01(t / 0.18f);
    float p = clamp01((t - 0.12f) / 0.88f);
    if (s_have_frame && fb && p > 0.0f) {
        float swing = fx_ease_out_back(p);
        float open = fx_smooth(p * 2.2f);
        float pitch = 1.1f * (1.0f - swing), z = 240.0f * (1.0f - fx_smooth(p)), scale = 0.85f + 0.15f * fx_smooth(p);
        float squash = 0.02f + 0.98f * open;
        fx_card(fb, s_frame, pitch, z, scale, 0.3f + 0.7f * fx_smooth(p * 1.3f), squash);
        fx_card_rim(g, pitch, z, scale, squash, 0xC6FF, 1.0f - fx_smooth((p - 0.45f) / 0.55f));
    }
    if (q < 1.0f || p < 0.35f) {
        float lb = q < 1.0f ? 1.0f : 1.0f - p / 0.35f;
        crt_dot(g, 4.0f * (1.0f - q), 1.0f - q);
        crt_line(g, (int)(210 * fx_smooth(q)), lb);
    }
}

static void anim_touch(int, int, bool pressed) {
    if (pressed && s_kind == ANIM_BOOT && millis() - s_start_ms > 300) s_start_ms = millis() - DUR_BOOT;   // skip
}

static void anim_tick() {
    if (millis() - s_start_ms >= duration()) {
        if (s_kind == ANIM_LOCK && s_sleep_after) {
            // Let the "lock" finish visually before the display actually
            // goes to sleep, rather than sleeping first and hiding it.
            sleep_force_sleep();
        }
        ui_pop_screen();
    }
}

Screen lock_anim_screen = {
    "", GESTURE_MODE_FREE,
    16, // ~60fps cap
    anim_create, anim_draw, anim_touch, anim_tick, nullptr, nullptr,
    0, false, false,
    true,   // no_transition - this IS the transition (boot/lock/unlock)
    true,   // no_drag_back
    true,   // hide_status
};
