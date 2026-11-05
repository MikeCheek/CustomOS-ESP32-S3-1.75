// PC test for firmware/sleep_track.cpp:
//   g++ -std=c++17 -Ifirmware tools/sleep_test.cpp firmware/sleep_track.cpp -o /tmp/sleep_test && /tmp/sleep_test
#include "sleep_track.h"
#include <cmath>
#include <cstdio>

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

static SleepTracker s;
static uint32_t t = 0;
static unsigned seed = 7;
static float rnd() { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7fff) / 32768.0f; }

// minutes at ~6 samples/s; angle in degrees, wobble = how much it moves
static void phase(int minutes, float angle, float wobble) {
    for (int i = 0; i < minutes * 60 * 6; i++) {
        float a = (angle + (rnd() - 0.5f) * 2 * wobble) / 57.29578f;
        if (wobble > 10 && rnd() < 0.02f) a += (rnd() - 0.5f) * 2.0f;   // arm moving around
        s.feed(cosf(a), 0, sinf(a), t / 6);
        t++;
    }
}

int main() {
    s.start(0);
    phase(20, 30, 25);     // awake in bed, reading
    phase(60, 10, 0.4f);   // deep
    phase(3, 40, 6);       // turns over
    phase(90, 40, 1.2f);   // lighter sleep
    phase(2, 70, 30);      // wakes briefly
    phase(60, 70, 0.3f);   // deep again
    phase(4, 20, 4);       // stirring before the alarm
    int asleep, deep, waso;
    s.totals(asleep, deep, waso);
    printf("asleep %d min, deep %d, awake after onset %d\n", asleep, deep, waso);
    check(s.at(5) == SLEEP_AWAKE, "reading in bed is awake");
    check(s.at(50) == SLEEP_DEEP, "still for an hour is deep sleep");
    check(asleep >= 200 && asleep <= 225, "total sleep is about 3.5 h");
    check(deep >= 100, "most of the still stretches are deep");
    check(waso >= 2 && waso <= 12, "the short wake is counted");
    check(s.light_now(), "stirring at the end is a moment to wake");
    phase(10, 70, 0.2f);
    check(!s.light_now(), "lying still again is not");
    return fails ? 1 : 0;
}
