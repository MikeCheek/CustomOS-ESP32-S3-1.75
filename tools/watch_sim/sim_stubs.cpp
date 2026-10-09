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
    uint32_t s = g_sim.second + millis() / 1000;
    return { 2026, 10, 8, g_sim.hour, g_sim.minute + (int)(s / 60) % 60, (int)(s % 60), 4, true };
}
ImuSample imu_read() { return { true, 0, 0, 1, 0, 0, 0, 25 }; }
void imu_get_calibrated_tilt(const ImuSample &, float &x, float &y) { x = y = 0; }
uint32_t imu_get_step_count() { return g_sim.steps; }
void imu_step_counter_update(const ImuSample &) {}
TouchPoint touch_read() { return { false, 0, 0 }; }
bool touch_read_raw(uint16_t &, uint16_t &) { return false; }
void touch_set_gesture_mode(uint8_t) {}
void touch_set_fast_taps(bool) {}
void touch_cancel_swipes() {}
bool touch_swipe_down_detected() { return false; }
bool touch_swipe_up_detected() { return false; }
bool touch_swipe_left_detected() { return false; }
bool touch_swipe_right_detected() { return false; }
GpsPresence gps_presence() { return GPS_PRESENT; }
bool gps_is_enabled() { return false; }
void gps_enable() {}
void gps_disable() {}
void display_set_brightness(uint8_t) {}
void display_sleep() {}
void display_wakeup() {}

// ---- radios ------------------------------------------------------------------------
bool ble_is_enabled() { return true; }
bool ble_is_connected() { return g_sim.phone; }
bool ble_enable() { return true; }
void ble_disable() {}
bool ble_link_encrypted() { return true; }
bool ble_link_send(const char *) { return true; }
// Phone-link messages queued by sim_link() - the same JSON the app sends.
#include <deque>
#include <string>
static std::deque<std::string> s_link_q;
void sim_link(const char *json) { s_link_q.push_back(json); }
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
bool ble_get_fitness(const char **, int *) { return false; }
bool ble_get_weather(const char **d, int *n) {
    static const char W[] = "{\"tempC\":19,\"condition\":\"Rain\",\"humidity\":80}";
    *d = W; *n = sizeof(W) - 1; return true;
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
uint8_t audio_get_volume() { return 70; }
void audio_set_volume(uint8_t) {}
void audio_beep(uint16_t, uint16_t) {}
void audio_play_sfx(uint16_t, uint16_t) {}
bool audio_play_mp3(const char *) { return false; }
bool audio_play_wav(const char *) { return false; }
bool audio_is_playing() { return false; }
bool audio_is_paused() { return false; }
void audio_set_paused(bool) {}
void audio_stop_playback() {}
void audio_seek(float) {}
bool audio_get_stream_info(AudioStreamInfo *) { return false; }
bool audio_is_recording() { return g_sim.recording; }
bool audio_start_record(const char *) { g_sim.recording = true; return true; }
bool audio_start_record_mono(const char *) { return false; }
void audio_stop_record() { g_sim.recording = false; }
uint32_t audio_record_duration_s() { return g_sim.recording ? 83 : 0; }
bool audio_mic_ok() { return true; }
int audio_mic_level_percent() { return 40; }
void audio_set_mic_sensitivity(uint8_t) {}
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
