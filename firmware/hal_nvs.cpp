#include "hal_nvs.h"
#include "app_settings_state.h"
#include "config.h"
#include <Preferences.h>

static Preferences s_prefs;

void nvs_init() {
    // Open-then-close: this only confirms the NVS partition is usable and
    // creates the namespace. The handle MUST NOT stay open afterwards -
    // Preferences::begin() returns false when a namespace is already
    // open (checked against the arduino-esp32 2.0.17 and 3.x sources),
    // so leaving "watch" open here made the very next begin() - the
    // settings load at boot - fail silently and fall back to defaults on
    // EVERY boot. Symptom: onboarding, brightness, radios, calibration
    // and the user's name all "forgotten" after each real reboot.
    if (s_prefs.begin("watch", false)) {
        s_prefs.end();
        DEBUG_PRINTF("[nvs] NVS ready\n");
    } else {
        DEBUG_PRINTF("[nvs] WARNING: NVS namespace open failed\n");
    }
}

bool nvs_save_settings(const AppSettings &s) {
    s_prefs.begin("settings", false);
    s_prefs.putBool("touch_fb", s.touch_feedback);
    s_prefs.putUShort("sleep_s", s.sleep_timeout_s);
    s_prefs.putBool("wake_touch", s.wake_on_touch);
    s_prefs.putBool("wake_motion", s.wake_on_motion);
    s_prefs.putBool("stay_awake_chg", s.stay_awake_charging);
    s_prefs.putBool("haptic_touch", s.haptic_on_touch);
    s_prefs.putUChar("brightness", s.brightness);
    s_prefs.putUChar("wf_idx", s.watchface_index);
    s_prefs.putUChar("volume", s.volume);
    s_prefs.putUChar("mic_sens", s.mic_sens);
    s_prefs.putBool("wifi_on", s.wifi_on);
    s_prefs.putBool("gps_on", s.gps_on);
    s_prefs.putBool("ble_on", s.ble_on);
    s_prefs.putBool("espnow_on", s.espnow_on);
    s_prefs.putBool("auto_rot", s.auto_rotate);
    s_prefs.putBool("touch_cal_on", s.touch_calibrated);
    s_prefs.putFloat("touch_cal_ax", s.touch_cal_ax);
    s_prefs.putFloat("touch_cal_bx", s.touch_cal_bx);
    s_prefs.putFloat("touch_cal_ay", s.touch_cal_ay);
    s_prefs.putFloat("touch_cal_by", s.touch_cal_by);
    s_prefs.putBool("tilt_cal_on", s.tilt_calibrated);
    s_prefs.putChar("tilt_mapx", s.tilt_map_x_from);
    s_prefs.putChar("tilt_signx", s.tilt_sign_x);
    s_prefs.putChar("tilt_mapy", s.tilt_map_y_from);
    s_prefs.putChar("tilt_signy", s.tilt_sign_y);
    s_prefs.putBool("sfx_on", s.sfx_enabled);
    s_prefs.putBool("music_on", s.music_enabled);
    s_prefs.putUChar("usb_mode", s.usb_mode);
    s_prefs.putBool("dnd_on", s.dnd_on);
    s_prefs.putBool("dnd_bed", s.dnd_bedtime);
    s_prefs.putUShort("dnd_from", s.dnd_from_min);
    s_prefs.putUShort("dnd_to", s.dnd_to_min);
    s_prefs.putBool("smooth_font", s.smooth_fonts);
    s_prefs.putUChar("batt_mode", s.battery_mode);
    s_prefs.putBool("sv_wifi", s.saver_prev_wifi);
    s_prefs.putBool("sv_gps", s.saver_prev_gps);
    s_prefs.putBool("sv_espnow", s.saver_prev_espnow);
    s_prefs.putBool("sv_rot", s.saver_prev_autorot);
    s_prefs.putBool("sv_motion", s.saver_prev_motion);
    s_prefs.putBool("onboarded", s.onboarding_complete);
    s_prefs.putString("user_name", s.user_name);
    s_prefs.end();
    return true;
}

bool nvs_load_settings(AppSettings &s) {
    if (!s_prefs.begin("settings", true)) { // read-only; fails if namespace was never written
        DEBUG_PRINTF("[nvs] no saved settings namespace - using defaults\n");
        return false;
    }
    if (!s_prefs.isKey("touch_fb")) {
        s_prefs.end();
        return false; // no saved data - use defaults
    }
    s.touch_feedback = s_prefs.getBool("touch_fb", true);
    // "sleep_s" replaced the old whole-minutes "sleep_min" key.
    if (s_prefs.isKey("sleep_s")) s.sleep_timeout_s = s_prefs.getUShort("sleep_s", 120);
    else s.sleep_timeout_s = (uint16_t)(s_prefs.getInt("sleep_min", 2) * 60);
    s.wake_on_touch = s_prefs.getBool("wake_touch", true);
    s.wake_on_motion = s_prefs.getBool("wake_motion", true);
    s.stay_awake_charging = s_prefs.getBool("stay_awake_chg", false);
    s.haptic_on_touch = s_prefs.getBool("haptic_touch", false);
    s.brightness = s_prefs.getUChar("brightness", DEFAULT_BRIGHTNESS);
    s.watchface_index = s_prefs.getUChar("wf_idx", 0);
    s.volume = s_prefs.getUChar("volume", 80);
    s.mic_sens = s_prefs.getUChar("mic_sens", 3);
    if (s.mic_sens < 1 || s.mic_sens > 5) s.mic_sens = 3;
    s.wifi_on = s_prefs.getBool("wifi_on", false);
    s.gps_on = s_prefs.getBool("gps_on", false);
    s.ble_on = s_prefs.getBool("ble_on", false);
    s.espnow_on = s_prefs.getBool("espnow_on", false);
    s.auto_rotate = s_prefs.getBool("auto_rot", false);
    s.touch_calibrated = s_prefs.getBool("touch_cal_on", false);
    s.touch_cal_ax = s_prefs.getFloat("touch_cal_ax", -1.0f);
    s.touch_cal_bx = s_prefs.getFloat("touch_cal_bx", 465.0f);
    s.touch_cal_ay = s_prefs.getFloat("touch_cal_ay", -1.0f);
    s.touch_cal_by = s_prefs.getFloat("touch_cal_by", 465.0f);
    s.tilt_calibrated = s_prefs.getBool("tilt_cal_on", false);
    s.tilt_map_x_from = s_prefs.getChar("tilt_mapx", 0);
    s.tilt_sign_x = s_prefs.getChar("tilt_signx", 1);
    s.tilt_map_y_from = s_prefs.getChar("tilt_mapy", 1);
    s.tilt_sign_y = s_prefs.getChar("tilt_signy", -1);
    s.sfx_enabled = s_prefs.getBool("sfx_on", true);
    s.music_enabled = s_prefs.getBool("music_on", true);
    s.usb_mode = s_prefs.getUChar("usb_mode", 1);
    s.dnd_on = s_prefs.getBool("dnd_on", false);
    s.dnd_bedtime = s_prefs.getBool("dnd_bed", false);
    s.dnd_from_min = s_prefs.getUShort("dnd_from", 23 * 60);
    s.dnd_to_min = s_prefs.getUShort("dnd_to", 7 * 60);
    if (s.dnd_from_min >= 1440) s.dnd_from_min = 23 * 60;
    if (s.dnd_to_min >= 1440) s.dnd_to_min = 7 * 60;
    s.smooth_fonts = s_prefs.getBool("smooth_font", true);
    s.battery_mode = s_prefs.getUChar("batt_mode", 1);
    s.saver_prev_wifi = s_prefs.getBool("sv_wifi", false);
    s.saver_prev_gps = s_prefs.getBool("sv_gps", false);
    s.saver_prev_espnow = s_prefs.getBool("sv_espnow", false);
    s.saver_prev_autorot = s_prefs.getBool("sv_rot", false);
    s.saver_prev_motion = s_prefs.getBool("sv_motion", true);
    s.onboarding_complete = s_prefs.getBool("onboarded", false);
    s_prefs.getString("user_name", s.user_name, sizeof(s.user_name));
    s_prefs.end();
    return true;
}

void nvs_save_high_score(const char *game, uint32_t score) {
    char ns[16];
    snprintf(ns, sizeof(ns), "hs_%s", game);
    s_prefs.begin(ns, false);
    s_prefs.putUInt("score", score);
    s_prefs.end();
}

uint32_t nvs_load_high_score(const char *game) {
    char ns[16];
    snprintf(ns, sizeof(ns), "hs_%s", game);
    s_prefs.begin(ns, true);
    uint32_t score = s_prefs.getUInt("score", 0);
    s_prefs.end();
    return score;
}

void nvs_save_steps(uint32_t steps) {
    s_prefs.begin("steps", false);
    s_prefs.putUInt("count", steps);
    s_prefs.end();
}

// Day the saved count belongs to (yyyymmdd), so steps restart at midnight.
void nvs_save_steps_day(uint32_t day) {
    s_prefs.begin("steps", false);
    s_prefs.putUInt("day", day);
    s_prefs.end();
}

uint32_t nvs_load_steps_day() {
    s_prefs.begin("steps", true);
    uint32_t d = s_prefs.getUInt("day", 0);
    s_prefs.end();
    return d;
}

uint32_t nvs_load_steps() {
    s_prefs.begin("steps", true);
    uint32_t steps = s_prefs.getUInt("count", 0);
    s_prefs.end();
    return steps;
}

void nvs_save_wifi(const char *ssid, const char *pass) {
    s_prefs.begin("wifi", false);
    s_prefs.putString("ssid", ssid);
    s_prefs.putString("pass", pass);
    s_prefs.end();
}

bool nvs_load_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    s_prefs.begin("wifi", true);
    if (!s_prefs.isKey("ssid")) {
        s_prefs.end();
        return false;
    }
    s_prefs.getString("ssid", ssid, ssid_len);
    s_prefs.getString("pass", pass, pass_len);
    s_prefs.end();
    return true;
}

void nvs_save_bambu_code(const char *code) {
    s_prefs.begin("bambu", false);
    s_prefs.putString("code", code);
    s_prefs.end();
}

bool nvs_load_bambu_code(char *code, size_t code_len) {
    s_prefs.begin("bambu", true);
    if (!s_prefs.isKey("code")) {
        s_prefs.end();
        return false;
    }
    s_prefs.getString("code", code, code_len);
    s_prefs.end();
    return true;
}

void nvs_save_watchface_index(uint8_t idx) {
    s_prefs.begin("settings", false);
    s_prefs.putUChar("wf_idx", idx);
    s_prefs.end();
}

uint8_t nvs_load_watchface_index() {
    s_prefs.begin("settings", true);
    uint8_t idx = s_prefs.getUChar("wf_idx", 0);
    s_prefs.end();
    return idx;
}

bool nvs_save_watchface_json(const char *json, int len) {
    s_prefs.begin("wfdata", false);
    s_prefs.putBytes("json", json, len);
    s_prefs.putBool("has_wf", true);
    s_prefs.end();
    DEBUG_PRINTF("[nvs] watchface JSON saved: %d bytes\n", len);
    return true;
}

bool nvs_load_watchface_json(char *buf, int buf_size, int *out_len) {
    s_prefs.begin("wfdata", true);
    if (!s_prefs.isKey("has_wf") || !s_prefs.getBool("has_wf", false)) {
        s_prefs.end();
        return false;
    }
    size_t len = s_prefs.getBytes("json", buf, buf_size);
    s_prefs.end();
    if (len == 0) return false;
    *out_len = (int)len;
    DEBUG_PRINTF("[nvs] watchface JSON loaded: %d bytes\n", *out_len);
    return true;
}

void nvs_clear_watchface_json() {
    s_prefs.begin("wfdata", false);
    s_prefs.remove("json");
    s_prefs.putBool("has_wf", false);
    s_prefs.end();
    DEBUG_PRINTF("[nvs] watchface JSON cleared\n");
}

bool nvs_has_watchface_json() {
    s_prefs.begin("wfdata", true);
    bool has = s_prefs.getBool("has_wf", false);
    s_prefs.end();
    return has;
}
