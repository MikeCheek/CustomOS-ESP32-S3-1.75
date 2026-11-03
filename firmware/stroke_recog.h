/*
 * stroke_recog.h
 * One-stroke letter recognition for "Draw to open" (app_draw.cpp): the $1
 * Unistroke Recognizer (Wobbrock, Wilson & Li, UIST 2007) - resample the
 * stroke to 64 points, scale it to a square, centre it, and pick the
 * template with the smallest average point distance (searched over
 * +-20 degrees). The usual turn to the "indicative angle" is left out on
 * purpose, so a V and an upside-down V (our A) stay different letters.
 * Pure C++; tools/stroke_test.cpp checks it.
 */
#pragma once

struct StrokePt { float x, y; };

// The best match among the built-in letters for a stroke of n points, or
// 0 when none is close; *score gets 0..1 (1 = perfect).
char stroke_recognize(const StrokePt *pts, int n, float *score = nullptr);
// The letters it knows, for the help line ("ACMNPSVZ").
const char *stroke_letters();
// A template's points (0..100 box), for drawing hints and for the test.
int stroke_template(char letter, StrokePt *out, int max);
