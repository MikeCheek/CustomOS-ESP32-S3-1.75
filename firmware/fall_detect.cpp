#include "fall_detect.h"
#include <math.h>

static const int FREEFALL_MIN = 5;                          // 80 ms
static const int IMPACT_WINDOW = (int)(1.0f * FallDetector::HZ);
static const int SETTLE_LEN = (int)(1.0f * FallDetector::HZ);
static const int STILL_LEN = (int)(2.5f * FallDetector::HZ);

void FallDetector::reset() {
    phase_ = IDLE;
    n_ = 0;
}

static float angle_deg(const float a[3], const float b[3]) {
    float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (na < 1e-3f || nb < 1e-3f) return 0;
    float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (na * nb);
    if (c > 1) c = 1;
    if (c < -1) c = -1;
    return acosf(c) * 57.29578f;
}

bool FallDetector::feed(float ax, float ay, float az) {
    float m = sqrtf(ax * ax + ay * ay + az * az);
    switch (phase_) {
    case IDLE:
        // ~1 s average of the direction while the arm is roughly still
        if (m > 0.8f && m < 1.2f) {
            avg_[0] += (ax - avg_[0]) * 0.03f;
            avg_[1] += (ay - avg_[1]) * 0.03f;
            avg_[2] += (az - avg_[2]) * 0.03f;
        }
        if (m < 0.55f) {
            for (int i = 0; i < 3; i++) pre_[i] = avg_[i];
            phase_ = FREEFALL;
            n_ = 1;
        }
        break;
    case FREEFALL:
        if (m < 0.55f) {
            n_++;
        } else if (n_ >= FREEFALL_MIN) {
            phase_ = AWAIT_IMPACT;
            n_ = 0;
            if (m > 2.4f) { phase_ = SETTLE; n_ = 0; }
        } else {
            reset();
        }
        break;
    case AWAIT_IMPACT:
        if (m > 2.4f) { phase_ = SETTLE; n_ = 0; }
        else if (++n_ > IMPACT_WINDOW) reset();
        break;
    case SETTLE:
        // bouncing and rolling right after the impact
        if (++n_ >= SETTLE_LEN) {
            phase_ = STILL;
            n_ = 0;
            sum_ = sum2_ = 0;
            still_[0] = still_[1] = still_[2] = 0;
        }
        break;
    case STILL:
        n_++;
        sum_ += m;
        sum2_ += (double)m * m;
        still_[0] += ax; still_[1] += ay; still_[2] += az;
        if (m < 0.6f || m > 1.5f) { reset(); break; }   // moving again: got up
        if (n_ >= STILL_LEN) {
            double mean = sum_ / n_;
            double sd = sqrt(fmax(0.0, sum2_ / n_ - mean * mean));
            float dir[3] = {still_[0] / n_, still_[1] / n_, still_[2] / n_};
            bool fell = sd < 0.12 && fabs(mean - 1.0) < 0.2 && angle_deg(pre_, dir) > 35.0f;
            reset();
            return fell;
        }
        break;
    }
    return false;
}
