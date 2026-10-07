/*
 * app_poweroff.cpp
 * Slide-to-confirm power off, triggered by a PWR button long-press
 * (the existing 600ms firmware-level long-press detection in
 * hal_buttons.cpp - previously unconsumed for PWR). Calls the real
 * power_shutdown() (verified against XPowersLib's actual interface
 * source - see hal_power.cpp) only once the slide gesture actually
 * completes; releasing early or leaving it idle cancels back to
 * whatever was showing before, so an accidental long-press doesn't
 * risk the same one-hold-and-it's-off outcome the raw hardware timer
 * would have given with no confirmation step at all.
 *
 * The hardware long-press-shutdown configured in hal_power.cpp's
 * power_init() stays in place too, as a last-resort failsafe if
 * firmware ever hangs - this screen is the normal, confirmed path,
 * not a replacement for that safety net.
 */
#include "app_poweroff.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "diag.h"
#include "hal_power.h"
#include <Arduino_GFX_Library.h>
#include <math.h>

#define TRACK_Y 280
#define TRACK_X0 100
#define TRACK_X1 366
#define KNOB_R 30
#define COMPLETE_THRESHOLD 0.85f
#define IDLE_CANCEL_MS 6000
#define SHUTDOWN_ANIM_MS 700

enum PoffState { POFF_SLIDE, POFF_SHUTTING_DOWN };
static PoffState s_state;
static float s_knob_t; // 0..1 position along the track
static bool s_dragging;
static uint32_t s_state_enter_ms;
static uint32_t s_last_touch_ms;

static void poweroff_create() {
    s_state = POFF_SLIDE;
    s_knob_t = 0.0f;
    s_dragging = false;
    s_state_enter_ms = millis();
    s_last_touch_ms = millis();
}

static void poweroff_touch(int x, int y, bool pressed) {
    if (s_state != POFF_SLIDE) return;
    s_last_touch_ms = millis();

    if (pressed) {
        s_dragging = true;
        float t = (float)(x - TRACK_X0) / (float)(TRACK_X1 - TRACK_X0);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        s_knob_t = t;
        if (s_knob_t >= COMPLETE_THRESHOLD) {
            s_state = POFF_SHUTTING_DOWN;
            s_state_enter_ms = millis();
        }
    } else {
        s_dragging = false;
        if (s_knob_t < COMPLETE_THRESHOLD) {
            s_knob_t = 0.0f; // didn't complete - snap back and cancel
            ui_pop_screen();
        }
    }
}

static void poweroff_tick() {
    uint32_t now = millis();
    if (s_state == POFF_SLIDE) {
        if (!s_dragging && now - s_last_touch_ms > IDLE_CANCEL_MS) {
            ui_pop_screen();
        }
    } else if (s_state == POFF_SHUTTING_DOWN) {
        if (now - s_state_enter_ms >= SHUTDOWN_ANIM_MS) {
            diag_mark_good();   // a deliberate power-off isn't a failed update
            power_shutdown();
            // Still here means shutdown() didn't actually cut power
            // (e.g. on USB power with no battery, or the PMU call
            // failed) - don't get stuck pretending to be off forever.
            ui_pop_screen();
        }
    }
}

static void poweroff_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    if (s_state == POFF_SHUTTING_DOWN) {
        float t = (float)(millis() - s_state_enter_ms) / SHUTDOWN_ANIM_MS;
        if (t > 1.0f) t = 1.0f;
        int r = (int)(160 * (1.0f - t));
        g->fillScreen(COLOR_BG);
        if (r > 2) g->drawCircle(LCD_WIDTH / 2, LCD_HEIGHT / 2, r, COLOR_BAD);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_TEXT_DIM, "Powering off...", 1);
        return;
    }

    ui_draw_centered_text(160, COLOR_TEXT, "Slide to power off", 2);

    g->fillRoundRect(TRACK_X0 - KNOB_R, TRACK_Y - 18,
                      (TRACK_X1 - TRACK_X0) + KNOB_R * 2, 36, 18, COLOR_PANEL);
    int fill_w = (int)((TRACK_X1 - TRACK_X0) * s_knob_t);
    if (fill_w > 4) {
        g->fillRoundRect(TRACK_X0 - KNOB_R, TRACK_Y - 18, fill_w + KNOB_R, 36, 18, COLOR_BAD);
    }
    int knob_x = TRACK_X0 + (int)((TRACK_X1 - TRACK_X0) * s_knob_t);
    g->fillCircle(knob_x, TRACK_Y, KNOB_R, COLOR_TEXT);

    ui_draw_centered_text(TRACK_Y + 70, COLOR_TEXT_DIM, "Press BOOT to cancel", 1);
}

Screen poweroff_screen = {
    nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_DEFAULT,
    poweroff_create, poweroff_draw, poweroff_touch, poweroff_tick, nullptr, nullptr,
    0, false, false,
    true, // no_transition - an overlay with its own animation
};
