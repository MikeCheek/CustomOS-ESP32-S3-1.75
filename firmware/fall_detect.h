/*
 * fall_detect.h
 * Fall detection from the accelerometer alone (62.5 Hz, in g) - the
 * classic three-phase test used by wrist and pendant fall detectors:
 *
 *   1. free fall   |a| < 0.55 g for at least 80 ms
 *   2. impact      |a| > 2.4 g within 1 s after it
 *   3. lying still  from 1 s after the impact, 2.5 s with |a| close to 1 g
 *                   (std < 0.12 g) and the wrist's angle changed by > 35°
 *                   compared with before the fall
 *
 * A fall is reported once phase 3 holds; anything else resets. Pure C++
 * (no Arduino), tested on the PC by tools/fall_test.cpp.
 */
#pragma once
#include <stdint.h>

class FallDetector {
public:
    static constexpr float HZ = 62.5f;
    // One accelerometer sample; true when a fall has just been recognised.
    bool feed(float ax, float ay, float az);
    void reset();

private:
    enum Phase : uint8_t { IDLE, FREEFALL, AWAIT_IMPACT, SETTLE, STILL };
    Phase phase_ = IDLE;
    int n_ = 0;                 // samples in the current phase
    float pre_[3] = {0, 0, 1};  // the wrist's resting direction before the fall
    float avg_[3] = {0, 0, 1};  // slow average of the direction
    double sum_ = 0, sum2_ = 0; // |a| statistics while lying still
    float still_[3] = {0, 0, 0};
};
