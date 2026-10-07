/*
 * app_findme.cpp
 * "Find My Watch" - triggered from the companion app (see hal_ble.h's
 * ble_consume_findme_request() for how the request arrives). Previously
 * the app's "Find Watch" button just sent the word "find" as a normal
 * text notification - easy to miss, especially if the watch is asleep
 * under a couch cushion. This is a proper, unmissable response: a
 * full-screen pulsing display plus a repeating vibrate+beep pattern,
 * for a fixed duration or until tapped away.
 */
#include "app_findme.h"
#include "config.h"
#include "board_pins.h"
#include "hal_vibrate.h"
#include "hal_audio.h"
#include <Arduino_GFX_Library.h>
#include <math.h>

#define FINDME_DURATION_MS   8000
#define FINDME_PULSE_MS      700   // vibrate+beep repeat interval

static uint32_t s_start_ms;
static uint32_t s_last_pulse_ms;
static bool     s_prev_pressed; // rising-edge tracking, see findme_touch()

static void findme_create() {
    s_start_ms = millis();
    s_last_pulse_ms = 0; // force an immediate first pulse
    s_prev_pressed = false; // don't let a stale true from a previous session swallow this session's first tap
}

static void findme_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    uint32_t elapsed = millis() - s_start_ms;
    float t = (float)elapsed / 1000.0f;

    // Pulsing background between two colors - reads as "urgent" without
    // needing to flash pure white/black (harsh on an AMOLED at night).
    float pulse = 0.5f + 0.5f * sinf(t * 6.0f);
    uint16_t bg = pulse > 0.5f ? COLOR_BAD : COLOR_PANEL;
    g->fillScreen(bg);

    int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;

    // Expanding ring pulses, timed to the vibrate/beep pulses so the
    // visual, haptic, and audible cues all land together.
    float ring_t = fmodf((float)elapsed, (float)FINDME_PULSE_MS) / (float)FINDME_PULSE_MS;
    int ring_r = (int)(20 + ring_t * 140);
    uint16_t ring_c = pulse > 0.5f ? COLOR_TEXT : COLOR_BAD;
    g->drawCircle(cx, cy, ring_r, ring_c);
    g->drawCircle(cx, cy, ring_r > 0 ? ring_r - 1 : 0, ring_c);

    ui_draw_centered_text(cy - 60, COLOR_TEXT, "FIND ME", 4);
    ui_draw_centered_text(cy + 20, COLOR_TEXT, "I'm over here!", 2);

    uint32_t remaining_s = (FINDME_DURATION_MS - elapsed + 999) / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%lus", (unsigned long)remaining_s);
    ui_draw_centered_text(LCD_HEIGHT - 70, COLOR_TEXT_DIM, buf, 2);
    ui_draw_centered_text(LCD_HEIGHT - 40, COLOR_TEXT_DIM, "Tap to dismiss", 1);
}

static void findme_touch(int, int, bool pressed) {
    // Rising-edge only - this used to fire ui_pop_screen() on every
    // single frame the finger stayed down (not just once), which
    // could pop multiple screens off the stack for what was
    // physically one tap, if the finger lingered even slightly past
    // the first frame it registered.
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (tap_edge) ui_pop_screen(); // acknowledged - found it
}

static void findme_tick() {
    uint32_t now = millis();
    uint32_t elapsed = now - s_start_ms;

    if (now - s_last_pulse_ms >= FINDME_PULSE_MS) {
        s_last_pulse_ms = now;
        vibrate_ms(280);
        audio_play_sfx(2200, 180); // sharp, attention-getting tone
    }

    if (elapsed >= FINDME_DURATION_MS) {
        ui_pop_screen();
    }
}

Screen findme_screen = {
    "", GESTURE_MODE_FREE,
    UI_FRAME_MS_DEFAULT,
    findme_create, findme_draw, findme_touch, findme_tick, nullptr, nullptr,
    0, false, false,
    true, // no_transition - an alert should just appear
};
