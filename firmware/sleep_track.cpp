#include "sleep_track.h"
#include <math.h>
#include <string.h>

void SleepTracker::start(uint32_t t) {
    t0_ = t;
    n_ = 0;
    cur_min_ = -1;
    ep_n_ = 0;
    last_ep_angle_ = 1000;
    max_jump_ = 0;
    any_ = false;
}

void SleepTracker::close_minute() {
    if (n_ >= MAX_MIN) return;
    float c = any_ ? max_jump_ : 25.5f;
    if (!any_ && n_ > 0) c = 25.5f;
    int v = (int)lroundf(c * 10);
    change_[n_++] = (uint8_t)(any_ ? (v > 254 ? 254 : v) : 255);
    max_jump_ = 0;
    any_ = false;
}

void SleepTracker::feed(float ax, float ay, float az, uint32_t t) {
    if (t < t0_) return;
    int minute = (int)((t - t0_) / 60);
    if (cur_min_ < 0) cur_min_ = minute;
    while (cur_min_ < minute && n_ < MAX_MIN) {   // finish this minute, and any without samples
        close_minute();
        cur_min_++;
    }
    uint32_t epoch = (t - t0_) / 5;
    if (epoch != epoch_ && ep_n_ > 0) {
        float ang = (float)(ep_sum_ / ep_n_);
        if (last_ep_angle_ < 999) {
            float j = fabsf(ang - last_ep_angle_);
            if (j > max_jump_) max_jump_ = j;
            any_ = true;
        }
        last_ep_angle_ = ang;
        ep_sum_ = 0;
        ep_n_ = 0;
    }
    epoch_ = epoch;
    float h = sqrtf(ax * ax + ay * ay);
    ep_sum_ += atan2f(az, h) * 57.29578f;
    ep_n_++;
}

void SleepTracker::load(const uint8_t *raw, int n) {
    n_ = n > MAX_MIN ? MAX_MIN : (n < 0 ? 0 : n);
    memcpy(change_, raw, n_);
}

static bool still(uint8_t c) { return c != 255 && c <= 50; }
static bool very_still(uint8_t c) { return c != 255 && c <= 15; }

SleepClass SleepTracker::at(int m) const {
    if (m < 0 || m >= n_) return SLEEP_NONE;
    if (!still(change_[m])) return SLEEP_AWAKE;
    // in a run of >= 5 still minutes?
    int a = m, b = m;
    while (a > 0 && still(change_[a - 1])) a--;
    while (b + 1 < n_ && still(change_[b + 1])) b++;
    if (b - a + 1 < 5) return SLEEP_AWAKE;
    // deep: inside 10 very still minutes in a row
    a = m;
    b = m;
    if (!very_still(change_[m])) return SLEEP_LIGHT;
    while (a > 0 && very_still(change_[a - 1])) a--;
    while (b + 1 < n_ && very_still(change_[b + 1])) b++;
    return b - a + 1 >= 10 ? SLEEP_DEEP : SLEEP_LIGHT;
}

bool SleepTracker::light_now() const {
    if (n_ < 3) return false;
    for (int m = n_ - 3; m < n_; m++) {
        uint8_t c = change_[m];
        if (c != 255 && c > 20) return true;   // turned over, stirred
    }
    return false;
}

void SleepTracker::totals(int &asleep, int &deep, int &waso) const {
    asleep = deep = waso = 0;
    int first = -1, last = -1;
    for (int m = 0; m < n_; m++)
        if (at(m) != SLEEP_AWAKE) { if (first < 0) first = m; last = m; }
    if (first < 0) return;
    for (int m = first; m <= last; m++) {
        SleepClass c = at(m);
        if (c == SLEEP_AWAKE) waso++;
        else { asleep++; if (c == SLEEP_DEEP) deep++; }
    }
}
