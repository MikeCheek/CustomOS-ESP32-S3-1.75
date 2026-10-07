/*
 * hal_imu.h
 * QMI8658 6-axis IMU (3-axis accel + 3-axis gyro). Uses SensorLib, same
 * as Waveshare's own QMI8658 demo for this board.
 */

#pragma once
#include <stdint.h>

struct ImuSample {
    bool valid;
    float ax, ay, az;   // g
    float gx, gy, gz;   // deg/s
    float temperature_c;
};

bool imu_init();
ImuSample imu_read();

// Applies the axis-mapping calibration from AppSettings (see
// app_settings_state.h) to a raw sample, producing logical tilt values
// that mean the same thing ("tilt right" = positive tilt_x, "tilt up" =
// positive tilt_y) regardless of how the IMU is physically mounted on
// the PCB. Every game/feature that steers off tilt should go through
// this instead of reading sample.ax/ay directly - see game_plane.cpp,
// game_dodger.cpp, and ui.cpp's auto-rotate for the pattern. Falls back
// to sane defaults if calibration has never been run (app_calibration.cpp).
void imu_get_calibrated_tilt(const ImuSample &sample, float &tilt_x, float &tilt_y);

// Very simple step counter built on accel magnitude peak detection.
// Good enough for a "does the sensor work" test screen; swap for a
// real pedometer algorithm if you want production-quality step counts.
// Takes a pre-read sample to avoid double I2C transactions.
void imu_step_counter_update(const ImuSample &sample);
uint32_t imu_get_step_count();
void imu_step_counter_reset();
// Reads the QMI8658's hardware pedometer (counts with the screen off too)
// and rolls the count over at midnight. Call from the main loop, awake or
// asleep - it rate-limits itself.
void imu_steps_poll();
