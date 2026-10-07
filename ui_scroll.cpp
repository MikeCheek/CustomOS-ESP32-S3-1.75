/*
 * ui_scroll.cpp
 * Kinetic vertical scroller shared by every list screen - see UiScroll in
 * ui.h. Time-based (millis), so it behaves the same at 30 or 60 fps.
 */
#include "ui.h"
#include <math.h>

static const int   TAP_SLOP_PX      = 10;      // travel before a touch counts as a drag
static const float RUBBER           = 0.35f;   // content moves this much per px of finger past an end
static const float FRICTION_PER_MS  = 0.0025f; // coasting velocity lost per ms (~400 ms time constant)
static const float OVERSCROLL_FRICTION_MULT = 8.0f; // coasting into the rubber band stops fast
static const float MIN_FLING        = 0.10f;   // px/ms - slower releases don't coast
static const float STOP_VEL         = 0.03f;   // px/ms - below this, coasting ends and snapping starts
static const float SETTLE_TAU_MS    = 70.0f;   // snap/spring-back time constant
static const uint32_t STILL_MS      = 80;      // finger held still this long before lifting = no fling

void ui_scroll_reset(UiScroll *s, int max_scroll, int snap_pitch) {
    *s = UiScroll();
    s->max = max_scroll > 0 ? max_scroll : 0;
    s->snap = snap_pitch > 0 ? snap_pitch : 0;
    s->tick_ms = millis();
}

void ui_scroll_set_max(UiScroll *s, int max_scroll) {
    s->max = max_scroll > 0 ? max_scroll : 0;
}

// Where the list wants to come to rest from its current position.
static float rest_target(const UiScroll *s) {
    float t = s->pos;
    if (t <= 0.0f) return 0.0f;
    if (t >= (float)s->max) return (float)s->max;
    if (s->snap > 0) {
        // Near the end, settle on the end itself rather than on a row
        // boundary that would leave the last row half off screen.
        if (t > (float)s->max - s->snap * 0.5f) return (float)s->max;
        t = roundf(t / (float)s->snap) * (float)s->snap;
        if (t > (float)s->max) t = (float)s->max;
    }
    return t;
}

bool ui_scroll_is_moving(const UiScroll *s) {
    if (s->dragging || s->vel != 0.0f) return true;
    return fabsf(rest_target(s) - s->pos) >= 0.5f;
}

int ui_scroll_offset(const UiScroll *s) {
    return (int)lroundf(s->pos);
}

bool ui_scroll_touch(UiScroll *s, int y, bool pressed) {
    uint32_t now = millis();
    if (pressed) {
        if (!s->dragging) {
            // Touching a list that's still coasting just stops it (like a
            // phone) - that press must not also select a row.
            s->caught = ui_scroll_is_moving(s);
            s->dragging = true;
            s->moved = false;
            s->vel = 0.0f;
            s->start_y = y;
            s->start_pos = s->pos;
            s->last_y = y;
            s->last_ms = now;
            return false;
        }
        int dy = y - s->start_y;
        if (!s->moved && abs(dy) > TAP_SLOP_PX) s->moved = true;
        if (!s->moved) return false;

        // Track the finger 1:1 (minus the slop, so the content doesn't
        // jump when dragging begins), with rubber-band resistance past
        // either end.
        float eff = (float)(dy > 0 ? dy - TAP_SLOP_PX : dy + TAP_SLOP_PX);
        float target = s->start_pos - eff;
        if (target < 0.0f) target *= RUBBER;
        else if (target > (float)s->max) target = (float)s->max + (target - (float)s->max) * RUBBER;
        s->pos = target;

        uint32_t dt = now - s->last_ms;
        if (dt > 0) {
            float v = (float)(s->last_y - y) / (float)dt;
            s->vel = s->vel * 0.5f + v * 0.5f;
            s->last_y = y;
            s->last_ms = now;
        }
        return false;
    }

    if (!s->dragging) return false;
    s->dragging = false;
    bool tap = !s->moved && !s->caught;
    if (!s->moved || now - s->last_ms > STILL_MS || fabsf(s->vel) < MIN_FLING) s->vel = 0.0f;
    // Flinging away from an end you're already pulled past makes no sense.
    if ((s->pos < 0.0f && s->vel < 0.0f) || (s->pos > (float)s->max && s->vel > 0.0f)) s->vel = 0.0f;
    s->tick_ms = now;
    return tap;
}

void ui_scroll_tick(UiScroll *s) {
    uint32_t now = millis();
    uint32_t dt = now - s->tick_ms;
    s->tick_ms = now;
    if (dt > 50) dt = 50; // after a stall, don't teleport
    if (s->dragging || dt == 0) return;

    if (s->vel != 0.0f) {
        s->pos += s->vel * (float)dt;
        float fr = FRICTION_PER_MS * (float)dt;
        if (s->pos < 0.0f || s->pos > (float)s->max) fr *= OVERSCROLL_FRICTION_MULT;
        s->vel = fr >= 1.0f ? 0.0f : s->vel * (1.0f - fr);
        if (fabsf(s->vel) < STOP_VEL) s->vel = 0.0f;
        return;
    }

    float target = rest_target(s);
    float diff = target - s->pos;
    if (fabsf(diff) < 0.5f) {
        s->pos = target;
        return;
    }
    float k = (float)dt / SETTLE_TAU_MS;
    if (k > 1.0f) k = 1.0f;
    s->pos += diff * k;
}
