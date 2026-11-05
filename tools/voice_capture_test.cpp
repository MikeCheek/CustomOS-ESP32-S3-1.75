// PC test for firmware/voice_capture.h:
//   g++ -std=c++17 -I firmware tools/voice_capture_test.cpp -o /tmp/vc && /tmp/vc
// Quiet room, then speech: heard, and still talking until 1.3 s of quiet.
// Only the room: nothing heard. Speech that runs past the buffer: kept up
// to the buffer. normalize() brings a quiet question up, a loud one stays.
#include "voice_capture.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok: %s\n", msg); } while (0)

static const int CH = 512;   // the front end's fetch size

// ms of "room" (tiny noise) or "speech" (a 220 Hz tone at amp) into vc
static void feed(VoiceCapture &vc, int ms, int amp, unsigned &seed) {
    int n = VoiceCapture::RATE * ms / 1000;
    std::vector<int16_t> s(CH);
    static long t = 0;
    for (int done = 0; done < n; done += CH) {
        for (int i = 0; i < CH; i++, t++) {
            seed = seed * 1103515245 + 12345;
            int noise = (int)((seed >> 16) % 61) - 30;   // about +-30
            s[i] = (int16_t)(noise + (amp ? amp * sin(2 * M_PI * 220 * t / VoiceCapture::RATE) : 0));
        }
        vc.add(s.data(), CH);
    }
}

int main() {
    unsigned seed = 7;
    std::vector<int16_t> buf(VoiceCapture::RATE * 10);

    VoiceCapture a;
    a.begin(buf.data(), buf.size());
    feed(a, 500, 0, seed);
    CHECK(!a.spoke(), "the room alone isn't speech");
    feed(a, 1500, 3000, seed);
    CHECK(a.spoke() && a.still_talking(), "speech is heard, still talking");
    feed(a, 800, 0, seed);
    CHECK(a.still_talking(), "a 0.8 s pause: still talking");
    feed(a, 700, 0, seed);
    CHECK(!a.still_talking(), "1.5 s quiet: the question is over");

    VoiceCapture b;
    b.begin(buf.data(), buf.size());
    feed(b, 4000, 0, seed);
    CHECK(!b.spoke(), "4 s of room: nothing heard");

    VoiceCapture c;
    c.begin(buf.data(), VoiceCapture::RATE * 2);
    feed(c, 300, 0, seed);
    feed(c, 3000, 3000, seed);
    CHECK(c.full() && c.samples() == VoiceCapture::RATE * 2, "longer than the buffer: kept up to it");

    VoiceCapture d;
    d.begin(buf.data(), buf.size());
    feed(d, 300, 0, seed);
    feed(d, 1000, 3000, seed);   // ~9% of full scale: about 7.5x
    d.normalize();
    int peak = 0;
    for (int i = 0; i < d.samples(); i++) peak = std::max(peak, std::abs((int)d.data()[i]));
    CHECK(peak > 20000 && peak <= 32767, "a quiet question is brought up to ~70%");

    VoiceCapture e;
    e.begin(buf.data(), buf.size());
    feed(e, 300, 0, seed);
    feed(e, 1000, 30000, seed);
    int before = 0;
    for (int i = 0; i < e.samples(); i++) before = std::max(before, std::abs((int)e.data()[i]));
    e.normalize();
    int after = 0;
    for (int i = 0; i < e.samples(); i++) after = std::max(after, std::abs((int)e.data()[i]));
    CHECK(after == before, "a loud one is left alone");

    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
