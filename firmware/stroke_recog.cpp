#include "stroke_recog.h"
#include <math.h>
#include <string.h>

static const int N = 64;
static const float SIZE = 250.0f;

struct Tmpl { char c; int n; StrokePt p[12]; };
// Drawn the usual way, in a 0..100 box, y down.
static const Tmpl TMPL[] = {
    {'A', 3, {{0, 100}, {50, 0}, {100, 100}}},                                   // ^ (no crossbar)
    {'V', 3, {{0, 0}, {50, 100}, {100, 0}}},
    {'C', 9, {{90, 15}, {65, 2}, {35, 2}, {10, 20}, {2, 50}, {10, 80}, {35, 98}, {65, 98}, {90, 85}}},
    {'M', 5, {{0, 100}, {0, 0}, {50, 60}, {100, 0}, {100, 100}}},
    {'N', 4, {{0, 100}, {0, 0}, {100, 100}, {100, 0}}},
    {'Z', 4, {{0, 0}, {100, 0}, {0, 100}, {100, 100}}},
    {'P', 7, {{0, 100}, {0, 0}, {60, 0}, {88, 12}, {88, 38}, {60, 50}, {0, 50}}},
    {'S', 11, {{90, 12}, {65, 0}, {30, 0}, {8, 15}, {12, 38}, {50, 50}, {88, 62}, {92, 85}, {70, 100}, {35, 100}, {8, 88}}},
};
static const int NT = sizeof(TMPL) / sizeof(TMPL[0]);

const char *stroke_letters() { return "ACMNPSVZ"; }

int stroke_template(char c, StrokePt *out, int max) {
    for (int t = 0; t < NT; t++) {
        if (TMPL[t].c != c) continue;
        int n = TMPL[t].n < max ? TMPL[t].n : max;
        memcpy(out, TMPL[t].p, n * sizeof(StrokePt));
        return n;
    }
    return 0;
}

static float plen(const StrokePt *p, int n) {
    float d = 0;
    for (int i = 1; i < n; i++) d += hypotf(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
    return d;
}

// resample to N evenly spaced points
static bool resample(const StrokePt *in, int n, StrokePt *out) {
    float L = plen(in, n);
    if (n < 2 || L < 1e-3f) return false;
    float I = L / (N - 1), D = 0;
    StrokePt prev = in[0];
    int k = 0;
    out[k++] = prev;
    for (int i = 1; i < n && k < N; i++) {
        StrokePt cur = in[i];
        float d = hypotf(cur.x - prev.x, cur.y - prev.y);
        while (D + d >= I && k < N) {
            float t = (I - D) / d;
            StrokePt q = {prev.x + t * (cur.x - prev.x), prev.y + t * (cur.y - prev.y)};
            out[k++] = q;
            prev = q;
            d = hypotf(cur.x - prev.x, cur.y - prev.y);
            D = 0;
        }
        D += d;
        prev = cur;
    }
    while (k < N) out[k++] = in[n - 1];
    return true;
}

static void normalize(StrokePt *p) {
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < N; i++) {
        if (p[i].x < minx) minx = p[i].x;
        if (p[i].x > maxx) maxx = p[i].x;
        if (p[i].y < miny) miny = p[i].y;
        if (p[i].y > maxy) maxy = p[i].y;
    }
    // uniform scale keeps thin letters thin; at least 1 so lines don't blow up
    float s = fmaxf(fmaxf(maxx - minx, maxy - miny), 1.0f);
    float cx = 0, cy = 0;
    for (int i = 0; i < N; i++) {
        p[i].x = (p[i].x - minx) * SIZE / s;
        p[i].y = (p[i].y - miny) * SIZE / s;
        cx += p[i].x;
        cy += p[i].y;
    }
    cx /= N;
    cy /= N;
    for (int i = 0; i < N; i++) { p[i].x -= cx; p[i].y -= cy; }
}

static float dist_at(const StrokePt *a, const StrokePt *b, float ang) {
    float c = cosf(ang), s = sinf(ang), d = 0;
    for (int i = 0; i < N; i++) {
        float x = a[i].x * c - a[i].y * s, y = a[i].x * s + a[i].y * c;
        d += hypotf(x - b[i].x, y - b[i].y);
    }
    return d / N;
}

static float best_dist(const StrokePt *a, const StrokePt *b) {
    const float phi = 0.5f * (-1.0f + sqrtf(5.0f));
    float lo = -0.35f, hi = 0.35f;   // +-20 degrees
    float x1 = phi * lo + (1 - phi) * hi, x2 = (1 - phi) * lo + phi * hi;
    float f1 = dist_at(a, b, x1), f2 = dist_at(a, b, x2);
    while (hi - lo > 0.035f) {
        if (f1 < f2) { hi = x2; x2 = x1; f2 = f1; x1 = phi * lo + (1 - phi) * hi; f1 = dist_at(a, b, x1); }
        else { lo = x1; x1 = x2; f1 = f2; x2 = (1 - phi) * lo + phi * hi; f2 = dist_at(a, b, x2); }
    }
    return fminf(f1, f2);
}

char stroke_recognize(const StrokePt *pts, int n, float *score) {
    if (score) *score = 0;
    StrokePt a[N], b[N];
    if (!resample(pts, n, a)) return 0;
    normalize(a);
    float best = 1e9f;
    char who = 0;
    for (int t = 0; t < NT; t++) {
        if (!resample(TMPL[t].p, TMPL[t].n, b)) continue;
        normalize(b);
        float d = best_dist(a, b);
        if (d < best) { best = d; who = TMPL[t].c; }
    }
    float sc = 1.0f - best / (0.5f * sqrtf(2.0f * SIZE * SIZE));
    if (score) *score = sc;
    return sc >= 0.80f ? who : 0;
}
