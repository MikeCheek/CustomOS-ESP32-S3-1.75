#include "motion_detect.h"
#include <math.h>

const char *activity_name(Activity a) {
    switch (a) {
    case ACT_STILL: return "Still";
    case ACT_WALK: return "Walking";
    case ACT_RUN: return "Running";
    case ACT_MOVING: return "Moving";
    default: return "";
    }
}

static float mag(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }

bool ActivityClassifier::feed(float ax, float ay, float az) {
    m_[n_++] = mag(ax, ay, az);
    if (n_ < WIN) return false;
    classify();
    n_ = 0;
    return true;
}

void ActivityClassifier::classify() {
    double mean = 0;
    for (int i = 0; i < WIN; i++) mean += m_[i];
    mean /= WIN;
    double var = 0;
    for (int i = 0; i < WIN; i++) var += (m_[i] - mean) * (m_[i] - mean);
    var /= WIN;
    float sd = (float)sqrt(var);
    cadence_ = 0;
    if (sd < 0.05f) { cur_ = ACT_STILL; return; }
    // rhythm: the best autocorrelation for a step every 0.25-1.2 s
    int lo = HZ / 4, hi = HZ * 6 / 5;
    static float ac[ActivityClassifier::HZ * 6 / 5 + 2];
    double best = 0;
    for (int lag = lo; lag <= hi; lag++) {
        double c = 0;
        for (int i = 0; i + lag < WIN; i++) c += (m_[i] - mean) * (m_[i + lag] - mean);
        ac[lag] = (float)(c / ((WIN - lag) * var));
        if (ac[lag] > best) best = ac[lag];
    }
    // the shortest period that's nearly as strong: a step, not two
    int best_lag = 0;
    for (int lag = lo + 1; lag < hi; lag++) {
        if (ac[lag] >= 0.85 * best && ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1]) { best_lag = lag; break; }
    }
    if (best > 0.35 && best_lag) {
        cadence_ = (int)lroundf(60.0f * HZ / best_lag);
        bool run = cadence_ >= 140 || sd > 0.55f;
        cur_ = run ? ACT_RUN : ACT_WALK;
        return;
    }
    cur_ = ACT_MOVING;
}

void RepCounter::reset() {
    mean_ = 1;
    var_ = 0.01f;
    high_ = false;
    since_ = 1000;
    count_ = 0;
    warm_ = 0;
}

bool RepCounter::feed(float ax, float ay, float az) {
    float m = mag(ax, ay, az);
    // slow mean / spread (about 3 s), the band is +-0.6 sd around the mean
    mean_ += (m - mean_) * 0.006f;
    var_ += ((m - mean_) * (m - mean_) - var_) * 0.006f;
    since_++;
    if (++warm_ < 2 * 62) return false;   // settle first
    float sd = sqrtf(var_);
    if (sd < 0.06f) return false;          // not moving enough to be an exercise
    if (!high_ && m > mean_ + 0.6f * sd) {
        high_ = true;
        if (since_ >= 37) {                // 0.6 s
            since_ = 0;
            count_++;
            return true;
        }
    } else if (high_ && m < mean_ - 0.3f * sd) {
        high_ = false;
    }
    return false;
}

bool FlickDetector::feed(float ax, float ay, float az) {
    float d = fabsf(mag(ax, ay, az) - 1.0f);
    since_peak_++;
    if (cool_ > 0) { cool_--; return false; }
    if (refractory_ > 0) { refractory_--; return false; }
    if (d > 1.3f) {
        if (peaks_ == 1 && since_peak_ >= 9 && since_peak_ <= 37) {   // 150-600 ms after the first
            peaks_ = 0;
            cool_ = 62;                     // 1 s before another
            return true;
        }
        peaks_ = 1;
        since_peak_ = 0;
        refractory_ = 6;                    // the same flick's samples
    } else if (since_peak_ > 37) {
        peaks_ = 0;
    }
    return false;
}
