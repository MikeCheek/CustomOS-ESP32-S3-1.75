/*
 * app_settings_state.h
 * Small shared struct for user-toggleable options that more than one
 * file needs to see (e.g. hal_touch.cpp reading whether to draw touch
 * feedback, app_settings.cpp writing it). Kept as one plain global
 * struct rather than getters/setters per option - add a field here and
 * a row in app_settings.cpp's build_toggle_row() calls to expose a new
 * one.
 *
 * Persisted across reboots via NVS (see hal_nvs.cpp). On first boot,
 * the struct defaults below are used; on later boots, NVS values
 * override them.
 *
 * To expose a new toggleable option: add a field to AppSettings, then
 * call make_toggle_row() in app_settings.cpp to add a UI row for it.
 */

#pragma once
#include "config.h"

struct AppSettings {
    bool touch_feedback = true; // show a dot under your finger while touching

    // Screen timeout in seconds, set by the battery mode (battery_mode.h).
    // 0 = never auto-sleep. Display is fully hardware-powered-off while
    // asleep (see hal_display.cpp's display_sleep()), not just dimmed.
    uint16_t sleep_timeout_s = 120;
    bool wake_on_touch = true;
    bool wake_on_motion = true;
    // Skips the sleep_timeout_s auto-sleep entirely while the watch
    // is on charge (power_is_charging(), hal_power.h) - no reason to
    // dim/lock a watch sitting on its charger.
    bool stay_awake_charging = false;
    // Brief vibrate_buzz() (hal_vibrate.h) on every touch press - see
    // ui.cpp's ui_handle_touch(). No-ops safely if FEATURE_VIBRATE or
    // PIN_VIBRATE isn't set up on this board, same as every other
    // vibrate_* call.
    bool haptic_on_touch = false;

    uint8_t brightness = DEFAULT_BRIGHTNESS; // persisted via NVS
    uint8_t watchface_index = 0;             // index into watchface registry
    uint8_t volume = 80;                     // 0-100 speaker volume
    uint8_t mic_sens = 3;                    // 1-5 microphone sensitivity (Settings > Microphone)
    bool wifi_on = false;                    // WiFi radio state (starts OFF)
    bool gps_on = false;                     // GPS module state (starts OFF - see hal_gps.h)
    bool ble_on = false;                     // BLE stack state (starts OFF - previously always started at boot regardless of this, see hal_ble.h)
    bool espnow_on = false;                  // ESP-NOW/joystick-pairing radio state (starts OFF - previously always started at boot regardless of this, see hal_espnow.h)
    bool auto_rotate = false;                // IMU-based display auto-rotation

    // ---- Touch calibration --------------------------------------------
    // screen_x = touch_cal_ax * raw_x + touch_cal_bx  (same for y).
    // Defaults reproduce the previous hardcoded flip
    // (x = (LCD_WIDTH-1)-raw_x) so uncalibrated behaviour is unchanged
    // until app_calibration.cpp's touch step runs and overwrites these.
    bool  touch_calibrated = false;
    float touch_cal_ax = -1.0f, touch_cal_bx = 465.0f;
    float touch_cal_ay = -1.0f, touch_cal_by = 465.0f;

    // ---- Tilt axis calibration ------------------------------------------
    // Which raw IMU axis (0=ax, 1=ay) feeds each logical direction, and
    // its sign, so "tilt right"/"tilt up" mean the same thing to every
    // game and to auto-rotate regardless of how the IMU is mounted on
    // the PCB. Defaults match what the games assumed before this
    // calibration app existed (logical X = ax, logical Y = -ay).
    bool  tilt_calibrated = false;
    int8_t tilt_map_x_from = 0;  // 0 = raw ax, 1 = raw ay
    int8_t tilt_sign_x     = 1;
    int8_t tilt_map_y_from = 1;  // 0 = raw ax, 1 = raw ay
    int8_t tilt_sign_y     = -1;

    // ---- Game audio -----------------------------------------------------
    // Independent toggles, both exposed in Settings and the quick
    // top panel — see game_audio.h for how games check these.
    bool sfx_enabled   = true;
    bool music_enabled = true;

    // ---- USB --------------------------------------------------------------
    // What the USB cable does - a UsbMode from hal_usb.h (0 charging only,
    // 1 firmware & debug, 2 file storage, 3 media remote). Applied at boot
    // by usb_mode_init(); changed from Settings > USB Mode.
    uint8_t usb_mode = 1;

    // ---- Top panel ----------------------------------------------------------
    // Do not disturb: notifications still arrive and go to the history,
    // just without the pop-up toast (app_notifications.cpp).
    bool dnd_on = false;
    bool dnd_bedtime = false;                // DND every night (dnd.cpp)
    uint16_t dnd_from_min = 23 * 60;         // bedtime start, minutes after midnight
    uint16_t dnd_to_min = 7 * 60;            // bedtime end
    bool smooth_fonts = true;                // ui_font.h - smooth fonts with accents

    // ---- Battery mode (battery_mode.h) ---------------------------------------
    uint8_t battery_mode = 1;           // BatteryMode, 1 = Balanced
    // What Saver/Ultra switched off, restored when going back to a normal mode.
    bool    saver_prev_wifi = false;
    bool    saver_prev_gps = false;
    bool    saver_prev_espnow = false;
    bool    saver_prev_autorot = false;
    bool    saver_prev_motion = true;

    // ---- First-boot setup -------------------------------------------------
    // Gates app_onboarding.cpp's welcome/setup flow - false only ever
    // on a genuinely fresh device (or after an NVS wipe), since this
    // is saved true the moment the flow finishes and never reset
    // otherwise. user_name is optional (the flow's name step can be
    // skipped) and, once set, is available for any screen that wants
    // to personalize itself with it - nothing currently reads it
    // beyond the onboarding flow's own confirmation step.
    bool onboarding_complete = false;
    char user_name[24] = "";
};

extern AppSettings g_app_settings;
