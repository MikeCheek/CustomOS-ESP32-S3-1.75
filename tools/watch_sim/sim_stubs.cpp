#include <sys/stat.h>
#include <SD.h>
#include <time.h>
#include "board_pins.h"
#include "app_settings_state.h"
// Stand-ins for the hardware layers (hal_*.cpp, diag.cpp, the .ino's
// helpers) with demo values, so screens show a lived-in watch.
#include <Arduino.h>
#include <math.h>
#include <ArduinoJson.h>
#include "config.h"
#include "ui.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_imu.h"
#include "hal_touch.h"
#include "hal_ble.h"
#include "hal_gps.h"
#include "hal_usb.h"
#include "hal_audio.h"
#include "hal_ota.h"
#include "hal_fwupdate.h"
#include "media_decode.h"
#include "diag.h"
#include "sim_state.h"

SimState g_sim;

// ---- power / clock / sensors ------------------------------------------------
PowerStatus power_read() { return { true, 4.02f, g_sim.battery, g_sim.charging, g_sim.charging, 5.0f, 4.0f, 31.0f }; }
int power_get_battery_percent() { return g_sim.battery; }
bool power_is_charging() { return g_sim.charging; }
bool power_pek_long_press_pending() { return false; }
bool power_pek_short_press_pending() { return false; }
bool power_auto_sleep_available() { return true; }
bool power_since_unplugged(int *p, uint32_t *ms) { if (p) *p = 97; if (ms) *ms = 5400000UL; return true; }
void power_poll_pek_button() {}
void power_shutdown() {}
WatchTime rtc_now() {
    if (g_sim.live) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        return { t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday, true };
    }
    uint32_t s = g_sim.second + millis() / 1000;
    return { 2026, 10, 8, g_sim.hour, g_sim.minute + (int)(s / 60) % 60, (int)(s % 60), 4, true };
}
// The accelerometer: flat for the screenshots, the phone's own in the
// emulator (sim_set_accel, ~62.5 Hz like the QMI8658), also into the FIFO.
static float s_acc[3] = {0, 0, 1};
static float s_fifo[256][3];
static int s_fifo_n = 0;
static bool s_fifo_on = false;
void sim_set_accel(float x, float y, float z) {
    s_acc[0] = x; s_acc[1] = y; s_acc[2] = z;
    if (s_fifo_on && s_fifo_n < 256) { s_fifo[s_fifo_n][0] = x; s_fifo[s_fifo_n][1] = y; s_fifo[s_fifo_n][2] = z; s_fifo_n++; }
}
ImuSample imu_read() { return { true, s_acc[0], s_acc[1], s_acc[2], 0, 0, 0, 25 }; }
bool imu_fifo_enable(bool on) { s_fifo_on = on; if (!on) s_fifo_n = 0; return true; }
int imu_fifo_read(float *xyz, int max) {
    int n = s_fifo_n < max ? s_fifo_n : max;
    memcpy(xyz, s_fifo, n * 3 * sizeof(float));
    memmove(s_fifo, s_fifo[n], (s_fifo_n - n) * 3 * sizeof(float));
    s_fifo_n -= n;
    return n;
}
// Same axis mapping as hal_imu.cpp (Settings > Calibration).
void imu_get_calibrated_tilt(const ImuSample &sample, float &tilt_x, float &tilt_y) {
    float raw_axes[2] = { sample.ax, sample.ay };
    int8_t mx = g_app_settings.tilt_map_x_from, my = g_app_settings.tilt_map_y_from;
    if (mx < 0 || mx > 1) mx = 0;
    if (my < 0 || my > 1) my = 1;
    tilt_x = (float)g_app_settings.tilt_sign_x * raw_axes[mx];
    tilt_y = (float)g_app_settings.tilt_sign_y * raw_axes[my];
}
uint32_t imu_get_step_count() { return g_sim.steps; }
void imu_step_counter_update(const ImuSample &) {}
TouchPoint touch_read() { return { false, 0, 0 }; }
bool touch_read_raw(uint16_t &, uint16_t &) { return false; }
// Swipes, classified like hal_touch.cpp's evaluate_swipe() from the
// touches the emulator passes in (sim_touch_track); the screenshot
// scenarios never swipe.
static uint8_t s_gmode = 0;
static int s_sx, s_sy, s_lx, s_ly;
static uint32_t s_sms = 0, s_last_swipe = 0;
static bool s_tracking = false, s_sw_l = false, s_sw_r = false, s_sw_u = false, s_sw_d = false;
void sim_touch_track(int x, int y, bool down) {
    uint32_t now = millis();
    if (down) {
        if (!s_tracking) { s_tracking = true; s_sx = x; s_sy = y; s_sms = now; }
        s_lx = x; s_ly = y;
        return;
    }
    if (!s_tracking) return;
    s_tracking = false;
    if (now - s_last_swipe < 350 || now - s_sms >= 500) return;
    int dx = s_lx - s_sx, dy = s_ly - s_sy, adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int cx = s_sx - LCD_WIDTH / 2, cy = s_sy - LCD_HEIGHT / 2;
    bool on_screen = cx * cx + cy * cy <= (LCD_WIDTH / 2) * (LCD_WIDTH / 2);
    bool edge = s_gmode == 1;
    if (dx > 55 && ady < 50 && (!edge || (s_sx < 70 && on_screen))) { s_sw_r = true; s_last_swipe = now; return; }
    if (dx < -55 && ady < 50 && (!edge || (s_sx > LCD_WIDTH - 70 && on_screen))) { s_sw_l = true; s_last_swipe = now; return; }
    if (dy < -55 && adx < 50) { s_sw_u = true; s_last_swipe = now; return; }
    if (dy > 50 && adx < 50 && (!edge || s_sy < 100)) { s_sw_d = true; s_last_swipe = now; return; }
}
static bool take(bool &f) { bool v = f; f = false; return v; }
void touch_set_gesture_mode(uint8_t m) { s_gmode = m; }
void touch_set_fast_taps(bool) {}
void touch_cancel_swipes() { s_sw_l = s_sw_r = s_sw_u = s_sw_d = false; }
bool touch_swipe_down_detected() { return take(s_sw_d); }
bool touch_swipe_up_detected() { return take(s_sw_u); }
bool touch_swipe_left_detected() { return take(s_sw_l); }
bool touch_swipe_right_detected() { return take(s_sw_r); }
GpsPresence gps_presence() { return GPS_PRESENT; }
bool gps_is_enabled() { return false; }
void gps_enable() {}
void gps_disable() {}
GpsFix gps_get_fix() { return GpsFix{}; }
void display_set_brightness(uint8_t) {}
void display_sleep() {}
void display_wakeup() {}

// ---- radios ------------------------------------------------------------------------
bool ble_is_enabled() { return true; }
bool ble_is_connected() { return g_sim.phone; }
bool ble_enable() { return true; }
void ble_disable() {}
bool ble_link_encrypted() { return true; }
// Phone-link messages queued by sim_link() - the same JSON the app sends -
// and, in the emulator, the watch's own messages for the app (sim_link_pop).
#include <deque>
#include <map>
#include <string>
static std::deque<std::string> s_link_q, s_out_q;
void sim_link(const char *json) { s_link_q.push_back(json); }
bool ble_link_send(const char *json) {
    if (!g_sim.live) return true;
    if (!g_sim.phone) return false;
    if (s_out_q.size() < 64) s_out_q.push_back(json);
    return true;
}
bool sim_link_pop(std::string &out) {
    if (s_out_q.empty()) return false;
    out.swap(s_out_q.front());
    s_out_q.pop_front();
    return true;
}
bool ble_link_recv(char *out, int cap) {
    if (s_link_q.empty()) return false;
    snprintf(out, cap, "%s", s_link_q.front().c_str());
    s_link_q.pop_front();
    return true;
}
int ble_bond_count() { return 1; }
bool ble_forget_bonds() { return true; }
int ble_get_connect_count() { return 3; }
const char *ble_get_connected_device_name() { return "Pixel 9"; }
uint32_t ble_get_passkey() { return 0; }
int ble_take_pairing_result() { return 0; }
void ble_fold_utf8(char *) {}
bool ble_get_notification(BleNotification &) { return false; }
bool ble_get_contacts(const char **, int *) { return false; }
void ble_consume_contacts() {}
// Weather / fitness: a fixed sample for the screenshots, what the app sent
// in the emulator (sim_set_weather / sim_set_fitness).
static std::string s_weather, s_fitness;
void sim_set_weather(const char *json) { s_weather = json; }
void sim_set_fitness(const char *json) { s_fitness = json; }
bool ble_get_fitness(const char **d, int *n) {
    if (s_fitness.empty()) return false;
    *d = s_fitness.c_str(); *n = (int)s_fitness.size(); return true;
}
bool ble_get_weather(const char **d, int *n) {
    if (g_sim.live) {
        if (s_weather.empty()) return false;
        *d = s_weather.c_str(); *n = (int)s_weather.size(); return true;
    }
    static const char W[] = "{\"tempC\":19,\"condition\":\"Rain\",\"humidity\":80,\"forecast\":[{\"day\":\"Fri\",\"high\":22,\"low\":13,\"cond\":\"Partly cloudy\"},{\"day\":\"Sat\",\"high\":24,\"low\":14,\"cond\":\"Clear\"},{\"day\":\"Sun\",\"high\":18,\"low\":11,\"cond\":\"Storm\"}]}";
    *d = W; *n = sizeof(W) - 1; return true;
}

// ---- NVS (shim/Preferences.h): a store saved to a file, emulator only -----------------
static std::map<std::string, std::string> s_prefs;
static std::string s_prefs_path;
bool sim_prefs_live() { return !s_prefs_path.empty(); }
bool sim_prefs_get(const std::string &k, std::string &out) {
    auto it = s_prefs.find(k);
    if (it == s_prefs.end()) return false;
    out = it->second;
    return true;
}
void sim_prefs_put(const std::string &k, const void *v, size_t n) { s_prefs[k].assign((const char *)v, n); }
void sim_prefs_erase(const std::string &k) { s_prefs.erase(k); }
void sim_prefs_clear_ns(const std::string &ns) {
    for (auto it = s_prefs.begin(); it != s_prefs.end();)
        it = it->first.compare(0, ns.size() + 1, ns + "/") == 0 ? s_prefs.erase(it) : std::next(it);
}
// file: repeated  key length (u32), key, value length (u32), value
void sim_prefs_save() {
    if (s_prefs_path.empty()) return;
    std::string tmp = s_prefs_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    for (auto &e : s_prefs) {
        uint32_t a = (uint32_t)e.first.size(), b = (uint32_t)e.second.size();
        fwrite(&a, 4, 1, f); fwrite(e.first.data(), 1, a, f);
        fwrite(&b, 4, 1, f); fwrite(e.second.data(), 1, b, f);
    }
    fclose(f);
    rename(tmp.c_str(), s_prefs_path.c_str());
}
void sim_prefs_enable(const char *path) {
    s_prefs_path = path;
    s_prefs.clear();
    FILE *f = fopen(path, "rb");
    if (!f) return;
    for (;;) {
        uint32_t a, b;
        if (fread(&a, 4, 1, f) != 1 || a > 4096) break;
        std::string k(a, 0);
        if (fread(&k[0], 1, a, f) != a || fread(&b, 4, 1, f) != 1 || b > 1 << 20) break;
        std::string v(b, 0);
        if (b && fread(&v[0], 1, b, f) != b) break;
        s_prefs[k] = v;
    }
    fclose(f);
}
bool ble_get_watchface_data(const char **, int *) { return false; }
bool ble_watchface_ready() { return false; }
void ble_consume_watchface() {}
bool wifi_is_connected() { return g_sim.wifi; }
void wifi_enable() {}
void wifi_disable() {}
void wifi_xfer_request(bool) {}
void wifi_cfg_handle(JsonDocument &) {}
void ntp_sync() {}
bool espnow_is_enabled() { return false; }
void espnow_enable() {}
void espnow_disable() {}

// ---- audio -----------------------------------------------------------------------------
// Sound: in the emulator the tones are queued for the app to play
// (sim_take_tones); the screenshots stay silent.
struct SimTone { uint16_t f, ms; uint8_t kind; };   // kind 0 beep / effect, 1 piano note
static SimTone s_tones[64];
static int s_tone_n = 0;
static uint8_t s_volume = 70;
static void sim_tone(uint16_t f, uint16_t ms, uint8_t kind) {
    if (g_sim.live && s_tone_n < 64) s_tones[s_tone_n++] = { f, ms, kind };
}
int sim_take_tones(int32_t *out, int max) {
    int n = s_tone_n < max ? s_tone_n : max;
    for (int i = 0; i < n; i++) {
        out[i * 3] = s_tones[i].f;
        out[i * 3 + 1] = s_tones[i].ms;
        out[i * 3 + 2] = s_tones[i].kind | (s_volume << 8);
    }
    memmove(s_tones, s_tones + n, (s_tone_n - n) * sizeof(SimTone));
    s_tone_n -= n;
    return n;
}
uint8_t audio_get_volume() { return g_sim.live ? s_volume : 70; }
void audio_set_volume(uint8_t v) { s_volume = v > 100 ? 100 : v; }
void audio_beep(uint16_t f, uint16_t ms) { sim_tone(f, ms, 0); }
void audio_play_sfx(uint16_t f, uint16_t ms) { sim_tone(f, ms, 0); }
// Playback and recording: in the emulator they're events for the app
// (sim_take_event: the phone plays the file, the phone's microphone records
// into the SD folder); the screenshots fake a recording.
static std::deque<std::string> s_events;
static bool s_playing = false;
static uint32_t s_rec_ms = 0;
static int s_mic_level = 0;
static void sim_emit(const std::string &json) { if (s_events.size() < 32) s_events.push_back(json); }
static std::string json_str(const std::string &v) {
    std::string o = "\"";
    for (char c : v) { if (c == '"' || c == '\\') o += '\\'; o += c; }
    return o + "\"";
}
bool sim_take_event(std::string &out) {
    if (s_events.empty()) return false;
    out.swap(s_events.front());
    s_events.pop_front();
    return true;
}
void sim_set_playing(bool on) { s_playing = on; }
void sim_set_mic_level(int pct) { s_mic_level = pct; }
static bool sim_play(const char *path) {
    if (!g_sim.live || !SD.exists(path)) return false;
    s_playing = true;
    sim_emit("{\"ev\":\"play\",\"path\":" + json_str(sim_sd_path(path)) + "}");
    return true;
}
bool audio_play_mp3(const char *p) { return sim_play(p); }
bool audio_play_wav(const char *p) { return sim_play(p); }
bool audio_is_playing() { return s_playing; }
bool audio_is_paused() { return false; }
void audio_set_paused(bool) {}
void audio_stop_playback() {
    if (s_playing) sim_emit("{\"ev\":\"stop\"}");
    s_playing = false;
}
void audio_seek(float) {}
bool audio_get_stream_info(AudioStreamInfo *) { return false; }
bool audio_is_recording() { return g_sim.recording; }
static bool sim_record(const char *file, bool mono) {
    g_sim.recording = true;
    if (!g_sim.live) return true;
    if (!file || strchr(file, '/')) { g_sim.recording = false; return false; }
    SD.mkdir("/Recordings");
    s_rec_ms = millis();
    sim_emit(std::string("{\"ev\":\"rec\",\"on\":1,\"mono\":") + (mono ? "1" : "0") + ",\"path\":" +
             json_str(sim_sd_path((std::string("/Recordings/") + file).c_str())) + "}");
    return true;
}
bool audio_start_record(const char *f) { return sim_record(f, false); }
bool audio_start_record_mono(const char *f) { return sim_record(f, true); }
void audio_stop_record() {
    if (g_sim.live && g_sim.recording) sim_emit("{\"ev\":\"rec\",\"on\":0}");
    g_sim.recording = false;
}
uint32_t audio_record_duration_s() {
    if (!g_sim.recording) return 0;
    return g_sim.live ? (millis() - s_rec_ms) / 1000 : 83;
}
bool audio_mic_ok() { return true; }
int audio_mic_level_percent() { return g_sim.live ? s_mic_level : 40; }
void audio_set_mic_sensitivity(uint8_t) {}
void audio_play_note(uint16_t f, uint16_t ms) { sim_tone(f, ms, 1); }
// Speech-like: syllables (~4/s) of a few harmonics, mic 2 a bit quieter and later.
int audio_get_mic_waveform(int16_t *l, int16_t *r, int n) {
    if (!g_sim.recording) return 0;
    float t0 = millis() / 1000.0f;
    for (int c = 0; c < 2; c++) {
        float ts = t0 - c * 0.04f;
        float syl = sinf(ts * 2 * 3.14159f * 3.7f), env = syl > 0 ? syl : 0;
        env *= 0.55f + 0.45f * sinf(ts * 0.9f);
        env *= c ? 0.7f : 1.0f;
        for (int i = 0; i < n; i++) {
            float t = ts + i / 16000.0f;
            float v = env * (0.6f * sinf(t * 2 * 3.14159f * 180) + 0.3f * sinf(t * 2 * 3.14159f * 410 + c) +
                             0.15f * sinf(t * 2 * 3.14159f * 1250));
            (c ? r : l)[i] = (int16_t)(v * 9000);
        }
    }
    return n;
}

// ---- storage / usb / media ------------------------------------------------------------
bool sd_is_mounted() { return true; }
// The SD card (shim/FS.h): a folder in the emulator, nothing for the screenshots.
SDFS SD;
std::string &sim_sd_root() { static std::string r; return r; }
void sim_sd_enable(const char *dir) {
    sim_sd_root() = dir;
    mkdir(dir, 0755);
    SD.mkdir("/Recordings");
}
bool usb_cable_present() { return false; }
bool usb_host_connected() { return false; }
UsbMode usb_mode_active() { return USB_MODE_FIRMWARE; }
UsbMode usb_mode_saved() { return USB_MODE_FIRMWARE; }
const char *usb_mode_name(UsbMode) { return "Firmware & Debug"; }
const char *usb_mode_status_text() { return "No cable"; }
bool usb_mode_supported(UsbMode m) { return m <= USB_MODE_FIRMWARE; }
bool usb_mode_switch_needs_restart(UsbMode) { return false; }
UsbSwitchResult usb_mode_select(UsbMode) { return USB_SWITCH_DONE; }
bool usb_mode_restart_pending() { return false; }
void usb_mode_restart() {}
void usb_enter_download_mode() {}
bool usb_msc_host_active() { return false; }
uint32_t usb_msc_activity_ms() { return 0; }
bool usb_remote_tap(uint16_t) { return false; }
bool media_is_image(const char *) { return false; }
bool media_is_video(const char *) { return false; }
bool img_decode_file(const char *, int, int, bool, DecodedImage *, char *, size_t) { return false; }
void img_free(DecodedImage *) {}
bool img_decode_jpeg_mem(const uint8_t *, size_t, int, int, bool, DecodedImage *) { return false; }
void video_open(const char *) {}
bool video_poster(const char *, int, int, bool, DecodedImage *) { return false; }
uint32_t recordings_revision() { return 1; }
bool recordings_file_busy(const char *) { return false; }

// ---- diagnostics / updates ------------------------------------------------------------------
const char *diag_fw_version() { return FW_VERSION; }
const char *diag_reset_reason() { return "Power on"; }
const char *diag_crash_text() { return ""; }
uint32_t diag_crash_id() { return 0; }
void diag_clear_crash() {}
void diag_mark_good() {}
void diag_status_text(char *out, int cap) {
    snprintf(out, cap, "Firmware %s\nUptime 2 h 14 min\nMemory: 182 KB internal, 7.1 MB PSRAM free\n"
                       "Bluetooth: connected (Pixel 9)\nWi-Fi: off\nBattery 82%% (4.02 V)", FW_VERSION);
}
OtaState ota_state() { return OTA_IDLE; }
uint32_t ota_total() { return 0; }
uint32_t ota_written() { return 0; }
const char *ota_error() { return ""; }
void ota_cancel() {}
FwupState fwup_state() { return (FwupState)g_sim.fwup; }
void fwup_check() {}
void fwup_install() {}
void fwup_cancel() {}
void fwup_dismiss() {}
bool fwup_failed_installing() { return false; }
const char *fwup_latest_version() { return "3.5.0"; }
const char *fwup_error() { return ""; }
uint32_t fwup_total() { return 2401919; }
uint32_t fwup_written() { return 1530000; }

// Screens that need hardware the simulator doesn't have.
static void none() {}
Screen bambu_printer_screen = { "Printer", GESTURE_MODE_EDGE, 33, none, none, nullptr, nullptr, nullptr, nullptr };
Screen espnow_status_screen = { "ESP-NOW", GESTURE_MODE_EDGE, 33, none, none, nullptr, nullptr, nullptr, nullptr };
Screen wifi_setup_screen = { "WiFi Setup", GESTURE_MODE_EDGE, 33, none, none, nullptr, nullptr, nullptr, nullptr };
uint64_t sd_card_size_mb() { return 30436; }
String sd_list_root(int) { return String("Music/\nRecordings/\nPhotos/\nnotes.txt  (2 KB)\n"); }
