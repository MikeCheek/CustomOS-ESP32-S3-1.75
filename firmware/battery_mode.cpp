#include "battery_mode.h"
#include "config.h"
#include "app_settings_state.h"
#include "hal_display.h"
#include "hal_nvs.h"
#include "hal_wifi.h"
#include "hal_ntp.h"
#include "hal_gps.h"
#include "hal_espnow.h"
#include "ui.h"

static const BatteryProfile PROFILES[BATT_MODE_COUNT] = {
    { "Performance", "Full brightness, 5 min timeout",   "Fastest CPU, everything on",
      COLOR_WARN,    255, 300, false, false, false, false, 240 },
    { "Balanced",    "Normal brightness, 2 min timeout", "Everything on (default)",
      COLOR_ACCENT2, DEFAULT_BRIGHTNESS, 120, false, false, false, false, 240 },
    { "Saver",       "Dimmer, 1 min timeout",            "Wi-Fi, GPS, auto-rotate off",
      COLOR_GOOD,    100, 60,  true,  false, true,  false, 240 },
    { "Ultra saver", "Very dim, 30 s timeout, 160 MHz",  "Radios off except Bluetooth",
      COLOR_ACCENT,  40,  30,  true,  true,  true,  true,  160 },
};

const BatteryProfile &battery_profile(BatteryMode m) {
    return PROFILES[m < BATT_MODE_COUNT ? m : BATT_BALANCED];
}

BatteryMode battery_mode_current() {
    uint8_t m = g_app_settings.battery_mode;
    return m < BATT_MODE_COUNT ? (BatteryMode)m : BATT_BALANCED;
}

static bool is_saving(BatteryMode m) { return m == BATT_SAVER || m == BATT_ULTRA; }

void battery_mode_apply(BatteryMode m) {
    if (m >= BATT_MODE_COUNT) return;
    AppSettings &s = g_app_settings;
    const BatteryProfile &p = PROFILES[m];
    BatteryMode from = battery_mode_current();

    // Entering a saving mode from a normal one: remember what it turns off.
    if (is_saving(m) && !is_saving(from)) {
        s.saver_prev_wifi    = s.wifi_on;
        s.saver_prev_gps     = s.gps_on;
        s.saver_prev_espnow  = s.espnow_on;
        s.saver_prev_autorot = s.auto_rotate;
        s.saver_prev_motion  = s.wake_on_motion;
    }

    s.brightness = p.brightness;
    s.sleep_timeout_s = p.sleep_s;

    if (is_saving(m)) {
        if (p.radios_off) {
            if (s.wifi_on) { s.wifi_on = false; wifi_disable(); }
            if (s.gps_on)  { s.gps_on = false;  gps_disable(); }
        }
        if (p.espnow_off && s.espnow_on) { s.espnow_on = false; espnow_disable(); }
        else if (!p.espnow_off && is_saving(from) && s.saver_prev_espnow && !s.espnow_on) {
            s.espnow_on = true; espnow_enable();       // Ultra -> Saver gives the joystick back
        }
        if (p.autorot_off && s.auto_rotate) { s.auto_rotate = false; ui_reset_auto_rotate_state(); }
        if (p.motion_wake_off) s.wake_on_motion = false;
        else if (is_saving(from)) s.wake_on_motion = s.saver_prev_motion;
    } else if (is_saving(from)) {
        // Back to a normal mode: undo what the saving mode switched off.
        if (s.saver_prev_wifi && !s.wifi_on)     { s.wifi_on = true; wifi_enable(); ntp_sync(); }
        if (s.saver_prev_gps && !s.gps_on)       { s.gps_on = true; gps_enable(); }
        if (s.saver_prev_espnow && !s.espnow_on) { s.espnow_on = true; espnow_enable(); }
        if (s.saver_prev_autorot && s.tilt_calibrated) s.auto_rotate = true;
        s.wake_on_motion = s.saver_prev_motion;
    }

    s.battery_mode = (uint8_t)m;
    display_set_brightness(s.brightness);
    nvs_save_settings(s);
    // CPU cap: power_update_sleep_policy() (called every loop) notices the
    // changed battery_mode_cpu_max_mhz() and reconfigures esp_pm.
}

BatteryMode battery_mode_next(BatteryMode m) {
    return (BatteryMode)((m + 1) % BATT_MODE_COUNT);
}

bool battery_mode_adjusted() {
    const AppSettings &s = g_app_settings;
    const BatteryProfile &p = battery_profile(battery_mode_current());
    if (s.brightness != p.brightness || s.sleep_timeout_s != p.sleep_s) return true;
    if (p.radios_off && (s.wifi_on || s.gps_on)) return true;
    if (p.espnow_off && s.espnow_on) return true;
    if (p.autorot_off && s.auto_rotate) return true;
    if (p.motion_wake_off && s.wake_on_motion) return true;
    return false;
}

uint16_t battery_mode_cpu_max_mhz() { return battery_profile(battery_mode_current()).cpu_max_mhz; }

uint32_t battery_mode_sleep_timeout_ms() { return (uint32_t)g_app_settings.sleep_timeout_s * 1000UL; }

// ---- icon -------------------------------------------------------------------
#include <Arduino_GFX_Library.h>

void battery_mode_draw_icon(Arduino_GFX *g, BatteryMode m, int cx, int cy, uint16_t c, uint16_t bg) {
    if (!g) return;
    // Battery outline (vertical), shared by all four.
    g->fillRect(cx - 4, cy - 15, 8, 3, c);
    g->fillRoundRect(cx - 9, cy - 12, 18, 27, 4, c);
    g->fillRoundRect(cx - 6, cy - 9, 12, 21, 2, bg);
    switch (m) {
    case BATT_PERFORMANCE:   // lightning bolt
        g->fillTriangle(cx + 2, cy - 9, cx - 5, cy + 2, cx + 1, cy + 2, c);
        g->fillTriangle(cx - 1, cy - 1, cx + 5, cy - 1, cx - 2, cy + 11, c);
        break;
    case BATT_BALANCED:      // half full
        g->fillRect(cx - 4, cy + 1, 8, 9, c);
        break;
    case BATT_ULTRA:         // ring around, then the leaf
        ui_arc(cx, cy, 21, 3, 30, 300, c);
        // fall through
    case BATT_SAVER:         // leaf
        g->fillTriangle(cx - 5, cy + 8, cx - 3, cy - 1, cx + 5, cy - 6, c);
        g->fillTriangle(cx - 5, cy + 8, cx + 5, cy - 6, cx + 3, cy + 3, c);
        break;
    default: break;
    }
}
