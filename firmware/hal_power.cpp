#include "hal_power.h"
#include "board_pins.h"
#include "config.h"
#include "hal_ble.h"
#include "hal_wifi.h"
#include "hal_espnow.h"
#include "hal_usb.h"
#include "battery_mode.h"
#include <esp_pm.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "app_settings_state.h"

// XPowersLib - install via Library Manager ("XPowersLib" by lewisxhe)
// or from the Waveshare demo package if not found online.
#include <XPowersLib.h>

static XPowersPMU s_pmu;
static bool s_ok = false;

bool power_init() {
    // 1. Initialize the PMU
    s_ok = s_pmu.begin(Wire, PMU_I2C_ADDR, PIN_IIC_SDA, PIN_IIC_SCL);
    if (!s_ok) {
        DEBUG_PRINTF("[power] AXP2101 init FAILED at 0x%02X\n", PMU_I2C_ADDR);
        return false;
    }

    // 2. Set voltages for the rails (Public methods, no longer protected)
    s_pmu.setALDO1Voltage(3300); // AMOLED Screen
    s_pmu.setALDO2Voltage(3300);
    s_pmu.setALDO3Voltage(3300); // Sensors
    s_pmu.setALDO4Voltage(3300); // Touch IC
    s_pmu.setBLDO1Voltage(3300); // Audio Codec
    s_pmu.setBLDO2Voltage(3300);

    // 3. Enable the rails
    s_pmu.enableALDO1();
    s_pmu.enableALDO2();
    s_pmu.enableALDO3();
    s_pmu.enableALDO4();
    s_pmu.enableBLDO1();
    s_pmu.enableBLDO2();

    // 4. Battery & Measurement settings
    s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
    s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    s_pmu.enableBattDetection();
    s_pmu.enableVbusVoltageMeasure();
    s_pmu.enableBattVoltageMeasure();
    s_pmu.enableSystemVoltageMeasure();
    s_pmu.enableTemperatureMeasure();

    // PWR button: this board doesn't expose it as a raw ESP32 GPIO (see
    // board_pins.h) - it's wired into the AXP2101's own PEK (power-key)
    // circuit and read over I2C via IRQ status bits, using the same PMU
    // instance already talking to the chip for battery status.
    s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
    s_pmu.clearIrqStatus();

    // True power off (not sleep - this actually cuts power to the
    // whole board via the PMU, not just idles the ESP32) on a long
    // hold, handled entirely by the AXP2101 chip itself rather than
    // firmware. This is deliberately NOT the same "long press" the
    // software button state machine detects at 600ms (hal_buttons.cpp)
    // for things like BOOT's "go home" - this is a much longer,
    // hardware-timed hold (4s here) that the PMU acts on autonomously,
    // so it still works even if the firmware has hung or crashed.
    // "Power on" needs no firmware code on our side at all: pressing
    // PEK while fully off is the AXP2101's own native wake behavior,
    // since the ESP32 isn't running to do anything about it anyway.
    //
    // Honesty check: setPowerKeyPressOffTime()/enableLongPressShutdown()
    // are inferred from a different language binding of this same
    // AXP2101 IRQ/register model (couldn't confirm XPowersLib's exact
    // method names from its source directly) - if either doesn't
    // compile against the installed XPowersLib version, check
    // XPowersAXP2101.tpp for the actual method names and adjust here;
    // the intent (configure + enable a hardware long-press shutdown)
    // is what matters, not these exact two calls.
    s_pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    s_pmu.enableLongPressShutdown();

    DEBUG_PRINTF("[power] AXP2101 init OK\n");
    return true; 
}

PowerStatus power_read() {
    PowerStatus st{};
    st.valid = s_ok;
    if (!s_ok) return st;

    st.battery_voltage_v = s_pmu.getBattVoltage() / 1000.0f;
    st.battery_percent = s_pmu.getBatteryPercent(); // -1 if no battery / unknown
    st.is_charging = s_pmu.isCharging();
    st.usb_connected = s_pmu.isVbusIn();
    st.vbus_voltage_v = s_pmu.getVbusVoltage() / 1000.0f;
    st.system_voltage_v = s_pmu.getSystemVoltage() / 1000.0f;
    st.temperature_c = s_pmu.getTemperature();
    return st;
}

int power_get_battery_percent() {
    if (!s_ok) return -1;
    int pct = s_pmu.getBatteryPercent();
    return pct;
}

bool power_vbus_present() {
    if (!s_ok) return false;
    return s_pmu.isVbusIn();
}

bool power_is_charging() {
    if (!s_ok) return false;
    return s_pmu.isCharging();
}

// Verified real method - XPowersLibInterface.hpp declares
// "virtual void shutdown() = 0", documented as turning off all power
// channels (only VRTC, the RTC backup rail, stays powered) - a genuine
// immediate software-triggered shutdown, not the same thing as the
// hardware long-press-shutdown configured in power_init() (that one's
// a last-resort failsafe for a hung firmware; this is the normal path,
// called once the user has actually confirmed via the slide-to-power-
// off screen).
void power_shutdown() {
    if (!s_ok) return;
    s_pmu.shutdown();
}

static bool s_pek_short_pending = false;
static bool s_pek_long_pending = false;

void power_poll_pek_button() {
    if (!s_ok) return;
    uint32_t status = s_pmu.getIrqStatus();
    if (s_pmu.isPekeyShortPressIrq()) s_pek_short_pending = true;
    if (s_pmu.isPekeyLongPressIrq()) s_pek_long_pending = true;
    if (status != 0) s_pmu.clearIrqStatus();
}

bool power_pek_short_press_pending() {
    if (!s_pek_short_pending) return false;
    s_pek_short_pending = false;
    return true;
}

bool power_pek_long_press_pending() {
    if (!s_pek_long_pending) return false;
    s_pek_long_pending = false;
    return true;
}

// Automatic light sleep (CONFIG_FREERTOS_USE_TICKLESS_IDLE-backed,
// engaged transparently whenever FreeRTOS has nothing else to run -
// e.g. during delay() calls) has documented compatibility problems
// with active BLE, and shares the same underlying radio-timing
// sensitivity with an active WiFi/ESP-NOW connection. Since all three
// radios now default OFF and only run when the user explicitly turns
// them on (see app_settings_state.h), the common case has none of
// them active - safe to enable light sleep in exactly that window,
// and only that window, reconfiguring live as radios toggle rather
// than picking one fixed policy at boot.
static bool s_light_sleep_currently_enabled = false;
static bool s_pm_config_attempted = false;
static bool s_pm_config_supported = true; // set false the first time esp_pm_configure() itself returns ESP_ERR_NOT_SUPPORTED, so we stop retrying a call the running core build can't honor

// CPU ceiling from the battery mode (battery_mode.h). The Arduino core is
// built without CONFIG_PM_ENABLE, so esp_pm_configure() below reports
// ESP_ERR_NOT_SUPPORTED and DFS/light sleep never engage - the CPU would
// sit at 240 MHz regardless. setCpuFrequencyMhz() works without esp_pm, so
// the cap is applied with it, independently of the policy below.
static bool s_screen_off = false;

void power_set_screen_off(bool off) { s_screen_off = off; }

static void apply_cpu_cap() {
    uint32_t want = battery_mode_cpu_max_mhz();
    // Screen off: nothing to render - the lowest clock that keeps the
    // radios and the 80 MHz APB bus happy is plenty.
    if (s_screen_off) want = 80;
    if (want != 240 && want != 160 && want != 80) want = 240;
    if (getCpuFrequencyMhz() == want) return;
    if (setCpuFrequencyMhz(want)) DEBUG_PRINTF("[power] CPU now %lu MHz\n", (unsigned long)want);
}

void power_update_sleep_policy() {
    apply_cpu_cap();
    if (!s_pm_config_supported) return;
    bool any_radio_active = ble_is_enabled() || wifi_is_enabled() || espnow_is_enabled();
    // A USB data mode with a cable in also rules it out - see
    // usb_mode_blocks_light_sleep() (hal_usb.cpp).
    bool usb_busy = usb_mode_blocks_light_sleep();
    bool want_light_sleep = !any_radio_active && !usb_busy;
    static uint16_t s_pm_max = 0;
    uint16_t want_max = battery_mode_cpu_max_mhz();
    if (s_pm_config_attempted && want_light_sleep == s_light_sleep_currently_enabled && want_max == s_pm_max) return;
    s_pm_max = want_max;

    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = want_max;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = want_light_sleep;
    esp_err_t result = esp_pm_configure(&pm_config);
    s_pm_config_attempted = true;
    if (result == ESP_ERR_NOT_SUPPORTED) {
        s_pm_config_supported = false;
        DEBUG_PRINTF("[power] light sleep not supported by this core build\n");
        return;
    }
    if (result == ESP_OK) {
        s_light_sleep_currently_enabled = want_light_sleep;
        DEBUG_PRINTF("[power] light sleep %s (radios %s%s)\n",
                     want_light_sleep ? "enabled" : "disabled",
                     any_radio_active ? "active" : "all off",
                     usb_busy ? ", USB data cable in" : "");
    } else {
        DEBUG_PRINTF("[power] esp_pm_configure retry failed: %s\n", esp_err_to_name(result));
    }
}
// ---- Manual light sleep -------------------------------------------------------------
// This Arduino core has no CONFIG_PM_ENABLE, so FreeRTOS never light-sleeps
// by itself (and the BT controller isn't built with modem sleep either, so
// light sleep with Bluetooth on would drop the link). With every radio off
// and no cable in, the screen-off poll loop sleeps here instead of busy-
// waiting in delay(): ~1-2 mA instead of ~20 mA between polls. The timer
// keeps the old poll cadence; BOOT and a touch wake it at once.
bool power_light_sleep(uint32_t ms) {
    static uint32_t s_vbus_check_ms = 0;
    static bool s_vbus = true;
    if (millis() - s_vbus_check_ms > 2000 || s_vbus_check_ms == 0) {
        s_vbus_check_ms = millis();
        s_vbus = power_vbus_present();   // USB serial / charging: stay awake
    }
    if (s_vbus || ble_is_enabled() || wifi_is_enabled() || espnow_is_enabled() || usb_mode_blocks_light_sleep())
        return false;

    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_BOOT, GPIO_INTR_LOW_LEVEL);
    bool touch_wake = g_app_settings.wake_on_touch && PIN_TP_INT >= 0;
    if (touch_wake) gpio_wakeup_enable((gpio_num_t)PIN_TP_INT, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    Serial.flush();
    esp_light_sleep_start();
    gpio_wakeup_disable((gpio_num_t)PIN_BTN_BOOT);
    if (touch_wake) gpio_wakeup_disable((gpio_num_t)PIN_TP_INT);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    return true;
}
