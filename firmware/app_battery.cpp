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
#include <Arduino_GFX_Library.h>

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

static void battery_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    PowerStatus st = power_read();
    if (!st.valid) {
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_BAD, "PMU not detected", 2);
        return;
    }

    ui_draw_centered_text(66, COLOR_TEXT, "Battery", 2);

    char buf[40];
    uint16_t pct_color = st.is_charging ? COLOR_ACCENT2 : (st.battery_percent > 20 ? COLOR_GOOD : COLOR_BAD);
    snprintf(buf, sizeof(buf), "%d%%", st.battery_percent);
    ui_draw_centered_text(130, pct_color, buf, 4);

    ui_draw_centered_text(178, COLOR_TEXT_DIM,
        st.is_charging ? "Charging" : (st.usb_connected ? "USB connected, not charging" : "On battery"), 1);

    int y = 210, row_h = 22;
    snprintf(buf, sizeof(buf), "Battery voltage: %.2f V", st.battery_voltage_v);
    ui_draw_centered_text(y, COLOR_TEXT_DIM, buf, 1); y += row_h;
    snprintf(buf, sizeof(buf), "System voltage: %.2f V", st.system_voltage_v);
    ui_draw_centered_text(y, COLOR_TEXT_DIM, buf, 1); y += row_h;
    if (st.usb_connected) {
        snprintf(buf, sizeof(buf), "VBUS voltage: %.2f V", st.vbus_voltage_v);
        ui_draw_centered_text(y, COLOR_TEXT_DIM, buf, 1); y += row_h;
    }
    snprintf(buf, sizeof(buf), "PMU temperature: %.1f C", st.temperature_c);
    ui_draw_centered_text(y, COLOR_TEXT_DIM, buf, 1); y += row_h;

    // The real-world number: drain since the cable came out, screen on
    // and off together (this screen only samples while it's open).
    int p0;
    uint32_t el;
    if (!st.usb_connected && power_since_unplugged(&p0, &el) && el > 600000UL) {
        float h = el / 3600000.0f;
        int used = p0 - st.battery_percent;
        snprintf(buf, sizeof(buf), "Since unplugged: %d%% in %dh%02dm (%.1f%%/h)", used, (int)h,
                 (int)((el / 60000UL) % 60), used / h);
        ui_draw_centered_text(y, COLOR_TEXT, buf, 1); y += row_h;
    }

    y += 10;
    float rate = estimate_rate_per_hour();
    if (s_history_count < 3) {
        ui_draw_centered_text(y, COLOR_TEXT_DIM, "Estimating drain rate...", 1); y += row_h;
        ui_draw_centered_text(y, COLOR_TEXT_DIM, "(collecting a few minutes", 1); y += row_h - 6;
        ui_draw_centered_text(y, COLOR_TEXT_DIM, "of samples first)", 1);
    } else if (st.is_charging) {
        ui_draw_centered_text(y, COLOR_ACCENT2, "Charging - rate not shown", 1);
    } else {
        snprintf(buf, sizeof(buf), "Est. drain: %.1f%%/hour", rate);
        ui_draw_centered_text(y, COLOR_TEXT, buf, 1); y += row_h;

        // Qualitative bands, not a device-specific spec - reasonable
        // general guidance for an AMOLED smartwatch's idle-ish drain,
        // not validated against this exact board's capacity/power
        // budget (which isn't known here).
        const char *verdict;
        uint16_t vcolor;
        if (rate <= 0.0f)      { verdict = "Stable";     vcolor = COLOR_GOOD; }
        else if (rate < 5.0f)  { verdict = "Good";        vcolor = COLOR_GOOD; }
        else if (rate < 10.0f) { verdict = "Normal";      vcolor = COLOR_WARN; }
        else                    { verdict = "High - check what's running"; vcolor = COLOR_BAD; }
        ui_draw_centered_text(y, vcolor, verdict, 1);
    }
}

Screen battery_screen = {
    nullptr, GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    battery_create, battery_draw, nullptr, battery_tick, nullptr, nullptr
};
