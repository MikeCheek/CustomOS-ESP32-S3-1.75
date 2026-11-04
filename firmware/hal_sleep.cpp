#include "hal_sleep.h"
#include "aod.h"
#include "hal_display.h"
#include "hal_touch.h"
#include "hal_imu.h"
#include "hal_power.h"
#include "app_settings_state.h"
#include "config.h"
#include "ui.h"
#include <Arduino.h>
#include <math.h>

#if FEATURE_NVS
#include "hal_nvs.h"
#endif

static bool s_asleep = false;
static uint32_t s_last_activity_ms = 0;

// Motion-wake: compares accelerometer magnitude against a resting ~1g
// baseline. This is a deliberately simple heuristic (not a real
// wrist-raise gesture classifier) - a big-enough deviation from 1g,
// from being picked up, shaken, or rotated, counts as motion. Rate-
// limited while asleep since sleep mode's whole point is saving power,
// and polling the IMU over I2C every single loop iteration would work
// against that.
static uint32_t s_last_motion_check_ms = 0;
static const uint32_t MOTION_CHECK_PERIOD_MS = 200;
static const float MOTION_THRESHOLD_G = 0.35f;

// Raise to wake: a movement, then within a second the screen facing up
// (gravity mostly through the glass, |z| > 0.45 g) - so a swinging arm
// hanging down (walking) no longer wakes it.
static uint32_t s_moved_ms = 0;
static bool check_motion_now() {
    ImuSample s = imu_read();
    if (!s.valid) return false;
    uint32_t now = millis();
    float mag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
    if (fabsf(mag - 1.0f) > MOTION_THRESHOLD_G) s_moved_ms = now ? now : 1;
    bool facing = fabsf(s.az) > 0.45f && fabsf(mag - 1.0f) < 0.4f;
    return s_moved_ms && now - s_moved_ms < 1000 && facing;
}

// Lower the wrist to sleep: with no touch for 3 s, the screen turned to
// the side (|z| < 0.35 g: the arm hanging, or turned away) for 1.2 s puts
// the watch to sleep - most screen-on time is the timeout running out.
static uint32_t s_down_since = 0, s_last_down_check_ms = 0;
static bool wrist_down(uint32_t now) {
    if (now - s_last_down_check_ms < 200) return false;
    s_last_down_check_ms = now;
    ImuSample s = imu_read();
    if (!s.valid) { s_down_since = 0; return false; }
    float mag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
    bool down = fabsf(s.az) < 0.35f && fabsf(mag - 1.0f) < 0.35f;
    if (!down) { s_down_since = 0; return false; }
    if (!s_down_since) s_down_since = now ? now : 1;
    return now - s_down_since >= 1200;
}

void sleep_init() {
    s_asleep = false;
    s_last_activity_ms = millis();
}

void sleep_register_activity() {
    s_last_activity_ms = millis();
    if (s_asleep) {
        s_asleep = false;
        aod_wake();
        display_wakeup();
    }
}

bool sleep_is_asleep() {
    return s_asleep;
}

uint32_t sleep_ms_since_activity() {
    return millis() - s_last_activity_ms;
}

void sleep_force_sleep() {
    if (s_asleep) return;
#if FEATURE_NVS
    nvs_save_steps(imu_get_step_count());
#endif
    s_asleep = true;
    aod_sleep_display();
}

bool sleep_update() {
    uint32_t now = millis();

    if (!s_asleep) {
        // 0 = never auto-sleep. Games with suppress_idle set never
        // auto-sleep either, regardless of this setting - a long
        // touch-free stretch mid-game (watching a sequence, an idle
        // moment) isn't the same thing as the watch being put down.
        // Same reasoning extends to stay_awake_charging: a watch
        // sitting on its charger isn't "put down" in the sense this
        // timeout exists to catch.
        bool charging_exempt = g_app_settings.stay_awake_charging && power_is_charging();
        if (g_app_settings.wrist_down_sleep && !ui_current_screen_suppresses_idle() && !charging_exempt &&
            now - s_last_activity_ms >= 3000 && wrist_down(now)) {
#if FEATURE_NVS
            nvs_save_steps(imu_get_step_count());
#endif
            s_down_since = 0;
            s_moved_ms = 0;
            s_asleep = true;
            aod_sleep_display();
            return s_asleep;
        }
        if (now - s_last_activity_ms < 3000) s_down_since = 0;
        if (g_app_settings.sleep_timeout_s > 0 && !ui_current_screen_suppresses_idle() && !charging_exempt) {
            uint32_t timeout_ms = (uint32_t)g_app_settings.sleep_timeout_s * 1000UL;
            if (now - s_last_activity_ms >= timeout_ms) {
#if FEATURE_NVS
                nvs_save_steps(imu_get_step_count());
#endif
                s_asleep = true;
                aod_sleep_display();
            }
        }
        return s_asleep;
    }

    // Asleep: LVGL's normal touch polling is paused while asleep (see
    // header note), so check directly here instead, rate-limited the
    // same as the touch controller's own natural poll rate would be -
    // no real power cost either way since we're already reading I2C
    // this often for the touch controller when awake.
    if (g_app_settings.wake_on_touch) {
        TouchPoint tp = touch_read();
        if (tp.touched) {
            sleep_register_activity();
            return s_asleep;
        }
    }

    if (g_app_settings.wake_on_motion && (now - s_last_motion_check_ms) >= MOTION_CHECK_PERIOD_MS) {
        s_last_motion_check_ms = now;
        if (check_motion_now()) {
            s_moved_ms = 0;
            sleep_register_activity();
            return s_asleep;
        }
    }

    return s_asleep;
}
