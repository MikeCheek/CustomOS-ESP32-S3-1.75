/*
 * motion_detect.h
 * Movement from the accelerometer alone (62.5 Hz, in g), no training data
 * needed - fixed rules on the signal, tested on the PC by tools/motion_test.cpp:
 *
 *  ActivityClassifier  every 5 s: still / walking / running / moving, from
 *                      how much |a| varies and its rhythm (autocorrelation:
 *                      a step every 0.25-1.2 s).
 *  RepCounter          exercise repetitions (squats, push-ups, curls...): one
 *                      per swing of |a| through an adaptive band, at most
 *                      one every 0.6 s.
 *  FlickDetector       two sharp wrist flicks within 0.6 s ("go back").
 */
#pragma once
#include <stdint.h>

enum Activity : uint8_t { ACT_STILL, ACT_WALK, ACT_RUN, ACT_MOVING, ACT_COUNT };
const char *activity_name(Activity a);

class ActivityClassifier {
public:
    static constexpr int HZ = 62;          // ~62.5
    static constexpr int WIN = 5 * HZ;     // 5 s windows
    // True when a window just finished; current() is its class, cadence()
    // the steps per minute it found (0 = no rhythm).
    bool feed(float ax, float ay, float az);
    Activity current() const { return cur_; }
    int cadence() const { return cadence_; }
private:
    float m_[WIN];
    int n_ = 0;
    Activity cur_ = ACT_STILL;
    int cadence_ = 0;
    void classify();
};

class RepCounter {
public:
    void reset();
    // True when a repetition was just counted.
    bool feed(float ax, float ay, float az);
    int count() const { return count_; }
private:
    float mean_ = 1, var_ = 0.01f;
    bool high_ = false;
    int since_ = 1000, count_ = 0, warm_ = 0;
};

class FlickDetector {
public:
    bool feed(float ax, float ay, float az);   // true on a double flick
private:
    int since_peak_ = 1000, peaks_ = 0, refractory_ = 0, cool_ = 0;
};
