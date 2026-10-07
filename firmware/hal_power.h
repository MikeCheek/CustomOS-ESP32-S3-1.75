/*
 * hal_power.h
 * AXP2101 PMU: battery %, voltage, charge status. Uses XPowersLib
 * (the same library Waveshare's own AXP2101 demo for this board uses).
 */

#pragma once
#include <stdint.h>

struct PowerStatus {
    bool valid;
    float battery_voltage_v;
    int battery_percent;   // 0-100, -1 if unknown
    bool is_charging;
    bool usb_connected;
    float vbus_voltage_v;
    float system_voltage_v;
    float temperature_c;
};

bool power_init();
PowerStatus power_read();

// Convenience for the watch face / status bar.
int power_get_battery_percent();
bool power_is_charging();
// USB cable present (AXP2101 VBUS-good) - a fresh I2C read, poll sparingly.
bool power_vbus_present();
void power_shutdown(); // immediate, real - see hal_power.cpp for the verification note

// PWR button state, via the AXP2101's own PEK (power-key) IRQ status -
// this board doesn't wire PWR to a raw ESP32 GPIO (see board_pins.h).
// Call power_poll_pek_button() once per loop() iteration to check for
// new presses; the two "was pressed" getters report (and consume) a
// press detected since the last poll, similar to reading-and-clearing a
// flag.
void power_poll_pek_button();
bool power_pek_short_press_pending();
bool power_pek_long_press_pending();

// Dynamically enables/disables automatic FreeRTOS-tickless-idle light
// sleep depending on whether any radio (BLE, WiFi, ESP-NOW) is
// currently active - see hal_power.cpp for why this can't just be
// left on unconditionally. Cheap to call every loop() iteration; it
// only actually reconfigures the power-management policy when the
// desired state has changed since the last call.
void power_update_sleep_policy();

// Screen off: CPU drops to 80 MHz (applied by power_update_sleep_policy()).
void power_set_screen_off(bool off);
// Light-sleeps for up to `ms` if nothing needs the chip awake (no radio,
// no USB). Returns false (did nothing) otherwise - caller then delay()s.
bool power_light_sleep(uint32_t ms);