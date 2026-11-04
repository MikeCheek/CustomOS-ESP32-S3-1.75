// Activity rings - see fitness.h.
#include "haptics.h"
#include "fitness.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "ui_font.h"
#include "dnd.h"
#include "hal_audio.h"
#include "hal_imu.h"
#include "hal_rtc.h"
#include "hal_sleep.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

static float    s_kcal = 0;
static uint32_t s_ex_s = 0;
static uint32_t s_stand_mask = 0;      // bit h: hour h counted
static uint16_t s_hour_move_s = 0;     // moving this hour so far
static int      s_hour = -1;
static uint32_t s_date = 0;            // yyyymmdd of the values above
static FitDay   s_hist[7];
static int      s_hist_n = 0;
static bool     s_dirty = false;
static uint32_t s_saved_ms = 0;
static bool     s_workout = false;
static int      s_workout_met = 0;
static uint8_t  s_closed = 0;          // rings already celebrated today (bit 0 move, 1 exercise, 2 stand)
static int      s_reminded_hour = -1;

uint16_t fitness_move_goal() { return g_app_settings.move_goal; }
uint16_t fitness_exercise_goal() { return g_app_settings.exercise_goal; }
uint8_t  fitness_stand_goal() { return g_app_settings.stand_goal; }
float    fitness_move_kcal() { return s_kcal; }
uint32_t fitness_exercise_min() { return s_ex_s / 60; }
int      fitness_stand_hours() { return __builtin_popcount(s_stand_mask); }

int fitness_history(FitDay *out, int max) {
    int n = s_hist_n < max ? s_hist_n : max;
    memcpy(out, s_hist, sizeof(FitDay) * n);
    return n;
}

static void save() {
    Preferences p;
    if (!p.begin("fit", false)) return;
    p.putUInt("date", s_date);
    p.putFloat("kcal", s_kcal);
    p.putUInt("ex", s_ex_s);
    p.putUInt("stand", s_stand_mask);
    p.putUChar("closed", s_closed);
    p.putUChar("hn", (uint8_t)s_hist_n);
    p.putBytes("hist", s_hist, sizeof(FitDay) * s_hist_n);
    p.end();
    s_dirty = false;
    s_saved_ms = millis();
}

void fitness_init() {
    Preferences p;
    if (!p.begin("fit", true)) return;
    s_date = p.getUInt("date", 0);
    s_kcal = p.getFloat("kcal", 0);
    s_ex_s = p.getUInt("ex", 0);
    s_stand_mask = p.getUInt("stand", 0);
    s_closed = p.getUChar("closed", 0);
    s_hist_n = p.getUChar("hn", 0);
    if (s_hist_n > 7 || (s_hist_n && p.getBytes("hist", s_hist, sizeof(FitDay) * s_hist_n) != sizeof(FitDay) * s_hist_n))
        s_hist_n = 0;
    p.end();
}

static uint32_t date_of(const WatchTime &t) { return (uint32_t)t.year * 10000 + t.month * 100 + t.day; }

// A new day: yesterday goes to the history, the rings start empty.
static void roll(uint32_t today) {
    if (s_date && s_date != today) {
        memmove(&s_hist[1], &s_hist[0], sizeof(FitDay) * 6);
        s_hist[0] = {(uint16_t)s_kcal, (uint16_t)(s_ex_s / 60), (uint8_t)__builtin_popcount(s_stand_mask), 0,
                     imu_get_step_count(), s_date};
        if (s_hist_n < 7) s_hist_n++;
        s_kcal = 0;
        s_ex_s = 0;
        s_stand_mask = 0;
        s_hour_move_s = 0;
        s_closed = 0;
    }
    s_date = today;
    s_dirty = true;
}

void fitness_workout(bool on, int met_x10) {
    s_workout = on;
    s_workout_met = met_x10;
}

float fitness_kg() { return g_app_settings.weight_kg >= 30 ? g_app_settings.weight_kg : 70; }
float fitness_stride_scale() { return (g_app_settings.height_cm >= 100 ? g_app_settings.height_cm : 175) / 175.0f; }

// Active calories in one 5 s window: MET minus resting, for the wearer's
// weight (kcal/min = MET * 3.5 * kg / 200, Settings > Activity).
static float window_kcal(Activity a, int cadence) {
    float met = 1.0f;
    if (s_workout && s_workout_met > 0) met = s_workout_met / 10.0f;
    else if (a == ACT_RUN) met = 9.0f;
    else if (a == ACT_WALK) met = cadence >= 115 ? 4.3f : cadence >= 100 ? 3.5f : 2.8f;
    else if (a == ACT_MOVING) met = 2.0f;
    float per_min = (met - 1.0f) * 3.5f * fitness_kg() / 200.0f;
    return per_min > 0 ? per_min / 12.0f : 0;
}

void fitness_feed(Activity a, int cadence) {
    WatchTime t = rtc_now();
    if (!t.valid) return;
    uint32_t today = date_of(t);
    if (today != s_date) roll(today);
    if (t.hour != s_hour) { s_hour = t.hour; s_hour_move_s = 0; }
    s_kcal += window_kcal(a, cadence);
    bool brisk = (a == ACT_WALK && cadence >= 100) || a == ACT_RUN;
    if (brisk || s_workout) s_ex_s += 5;
    if (a != ACT_STILL || s_workout) {
        s_hour_move_s += 5;
        if (s_hour_move_s >= 60) s_stand_mask |= 1u << t.hour;
    }
    s_dirty = true;
}

static void celebrate_if_closed() {
    uint8_t now = 0;
    if (s_kcal >= fitness_move_goal()) now |= 1;
    if (fitness_exercise_min() >= fitness_exercise_goal()) now |= 2;
    if (fitness_stand_hours() >= fitness_stand_goal()) now |= 4;
    uint8_t fresh = now & ~s_closed;
    if (!fresh) return;
    s_closed |= fresh;
    save();   // celebrated once, even across a restart
    if (dnd_active()) return;
    sleep_register_activity();
    if (!ui_screen_on_top(&rings_celebrate_screen)) ui_push(&rings_celebrate_screen);
}

void fitness_update() {
    static uint32_t last = 0;
    uint32_t ms = millis();
    if (ms - last < 2000) return;
    last = ms;
    WatchTime t = rtc_now();
    if (!t.valid) return;
    uint32_t today = date_of(t);
    if (today != s_date) roll(today);
    celebrate_if_closed();
    // Ten to the hour, daytime, nothing yet this hour: a nudge.
    if (g_app_settings.stand_remind && g_app_settings.activity_track && t.minute == 50 && t.hour >= 8 && t.hour <= 21 &&
        s_reminded_hour != t.hour && !(s_stand_mask & (1u << t.hour)) && !dnd_active()) {
        s_reminded_hour = t.hour;
        sleep_register_activity();
        haptic(HAP_REMINDER);
        ui_show_toast("Time to stand - move around for a minute", 6000);
    }
    if (s_dirty && ms - s_saved_ms > 10 * 60000UL) save();
}

// ---- drawing -------------------------------------------------------------------------------------
static void one_ring(int cx, int cy, int r, int thick, float frac, uint16_t col) {
    ui_arc(cx, cy, r, thick, 0, 360, ui_dim(col, 0.22f), false);
    if (frac <= 0.004f) return;
    if (frac < 1) {
        ui_arc(cx, cy, r, thick, 0, 360 * frac, col, true);
    } else {
        // past the goal: the whole ring, then the next lap on top, a shade darker at its start
        ui_arc(cx, cy, r, thick, 0, 360, ui_dim(col, 0.75f), true);
        float more = frac - floorf(frac);
        if (more > 0.004f) ui_arc(cx, cy, r, thick, 0, 360 * more, col, true);
    }
}

void fitness_draw_rings_frac(int cx, int cy, int r, int thick, float mv, float ex, float st) {
    int gap = thick / 5 + 1;
    one_ring(cx, cy, r, thick, mv, FIT_MOVE_COLOR);
    one_ring(cx, cy, r - thick - gap, thick, ex, FIT_EXERCISE_COLOR);
    one_ring(cx, cy, r - 2 * (thick + gap), thick, st, FIT_STAND_COLOR);
}

void fitness_draw_rings(int cx, int cy, int r, int thick, float grow) {
    fitness_draw_rings_frac(cx, cy, r, thick, s_kcal / fitness_move_goal() * grow,
                            (float)fitness_exercise_min() / fitness_exercise_goal() * grow,
                            (float)fitness_stand_hours() / fitness_stand_goal() * grow);
}

// ---- a ring closed -----------------------------------------------------------------------------------
static uint32_t s_cel_ms = 0;

static void cel_create() {
    s_cel_ms = millis();
    haptic(HAP_SUCCESS);
}

static void cel_tick() {
    if (millis() - s_cel_ms > 7000) ui_pop_screen();
}

static void cel_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2 - 30;
    float t = (millis() - s_cel_ms) / 1000.0f;
    float grow = t < 1.2f ? 1 - (1 - t / 1.2f) * (1 - t / 1.2f) : 1;
    // sparks flying out of the rings
    for (int i = 0; i < 24; i++) {
        float a = i * 0.2618f + 0.3f * sinf(i * 1.7f);
        float d = 120 + fmodf(t * 160 + i * 37, 120);
        uint16_t c = i % 3 == 0 ? FIT_MOVE_COLOR : i % 3 == 1 ? FIT_EXERCISE_COLOR : FIT_STAND_COLOR;
        g->fillCircle(CX + (int)(d * cosf(a)), CY + (int)(d * sinf(a)), 3, ui_dim(c, 1.0f - (d - 120) / 140));
    }
    fitness_draw_rings(CX, CY, 110, 24, grow);
    const char *msg = "Goal reached!";
    if (s_closed == 7) msg = "All rings closed!";
    else if (s_closed & 1) msg = "Move goal reached!";
    ui_text_center(LCD_WIDTH / 2, CY + 150, COLOR_TEXT, msg, 3);
    char b[48];
    snprintf(b, sizeof(b), "%d kcal  -  %lu min  -  %d h", (int)s_kcal, (unsigned long)fitness_exercise_min(), fitness_stand_hours());
    ui_text_center(LCD_WIDTH / 2, CY + 190, COLOR_TEXT_DIM, b, 1);
}

static void cel_touch(int, int, bool pressed) {
    static bool prev = false;
    if (!pressed && prev && millis() - s_cel_ms > 600) ui_pop_screen();
    prev = pressed;
}

Screen rings_celebrate_screen = {
    nullptr, GESTURE_MODE_NONE, UI_FRAME_MS_SMOOTH,
    cel_create, cel_draw, cel_touch, cel_tick, nullptr, nullptr,
    0, true,
};
