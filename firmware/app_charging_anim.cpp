/*
 * app_charging_anim.cpp
 * Brief "charging started" animation - a bolt icon with pulsing rings,
 * shown for ~1.3s then self-pops back to whatever was showing
 * underneath. Triggered by AmoledSmartWatchOS.ino detecting the
 * RISING EDGE of power_is_charging() (false->true) - this screen
 * itself doesn't poll charging state, it's just told to play once.
 */
#include "app_charging_anim.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>

#define ANIM_DURATION_MS 1300

static uint32_t s_start_ms;

static void charging_anim_create() {
    s_start_ms = millis();
}

static void charging_anim_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    float t = (float)(millis() - s_start_ms) / ANIM_DURATION_MS;
    if (t > 1.0f) t = 1.0f;

    int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;

    // Pulsing rings expanding outward, staggered - no alpha blending
    // available cross-file here, so distance/spacing carries the
    // "fading outward" read instead of true transparency.
    for (int i = 0; i < 3; i++) {
        float ring_t = t - i * 0.18f;
        if (ring_t < 0.0f || ring_t > 1.0f) continue;
        int r = (int)(20 + ring_t * 130);
        g->drawCircle(cx, cy, r, COLOR_GOOD);
    }

    // Bolt icon
    g->fillTriangle(cx - 2, cy - 30, cx + 16, cy - 4, cx + 4, cy - 4, COLOR_WARN);
    g->fillTriangle(cx + 4, cy - 4, cx - 16, cy + 30, cx - 4, cy + 4, COLOR_WARN);

    ui_draw_centered_text(cy + 70, COLOR_TEXT, "Charging", 2);
}

static void charging_anim_tick() {
    if (millis() - s_start_ms >= ANIM_DURATION_MS) {
        ui_pop_screen();
    }
}

Screen charging_anim_screen = {
    nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_DEFAULT,
    charging_anim_create, charging_anim_draw, nullptr, charging_anim_tick, nullptr, nullptr,
    0, false, false,
    true, // no_transition - an overlay with its own animation
};
