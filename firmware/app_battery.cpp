/*
 * app_battery.cpp
 * Battery status and a derived power-consumption estimate.
 *
 * Honesty check: the AXP2101 setup in hal_power.cpp doesn't currently
 * expose a direct hardware current-sensor reading (no confirmed
 * getBatteryChargeCurrent()/getBatteryDischargeCurrent() equivalent
 * wired up) - rather than guess at a library method that might not
 * actually be there, this screen DERIVES a %/hour rate by tracking
 * battery_percent over time, in software, entirely within this file.
 * That's an estimate from a coarse, integer 0-100 reading (so it's
 * noisy over short windows), not a real-time current draw - the UI
 * says "estimated" rather than presenting it as a precise measurement.
 * If this board's PMU library does support real current sensing, that
 * would be a strictly better replacement for this whole rate-tracking
 * block, but implementing it without confirming the actual method
 * exists risks a silent compile failure or, worse, a wrong number
 * presented with false confidence.
 */
#include "app_battery.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "hal_power.h"
#include "ui_font.h"
#include "battery_mode.h"
#include "low_power.h"
#include "app_settings_state.h"
#include <Arduino_GFX_Library.h>
#include <stdlib.h>

#define HISTORY_LEN 30
#define SAMPLE_INTERVAL_MS 60000 // one sample per minute - enough resolution
                                  // for an hourly rate without needing to
                                  // remember samples across a full hour

struct Sample { uint32_t ms; int percent; };
static Sample s_history[HISTORY_LEN];
static int s_history_count = 0;
static uint32_t s_last_sample_ms = 0;

static void battery_create() {
    s_history_count = 0;
    s_last_sample_ms = 0;
}

static void battery_tick() {
    uint32_t now = millis();
    if (s_last_sample_ms != 0 && now - s_last_sample_ms < SAMPLE_INTERVAL_MS) return;
    s_last_sample_ms = now;

    int pct = power_get_battery_percent();
    if (pct < 0) return;

    if (s_history_count < HISTORY_LEN) {
        s_history[s_history_count++] = { now, pct };
    } else {
        // Slide the window - drop oldest, keep it a fixed-size ring
        // rather than growing unbounded over a long-open screen.
        for (int i = 1; i < HISTORY_LEN; i++) s_history[i - 1] = s_history[i];
        s_history[HISTORY_LEN - 1] = { now, pct };
    }
}

// Returns %/hour (positive = draining, negative = charging), or 0 if
// not enough history yet to estimate anything.
static float estimate_rate_per_hour() {
    if (s_history_count < 2) return 0.0f;
    const Sample &first = s_history[0];
    const Sample &last = s_history[s_history_count - 1];
    float elapsed_hours = (last.ms - first.ms) / 3600000.0f;
    if (elapsed_hours < 0.02f) return 0.0f; // under ~1 minute of data - too noisy to report
    return (first.percent - last.percent) / elapsed_hours;
}

// ---- the screen: a charge ring, how long it lasts, the two switches that
// matter; the electrical details only in developer mode ----------------------
extern Screen settings_battery_screen;
static const int CX = LCD_WIDTH / 2;
static int s_press = -1;
static const int ROW_Y = 314, ROW_H = 54, ROW_GAP = 8;

// Hours left from the drain since the cable came out (the honest number,
// screen on and off together), else from this screen's own samples.
static float hours_left(const PowerStatus &st) {
    int p0;
    uint32_t el;
    float rate = 0;
    if (!st.usb_connected && power_since_unplugged(&p0, &el) && el > 1800000UL && p0 > st.battery_percent)
        rate = (p0 - st.battery_percent) / (el / 3600000.0f);
    else if (s_history_count >= 3)
        rate = estimate_rate_per_hour();
    return rate > 0.2f ? st.battery_percent / rate : -1;
}

static void battery_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    PowerStatus st = power_read();
    if (!st.valid) {
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_BAD, "PMU not detected", 2);
        return;
    }
    int pct = st.battery_percent;
    uint16_t col = st.is_charging ? COLOR_ACCENT2 : g_app_settings.low_power ? COLOR_WARN : pct > 20 ? COLOR_GOOD : COLOR_BAD;
    const int RY = 160;
    ui_arc(CX, RY, 96, 14, 0, 360, ui_dim(col, 0.22f), false);
    if (pct > 0) ui_arc(CX, RY, 96, 14, 0, 360.0f * pct / 100, col, true);
    char b[48];
    snprintf(b, sizeof(b), "%d%%", pct);
    ui_text_center(CX, RY - 12, COLOR_TEXT, b, 5);
    const char *state = st.is_charging ? "Charging" : st.usb_connected ? "Plugged in" : g_app_settings.low_power ? "Low Power Mode" : "On battery";
    ui_text_center(CX, RY + 30, col, state, 1);

    // how long it lasts
    float h = st.is_charging ? -1 : hours_left(st);
    if (h > 0) {
        if (h >= 48) snprintf(b, sizeof(b), "About %d days left", (int)(h / 24 + 0.5f));
        else if (h >= 1) snprintf(b, sizeof(b), "About %d h left", (int)(h + 0.5f));
        else snprintf(b, sizeof(b), "About %d min left", (int)(h * 60));
    } else {
        snprintf(b, sizeof(b), "%s", st.is_charging ? "Filling up" : "Working out how long it lasts...");
    }
    ui_text_center(CX, 274, COLOR_TEXT, b, 2);
    int p0;
    uint32_t el;
    if (!st.usb_connected && power_since_unplugged(&p0, &el) && el > 600000UL) {
        snprintf(b, sizeof(b), "%d%% used in %dh%02dm since unplugged", p0 - pct, (int)(el / 3600000UL), (int)(el / 60000UL % 60));
        ui_text_center(CX, 296, COLOR_TEXT_DIM, b, 1);
    }

    // Low Power Mode and the battery mode
    for (int i = 0; i < 2; i++) {
        int y = ROW_Y + i * (ROW_H + ROW_GAP);
        int w = i == 0 ? 330 : 256, x = CX - w / 2;
        g->fillRoundRect(x, y, w, ROW_H, 24, s_press == i ? 0x31A6 : 0x18E3);
        if (i == 0) {
            ui_print(x + 22, y + 17, 2, COLOR_TEXT, "Low Power Mode");
            bool on = low_power_on();
            g->fillRoundRect(x + w - 74, y + 13, 54, 28, 14, on ? COLOR_WARN : 0x39E7);
            g->fillCircle(on ? x + w - 34 : x + w - 60, y + 27, 11, COLOR_TEXT);
        } else {
            ui_print(x + 22, y + 17, 2, COLOR_TEXT, "Battery mode");
            const char *m = battery_mode_adjusted() ? "Adjusted" : battery_profile(battery_mode_current()).name;
            ui_print(x + w - 22 - ui_text_width(m, 1), y + 22, 1, COLOR_TEXT_DIM, m);
        }
    }
    // the electrical details, for developers
    if (g_app_settings.developer_mode) {
        snprintf(b, sizeof(b), "%.2f V  sys %.2f V  %.1f C", st.battery_voltage_v, st.system_voltage_v, st.temperature_c);
        ui_text_center(CX, 448, COLOR_TEXT_DIM, b, 1);
    }
}

static int battery_hit(int x, int y) {
    for (int i = 0; i < 2; i++) {
        int yy = ROW_Y + i * (ROW_H + ROW_GAP);
        if (y >= yy && y < yy + ROW_H && abs(x - CX) < 170) return i;
    }
    return -1;
}

static void battery_touch(int x, int y, bool pressed) {
    int hit = battery_hit(x, y);
    if (pressed) { if (s_press < 0) s_press = hit; return; }
    int p = s_press;
    s_press = -1;
    if (p < 0 || p != hit) return;
    if (p == 0) low_power_set(!low_power_on());
    else ui_push(&settings_battery_screen);
}

static void battery_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen battery_screen = {
    nullptr, GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    battery_create, battery_draw, battery_touch, battery_tick, nullptr, battery_gesture,
    1000,
};
