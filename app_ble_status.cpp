#include "config.h"
#include "board_pins.h"

#include "ui.h"
#include "ui_font.h"
#include "hal_ble.h"
#include "hal_power.h"
#include <Arduino_GFX_Library.h>

// =========================================================================
//  BLE Status screen
// =========================================================================

static bool s_forget_armed = false;
static uint32_t s_forget_ms = 0;

static void ble_status_draw() {
    if (s_forget_armed && millis() - s_forget_ms > 4000) s_forget_armed = false;
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    // No manual header draw - the framework draws it automatically
    // from Screen.title.

    int y = 100;
    auto row = [&](const char *label, const char *val) {
        g->setTextSize(2);
        g->setTextColor(COLOR_TEXT_DIM);
        ui_print(60, y, 2, COLOR_TEXT_DIM, label);
        g->setTextColor(COLOR_TEXT);
        ui_print(LCD_WIDTH / 2, y, 2, COLOR_TEXT, val);
        y += 36;
    };

    row("Device", "AmoledWatch");
    row("State", ble_is_connected() ? "Connected" : "Advertising");
    row("Device", ble_get_connected_device_name());
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", power_get_battery_percent());
    row("Battery", buf);
    snprintf(buf, sizeof(buf), "%d", ble_get_connect_count());
    row("Connects", buf);

    // fw 3.0: the pairing code is random and shown only while pairing.
    row("Link", !ble_is_connected() ? "-" : ble_link_encrypted() ? "Encrypted" : "Not encrypted");
    snprintf(buf, sizeof(buf), "%d", ble_bond_count());
    row("Paired", buf);

    // Unpair every phone (then remove "AmoledWatch" in the phone's
    // Bluetooth settings too, or it can't reconnect).
    static const int BY = 360;
    uint16_t c = s_forget_armed ? COLOR_BAD : COLOR_PANEL;
    g->fillRoundRect(LCD_WIDTH / 2 - 110, BY, 220, 44, 22, c);
    ui_draw_centered_text(BY + 14, COLOR_TEXT, s_forget_armed ? "Tap again to unpair" : "Unpair all phones", 1);
}

static void ble_status_touch(int x, int y, bool pressed) {
    static bool s_prev_pressed = false;
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;
    if (x < 54 && y < 54) { ui_pop_screen(); return; }
    if (y >= 350 && y <= 414 && x > LCD_WIDTH / 2 - 120 && x < LCD_WIDTH / 2 + 120) {
        if (!s_forget_armed) { s_forget_armed = true; s_forget_ms = millis(); return; }
        s_forget_armed = false;
        ui_show_toast(ble_forget_bonds() ? "Unpaired - also forget the watch on the phone" : "Bluetooth is off", 3000);
    }
}

static void ble_status_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) ui_pop_screen();
}

Screen ble_status_screen = {
    "BLE Status", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, ble_status_draw, ble_status_touch, nullptr, nullptr, ble_status_gesture
};
