/*
 * battery_mode.h
 * The four power profiles, shared by Settings > Battery Mode and the top
 * panel tile, and read by the power policy (hal_power.cpp).
 *
 *                 bright.  screen off  Wi-Fi/GPS  joystick  auto-rot  raise-wake  CPU max
 *   Performance   100%     5 min       restored   restored  restored  restored    240 MHz
 *   Balanced      78%      2 min       restored   restored  restored  restored    240 MHz
 *   Saver         39%      1 min       off        on        off       on          240 MHz
 *   Ultra saver   16%      30 s        off        off       off       off         160 MHz
 *
 * "restored": going from Saver/Ultra back to Performance/Balanced turns
 * back on whatever the saving mode switched off (a snapshot is taken when
 * a saving mode is entered from a normal one). Bluetooth is never touched -
 * it's the phone link, and NimBLE is cheap when idle.
 *
 * The CPU still scales down to 80 MHz when idle in every mode (esp_pm
 * DFS); the cap only limits how fast it may go under load. Brightness and
 * the others can still be changed by hand afterwards - the mode then shows
 * as "adjusted" until a mode is picked again.
 */
#pragma once
#include <stdint.h>

enum BatteryMode : uint8_t {
    BATT_PERFORMANCE = 0,
    BATT_BALANCED    = 1,
    BATT_SAVER       = 2,
    BATT_ULTRA       = 3,
    BATT_MODE_COUNT
};

struct BatteryProfile {
    const char *name;
    const char *line1;       // short summary lines for the UI
    const char *line2;
    uint16_t    color;
    uint8_t     brightness;
    uint16_t    sleep_s;     // screen timeout
    bool        radios_off;  // Wi-Fi + GPS
    bool        espnow_off;
    bool        autorot_off;
    bool        motion_wake_off;
    uint16_t    cpu_max_mhz;
};

const BatteryProfile &battery_profile(BatteryMode m);
BatteryMode battery_mode_current();
void        battery_mode_apply(BatteryMode m);   // applies + saves
BatteryMode battery_mode_next(BatteryMode m);    // panel tile cycling order
bool        battery_mode_adjusted();             // settings drifted from the profile
uint16_t    battery_mode_cpu_max_mhz();          // for power_update_sleep_policy()
uint32_t    battery_mode_sleep_timeout_ms();     // 0 = never (hal_sleep.cpp)

// Mode glyph (~34 px tall), drawn with fill primitives only:
// Performance = bolt, Balanced = half charge, Saver = leaf,
// Ultra = leaf inside a ring. `bg` is used for cut-outs.
struct Arduino_GFX;
void battery_mode_draw_icon(Arduino_GFX *g, BatteryMode m, int cx, int cy, uint16_t color, uint16_t bg);
