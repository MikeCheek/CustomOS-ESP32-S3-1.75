// Pairing code screen (fw 3.0). When a phone pairs, the watch makes up a
// random 6-digit code (hal_ble.cpp onPassKeyDisplay) and shows it here; the
// phone asks for it. Pops by itself when pairing ends.
#include "config.h"
#include "board_pins.h"
#include <Arduino_GFX_Library.h>
#include "ui.h"
#include "ui_font.h"
#include "hal_ble.h"
#include "hal_sleep.h"
#include <Arduino.h>

// Presence is checked with ui_screen_in_stack(): on_destroy also runs when
// another screen (a call, a notification) is pushed on top of this one.
static uint32_t s_shown_code = 0;
static int8_t s_result = 0;          // shown for a moment after pairing
static uint32_t s_result_ms = 0;

static void pairing_create() { s_result = 0; }

static void pairing_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_result != 0) {
        bool ok = s_result > 0;
        g->fillCircle(LCD_WIDTH / 2, 180, 44, ok ? COLOR_GOOD : COLOR_BAD);
        ui_draw_centered_text(168, COLOR_BG, ok ? "OK" : "X", 3);
        ui_draw_centered_text(260, COLOR_TEXT, ok ? "Paired" : "Pairing failed", 2);
        if (!ok) ui_draw_centered_text(296, COLOR_TEXT_DIM, "Try again from the app", 1);
        return;
    }
    ui_draw_centered_text(120, COLOR_TEXT_DIM, "Pairing with your phone", 1);
    char code[8];
    snprintf(code, sizeof(code), "%06lu", (unsigned long)s_shown_code);
    // "123 456" reads easier than six digits in a row
    char spaced[10];
    snprintf(spaced, sizeof(spaced), "%.3s %.3s", code, code + 3);
    ui_draw_centered_text(200, COLOR_ACCENT3, spaced, 4);
    ui_draw_centered_text(290, COLOR_TEXT, "Type this code", 2);
    ui_draw_centered_text(318, COLOR_TEXT, "on your phone", 2);
}

static void pairing_touch(int, int, bool) {}
static void pairing_gesture(Gesture g) {
    // A way out if the screen is ever left behind (pairing_update re-opens
    // it while a code is still being shown).
    if (g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen pairing_screen = {
    "Pairing", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    pairing_create, pairing_draw, pairing_touch, nullptr, nullptr, pairing_gesture
};

// Main loop: open the screen when a code appears, close it after.
void pairing_update() {
    uint32_t code = ble_get_passkey();
    bool open = ui_screen_in_stack(&pairing_screen);
    if (code) {
        s_shown_code = code;
        if (!open) {
            sleep_register_activity();      // wake the screen: the code must be visible
            ui_push(&pairing_screen);
            open = ui_screen_in_stack(&pairing_screen);
        }
    }
    int r = ble_take_pairing_result();
    if (r != 0 && open) { s_result = (int8_t)r; s_result_ms = millis(); }
    // Only ever pop it from the top; a covered one closes once it's back on top.
    if (!ui_screen_on_top(&pairing_screen)) return;
    bool done = s_result != 0 ? millis() - s_result_ms > (s_result > 0 ? 1200u : 2500u)
                              : code == 0;   // pairing ended without a result (stale screen)
    if (done) {
        s_result = 0;
        ui_pop_screen();
    }
}
