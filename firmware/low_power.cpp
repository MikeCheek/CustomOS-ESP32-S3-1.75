// Low Power Mode - see low_power.h.
#include "low_power.h"
#include "config.h"
#include "app_settings_state.h"
#include "battery_mode.h"
#include "hal_nvs.h"
#include "hal_power.h"
#include "haptics.h"
#include "ui.h"
#include <Arduino.h>

bool low_power_on() { return g_app_settings.low_power; }

void low_power_set(bool on) {
    AppSettings &s = g_app_settings;
    if (on == s.low_power) return;
    if (on) {
        s.lp_prev_aod = s.aod_on;
        s.lp_prev_wx = s.weather_fx;
        s.lp_prev_wake = s.voice_wake;
        s.lp_prev_mode = (uint8_t)battery_mode_current();
        s.low_power = true;
        s.aod_on = false;
        s.weather_fx = false;
        s.voice_wake = false;
        battery_mode_apply(BATT_SAVER);   // saves the settings too
    } else {
        s.low_power = false;
        s.aod_on = s.lp_prev_aod;
        s.weather_fx = s.lp_prev_wx;
        s.voice_wake = s.lp_prev_wake;
        BatteryMode m = (BatteryMode)s.lp_prev_mode;
        battery_mode_apply(m == BATT_SAVER || m == BATT_ULTRA ? BATT_BALANCED : m);
    }
    nvs_save_settings(s);
}

static void offer_cb(bool yes) {
    if (yes) { low_power_set(true); haptic(HAP_SUCCESS); }
}

void low_power_update() {
    static uint32_t last = 0;
    static bool offered = false;
    uint32_t now = millis();
    if (now - last < 5000) return;
    last = now;
    int pct = power_get_battery_percent();
    bool chg = power_is_charging();
    if (pct < 0) return;
    if (chg) offered = false;   // a new discharge gets a new offer
    if (g_app_settings.low_power && chg && pct >= 80) {
        low_power_set(false);
        ui_show_toast("Charged to 80% - Low Power Mode is off", 3000);
        return;
    }
    if (!g_app_settings.low_power && !chg && pct <= 10 && !offered && !ui_confirm_active()) {
        offered = true;
        haptic(HAP_REMINDER);
        ui_show_confirm("Battery at 10%", "Turn on Low Power Mode? Saver mode, always-on and the wake word off.",
                        "Turn on", "Not now", offer_cb);
    }
}
