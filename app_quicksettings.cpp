#include "app_quicksettings.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "app_settings_state.h"
#include "hal_display.h"
#include "hal_touch.h"
#include "hal_wifi.h"
#include "hal_ntp.h"
#include "hal_ble.h"
#include <Arduino_GFX_Library.h>

static const int SLIDER_Y = 200;
static const int SLIDER_LEFT = 70;
static const int SLIDER_RIGHT = LCD_WIDTH - 70;
static const int SLIDER_H = 10;
static const int BTN_Y = 290;
static const int BTN_W = 150;
static const int BTN_H = 50;
static const int BTN_GAP = 20;

static bool s_sliding = false;
// Rising-edge tracking for the WiFi/BLE toggle buttons and the
// dismiss-tap below them - these used to act on raw `pressed`
// directly (every frame the finger stayed down, not just once),
// meaning a tap that lingered even slightly could toggle WiFi/BLE an
// unpredictable number of times before release, or pop multiple
// screens off the stack for one tap. The slider deliberately keeps
// using raw `pressed` below - it's a continuous drag, not a tap.
static bool s_prev_pressed = false;

static void qs_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    g->setTextSize(3);
    g->setTextColor(COLOR_TEXT);
    ui_draw_centered_text(120, COLOR_TEXT, "Quick Settings", 3);

    ui_draw_centered_text(SLIDER_Y - 32, COLOR_TEXT_DIM, "Brightness", 2);

    g->fillRoundRect(SLIDER_LEFT, SLIDER_Y, SLIDER_RIGHT - SLIDER_LEFT, SLIDER_H, 5, COLOR_PANEL);
    int fill = (int)g_app_settings.brightness * (SLIDER_RIGHT - SLIDER_LEFT) / 255;
    g->fillRoundRect(SLIDER_LEFT, SLIDER_Y, fill, SLIDER_H, 5, COLOR_ACCENT);
    int tx = SLIDER_LEFT + fill;
    g->fillCircle(tx, SLIDER_Y + SLIDER_H / 2, 14, COLOR_ACCENT);
    g->fillCircle(tx, SLIDER_Y + SLIDER_H / 2, 9, COLOR_BG);

    char bstr[8];
    snprintf(bstr, sizeof(bstr), "%d", (int)g_app_settings.brightness * 100 / 255);
    ui_draw_centered_text(SLIDER_Y + 20, COLOR_TEXT_DIM, bstr, 2);

    int col1 = LCD_WIDTH / 2 - BTN_W - BTN_GAP / 2;
    int col2 = LCD_WIDTH / 2 + BTN_GAP / 2;

    bool wf = g_app_settings.wifi_on;
    g->fillRoundRect(col1, BTN_Y, BTN_W, BTN_H, 12, wf ? COLOR_GOOD : COLOR_PANEL);
    g->drawRoundRect(col1, BTN_Y, BTN_W, BTN_H, 12, wf ? COLOR_GOOD : COLOR_TEXT_DIM);
    ui_draw_centered_text(BTN_Y + 16, wf ? COLOR_TEXT : COLOR_TEXT_DIM, "WiFi", 2);

    bool bl = ble_is_enabled();
    g->fillRoundRect(col2, BTN_Y, BTN_W, BTN_H, 12, bl ? COLOR_GOOD : COLOR_PANEL);
    g->drawRoundRect(col2, BTN_Y, BTN_W, BTN_H, 12, bl ? COLOR_GOOD : COLOR_TEXT_DIM);
    ui_draw_centered_text(BTN_Y + 16, bl ? COLOR_TEXT : COLOR_TEXT_DIM, "BLE", 2);

    ui_draw_centered_text(BTN_Y + BTN_H + 30, COLOR_TEXT_DIM, "Tap here to close", 2);
}

static void qs_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    if (pressed) {
        if (s_sliding || (tap_edge && y >= SLIDER_Y - 20 && y <= SLIDER_Y + SLIDER_H + 20 &&
            x >= SLIDER_LEFT - 10 && x <= SLIDER_RIGHT + 10)) {
            s_sliding = true;
            int v = (x - SLIDER_LEFT) * 255 / (SLIDER_RIGHT - SLIDER_LEFT);
            if (v < 0) v = 0; if (v > 255) v = 255;
            g_app_settings.brightness = v;
            display_set_brightness(v);
        }
        int col1 = LCD_WIDTH / 2 - BTN_W - BTN_GAP / 2;
        int col2 = LCD_WIDTH / 2 + BTN_GAP / 2;
        if (tap_edge && y >= BTN_Y && y <= BTN_Y + BTN_H) {
            if (x >= col1 && x <= col1 + BTN_W) {
                g_app_settings.wifi_on = !g_app_settings.wifi_on;
                if (g_app_settings.wifi_on) { wifi_enable(); ntp_sync(); } else wifi_disable();
            }
            if (x >= col2 && x <= col2 + BTN_W) {
                if (ble_is_enabled()) ble_disable(); else if (!ble_enable()) ui_show_toast("Not enough memory for Bluetooth", 2500);
            }
        }
        if (tap_edge && y > BTN_Y + BTN_H + 20) {
            ui_pop_screen();
        }
    } else {
        s_sliding = false;
    }
}

// Nothing to poll: qs_touch() gets every event while the finger is down
// and keeps following it once a slide has started. (This used to call
// touch_read() itself - a second reader of the touch controller that
// could swallow the finger-up before the main loop saw it.)
static void qs_tick() {}

static void qs_gesture(Gesture g) { ui_pop_screen(); }

Screen quicksettings_screen = {
    nullptr, GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, qs_draw, qs_touch, qs_tick, nullptr, qs_gesture
};
