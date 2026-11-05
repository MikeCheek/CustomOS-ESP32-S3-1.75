/*
 * sleep_track.h
 * Sleep from wrist movement alone (the board has no heart-rate sensor),
 * after van Hees et al. 2015 / 2018: sleep is a stretch where the arm's
 * angle hardly changes. Each minute is classed from the accelerometer:
 *
 *   arm angle  z = atan(az / sqrt(ax^2 + ay^2)), averaged per 5 s
 *   change     the largest jump between those 5 s angles in the minute
 *   awake      change > 5 degrees (or the minute had no samples)
 *   sleep      change <= 5 degrees, in a run of at least 5 such minutes
 *   deep       sleep where 10 minutes in a row stayed under 1.5 degrees
 *
 * Pure C++ (no Arduino); tools/sleep_test.cpp checks it on the PC.
 */
#pragma once
#include <stdint.h>

enum SleepClass : uint8_t { SLEEP_AWAKE = 0, SLEEP_LIGHT = 1, SLEEP_DEEP = 2, SLEEP_NONE = 3 };

class SleepTracker {
public:
    static const int MAX_MIN = 16 * 60;         // one night, at most 16 h
    // One accelerometer sample at time t (seconds, any rate >= 1 Hz).
    void feed(float ax, float ay, float az, uint32_t t);
    void start(uint32_t t);                       // a new night
    int minutes() const { return n_; }
    // The night so far, re-classed with the run rules (raw: per minute).
    SleepClass at(int minute) const;
    // Moving a little in the last few minutes: a good moment to wake.
    bool light_now() const;
    // Totals in minutes, for the summary.
    void totals(int &asleep, int &deep, int &awake_after_onset) const;
    // Raw per-minute angle change (0.1 degree units, 255 = no data), for saving.
    const uint8_t *raw() const { return change_; }
    void load(const uint8_t *raw, int n);

private:
    uint32_t t0_ = 0;
    int n_ = 0;
    uint8_t change_[MAX_MIN];
    // the minute being built
    int cur_min_ = -1;
    uint32_t epoch_ = 0;
    double ep_sum_ = 0;
    int ep_n_ = 0;
    float last_ep_angle_ = 1000, max_jump_ = 0;
    bool any_ = false;
    void close_minute();
};
