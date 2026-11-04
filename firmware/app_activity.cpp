#include "app_activity.h"
#include "app_fall.h"
#include "motion_detect.h"
#include "fitness.h"
#include "app_workout.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "hal_audio.h"
#include "hal_imu.h"
#include "hal_rtc.h"
#include "hal_sleep.h"
#include "ui_font.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static ActivityClassifier s_act;
static RepCounter s_reps;
static FlickDetector s_flick;
static bool s_open = false, s_counting = false;
static uint32_t s_secs[ACT_COUNT] = {0};   // today, seconds per activity
static int s_day = -1;

void motion_update() {
    bool track = g_app_settings.activity_track || s_open;
    bool need = g_app_settings.fall_detect || track || g_app_settings.flick_back;
    imu_fifo_enable(need);
    if (!need) return;
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if (now - last_ms < 400) return;   // the FIFO holds 2 s
    last_ms = now;
    WatchTime t = rtc_now();
    if (t.valid && t.day != s_day) {   // a new day: start counting again
        s_day = t.day;
        for (auto &x : s_secs) x = 0;
    }
    bool awake = !sleep_is_asleep();
    static float xyz[128 * 3];
    int n = imu_fifo_read(xyz, 128);
    for (int i = 0; i < n; i++) {
        float x = xyz[i * 3], y = xyz[i * 3 + 1], z = xyz[i * 3 + 2];
        if (g_app_settings.fall_detect) fall_feed(x, y, z);
        if (track && s_act.feed(x, y, z)) {
            s_secs[s_act.current()] += 5;
            fitness_feed(s_act.current(), s_act.cadence());   // the rings
            workout_feed(s_act.current(), s_act.cadence());   // detection, "still working out?"
        }
        if (s_counting && s_reps.feed(x, y, z)) audio_play_sfx(1568, 40);
        if (g_app_settings.flick_back && awake && !s_counting && s_flick.feed(x, y, z)) {
            DEBUG_PRINTF("[motion] flick: back\n");
            ui_pop_screen();
        }
    }
}

// ---- the Activity app -----------------------------------------------------------------------
static int s_press = -1;
static UiScroll s_scroll;
static const int BTN_WO = 806, BTN_REPS = 876, CONTENT_H = 960;

static void act_create() {
    s_open = true;
    s_counting = false;
    s_press = -1;
    ui_scroll_reset(&s_scroll, CONTENT_H - LCD_HEIGHT, 0);
}
static void act_destroy() { s_open = false; s_counting = false; }

static void fmt_min(char *b, int n, uint32_t secs) {
    uint32_t m = secs / 60;
    if (m >= 60) snprintf(b, n, "%luh %02lum", (unsigned long)(m / 60), (unsigned long)(m % 60));
    else snprintf(b, n, "%lu min", (unsigned long)m);
}

static void act_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_counting) {
        char b[12];
        snprintf(b, sizeof(b), "%d", s_reps.count());
        g->fillCircle(CX, CY - 30, 92, ui_dim(COLOR_ACCENT, 0.25f));
        ui_text_center(CX, CY - 30, COLOR_TEXT, b, 5);
        ui_text_center(CX, CY + 48, COLOR_TEXT_DIM, "reps - move with the watch on", 1);
        g->fillRoundRect(CX - 150, CY + 84, 140, 50, 25, s_press == 1 ? COLOR_ACCENT : COLOR_PANEL);
        ui_text_center(CX - 80, CY + 109, COLOR_TEXT, "Reset", 2);
        g->fillRoundRect(CX + 10, CY + 84, 140, 50, 25, s_press == 2 ? ui_dim(COLOR_GOOD, 0.6f) : COLOR_GOOD);
        ui_text_center(CX + 80, CY + 109, COLOR_BG, "Done", 2);
        return;
    }
    // ---- the rings, today in numbers, the week, what you're doing now
    ui_scroll_tick(&s_scroll);
    int off = ui_scroll_offset(&s_scroll);
    int y = -off;
    fitness_draw_rings(CX, y + 176, 104, 24);
    struct { const char *label; uint16_t col; char val[24]; } rows[3] = {
        {"MOVE", FIT_MOVE_COLOR, ""}, {"EXERCISE", FIT_EXERCISE_COLOR, ""}, {"STAND", FIT_STAND_COLOR, ""}};
    snprintf(rows[0].val, sizeof(rows[0].val), "%d/%u kcal", (int)fitness_move_kcal(), (unsigned)fitness_move_goal());
    snprintf(rows[1].val, sizeof(rows[1].val), "%lu/%u min", (unsigned long)fitness_exercise_min(), (unsigned)fitness_exercise_goal());
    snprintf(rows[2].val, sizeof(rows[2].val), "%d/%u hrs", fitness_stand_hours(), (unsigned)fitness_stand_goal());
    for (int i = 0; i < 3; i++) {
        int ry = y + 306 + i * 36;
        ui_print(CX - 140, ry, 1, rows[i].col, rows[i].label);
        ui_print(CX + 140 - ui_text_width(rows[i].val, 2), ry - 4, 2, rows[i].col, rows[i].val);
    }
    // the week: today on the right
    FitDay h[7];
    int hn = fitness_history(h, 6);
    ui_text_center(CX, y + 440, COLOR_TEXT_DIM, "This week", 1);
    static const char *const WD[7] = {"S", "M", "T", "W", "T", "F", "S"};
    WatchTime now = rtc_now();
    for (int i = 0; i < 7; i++) {
        int x = CX - 156 + i * 52, cy = y + 490;
        int back = 6 - i;   // days before today
        float mv = 0, ex = 0, st = 0;
        if (back == 0) {
            mv = fitness_move_kcal() / fitness_move_goal();
            ex = (float)fitness_exercise_min() / fitness_exercise_goal();
            st = (float)fitness_stand_hours() / fitness_stand_goal();
        } else if (back - 1 < hn) {
            mv = (float)h[back - 1].move_kcal / fitness_move_goal();
            ex = (float)h[back - 1].exercise_min / fitness_exercise_goal();
            st = (float)h[back - 1].stand_hours / fitness_stand_goal();
        }
        fitness_draw_rings_frac(x, cy, 20, 5, mv, ex, st);
        int wd = now.valid ? (now.weekday - back + 70) % 7 : i;
        ui_text_center(x, cy + 34, back == 0 ? COLOR_TEXT : COLOR_TEXT_DIM, WD[wd], 1);
    }
    // now
    Activity a = s_act.current();
    static const uint16_t COL[ACT_COUNT] = {COLOR_TEXT_DIM, COLOR_ACCENT2, COLOR_ACCENT3, COLOR_ACCENT};
    ui_text_center(CX, y + 566, COLOR_TEXT_DIM, "Now", 1);
    ui_text_center(CX, y + 598, COL[a], activity_name(a), 3);
    if (s_act.cadence() > 0) {
        char b[24];
        snprintf(b, sizeof(b), "%d steps/min", s_act.cadence());
        ui_text_center(CX, y + 632, COLOR_TEXT_DIM, b, 1);
    }
    static const Activity ROWS[3] = {ACT_WALK, ACT_RUN, ACT_MOVING};
    for (int i = 0; i < 3; i++) {
        int ry = y + 668 + i * 34;
        char b[24];
        fmt_min(b, sizeof(b), s_secs[ROWS[i]]);
        g->fillCircle(CX - 120, ry, 6, COL[ROWS[i]]);
        ui_print(CX - 104, ry - 8, 2, COLOR_TEXT, activity_name(ROWS[i]));
        ui_print(CX + 140 - ui_text_width(b, 2), ry - 8, 2, COLOR_TEXT_DIM, b);
    }
    if (!g_app_settings.activity_track)
        ui_text_center(CX, y + 778, COLOR_TEXT_DIM, "Rings need Settings > Activity > Activity tracking", 1);
    g->fillRoundRect(CX - 110, y + BTN_WO, 220, 56, 28, s_press == 3 ? ui_dim(FIT_EXERCISE_COLOR, 0.6f) : FIT_EXERCISE_COLOR);
    ui_text_center(CX, y + BTN_WO + 28, COLOR_BG, workout_active() ? "Workout running" : "Start a workout", 2);
    g->fillRoundRect(CX - 110, y + BTN_REPS, 220, 56, 28, s_press == 0 ? ui_dim(COLOR_ACCENT, 0.6f) : COLOR_ACCENT);
    ui_text_center(CX, y + BTN_REPS + 28, COLOR_TEXT, "Count reps", 2);
    if (off > 0) g->fillRect(0, 0, LCD_WIDTH, 84, COLOR_BG);   // under the title
}

static void act_touch(int x, int y, bool pressed) {
    if (s_counting) {
        int hit = -1;
        if (y > CY + 80 && y < CY + 140) hit = x < CX ? 1 : 2;
        if (pressed) { s_press = hit; return; }
        int p = s_press;
        s_press = -1;
        if (p < 0 || p != hit) return;
        if (p == 1) s_reps.reset();
        else s_counting = false;
        return;
    }
    int cy = y + ui_scroll_offset(&s_scroll);
    int hit = -1;
    if (abs(x - CX) < 115 && cy > BTN_WO - 4 && cy < BTN_WO + 60) hit = 3;
    else if (abs(x - CX) < 115 && cy > BTN_REPS - 4 && cy < BTN_REPS + 60) hit = 0;
    if (pressed && !s_scroll.dragging) s_press = hit;
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    if (pressed) { if (s_scroll.moved) s_press = -1; return; }
    int p = s_press;
    s_press = -1;
    if (!tap || p < 0 || p != hit) return;
    if (p == 0) { s_reps.reset(); s_counting = true; }
    else if (p == 3) ui_push(workout_active() ? &workout_live_screen : &workout_screen);
}

static void act_gesture(Gesture gs) {
    if (gs == GESTURE_SWIPE_LEFT || gs == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen activity_screen = {
    "Activity", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    act_create, act_draw, act_touch, nullptr, act_destroy, act_gesture,
    250, true,
};
