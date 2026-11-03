#include "app_activity.h"
#include "app_fall.h"
#include "motion_detect.h"
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
        if (track && s_act.feed(x, y, z)) s_secs[s_act.current()] += 5;
        if (s_counting && s_reps.feed(x, y, z)) audio_play_sfx(1568, 40);
        if (g_app_settings.flick_back && awake && !s_counting && s_flick.feed(x, y, z)) {
            DEBUG_PRINTF("[motion] flick: back\n");
            ui_pop_screen();
        }
    }
}

// ---- the Activity app -----------------------------------------------------------------------
static int s_press = -1;

static void act_create() { s_open = true; s_counting = false; s_press = -1; }
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
    Activity a = s_act.current();
    static const uint16_t COL[ACT_COUNT] = {COLOR_TEXT_DIM, COLOR_ACCENT2, COLOR_ACCENT3, COLOR_ACCENT};
    ui_text_center(CX, 92, COLOR_TEXT_DIM, "Now", 1);
    ui_text_center(CX, 128, COL[a], activity_name(a), 3);
    if (s_act.cadence() > 0) {
        char b[24];
        snprintf(b, sizeof(b), "%d steps/min", s_act.cadence());
        ui_text_center(CX, 164, COLOR_TEXT_DIM, b, 1);
    }
    // today
    static const Activity ROWS[3] = {ACT_WALK, ACT_RUN, ACT_MOVING};
    for (int i = 0; i < 3; i++) {
        int y = 206 + i * 34;
        char b[24];
        fmt_min(b, sizeof(b), s_secs[ROWS[i]]);
        g->fillCircle(CX - 120, y, 6, COL[ROWS[i]]);
        ui_print(CX - 104, y - 8, 2, COLOR_TEXT, activity_name(ROWS[i]));
        ui_print(CX + 140 - ui_text_width(b, 2), y - 8, 2, COLOR_TEXT_DIM, b);
    }
    if (!g_app_settings.activity_track)
        ui_text_center(CX, 310, COLOR_TEXT_DIM, "Only while this is open - Settings: Activity tracking", 1);
    g->fillRoundRect(CX - 100, 336, 200, 52, 26, s_press == 0 ? ui_dim(COLOR_ACCENT, 0.6f) : COLOR_ACCENT);
    ui_text_center(CX, 362, COLOR_TEXT, "Count reps", 2);
}

static void act_touch(int x, int y, bool pressed) {
    int hit = -1;
    if (s_counting) {
        if (y > CY + 80 && y < CY + 140) hit = x < CX ? 1 : 2;
    } else if (abs(x - CX) < 105 && y > 330 && y < 394) {
        hit = 0;
    }
    if (pressed) { s_press = hit; return; }
    int p = s_press;
    s_press = -1;
    if (p < 0 || p != hit) return;
    if (p == 0) { s_reps.reset(); s_counting = true; }
    else if (p == 1) s_reps.reset();
    else s_counting = false;
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
