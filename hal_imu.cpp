#include "hal_imu.h"
#include "board_pins.h"
#include "config.h"
#include "app_settings_state.h"
#include "hal_rtc.h"

#include <SensorQMI8658.hpp>
#include <Wire.h>

#if FEATURE_NVS
#include "hal_nvs.h"
#endif

static SensorQMI8658 s_imu;
static bool s_ok = false;
static bool s_hw_ped = false;           // hardware pedometer running

// Steps today = s_offset + (hardware counter - s_hw_base). The hardware
// counter restarts at 0 whenever the chip is reset (every boot); s_offset
// carries what was counted before (from NVS), s_hw_base moves at midnight.
static uint32_t s_offset = 0;
static uint32_t s_hw_base = 0;
static uint32_t s_hw_last = 0;
static uint32_t s_day = 0;              // yyyymmdd the count belongs to, 0 = clock not set
static uint32_t s_saved_day = 0;        // day of the count loaded from NVS at boot
static uint32_t s_last_poll_ms = 0;
static uint32_t s_last_nvs_save_ms = 0;
static uint32_t s_saved_steps = 0;
static const uint32_t NVS_SAVE_INTERVAL_MS = 300000;

// Software fallback (only if the pedometer can't be enabled)
static uint32_t s_sw_steps = 0;
static bool s_rising = false;
static uint32_t s_last_step_ms = 0;

static ImuSample s_last;               // last real sample
static uint32_t s_last_ms = 0;

static uint32_t today() {
    WatchTime t = rtc_now();
    if (t.year < 2024) return 0;
    return (uint32_t)t.year * 10000 + t.month * 100 + t.day;
}

bool imu_init() {
    s_ok = s_imu.begin(Wire, IMU_I2C_ADDR, PIN_IIC_SDA, PIN_IIC_SCL);
    if (!s_ok) {
        DEBUG_PRINTF("[imu] QMI8658 init FAILED at 0x%02X\n", IMU_I2C_ADDR);
        return false;
    }

    // 62.5 Hz accelerometer only: enough for tilt controls and wrist
    // wake, what the on-chip pedometer is tuned for, and a fraction of
    // the power of the old 250 Hz accel + 224 Hz gyro (nothing used the
    // gyro).
    s_imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_62_5Hz,
        SensorQMI8658::LPF_MODE_0);
    s_imu.enableAccelerometer();

    // Pedometer windows are in samples at 62.5 Hz: 0.8 s batch, a step
    // within 0.32-3.2 s of the last, counting starts after 6 steps in a
    // row (filters out arm gestures), registers updated every step.
    s_hw_ped = s_imu.configPedometer(50, 200, 100, 200, 20, 6, 0, 1) && s_imu.enablePedometer();
    if (s_hw_ped) s_imu.clearPedometerCounter();

    DEBUG_PRINTF("[imu] QMI8658 init OK, chip id: 0x%02X, pedometer %s\n", s_imu.getChipID(),
                 s_hw_ped ? "on" : "unavailable (software steps)");

#if FEATURE_NVS
    uint32_t saved_day = nvs_load_steps_day();
    s_saved_day = saved_day;
    s_day = today();
    // Same day (or the clock isn't set yet - checked again once it is):
    // continue from the saved count.
    s_offset = (s_day == 0 || saved_day == 0 || saved_day == s_day) ? nvs_load_steps() : 0;
    s_sw_steps = s_offset;
    s_saved_steps = s_offset;
    DEBUG_PRINTF("[imu] %u steps today so far\n", (unsigned)s_offset);
#endif
    return true;
}

ImuSample imu_read() {
    ImuSample s{};
    if (!s_ok) { s.valid = false; return s; }

    if (s_imu.getDataReady()) {
        s_imu.getAccelerometer(s.ax, s.ay, s.az);
        s.temperature_c = s_last.temperature_c;
        s.valid = true;
        s_last = s;
        s_last_ms = millis();
        return s;
    }
    // No new sample yet (62.5 Hz vs a 60+ fps UI): the latest one is
    // still current.
    if (s_last.valid && millis() - s_last_ms < 100) return s_last;
    return s;
}

// ---- steps ----------------------------------------------------------------------------

static void save_steps(uint32_t steps) {
#if FEATURE_NVS
    nvs_save_steps(steps);
    if (s_day) nvs_save_steps_day(s_day);
    s_saved_steps = steps;
#endif
    s_last_nvs_save_ms = millis();
}

void imu_steps_poll() {
    if (!s_ok) return;
    uint32_t now = millis();
    if (now - s_last_poll_ms < 3000) return;
    s_last_poll_ms = now;

    if (s_hw_ped) {
        // Read the 24-bit counter directly so an I2C error (the library
        // returns 0 then) isn't mistaken for a counter reset.
        uint8_t b[3];
        Wire.beginTransmission(IMU_I2C_ADDR);
        Wire.write(0x5A);   // STEP_CNT_LOW, MID, HIGH
        bool ok = Wire.endTransmission(false) == 0 && Wire.requestFrom((int)IMU_I2C_ADDR, 3) == 3;
        if (ok) {
            b[0] = Wire.read(); b[1] = Wire.read(); b[2] = Wire.read();
            uint32_t hw = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16);
            if (hw < s_hw_last) {
                // The chip restarted counting: keep what it had counted.
                s_offset += s_hw_last - s_hw_base;
                s_hw_base = 0;
            }
            s_hw_last = hw;
        }
    }

    // Midnight (or the clock was just set from the phone).
    uint32_t d = today();
    if (d != 0 && d != s_day) {
        // A real day change, or the first time the clock is known and the
        // count loaded at boot was from another day.
        bool reset = s_day != 0 || (s_saved_day != 0 && s_saved_day != d);
        if (reset) {
            s_offset = 0;
            s_hw_base = s_hw_last;
            s_sw_steps = 0;
            DEBUG_PRINTF("[imu] new day - steps reset\n");
        }
        s_day = d;
        s_saved_day = d;
        save_steps(imu_get_step_count());
    }

    uint32_t steps = imu_get_step_count();
    if (steps != s_saved_steps && now - s_last_nvs_save_ms >= NVS_SAVE_INTERVAL_MS) save_steps(steps);
}

// Software step detection - only used when the hardware pedometer isn't.
void imu_step_counter_update(const ImuSample &sample) {
    if (s_hw_ped || !sample.valid) return;

    float mag = sqrtf(sample.ax * sample.ax + sample.ay * sample.ay + sample.az * sample.az);
    const float THRESH_HIGH = 1.15f;
    const float THRESH_LOW = 0.95f;
    const uint32_t MIN_STEP_INTERVAL_MS = 300;

    uint32_t now = millis();
    if (!s_rising && mag > THRESH_HIGH) {
        s_rising = true;
    } else if (s_rising && mag < THRESH_LOW) {
        s_rising = false;
        if (now - s_last_step_ms > MIN_STEP_INTERVAL_MS) {
            s_sw_steps++;
            s_last_step_ms = now;
        }
    }
}

uint32_t imu_get_step_count() {
    if (s_hw_ped) return s_offset + (s_hw_last - s_hw_base);
    return s_sw_steps;
}

void imu_step_counter_reset() {
    s_offset = 0;
    s_hw_base = s_hw_last;
    s_sw_steps = 0;
    save_steps(0);
}

void imu_get_calibrated_tilt(const ImuSample &sample, float &tilt_x, float &tilt_y) {
    float raw_axes[2] = { sample.ax, sample.ay };
    int8_t mx = g_app_settings.tilt_map_x_from;
    int8_t my = g_app_settings.tilt_map_y_from;
    if (mx < 0 || mx > 1) mx = 0;
    if (my < 0 || my > 1) my = 1;
    tilt_x = (float)g_app_settings.tilt_sign_x * raw_axes[mx];
    tilt_y = (float)g_app_settings.tilt_sign_y * raw_axes[my];
}
