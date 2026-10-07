#include "game_simon.h"
#include "config.h"
#include "board_pins.h"
#include "hal_controller.h"
#include "game_audio.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#define SIMON_COLORS 4
#define SIMON_MAX_SEQ 20
#define DEG2RAD (3.14159265f / 180.0f)

// The original electronic Simon Says toy was a round device with 4
// quarter-circle pie-slice buttons - not 4 rectangles filling a square
// screen. That's not just more authentic, it's also what actually fits
// this display: 4 full-bleed rectangular quadrants (the old layout)
// have their outer corners sitting well outside the visible circle
// (a corner ends up ~270px from center on a 233px-radius display).
// Pie slices radiating from the center stay inside the circle by
// construction.
static const uint16_t SIMON_PAL[SIMON_COLORS] = { COLOR_BAD, COLOR_ACCENT, COLOR_GOOD, COLOR_ACCENT2 };
// Quadrant centers, screen-angle convention (0=right, 90=down, 180=left, 270=up).
static const float QUAD_CENTER_DEG[SIMON_COLORS] = { 0.0f, 90.0f, 180.0f, 270.0f };
#define QUAD_OUTER_R   208.0f
#define QUAD_HUB_R      46.0f  // inner "hub" left uncolored for the score readout
#define QUAD_HALFWIDTH  42.0f  // < 45 so there's a visible gap between slices

static int s_sequence[SIMON_MAX_SEQ];
static int s_seq_len;
static int s_input_idx;
static bool s_showing;
static bool s_game_over;
static int s_score;
static uint32_t s_flash_start;
static int s_flash_idx;
static bool s_flash_on;
static bool s_input_phase;
static int s_sound_played_idx; // edge-detection for the playback-phase note, see simon_tick()
// Rising-edge tracking for touch input - handle_quad_press() has no
// guard of its own against being called more than once for a single
// physical tap, so without this, a tap that lasted more than one
// frame could advance s_input_idx multiple times in one touch. Since
// the finger stays on the same quadrant but the sequence then expects
// a different one, the very next (spurious) call would likely land on
// the wrong-quadrant branch and end the game on what should have been
// a correct tap. Not reset in simon_create() - that function is also
// called mid-handler for the game-over restart tap below, and
// resetting there would undo the true this same event just set,
// re-arming a false rising edge on the very next still-held frame.
static bool s_prev_pressed;

static void simon_create() {
    s_seq_len = 1;
    s_score = 0;
    s_game_over = false;
    s_sequence[0] = random(SIMON_COLORS);
    s_showing = true;
    s_input_phase = false;
    s_flash_start = millis();
    s_flash_idx = 0;
    s_flash_on = true;
    s_sound_played_idx = -1;
}

static void draw_pie_quadrant(Arduino_GFX *g, int cx, int cy, float center_deg, uint16_t color) {
    // Approximate an annular pie slice (hub excluded) as a fan of
    // small quads, same technique as game_breakout.cpp's sector bricks.
    int steps = 6;
    float a0 = center_deg - QUAD_HALFWIDTH;
    float step = (2.0f * QUAD_HALFWIDTH) / steps;
    for (int i = 0; i < steps; i++) {
        float ang0 = (a0 + i * step) * DEG2RAD;
        float ang1 = (a0 + (i + 1) * step) * DEG2RAD;
        int x1 = cx + (int)(cosf(ang0) * QUAD_HUB_R), y1 = cy + (int)(sinf(ang0) * QUAD_HUB_R);
        int x2 = cx + (int)(cosf(ang1) * QUAD_HUB_R), y2 = cy + (int)(sinf(ang1) * QUAD_HUB_R);
        int x3 = cx + (int)(cosf(ang1) * QUAD_OUTER_R), y3 = cy + (int)(sinf(ang1) * QUAD_OUTER_R);
        int x4 = cx + (int)(cosf(ang0) * QUAD_OUTER_R), y4 = cy + (int)(sinf(ang0) * QUAD_OUTER_R);
        g->fillTriangle(x1, y1, x2, y2, x3, y3, color);
        g->fillTriangle(x1, y1, x3, y3, x4, y4, color);
    }
}

static void draw_quadrants(Arduino_GFX *g, int highlight) {
    int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;
    for (int i = 0; i < SIMON_COLORS; i++) {
        uint16_t c = (highlight == i) ? COLOR_TEXT : SIMON_PAL[i];
        draw_pie_quadrant(g, cx, cy, QUAD_CENTER_DEG[i], c);
    }
    g->fillCircle(cx, cy, (int)QUAD_HUB_R - 3, COLOR_PANEL);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_score);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    int tw = (int)strlen(buf) * 12;
    g->setCursor(cx - tw / 2, cy - 8);
    g->print(buf);
}

static void simon_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    if (s_game_over) {
        g->fillScreen(COLOR_BG);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_BAD, "GAME OVER", 3);
        char buf[16];
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT, buf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 40, COLOR_TEXT_DIM, "Tap to retry", 2);
        return;
    }

    if (s_showing) {
        int hl = -1;
        if (s_flash_on) hl = s_sequence[s_flash_idx];
        draw_quadrants(g, hl);
    } else {
        draw_quadrants(g, -1);
    }
}

// Returns which quadrant (0..3) a screen point falls in, or -1 if
// it's outside the hub-to-outer-radius ring entirely (a tap in the
// gap between slices, or in the center hub, or off the pie entirely).
static int quad_from_xy(int x, int y) {
    int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;
    float dx = (float)(x - cx), dy = (float)(y - cy);
    float r = sqrtf(dx * dx + dy * dy);
    if (r < QUAD_HUB_R || r > QUAD_OUTER_R) return -1;
    float ang = atan2f(dy, dx) / DEG2RAD;
    if (ang < 0) ang += 360.0f;
    for (int i = 0; i < SIMON_COLORS; i++) {
        float d = fmodf(ang - QUAD_CENTER_DEG[i] + 540.0f, 360.0f) - 180.0f;
        if (fabsf(d) <= QUAD_HALFWIDTH) return i;
    }
    return -1;
}

static void handle_quad_press(int q) {
    game_sfx_simon_note(q); // play the tapped quadrant's own note, whether correct or not - matches the physical toy's behavior
    if (q == s_sequence[s_input_idx]) {
        s_input_idx++;
        if (s_input_idx >= s_seq_len) {
            s_score += 10;
            if (s_seq_len < SIMON_MAX_SEQ) {
                s_sequence[s_seq_len++] = random(SIMON_COLORS);
            }
            s_input_idx = 0;
            s_showing = true;
            s_flash_start = millis();
            s_flash_idx = 0;
            s_flash_on = true;
            s_sound_played_idx = -1;
        }
    } else {
        s_game_over = true;
    }
}

static void simon_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;
    if (s_game_over) { simon_create(); return; }
    if (s_showing) return;

    int q = quad_from_xy(x, y);
    if (q < 0) return; // tapped the hub, the gap between slices, or outside the pie

    handle_quad_press(q);
}

static void simon_tick() {
    if (controller_connected() && !s_showing && !s_game_over) {
        // Controller buttons map directly onto the 4 quadrants - a
        // cleaner fit here than anywhere else in this project, since
        // Simon already has exactly 4 discrete choices.
        static bool s_ctrl_prev[4] = { false, false, false, false };
        uint8_t bits[4] = { CTRL_BTN_A, CTRL_BTN_B, CTRL_BTN_X, CTRL_BTN_Y };
        for (int i = 0; i < 4 && i < SIMON_COLORS; i++) {
            bool held = controller_button(bits[i]);
            if (held && !s_ctrl_prev[i]) handle_quad_press(i);
            s_ctrl_prev[i] = held;
        }
    }
    if (!s_showing) return;
    uint32_t now = millis();
    uint32_t elapsed = now - s_flash_start;
    int flash_duration = 500;
    int gap_duration = 200;
    int cycle = flash_duration + gap_duration;
    int pos_in_cycle = elapsed % cycle;
    s_flash_on = pos_in_cycle < flash_duration;
    s_flash_idx = elapsed / cycle;
    // Play each quadrant's note once, right as it lights up - edge-
    // detected against s_sound_played_idx so this fires exactly once
    // per flash, not every frame for the ~500ms it stays lit.
    if (s_flash_on && s_flash_idx < s_seq_len && s_flash_idx != s_sound_played_idx) {
        game_sfx_simon_note(s_sequence[s_flash_idx]);
        s_sound_played_idx = s_flash_idx;
    }
    if (s_flash_idx >= s_seq_len) {
        s_showing = false;
        s_input_phase = true;
        s_input_idx = 0;
    }
}

Screen simon_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    simon_create, simon_draw, simon_touch, simon_tick, nullptr, nullptr,
    0, true // idle_frame_ms, suppress_idle - never dim or lock while a game is active
};
