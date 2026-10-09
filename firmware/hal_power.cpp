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

// ---- Sleep policy ---------------------------------------------------------------------
// Screen off (set every loop from the .ino) and "something needs the chip"
// decide the CPU clock and whether the chip may light-sleep.
static bool s_screen_off = false;

void power_set_screen_off(bool off) { s_screen_off = off; }

static bool s_keep_awake = false;
void power_set_keep_awake(bool keep) { s_keep_awake = keep; }

// USB cable in (serial log, charging): checked every 2 s, it's an I2C read.
// Also notes when the cable came out, for the "since unplugged" drain on
// the Battery screen.
static uint32_t s_unplug_ms = 0;
static int s_unplug_pct = -1;

static bool vbus_cached() {
    static uint32_t s_check_ms = 0;
    static bool s_vbus = true, s_first = true;
    if (s_check_ms == 0 || millis() - s_check_ms > 2000) {
        s_check_ms = millis();
        bool was = s_vbus;
        s_vbus = power_vbus_present();
        if (!s_vbus && (was || s_first)) {
            s_unplug_ms = millis();
            s_unplug_pct = power_get_battery_percent();
        }
        if (s_vbus) s_unplug_pct = -1;
        s_first = false;
    }
    return s_vbus;
}

bool power_since_unplugged(int *start_pct, uint32_t *elapsed_ms) {
    if (s_unplug_pct < 0) return false;
    if (start_pct) *start_pct = s_unplug_pct;
    if (elapsed_ms) *elapsed_ms = millis() - s_unplug_ms;
    return true;
}

static uint16_t cpu_cap_mhz() {
    uint16_t want = battery_mode_cpu_max_mhz();
    return (want == 240 || want == 160 || want == 80) ? want : 240;
}

#if CONFIG_PM_ENABLE
// ---- PlatformIO build: ESP-IDF power management ------------------------------------
// platformio.ini builds the core with CONFIG_PM_ENABLE, tickless idle and
// Bluetooth modem sleep. The policy is configured once (light sleep on,
// 40 MHz floor); two PM locks then say what's allowed right now:
//   - "ui" (CPU at the battery mode's maximum) while the screen is on
//   - "awake" (no light sleep) while the screen is on, or something needs
//     the chip: audio, GPS, a transfer, an update, a USB cable, ESP-NOW
// With the screen off and neither held, every wait in loop() (the 150 ms
// poll) is spent in light sleep with the phone still connected: the BT
// controller wakes for its connection events by itself. The BOOT button
// and the touch controller's interrupt wake the chip at once.
static esp_pm_lock_handle_t s_lock_awake = nullptr, s_lock_ui = nullptr;
static bool s_awake_held = false, s_ui_held = false;
static bool s_pm_ok = false;
static uint16_t s_pm_max = 0;

static void hold(esp_pm_lock_handle_t lock, bool &held, bool want) {
    if (!lock || held == want) return;
    if (want) esp_pm_lock_acquire(lock);
    else esp_pm_lock_release(lock);
    held = want;
}

bool power_auto_sleep_available() { return true; }

void power_update_sleep_policy() {
    bool vbus = vbus_cached();
    uint16_t want_max = cpu_cap_mhz();
    if (!s_lock_awake) {
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "awake", &s_lock_awake);
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui", &s_lock_ui);
        hold(s_lock_awake, s_awake_held, true);   // until the first decision below
        hold(s_lock_ui, s_ui_held, true);
        gpio_wakeup_enable((gpio_num_t)PIN_BTN_BOOT, GPIO_INTR_LOW_LEVEL);
        if (PIN_TP_INT >= 0) gpio_wakeup_enable((gpio_num_t)PIN_TP_INT, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    }
    if (!s_pm_ok || want_max != s_pm_max) {
        esp_pm_config_t cfg = {};
        cfg.max_freq_mhz = want_max;
        cfg.min_freq_mhz = 40;
        cfg.light_sleep_enable = true;
        esp_err_t r = esp_pm_configure(&cfg);
        s_pm_max = want_max;
        s_pm_ok = r == ESP_OK;
        DEBUG_PRINTF("[power] PM %lu-%u MHz, auto light sleep: %s\n", 40UL, want_max,
                     s_pm_ok ? "on" : esp_err_to_name(r));
    }
    bool may_sleep = s_pm_ok && s_screen_off && g_app_settings.power_saving && !s_keep_awake &&
                     !espnow_is_enabled() && !usb_mode_blocks_light_sleep() && !vbus;
    if (may_sleep != !s_awake_held) DEBUG_PRINTF("[power] light sleep %s\n", may_sleep ? "allowed" : "blocked");
    hold(s_lock_awake, s_awake_held, !may_sleep);
    hold(s_lock_ui, s_ui_held, !s_screen_off);
}

#else
// ---- Arduino IDE build: no power management in the prebuilt core -------------------
// esp_pm_configure() reports ESP_ERR_NOT_SUPPORTED and DFS / auto light
// sleep never engage, so the CPU cap is applied with setCpuFrequencyMhz()
// and the screen-off loop light-sleeps by hand (power_light_sleep()) when
// every radio is off.
bool power_auto_sleep_available() { return false; }

void power_update_sleep_policy() {
    vbus_cached();   // tracks the unplug moment
    uint32_t want = cpu_cap_mhz();
    // Screen off: nothing to render - the lowest clock that keeps the
    // radios and the 80 MHz APB bus happy is plenty.
    if (s_screen_off) want = 80;
    if (getCpuFrequencyMhz() == want) return;
    if (setCpuFrequencyMhz(want)) DEBUG_PRINTF("[power] CPU now %lu MHz\n", (unsigned long)want);
}
#endif

// ---- Manual light sleep -------------------------------------------------------------
// This Arduino core has no CONFIG_PM_ENABLE, so FreeRTOS never light-sleeps
// by itself (and the BT controller isn't built with modem sleep either, so
// light sleep with Bluetooth on would drop the link). With every radio off
// and no cable in, the screen-off poll loop sleeps here instead of busy-
// waiting in delay(): ~1-2 mA instead of ~20 mA between polls. The timer
// keeps the old poll cadence; BOOT and a touch wake it at once.
bool power_light_sleep(uint32_t ms) {
#if CONFIG_PM_ENABLE
    (void)ms;
    return false;   // the caller's delay() light-sleeps by itself (policy above)
#endif
    if (vbus_cached() || ble_is_enabled() || wifi_is_enabled() || espnow_is_enabled() || usb_mode_blocks_light_sleep())
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
