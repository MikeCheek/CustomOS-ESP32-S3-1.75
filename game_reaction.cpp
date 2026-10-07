#include "game_reaction.h"
#include "config.h"
#include "board_pins.h"
#include "hal_controller.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>

static enum { R_WAIT, R_READY, R_GO, R_DONE } s_state;
static uint32_t s_change_ms;
static uint32_t s_reaction_us;
static int s_best_ms;
static uint32_t s_go_delay;
static bool s_touch_active; // press-edge tracking, see reaction_touch()

static void reaction_create() {
    s_state = R_WAIT;
    s_best_ms = 9999;
    s_change_ms = millis();
    s_go_delay = random(2000, 5000);
    s_touch_active = false;
}

static void reaction_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    switch (s_state) {
        case R_WAIT:
            g->fillScreen(COLOR_BAD);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 30, COLOR_TEXT, "WAIT", 4);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 20, COLOR_TEXT_DIM, "for green", 2);
            break;
        case R_READY:
            g->fillScreen(COLOR_BAD);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_TEXT, "...", 4);
            break;
        case R_GO:
            g->fillScreen(COLOR_GOOD);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 30, COLOR_TEXT, "TAP!", 4);
            break;
        case R_DONE:
            g->fillScreen(COLOR_BG);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 60, COLOR_TEXT, "Reaction", 3);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d ms", s_reaction_us / 1000);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_ACCENT, buf, 4);
            snprintf(buf, sizeof(buf), "Best: %d ms", s_best_ms);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 40, COLOR_TEXT_DIM, buf, 2);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 80, COLOR_TEXT_DIM, "Tap to retry", 2);
            break;
    }
}

static void do_tap() {
    if (s_state == R_READY) {
        s_state = R_WAIT;
        s_change_ms = millis();
        return;
    }
    if (s_state == R_GO) {
        s_reaction_us = (millis() - s_change_ms) * 1000;
        int ms = s_reaction_us / 1000;
        if (ms < s_best_ms) s_best_ms = ms;
        s_state = R_DONE;
        return;
    }
    if (s_state == R_DONE) {
        int keep_best = s_best_ms;
        reaction_create();
        s_best_ms = keep_best; // retrying shouldn't wipe out a best time set on a previous attempt
        return;
    }
}

static void reaction_touch(int x, int y, bool pressed) {
    // Touch dispatch calls on_touch repeatedly with pressed=true for as
    // long as a physical touch is held (once per render frame) - not
    // just once per tap. Without edge-detection, a single tap could
    // dispatch twice: the first call transitions WAIT->READY, and
    // before the finger even lifts, the second call (still
    // pressed=true, same physical touch) sees state==R_READY and
    // immediately treats it as "tapped too early", flipping straight
    // back to WAIT - so the multi-second timer to R_GO never got a
    // chance to run. That's why it could "never turn green": the
    // state was resetting itself within the same tap, every time.
    if (!pressed) { s_touch_active = false; return; }
    if (s_touch_active) return; // still the same held press - ignore
    s_touch_active = true;
    do_tap();
}

static void reaction_tick() {
    if (controller_connected()) {
        static bool s_ctrl_a_prev = false;
        bool a = controller_button(CTRL_BTN_A);
        if (a && !s_ctrl_a_prev) do_tap();
        s_ctrl_a_prev = a;
    }
    if (s_state == R_WAIT && millis() - s_change_ms >= 1000) {
        // Auto-advances now rather than requiring a tap to leave this
        // state - "WAIT for green" was telling the player to just
        // wait, but the state machine never actually progressed on
        // its own from here, so literally waiting (not tapping) meant
        // it never turned green. This is also reached as the "tapped
        // too early" penalty from R_READY, where the same brief pause
        // before restarting makes sense too.
        s_state = R_READY;
        s_change_ms = millis();
        s_go_delay = random(2000, 5000);
    }
    if (s_state == R_READY && millis() - s_change_ms >= s_go_delay) {
        s_state = R_GO;
        s_change_ms = millis();
    }
}

Screen reaction_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    reaction_create, reaction_draw, reaction_touch, reaction_tick, nullptr, nullptr,
    0, true // idle_frame_ms, suppress_idle - never dim or lock while a game is active
};
