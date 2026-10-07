#include "app_torch.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "hal_display.h"
#include <Arduino_GFX_Library.h>

// Tap cycles the colour: white (light), warm (reading, easier on the eyes
// at night), red (keeps night vision). Swipe or BOOT to leave - the user's
// own brightness comes back on exit.
static const uint16_t TORCH_COLORS[] = { 0xFFFF, COLOR565(255, 190, 110), COLOR565(255, 0, 0) };
static const char *const TORCH_NAMES[] = { "White", "Warm", "Red" };
static const int TORCH_N = 3;
static int      s_color = 0;
static bool     s_prev = false;
static uint32_t s_hint_until = 0;

static void torch_create() {
    s_prev = false;
    s_hint_until = millis() + 1800;
    display_set_brightness(255);
}
static void torch_destroy() { display_set_brightness(g_app_settings.brightness); }

static void torch_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(TORCH_COLORS[s_color]);
    if (millis() < s_hint_until) {
        ui_text_center(LCD_WIDTH / 2, LCD_HEIGHT / 2 - 14, COLOR_BG, TORCH_NAMES[s_color], 3);
        ui_text_center(LCD_WIDTH / 2, LCD_HEIGHT / 2 + 22, ui_dim(TORCH_COLORS[s_color], 0.45f),
                       "tap: colour  swipe: exit", 1);
    }
}

static void torch_touch(int, int, bool pressed) {
    bool edge = pressed && !s_prev;
    s_prev = pressed;
    if (!edge) return;
    s_color = (s_color + 1) % TORCH_N;
    s_hint_until = millis() + 900;
}

static void torch_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) ui_pop_screen();
}

Screen torch_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    torch_create, torch_draw, torch_touch, nullptr, torch_destroy, torch_gesture,
    500,     // idle: nothing moves
    true,    // suppress_idle: a torch that dims itself is useless
    false, false, false,
    true,    // hide_status
};
