// PC test for firmware/stroke_recog.cpp:
//   g++ -std=c++17 -Ifirmware tools/stroke_test.cpp firmware/stroke_recog.cpp -o /tmp/stroke_test && /tmp/stroke_test
#include "stroke_recog.h"
#include <cmath>
#include <cstdio>

static unsigned seed = 3;
static float rnd() { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7fff) / 32768.0f - 0.5f; }

// a template drawn like a finger would: scaled, moved, tilted, shaky, with
// uneven speed (many points where it slows down)
static int draw(char c, StrokePt *out, float scale, float tilt_deg, float shake) {
    StrokePt t[16];
    int n = stroke_template(c, t, 16), k = 0;
    float a = tilt_deg / 57.3f;
    for (int i = 0; i + 1 < n; i++) {
        int steps = 6 + (i % 3) * 5;
        for (int s = 0; s < steps; s++) {
            float u = (float)s / steps;
            float x = t[i].x + u * (t[i + 1].x - t[i].x), y = t[i].y + u * (t[i + 1].y - t[i].y);
            x = x * scale / 100; y = y * scale / 100;
            out[k++] = {100 + x * cosf(a) - y * sinf(a) + rnd() * shake, 80 + x * sinf(a) + y * cosf(a) + rnd() * shake};
        }
    }
    out[k++] = {100 + t[n - 1].x * scale / 100, 80 + t[n - 1].y * scale / 100};
    return k;
}

int main() {
    int fails = 0, total = 0;
    for (const char *p = stroke_letters(); *p; p++) {
        for (int v = 0; v < 6; v++) {
            StrokePt s[256];
            int n = draw(*p, s, 120 + v * 40, (v - 3) * 4.0f, 6 + v * 2);
            float sc;
            char got = stroke_recognize(s, n, &sc);
            total++;
            if (got != *p) { fails++; printf("FAIL %c drawn (variant %d) -> %c (%.2f)\n", *p, v, got ? got : '-', sc); }
        }
    }
    // a scribble is nothing
    StrokePt sc[40];
    for (int i = 0; i < 40; i++) sc[i] = {100 + 60 * rnd() * (i % 7), 100 + 60 * rnd() * (i % 5)};
    char none = stroke_recognize(sc, 40);
    printf("scribble -> %c\n", none ? none : '-');
    printf("%d/%d letters recognised\n", total - fails, total);
    return fails || none ? 1 : 0;
}
