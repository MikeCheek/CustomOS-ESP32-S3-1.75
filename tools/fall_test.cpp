// PC test for firmware/fall_detect.cpp:
//   g++ -std=c++17 -Ifirmware tools/fall_test.cpp firmware/fall_detect.cpp -o /tmp/fall_test && /tmp/fall_test
#include "fall_detect.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

struct Sim {
    FallDetector d;
    bool fell = false;
    unsigned seed = 1;
    float noise(float a) { seed = seed * 1103515245u + 12345u; return a * (((seed >> 16) & 0x7fff) / 16384.0f - 1.0f); }
    void run(float ax, float ay, float az, float secs, float jitter = 0.02f) {
        for (int i = 0; i < (int)(secs * FallDetector::HZ); i++)
            fell |= d.feed(ax + noise(jitter), ay + noise(jitter), az + noise(jitter));
    }
};

int main() {
    {   // arm hanging (-x), fall, hit the floor, lie on the side (z)
        Sim s;
        s.run(-1, 0, 0, 2);
        s.run(-0.2f, 0.1f, 0.1f, 0.3f);
        s.run(2.0f, 2.2f, 1.5f, 0.05f);
        s.run(0.3f, 0.4f, 1.3f, 1.0f, 0.4f);
        s.run(0.05f, 0.1f, 0.98f, 3.0f);
        check(s.fell, "a fall is detected");
    }
    {   // sitting down hard: an impact, no free fall
        Sim s;
        s.run(-1, 0, 0, 2);
        s.run(1.5f, 2.2f, 1.0f, 0.1f);
        s.run(0, 0, 1, 4);
        check(!s.fell, "a hard sit-down is not a fall");
    }
    {   // fall, but up again straight away
        Sim s;
        s.run(-1, 0, 0, 2);
        s.run(-0.2f, 0.1f, 0.1f, 0.3f);
        s.run(2.0f, 2.2f, 1.5f, 0.05f);
        s.run(0.3f, 0.4f, 1.3f, 1.0f, 0.4f);
        s.run(0.2f, 0.9f, 0.6f, 3.0f, 0.5f);
        check(!s.fell, "getting up again is not reported");
    }
    {   // the watch dropped on a table: same angle before and after? no - flat
        Sim s;
        s.run(0, 0, 1, 2);
        s.run(0.1f, 0, 0.2f, 0.25f);
        s.run(0.5f, 0.5f, 2.8f, 0.05f);
        s.run(0, 0, 1, 1.0f, 0.3f);
        s.run(0, 0, 1, 3);
        check(!s.fell, "a drop flat onto a table (no change of angle) is not a fall");
    }
    {   // walking: |a| 0.7-1.4 g, never free fall
        Sim s;
        for (int k = 0; k < 20; k++) { s.run(-1, 0.3f, 0, 0.25f, 0.3f); s.run(-0.8f, -0.2f, 0.1f, 0.25f, 0.3f); }
        check(!s.fell, "walking is not a fall");
    }
    return fails ? 1 : 0;
}
