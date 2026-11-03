/*
 * app_sleep.cpp - sleep tracking and the smart alarm (see app_sleep.h)
 */
#include "app_sleep.h"
#include "sleep_track.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "hal_audio.h"
#include "hal_imu.h"
#include "hal_nvs.h"
#include "hal_rtc.h"
#include "hal_sleep.h"
#include "hal_vibrate.h"
#include "ui_font.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <new>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int NIGHT_FROM = 20 * 60, NIGHT_TO = 12 * 60;   // the night window, minutes of the day
static const int ALARM_WINDOW = 30;                           // smart alarm: up to 30 min early

static SleepTracker *s_trk = nullptr;     // tonight (PSRAM)
static SleepTracker *s_last = nullptr;    // the last finished night
static bool s_tracking = false;
static int s_start_min = 0, s_last_start_min = 0;   // minute of the day the night began
static int s_rung_yday = -1;
static bool s_ringing = false;
static uint32_t s_snooze_until = 0;

static int now_min(const WatchTime &t) { return t.hour * 60 + t.minute; }
static bool in_night(int m) { return m >= NIGHT_FROM || m < NIGHT_TO; }
static int yday(const WatchTime &t) { return t.month * 31 + t.day; }

static SleepTracker *alloc_tracker() {
    void *p = heap_caps_malloc(sizeof(SleepTracker), MALLOC_CAP_SPIRAM);
    return p ? new (p) SleepTracker() : nullptr;
}

static void save_last() {
    Preferences p;
    if (!p.begin("sleep", false)) return;
    p.putUShort("n", (uint16_t)s_last->minutes());
    p.putUShort("start", (uint16_t)s_last_start_min);
    p.putBytes("raw", s_last->raw(), s_last->minutes());
    p.end();
}

static void load_last() {
    if (!s_last) return;
    Preferences p;
    if (!p.begin("sleep", true)) return;
    int n = p.getUShort("n", 0);
    s_last_start_min = p.getUShort("start", 0);
    if (n > 0 && n <= SleepTracker::MAX_MIN) {
        uint8_t *buf = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (buf && p.getBytes("raw", buf, n) == (size_t)n) s_last->load(buf, n);
        free(buf);
    }
    p.end();
}

static void finish_night() {
    if (!s_tracking) return;
    s_tracking = false;
    int asleep, deep, waso;
    s_trk->totals(asleep, deep, waso);
    if (asleep < 60) return;          // a nap or a night without the watch: keep the last real one
    memcpy((void *)s_last, (const void *)s_trk, sizeof(SleepTracker));
    s_last_start_min = s_start_min;
    save_last();
    DEBUG_PRINTF("[sleep] night saved: %d min asleep, %d deep\n", asleep, deep);
}

static void ring() {
    s_ringing = true;
    sleep_register_activity();
    if (!ui_screen_on_top(&sleep_alarm_screen)) ui_push(&sleep_alarm_screen);
}

static bool ensure() {
    if (!s_trk) {
        s_trk = alloc_tracker();
        s_last = alloc_tracker();
        if (!s_trk || !s_last) { s_trk = nullptr; return false; }
        load_last();
    }
    return true;
}

void sleeptrack_update() {
    if (!ensure()) return;
    WatchTime t = rtc_now();
    if (!t.valid || t.year < 2024) return;
    int m = now_min(t);

    // tracking: the night window, sampled ~5 times a second
    if (g_app_settings.sleep_track && in_night(m)) {
        if (!s_tracking) {
            s_tracking = true;
            s_start_min = m;
            s_trk->start(millis() / 1000);
        }
        static uint32_t last_ms = 0;
        if (millis() - last_ms >= 200) {
            last_ms = millis();
            ImuSample a = imu_read();
            if (a.valid) s_trk->feed(a.ax, a.ay, a.az, millis() / 1000);
        }
    } else if (s_tracking) {
        finish_night();
    }

    // the smart alarm
    if (!g_app_settings.alarm_on || s_ringing) return;
    if (s_snooze_until) {
        if ((int32_t)(millis() - s_snooze_until) >= 0) { s_snooze_until = 0; ring(); }
        return;
    }
    int target = g_app_settings.alarm_min;
    int early = target - ALARM_WINDOW;
    bool due = m >= target && m < target + 60;
    bool window = early >= 0 ? (m >= early && m < target) : (m >= early + 1440 || m < target);
    if (yday(t) == s_rung_yday || !(due || (window && s_tracking && s_trk->light_now()))) return;
    s_rung_yday = yday(t);
    ring();
}

bool sleep_alarm_ringing() { return s_ringing; }

// A made-up night for the simulator's screenshot (tools/watch_sim).
void sleep_load_demo() {
    if (!ensure()) return;
    static uint8_t raw[7 * 60 + 25];
    int n = 0;
    auto run = [&](int mins, int change) { for (int i = 0; i < mins && n < (int)sizeof(raw); i++) raw[n++] = (uint8_t)(change + (i * 7) % 5); };
    run(18, 90); run(25, 30); run(70, 6); run(6, 45); run(55, 25); run(4, 120); run(80, 5);
    run(40, 28); run(3, 140); run(60, 8); run(30, 32); run(n < 445 ? 445 - n : 0, 110);
    s_last->load(raw, n);
    s_last_start_min = 23 * 60 + 12;
    g_app_settings.alarm_on = true;
}

// ---- the Sleep screen -----------------------------------------------------------------------
static int s_press = -1;

static void fmt_hm(char *b, int n, int minutes) {
    snprintf(b, n, "%d h %02d", minutes / 60, minutes % 60);
}

static void draw_night(Arduino_GFX *g, const SleepTracker &tr, int start_min, int y) {
    int n = tr.minutes();
    int asleep, deep, waso;
    tr.totals(asleep, deep, waso);
    char b[48], h[16];
    fmt_hm(h, sizeof(h), asleep);
    ui_text_center(CX, y, COLOR_TEXT, h, 3);
    snprintf(b, sizeof(b), "asleep  -  %d%% still  -  %d min awake", asleep ? deep * 100 / asleep : 0, waso);
    ui_text_center(CX, y + 34, COLOR_TEXT_DIM, b, 1);
    // the night as bars: awake low, light middle, still (deep) tall
    const int W = 320, X0 = CX - W / 2, BASE = y + 110;
    for (int x = 0; x < W && n > 0; x++) {
        int a = x * n / W, z = (x + 1) * n / W;
        if (z <= a) z = a + 1;
        int best = 0;
        for (int k = a; k < z && k < n; k++) {
            int c = tr.at(k);
            int v = c == SLEEP_DEEP ? 3 : (c == SLEEP_LIGHT ? 2 : 1);
            if (v > best) best = v;
        }
        int hgt = best == 3 ? 46 : (best == 2 ? 28 : 8);
        uint16_t col = best == 3 ? COLOR_ACCENT : (best == 2 ? COLOR_ACCENT2 : ui_dim(COLOR_TEXT_DIM, 0.6f));
        g->fillRect(X0 + x, BASE - hgt, 1, hgt, col);
    }
    int end_min = (start_min + n) % 1440;
    snprintf(b, sizeof(b), "%d:%02d", start_min / 60, start_min % 60);
    ui_print(X0, BASE + 8, 1, COLOR_TEXT_DIM, b);
    snprintf(b, sizeof(b), "%d:%02d", end_min / 60, end_min % 60);
    ui_print(X0 + W - ui_text_width(b, 1), BASE + 8, 1, COLOR_TEXT_DIM, b);
}

static void sleep_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g || !s_trk) return;
    int asleep, deep, waso;
    s_trk->totals(asleep, deep, waso);
    bool tonight = s_tracking && asleep > 0;
    if (tonight || s_last->minutes() > 0) {
        ui_text_center(CX, 88, COLOR_TEXT_DIM, tonight ? "Tonight so far" : "Last night", 1);
        draw_night(g, tonight ? *s_trk : *s_last, tonight ? s_start_min : s_last_start_min, 114);
    } else {
        ui_text_center(CX, 130, COLOR_TEXT, "No night yet", 2);
        ui_text_center(CX, 166, COLOR_TEXT_DIM,
                       g_app_settings.sleep_track ? "Wear the watch to bed" : "Tracking is off", 1);
    }

    // smart alarm row: [-]  7:00  [+]   and the on/off pill
    int y = 292;
    char b[24];
    snprintf(b, sizeof(b), "%d:%02d", g_app_settings.alarm_min / 60, g_app_settings.alarm_min % 60);
    uint16_t on = g_app_settings.alarm_on ? COLOR_TEXT : COLOR_TEXT_DIM;
    g->fillCircle(CX - 110, y, 26, s_press == 0 ? COLOR_ACCENT : COLOR_PANEL);
    ui_text_center(CX - 110, y, COLOR_TEXT, "-", 3);
    g->fillCircle(CX + 110, y, 26, s_press == 1 ? COLOR_ACCENT : COLOR_PANEL);
    ui_text_center(CX + 110, y, COLOR_TEXT, "+", 3);
    ui_text_center(CX, y, on, b, 3);
    g->fillRoundRect(CX - 90, y + 44, 180, 46, 23,
                     s_press == 2 ? ui_dim(COLOR_ACCENT, 0.6f) : (g_app_settings.alarm_on ? COLOR_ACCENT : COLOR_PANEL));
    ui_text_center(CX, y + 67, COLOR_TEXT, g_app_settings.alarm_on ? "Smart alarm on" : "Smart alarm off", 1);
    if (g_app_settings.alarm_on) {
        int e = (g_app_settings.alarm_min - ALARM_WINDOW + 1440) % 1440;
        snprintf(b, sizeof(b), "from %d:%02d", e / 60, e % 60);
        ui_text_center(CX, y + 104, COLOR_TEXT_DIM, b, 1);
    }
}

static void sleep_touch(int x, int y, bool pressed) {
    static int armed = -1;
    int hit = -1;
    if (abs(y - 292) < 34 && abs(x - (CX - 110)) < 34) hit = 0;
    else if (abs(y - 292) < 34 && abs(x - (CX + 110)) < 34) hit = 1;
    else if (abs(x - CX) < 95 && y > 330 && y < 386) hit = 2;
    if (pressed) { armed = hit; s_press = hit; return; }
    s_press = -1;
    if (armed < 0 || armed != hit) { armed = -1; return; }
    if (hit == 0) g_app_settings.alarm_min = (g_app_settings.alarm_min + 1440 - 15) % 1440;
    else if (hit == 1) g_app_settings.alarm_min = (g_app_settings.alarm_min + 15) % 1440;
    else g_app_settings.alarm_on = !g_app_settings.alarm_on;
    s_rung_yday = -1;
    nvs_save_settings(g_app_settings);
    armed = -1;
}

static void sleep_gesture(Gesture gs) {
    if (gs == GESTURE_SWIPE_LEFT || gs == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen sleep_screen = {
    "Sleep", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    [] { ensure(); }, sleep_draw, sleep_touch, nullptr, nullptr, sleep_gesture,
    250,
};

// ---- the alarm rings ------------------------------------------------------------------------
static uint32_t s_ring_start = 0, s_beep = 0;

static void alarm_create() { s_ring_start = s_beep = millis() - 2000; }
static void alarm_destroy() { s_ringing = false; }

static void alarm_tick() {
    uint32_t now = millis();
    if (now - s_ring_start > 120000) { ui_pop_screen(); return; }   // gives up after two minutes
    // gentle first, louder as it goes on
    uint32_t every = now - s_ring_start < 30000 ? 2500 : 1200;
    if (now - s_beep >= every) {
        s_beep = now;
        vibrate_ms(now - s_ring_start < 30000 ? 150 : 400);
        audio_play_sfx(880, 160);
        audio_play_sfx(1175, 220);
    }
}

static void alarm_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    WatchTime t = rtc_now();
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", t.hour, t.minute);
    float pu = 0.5f + 0.5f * sinf((millis() - s_ring_start) * 0.004f);
    g->fillCircle(CX, CY - 40, 86 + (int)(pu * 8), ui_dim(COLOR_ACCENT2, 0.22f));
    g->fillCircle(CX, CY - 40, 80, COLOR_ACCENT2);
    ui_text_center(CX, CY - 40, COLOR_BG, b, 4);
    ui_text_center(CX, CY + 66, COLOR_TEXT, "Good morning", 2);
    g->fillRoundRect(CX - 150, CY + 100, 140, 50, 25, s_press == 0 ? COLOR_ACCENT : COLOR_PANEL);
    ui_text_center(CX - 80, CY + 125, COLOR_TEXT, "Snooze", 2);
    g->fillRoundRect(CX + 10, CY + 100, 140, 50, 25, s_press == 1 ? ui_dim(COLOR_ACCENT2, 0.6f) : COLOR_ACCENT2);
    ui_text_center(CX + 80, CY + 125, COLOR_BG, "Stop", 2);
}

static void alarm_touch(int x, int y, bool pressed) {
    int hit = (y > CY + 90 && y < CY + 160) ? (x < CX ? 0 : 1) : -1;
    if (pressed) { s_press = hit; return; }
    int p = s_press;
    s_press = -1;
    if (p < 0 || p != hit) return;
    if (p == 0) s_snooze_until = millis() + 9 * 60 * 1000UL;
    ui_pop_screen();
}

Screen sleep_alarm_screen = {
    "Alarm", GESTURE_MODE_NONE,
    UI_FRAME_MS_SMOOTH,
    alarm_create, alarm_draw, alarm_touch, alarm_tick, alarm_destroy, nullptr,
    0, true,
};
