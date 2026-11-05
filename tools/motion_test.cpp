// PC test for firmware/motion_detect.cpp:
//   g++ -std=c++17 -Ifirmware tools/motion_test.cpp firmware/motion_detect.cpp -o /tmp/motion_test && /tmp/motion_test
#include "motion_detect.h"
#include <cmath>
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}
static unsigned seed = 11;
static float rnd() { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7fff) / 32768.0f - 0.5f; }
static const float HZ = 62.5f;

// |a| swinging around 1 g at f Hz with amplitude amp, plus noise
template <class F> static void signal(F &&f, float secs, float hz, float amp, float noise = 0.02f) {
    for (int i = 0; i < (int)(secs * HZ); i++) {
        float t = i / HZ;
        float m = 1.0f + amp * sinf(6.2832f * hz * t) + noise * rnd();
        f(0.1f * m, 0.2f * m, 0.97f * m);
    }
}

static Activity classify(float hz, float amp, int *cad = nullptr) {
    ActivityClassifier c;
    Activity last = ACT_STILL;
    signal([&](float x, float y, float z) { if (c.feed(x, y, z)) { last = c.current(); if (cad) *cad = c.cadence(); } }, 10, hz, amp);
    return last;
}

int main() {
    int cad = 0;
    check(classify(0, 0.0f) == ACT_STILL, "lying on a table is still");
    check(classify(1.8f, 0.3f, &cad) == ACT_WALK && cad > 95 && cad < 120, "1.8 steps/s is walking (~108/min)");
    check(classify(2.8f, 0.9f, &cad) == ACT_RUN && cad > 150, "2.8 steps/s, big swings is running");
    {
        ActivityClassifier c;
        Activity last = ACT_STILL;
        for (int i = 0; i < 10 * 62; i++) { float m = 1 + 0.3f * rnd() * 4; if (c.feed(0, 0, m)) last = c.current(); }
        check(last == ACT_MOVING, "moving without a rhythm is just moving");
    }
    {
        RepCounter r;
        r.reset();
        signal([&](float x, float y, float z) { r.feed(x, y, z); }, 2, 0, 0);   // settle
        signal([&](float x, float y, float z) { r.feed(x, y, z); }, 20, 0.5f, 0.5f);
        printf("    reps counted: %d of 10\n", r.count());
        check(r.count() >= 9 && r.count() <= 11, "10 squats (one every 2 s) are about 10 reps");
    }
    {
        FlickDetector f;
        int got = 0;
        auto still = [&](int n) { for (int i = 0; i < n; i++) got += f.feed(0, 0, 1); };
        auto flick = [&]() { for (int i = 0; i < 3; i++) got += f.feed(1.6f, 1.4f, 1.2f); };
        still(60); flick(); still(15); flick(); still(60);
        check(got == 1, "two flicks 0.3 s apart are one double flick");
        got = 0;
        still(60); flick(); still(90); flick(); still(90);
        check(got == 0, "two flicks 1.5 s apart are not");
        got = 0;
        signal([&](float x, float y, float z) { got += f.feed(x, y, z); }, 10, 1.8f, 0.3f);
        check(got == 0, "walking doesn't flick");
    }
    return fails ? 1 : 0;
}
