// Timer, Stopwatch, Alarms and World clock - see app_clock.h.
#include "app_clock.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "clock_fmt.h"
#include "app_assistant.h"
#include "app_calendar.h"
#include "app_sleep.h"
#include "hal_audio.h"
#include "hal_rtc.h"
#include "hal_sleep.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
// the clock apps' colour (watchOS uses orange for them too)
#define CLOCK_ORANGE ((uint16_t)0xFC60)

// ---- small shared pieces ---------------------------------------------------------------------

struct Btn {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x - 6 && px < x + w + 6 && py >= y - 6 && py < y + h + 6; }
};

static void pill(const Btn &b, uint16_t bg, uint16_t fg, const char *text, bool pressed = false) {
    ui_gfx()->fillRoundRect(b.x, b.y, b.w, b.h, b.h / 2, pressed ? ui_dim(bg, 0.6f) : bg);
    ui_text_center(b.x + b.w / 2, b.y + b.h / 2, fg, text, 2);
}

// Press and release on the same thing: a tap (for screens with a few buttons).
struct Taps {
    int down = -1;
    // id = what's under the finger now (-1: nothing). Returns the tapped id on release, else -1.
    int feed(int id, bool pressed) {
        if (pressed) { if (down < 0) down = id < 0 ? -2 : id; return -1; }
        int d = down;
        down = -1;
        return d >= 0 && d == id ? d : -1;
    }
    bool held(int id) const { return down == id; }
};

static void clear_header_band() { ui_gfx()->fillRect(0, 0, LCD_WIDTH, 84, COLOR_BG); }

static void fmt_hms(char *b, int n, uint32_t s) {
    if (s >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// =========================================================================
//  Timer
// =========================================================================
static const uint16_t PRESETS[8] = {1, 3, 5, 10, 15, 30, 60, 120};   // minutes; the 9th is Custom
static Taps s_tm_taps;
static bool s_tm_custom = false;
static int  s_tm_custom_min = 20;

static void preset_pos(int i, int &x, int &y) {
    x = CX + (i % 3 - 1) * 118;
    y = 146 + (i / 3) * 112;
}

static void timer_create() { s_tm_custom = false; s_tm_taps.down = -1; }

static void timer_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    if (assistant_timer_active()) {
        uint32_t len = assistant_timer_length_s();
        uint32_t left_ms = assistant_timer_left_ms();
        float frac = len ? left_ms / (len * 1000.0f) : 0;
        bool paused = assistant_timer_paused();
        uint16_t col = paused ? COLOR_TEXT_DIM : CLOCK_ORANGE;
        ui_arc(CX, CY, 214, 12, 0, 360, COLOR_PANEL, false);
        if (frac > 0.002f) ui_arc(CX, CY, 214, 12, 0, 360 * frac, col, true);
        char b[16];
        fmt_hms(b, sizeof(b), (left_ms + 999) / 1000);
        ui_text_center(CX, CY - 40, COLOR_TEXT, b, 5);
        char l[24];
        fmt_hms(l, sizeof(l), len);
        char t[40];
        snprintf(t, sizeof(t), paused ? "Paused - %s timer" : "%s timer", l);
        ui_text_center(CX, CY + 22, COLOR_TEXT_DIM, t, 1);
        pill({CX - 150, 300, 140, 60}, COLOR_PANEL, COLOR_TEXT, "Cancel", s_tm_taps.held(1));
        pill({CX + 10, 300, 140, 60}, paused ? COLOR_GOOD : CLOCK_ORANGE, COLOR_BG, paused ? "Resume" : "Pause", s_tm_taps.held(2));
        return;
    }
    if (s_tm_custom) {
        char b[16];
        if (s_tm_custom_min >= 60) snprintf(b, sizeof(b), "%dh %02d", s_tm_custom_min / 60, s_tm_custom_min % 60);
        else snprintf(b, sizeof(b), "%d", s_tm_custom_min);
        ui_text_center(CX, 190, COLOR_TEXT, b, 5);
        ui_text_center(CX, 240, COLOR_TEXT_DIM, s_tm_custom_min >= 60 ? "hours, minutes" : "minutes", 1);
        g->fillCircle(CX - 140, 190, 40, s_tm_taps.held(1) ? COLOR_ACCENT : COLOR_PANEL);
        g->fillCircle(CX + 140, 190, 40, s_tm_taps.held(2) ? COLOR_ACCENT : COLOR_PANEL);
        ui_text_center(CX - 140, 190, COLOR_TEXT, "-", 4);
        ui_text_center(CX + 140, 190, COLOR_TEXT, "+", 4);
        pill({CX - 90, 300, 180, 62}, CLOCK_ORANGE, COLOR_BG, "Start", s_tm_taps.held(3));
        return;
    }
    for (int i = 0; i < 9; i++) {
        int x, y;
        preset_pos(i, x, y);
        g->fillCircle(x, y, 48, s_tm_taps.held(10 + i) ? ui_dim(CLOCK_ORANGE, 0.5f) : 0x3186);
        if (i == 8) { ui_text_center(x, y, CLOCK_ORANGE, "Custom", 1); continue; }
        char b[8];
        int m = PRESETS[i];
        if (m >= 60) snprintf(b, sizeof(b), "%d", m / 60);
        else snprintf(b, sizeof(b), "%d", m);
        ui_text_center(x, y - 8, COLOR_TEXT, b, 3);
        ui_text_center(x, y + 24, COLOR_TEXT_DIM, m >= 60 ? (m == 60 ? "hour" : "hours") : "min", 1);
    }
}

static int custom_step(int m, int dir) {
    int step = m + (dir > 0 ? 0 : -1) < 10 ? 1 : m + (dir > 0 ? 0 : -1) < 60 ? 5 : 15;
    m += dir * step;
    if (m < 1) m = 1;
    if (m > 600) m = 600;
    return m;
}

static void timer_touch(int x, int y, bool pressed) {
    int id = -1;
    if (assistant_timer_active()) {
        if (Btn{CX - 150, 300, 140, 60}.hit(x, y)) id = 1;
        else if (Btn{CX + 10, 300, 140, 60}.hit(x, y)) id = 2;
    } else if (s_tm_custom) {
        if ((x - (CX - 140)) * (x - (CX - 140)) + (y - 190) * (y - 190) < 50 * 50) id = 1;
        else if ((x - (CX + 140)) * (x - (CX + 140)) + (y - 190) * (y - 190) < 50 * 50) id = 2;
        else if (Btn{CX - 90, 300, 180, 62}.hit(x, y)) id = 3;
    } else {
        for (int i = 0; i < 9; i++) {
            int px, py;
            preset_pos(i, px, py);
            if ((x - px) * (x - px) + (y - py) * (y - py) < 52 * 52) id = 10 + i;
        }
    }
    int t = s_tm_taps.feed(id, pressed);
    if (t < 0) return;
    sleep_register_activity();
    if (assistant_timer_active()) {
        if (t == 1) assistant_timer_set(0);
        else if (t == 2) assistant_timer_pause(!assistant_timer_paused());
    } else if (s_tm_custom) {
        if (t == 1) s_tm_custom_min = custom_step(s_tm_custom_min, -1);
        else if (t == 2) s_tm_custom_min = custom_step(s_tm_custom_min, +1);
        else if (t == 3) { assistant_timer_set(s_tm_custom_min * 60); s_tm_custom = false; }
    } else if (t == 18) {
        s_tm_custom = true;
    } else if (t >= 10) {
        assistant_timer_set(PRESETS[t - 10] * 60);
    }
}

static void timer_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT && s_tm_custom) { s_tm_custom = false; return; }
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen timer_screen = {
    "Timer", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    timer_create, timer_draw, timer_touch, nullptr, nullptr, timer_gesture,
    500,
};

// =========================================================================
//  Stopwatch
// =========================================================================
static bool     s_sw_run = false;
static uint32_t s_sw_start = 0;     // millis() at the last start
static uint32_t s_sw_acc = 0;       // ms counted before it
static const int MAX_LAPS = 30;
static uint32_t s_laps[MAX_LAPS];   // each lap's own length
static int      s_lap_n = 0;
static uint32_t s_lap_mark = 0;     // stopwatch time at the last lap
static Taps     s_sw_taps;

bool stopwatch_running() { return s_sw_run; }
uint32_t stopwatch_ms() { return s_sw_acc + (s_sw_run ? millis() - s_sw_start : 0); }

static void fmt_sw(char *b, int n, uint32_t ms) {
    uint32_t cs = ms / 10 % 100, s = ms / 1000;
    if (s >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%02lu:%02lu.%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60), (unsigned long)cs);
}

static void sw_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    uint32_t ms = stopwatch_ms();
    // the seconds hand round the edge
    float sec = (ms % 60000) / 60000.0f;
    ui_arc(CX, CY, 222, 4, 0, 360, COLOR_PANEL, false);
    if (ms > 0) ui_arc(CX, CY, 222, 4, 0, 360 * sec, CLOCK_ORANGE, true);
    char b[20];
    fmt_sw(b, sizeof(b), ms);
    ui_text_center(CX, 150, COLOR_TEXT, b, 4);
    // laps: the current one, then the last ones (best green, worst red)
    int best = -1, worst = -1;
    for (int i = 0; i < s_lap_n; i++) {
        if (best < 0 || s_laps[i] < s_laps[best]) best = i;
        if (worst < 0 || s_laps[i] > s_laps[worst]) worst = i;
    }
    int row = 0;
    if (s_lap_n > 0 || s_sw_run) {
        char l[24], t[20];
        snprintf(l, sizeof(l), "Lap %d", s_lap_n + 1);
        fmt_sw(t, sizeof(t), ms - s_lap_mark);
        ui_print(CX - 130, 200, 2, COLOR_TEXT, l);
        ui_print(CX + 130 - ui_text_width(t, 2), 200, 2, COLOR_TEXT, t);
        row = 1;
    }
    for (int i = s_lap_n - 1; i >= 0 && row < 3; i--, row++) {
        char l[24], t[20];
        snprintf(l, sizeof(l), "Lap %d", i + 1);
        fmt_sw(t, sizeof(t), s_laps[i]);
        uint16_t c = s_lap_n > 1 && i == best ? COLOR_GOOD : s_lap_n > 1 && i == worst ? COLOR_BAD : COLOR_TEXT_DIM;
        int y = 200 + row * 34;
        ui_print(CX - 130, y, 2, c, l);
        ui_print(CX + 130 - ui_text_width(t, 2), y, 2, c, t);
    }
    bool zero = !s_sw_run && ms == 0;
    pill({CX - 150, 316, 140, 60}, COLOR_PANEL, zero ? COLOR_TEXT_DIM : COLOR_TEXT, s_sw_run ? "Lap" : "Reset", s_sw_taps.held(1));
    pill({CX + 10, 316, 140, 60}, s_sw_run ? COLOR_BAD : COLOR_GOOD, COLOR_BG, s_sw_run ? "Stop" : "Start", s_sw_taps.held(2));
}

static void sw_touch(int x, int y, bool pressed) {
    int id = Btn{CX - 150, 316, 140, 60}.hit(x, y) ? 1 : Btn{CX + 10, 316, 140, 60}.hit(x, y) ? 2 : -1;
    int t = s_sw_taps.feed(id, pressed);
    if (t == 2) {
        if (s_sw_run) { s_sw_acc = stopwatch_ms(); s_sw_run = false; }
        else { s_sw_start = millis(); s_sw_run = true; }
    } else if (t == 1) {
        if (s_sw_run) {
            uint32_t now = stopwatch_ms();
            if (s_lap_n == MAX_LAPS) { memmove(s_laps, s_laps + 1, sizeof(s_laps[0]) * (MAX_LAPS - 1)); s_lap_n--; }
            s_laps[s_lap_n++] = now - s_lap_mark;
            s_lap_mark = now;
        } else {
            s_sw_acc = 0; s_lap_n = 0; s_lap_mark = 0;
        }
    }
}

static void sw_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

static void sw_create() { s_sw_taps.down = -1; }

Screen stopwatch_screen = {
    "Stopwatch", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    sw_create, sw_draw, sw_touch, nullptr, nullptr, sw_gesture,
    0,
    true,   // suppress_idle: the hundredths keep moving while it runs
};

// =========================================================================
//  Alarms
// =========================================================================
struct Alarm {
    uint16_t min;    // minutes after midnight
    uint8_t  days;   // bit 0 = Sunday ... bit 6 = Saturday; 0 = once
    uint8_t  on;
};
static const int MAX_ALARMS = 8;
static Alarm s_alarms[MAX_ALARMS];
static int   s_alarm_n = 0;
static int   s_last_checked_min = -1;
static int   s_ringing = -1;           // which alarm is ringing (-1: none)
static uint32_t s_snooze_at = 0;       // millis() to ring again (0: no snooze)
static int   s_snooze_alarm = -1;
static int   s_utc_off = -1440;        // unknown until the phone says

static void save_alarms() {
    Preferences p;
    if (!p.begin("clock", false)) return;
    p.putBytes("alarms", s_alarms, sizeof(Alarm) * s_alarm_n);
    p.putUChar("alarm_n", (uint8_t)s_alarm_n);
    p.end();
}

void clock_set_utc_offset(int minutes) {
    if (minutes < -14 * 60 || minutes > 14 * 60 || minutes == s_utc_off) return;
    s_utc_off = minutes;
    Preferences p;
    if (!p.begin("clock", false)) return;
    p.putShort("tz", (int16_t)minutes);
    p.end();
}
int clock_utc_offset() { return s_utc_off; }

static uint32_t s_city_mask = 0;   // world clock cities shown
static void load_cities_default();

void clock_init() {
    Preferences p;
    if (!p.begin("clock", true)) { load_cities_default(); return; }
    s_alarm_n = p.getUChar("alarm_n", 0);
    if (s_alarm_n > MAX_ALARMS) s_alarm_n = 0;
    if (s_alarm_n && p.getBytes("alarms", s_alarms, sizeof(Alarm) * s_alarm_n) != sizeof(Alarm) * s_alarm_n) s_alarm_n = 0;
    s_utc_off = p.getShort("tz", -1440);
    s_city_mask = p.getUInt("cities", 0);
    p.end();
    if (!s_city_mask) load_cities_default();
}

static void days_text(char *b, int n, uint8_t days) {
    static const char *const D[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (days == 0) { snprintf(b, n, "Once"); return; }
    if (days == 0x7F) { snprintf(b, n, "Every day"); return; }
    if (days == 0x3E) { snprintf(b, n, "Weekdays"); return; }
    if (days == 0x41) { snprintf(b, n, "Weekends"); return; }
    b[0] = 0;
    for (int i = 1; i <= 7; i++) {
        int d = i % 7;   // from Monday
        if (!(days & (1 << d))) continue;
        if (b[0]) strlcat(b, " ", n);
        strlcat(b, D[d], n);
    }
}

bool clock_alarm_ringing() { return s_ringing >= 0; }

static void ring(int i) {
    s_ringing = i;
    sleep_register_activity();
    if (!ui_screen_on_top(&alarm_ring_screen)) ui_push(&alarm_ring_screen);
}

void clock_update() {
    if (s_snooze_at && (int32_t)(millis() - s_snooze_at) >= 0) {
        s_snooze_at = 0;
        if (s_ringing < 0) ring(s_snooze_alarm);
    }
    WatchTime t = rtc_now();
    if (!t.valid) return;
    int now_min = t.hour * 60 + t.minute;
    if (now_min == s_last_checked_min) return;
    s_last_checked_min = now_min;
    if (s_ringing >= 0 || sleep_alarm_ringing()) return;
    for (int i = 0; i < s_alarm_n; i++) {
        Alarm &a = s_alarms[i];
        if (!a.on || a.min != now_min) continue;
        if (a.days && !(a.days & (1 << t.weekday))) continue;
        if (!a.days) { a.on = 0; save_alarms(); }   // a one-off goes off after ringing
        ring(i);
        break;
    }
}

bool clock_next_alarm(uint32_t &minutes_from_now, int &hour, int &minute) {
    WatchTime t = rtc_now();
    if (!t.valid) return false;
    int now_min = t.hour * 60 + t.minute;
    uint32_t best = UINT32_MAX;
    int best_i = -1;
    for (int i = 0; i < s_alarm_n; i++) {
        const Alarm &a = s_alarms[i];
        if (!a.on) continue;
        for (int d = 0; d < 8; d++) {   // today ... a week from now
            int wd = (t.weekday + d) % 7;
            if (a.days && !(a.days & (1 << wd))) continue;
            int32_t m = d * 1440 + a.min - now_min;
            if (m <= 0) continue;
            if ((uint32_t)m < best) { best = m; best_i = i; }
            break;
        }
    }
    if (best_i < 0) return false;
    minutes_from_now = best;
    hour = s_alarms[best_i].min / 60;
    minute = s_alarms[best_i].min % 60;
    return true;
}

// ---- the list ---------------------------------------------------------------------------------
static UiScroll s_al_scroll;
static int s_edit = -1;                 // the alarm being edited (MAX_ALARMS = a new one)
static Alarm s_edit_val;
static const int AL_ROW = 92, AL_TOP = 96;
extern Screen alarm_edit_screen;

static void alarms_create() {
    int rows = s_alarm_n + (s_alarm_n < MAX_ALARMS ? 1 : 0);
    ui_scroll_reset(&s_al_scroll, rows * AL_ROW - (LCD_HEIGHT - AL_TOP - 40), 0);
}

static void toggle_switch(int x, int y, bool on) {
    Arduino_GFX *g = ui_gfx();
    g->fillRoundRect(x, y, 64, 36, 18, on ? COLOR_GOOD : COLOR_PANEL);
    g->fillCircle(on ? x + 46 : x + 18, y + 18, 14, COLOR_TEXT);
}

static void alarms_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_scroll_tick(&s_al_scroll);
    g->fillScreen(COLOR_BG);
    int off = ui_scroll_offset(&s_al_scroll);
    int rows = s_alarm_n + (s_alarm_n < MAX_ALARMS ? 1 : 0);
    for (int i = 0; i < rows; i++) {
        int y = AL_TOP + i * AL_ROW - off;
        if (y > LCD_HEIGHT || y + AL_ROW < 0) continue;
        if (i == s_alarm_n) {
            g->fillRoundRect(CX - 150, y + 14, 300, 60, 30, 0x3186);
            ui_text_center(CX, y + 44, CLOCK_ORANGE, "+ Add alarm", 2);
            continue;
        }
        const Alarm &a = s_alarms[i];
        char hm[8], d[40];
        clock_hm(hm, sizeof(hm), a.min / 60, a.min % 60);
        days_text(d, sizeof(d), a.days);
        uint16_t c = a.on ? COLOR_TEXT : COLOR_TEXT_DIM;
        g->fillRoundRect(CX - 180, y + 6, 360, AL_ROW - 12, 24, 0x18E3);
        ui_print(CX - 150, y + 18, 3, c, hm);
        const char *ap = clock_ampm(a.min / 60);
        if (ap[0]) ui_print(CX - 150 + ui_text_width(hm, 3) + 6, y + 30, 1, c, ap);
        ui_print(CX - 150, y + 58, 1, COLOR_TEXT_DIM, d);
        toggle_switch(CX + 90, y + 28, a.on);
    }
    if (s_alarm_n == 0) ui_text_center(CX, AL_TOP + AL_ROW + 40, COLOR_TEXT_DIM, "No alarms yet", 1);
    clear_header_band();
}

static void alarms_touch(int x, int y, bool pressed) {
    if (!ui_scroll_touch(&s_al_scroll, y, pressed)) return;
    int row = (y - AL_TOP + ui_scroll_offset(&s_al_scroll)) / AL_ROW;
    if (y < AL_TOP || row < 0) return;
    if (row == s_alarm_n && s_alarm_n < MAX_ALARMS) {
        s_edit = MAX_ALARMS;
        s_edit_val = {7 * 60, 0x3E, 1};
        ui_push(&alarm_edit_screen);
        return;
    }
    if (row >= s_alarm_n) return;
    if (x > CX + 70) {   // the switch
        s_alarms[row].on = !s_alarms[row].on;
        save_alarms();
        return;
    }
    s_edit = row;
    s_edit_val = s_alarms[row];
    ui_push(&alarm_edit_screen);
}

static void alarms_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen alarms_screen = {
    "Alarms", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    alarms_create, alarms_draw, alarms_touch, nullptr, nullptr, alarms_gesture,
    250,
};

// ---- editing one --------------------------------------------------------------------------------
static Taps s_ed_taps;
static const int ED_Y = 186;   // the digits

static void edit_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    int h = s_edit_val.min / 60, m = s_edit_val.min % 60;
    char hb[4], mb[4];
    int hh = clock_24h() ? h : (h % 12 == 0 ? 12 : h % 12);
    snprintf(hb, sizeof(hb), clock_24h() ? "%02d" : "%d", hh);
    snprintf(mb, sizeof(mb), "%02d", m);
    // hour and minute, each with an arrow up and down
    const int HX = CX - 80, MX = CX + 80;
    for (int c = 0; c < 2; c++) {
        int x = c ? MX : HX;
        uint16_t up = s_ed_taps.held(c * 2) ? CLOCK_ORANGE : COLOR_TEXT_DIM, dn = s_ed_taps.held(c * 2 + 1) ? CLOCK_ORANGE : COLOR_TEXT_DIM;
        g->fillTriangle(x - 20, ED_Y - 58, x + 20, ED_Y - 58, x, ED_Y - 82, up);
        g->fillTriangle(x - 20, ED_Y + 58, x + 20, ED_Y + 58, x, ED_Y + 82, dn);
        ui_text_center(x, ED_Y, COLOR_TEXT, c ? mb : hb, 5);
    }
    ui_text_center(CX, ED_Y, COLOR_TEXT, ":", 5);
    const char *ap = clock_ampm(h);
    if (ap[0]) {
        g->fillRoundRect(CX + 160, ED_Y - 18, 52, 36, 18, s_ed_taps.held(4) ? CLOCK_ORANGE : COLOR_PANEL);
        ui_text_center(CX + 186, ED_Y, COLOR_TEXT, ap, 1);
    }
    // weekdays, Monday first
    static const char *const L[7] = {"M", "T", "W", "T", "F", "S", "S"};
    for (int i = 0; i < 7; i++) {
        int d = (i + 1) % 7, x = CX - 156 + i * 52, y = 300;
        bool on = s_edit_val.days & (1 << d);
        g->fillCircle(x, y, 21, on ? CLOCK_ORANGE : COLOR_PANEL);
        ui_text_center(x, y, on ? COLOR_BG : COLOR_TEXT, L[i], 2);
    }
    char d[40];
    days_text(d, sizeof(d), s_edit_val.days);
    ui_text_center(CX, 336, COLOR_TEXT_DIM, d, 1);
    bool existing = s_edit < MAX_ALARMS;
    if (existing) pill({CX - 150, 360, 140, 56}, COLOR_PANEL, COLOR_BAD, "Delete", s_ed_taps.held(5));
    pill(existing ? Btn{CX + 10, 360, 140, 56} : Btn{CX - 80, 360, 160, 56}, CLOCK_ORANGE, COLOR_BG, "Save", s_ed_taps.held(6));
}

static void edit_touch(int x, int y, bool pressed) {
    const int HX = CX - 80, MX = CX + 80;
    int id = -1;
    for (int c = 0; c < 2; c++) {
        int cx = c ? MX : HX;
        if (abs(x - cx) < 50 && y > ED_Y - 100 && y < ED_Y - 30) id = c * 2;
        if (abs(x - cx) < 50 && y > ED_Y + 30 && y < ED_Y + 100) id = c * 2 + 1;
    }
    if (!clock_24h() && Btn{CX + 160, ED_Y - 18, 52, 36}.hit(x, y)) id = 4;
    bool existing = s_edit < MAX_ALARMS;
    if (existing && Btn{CX - 150, 360, 140, 56}.hit(x, y)) id = 5;
    if ((existing ? Btn{CX + 10, 360, 140, 56} : Btn{CX - 80, 360, 160, 56}).hit(x, y)) id = 6;
    for (int i = 0; i < 7; i++) {
        int cx = CX - 156 + i * 52;
        if ((x - cx) * (x - cx) + (y - 300) * (y - 300) < 25 * 25) id = 10 + i;
    }
    int t = s_ed_taps.feed(id, pressed);
    if (t < 0) return;
    int h = s_edit_val.min / 60, m = s_edit_val.min % 60;
    switch (t) {
    case 0: h = (h + 1) % 24; break;
    case 1: h = (h + 23) % 24; break;
    case 2: m = (m + 5) / 5 * 5 % 60; break;
    case 3: m = m % 5 ? m / 5 * 5 : (m + 55) % 60; break;
    case 4: h = (h + 12) % 24; break;
    case 5:
        memmove(&s_alarms[s_edit], &s_alarms[s_edit + 1], sizeof(Alarm) * (s_alarm_n - s_edit - 1));
        s_alarm_n--;
        save_alarms();
        ui_pop_screen();
        return;
    case 6:
        s_edit_val.on = 1;
        if (s_edit >= MAX_ALARMS) { if (s_alarm_n < MAX_ALARMS) s_alarms[s_alarm_n++] = s_edit_val; }
        else s_alarms[s_edit] = s_edit_val;
        save_alarms();
        s_last_checked_min = -1;
        ui_pop_screen();
        return;
    default:
        if (t >= 10) s_edit_val.days ^= 1 << ((t - 10 + 1) % 7);
        return;
    }
    s_edit_val.min = h * 60 + m;
}

static void edit_destroy() {
    alarms_create();   // the list's length may have changed
}

Screen alarm_edit_screen = {
    "Alarm", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    nullptr, edit_draw, edit_touch, nullptr, edit_destroy, nullptr,
    250,
};

// ---- ringing -------------------------------------------------------------------------------------
static uint32_t s_ring_ms = 0, s_ring_beep = 0;
static Taps s_rg_taps;

static void rg_create() { s_ring_ms = s_ring_beep = millis(); s_rg_taps.down = -1; }

static void snooze() {
    s_snooze_alarm = s_ringing;
    s_snooze_at = millis() + 9 * 60000UL;
    if (!s_snooze_at) s_snooze_at = 1;
    s_ringing = -1;
    ui_pop_screen();
}

static void stop() {
    s_ringing = -1;
    s_snooze_at = 0;
    ui_pop_screen();
}

static void rg_tick() {
    uint32_t now = millis();
    sleep_register_activity();   // stays on while it rings
    if (now - s_ring_ms > 120000) { snooze(); return; }   // nobody answered: again in 9 minutes
    if (now - s_ring_beep >= 900) {
        s_ring_beep = now;
        audio_play_sfx(1175, 110);
        audio_play_sfx(1568, 110);
        audio_play_sfx(1175, 110);
    }
}

static void rg_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    WatchTime t = rtc_now();
    char hm[8];
    clock_hm(hm, sizeof(hm), t.hour, t.minute);
    float pu = 0.5f + 0.5f * sinf((millis() - s_ring_ms) * 0.006f);
    g->fillCircle(CX, 150, 84 + (int)(pu * 10), ui_dim(CLOCK_ORANGE, 0.25f));
    ui_text_center(CX, 150, COLOR_TEXT, hm, 5);
    ui_text_center(CX, 214, COLOR_TEXT_DIM, s_snooze_alarm >= 0 && s_ringing == s_snooze_alarm ? "Alarm (snoozed)" : "Alarm", 2);
    pill({CX - 160, 262, 320, 74}, CLOCK_ORANGE, COLOR_BG, "Snooze", s_rg_taps.held(1));
    pill({CX - 90, 352, 180, 56}, COLOR_PANEL, COLOR_TEXT, "Stop", s_rg_taps.held(2));
}

static void rg_touch(int x, int y, bool pressed) {
    int id = Btn{CX - 160, 262, 320, 74}.hit(x, y) ? 1 : Btn{CX - 90, 352, 180, 56}.hit(x, y) ? 2 : -1;
    int t = s_rg_taps.feed(id, pressed);
    if (t == 1) snooze();
    else if (t == 2) stop();
}

Screen alarm_ring_screen = {
    nullptr, GESTURE_MODE_NONE, UI_FRAME_MS_SMOOTH,
    rg_create, rg_draw, rg_touch, rg_tick, nullptr, nullptr,
    0, true,
    false, false,
    true,   // no_drag_back: answer it
};

// =========================================================================
//  World clock
// =========================================================================
enum TzRule : uint8_t { TZR_NONE, TZR_EU, TZR_US, TZR_AU, TZR_NZ };
struct City { const char *name; int16_t std_off; TzRule dst; };
static const City CITIES[] = {
    {"Honolulu", -600, TZR_NONE},   {"Los Angeles", -480, TZR_US},  {"Denver", -420, TZR_US},
    {"Mexico City", -360, TZR_NONE}, {"Chicago", -360, TZR_US},      {"New York", -300, TZR_US},
    {"Toronto", -300, TZR_US},      {"Sao Paulo", -180, TZR_NONE},  {"Buenos Aires", -180, TZR_NONE},
    {"London", 0, TZR_EU},          {"Lisbon", 0, TZR_EU},          {"Lagos", 60, TZR_NONE},
    {"Paris", 60, TZR_EU},          {"Rome", 60, TZR_EU},           {"Berlin", 60, TZR_EU},
    {"Madrid", 60, TZR_EU},         {"Athens", 120, TZR_EU},        {"Cairo", 120, TZR_NONE},
    {"Johannesburg", 120, TZR_NONE}, {"Istanbul", 180, TZR_NONE},   {"Moscow", 180, TZR_NONE},
    {"Dubai", 240, TZR_NONE},       {"Mumbai", 330, TZR_NONE},      {"Bangkok", 420, TZR_NONE},
    {"Singapore", 480, TZR_NONE},   {"Hong Kong", 480, TZR_NONE},   {"Beijing", 480, TZR_NONE},
    {"Tokyo", 540, TZR_NONE},       {"Sydney", 600, TZR_AU},        {"Auckland", 720, TZR_NZ},
};
static const int CITY_N = sizeof(CITIES) / sizeof(CITIES[0]);

static void load_cities_default() {
    s_city_mask = 0;
    for (int i = 0; i < CITY_N; i++) {
        const char *n = CITIES[i].name;
        if (!strcmp(n, "New York") || !strcmp(n, "London") || !strcmp(n, "Tokyo") || !strcmp(n, "Sydney"))
            s_city_mask |= 1u << i;
    }
}

static void save_cities() {
    Preferences p;
    if (!p.begin("clock", false)) return;
    p.putUInt("cities", s_city_mask);
    p.end();
}

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
static int32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static void civil_from_days(int32_t z, int &y, int &m, int &d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}
// The n-th Sunday of a month (n = -1: the last), as days since 1970.
static int32_t sunday(int y, int m, int n) {
    if (n > 0) {
        int32_t first = days_from_civil(y, m, 1);
        int wd = (first + 4) % 7;   // 0 = Sunday
        return first + (7 - wd) % 7 + (n - 1) * 7;
    }
    int32_t last = (m == 12 ? days_from_civil(y + 1, 1, 1) : days_from_civil(y, m + 1, 1)) - 1;
    int wd = (last + 4) % 7;
    return last - wd;
}

// Minutes east of UTC in that city at this UTC moment.
static int city_offset(const City &c, int64_t utc) {
    int64_t std_local = utc + c.std_off * 60;
    int y, m, d;
    civil_from_days((int32_t)(std_local / 86400), y, m, d);
    int64_t start = 0, end = 0;   // both in standard local seconds
    switch (c.dst) {
    case TZR_NONE: return c.std_off;
    case TZR_EU:   // 01:00 UTC on the last Sundays of March and October
        start = (int64_t)sunday(y, 3, -1) * 86400 + 3600 + c.std_off * 60;
        end = (int64_t)sunday(y, 10, -1) * 86400 + 3600 + c.std_off * 60;
        return std_local >= start && std_local < end ? c.std_off + 60 : c.std_off;
    case TZR_US:   // 2:00 on the 2nd Sunday of March, 1:00 standard on the 1st of November
        start = (int64_t)sunday(y, 3, 2) * 86400 + 2 * 3600;
        end = (int64_t)sunday(y, 11, 1) * 86400 + 3600;
        return std_local >= start && std_local < end ? c.std_off + 60 : c.std_off;
    case TZR_AU:   // southern: from the 1st Sunday of October to the 1st of April (2:00 standard)
        start = (int64_t)sunday(y, 10, 1) * 86400 + 2 * 3600;
        end = (int64_t)sunday(y, 4, 1) * 86400 + 2 * 3600;
        return std_local >= start || std_local < end ? c.std_off + 60 : c.std_off;
    case TZR_NZ:   // from the last Sunday of September to the 1st of April
        start = (int64_t)sunday(y, 9, -1) * 86400 + 2 * 3600;
        end = (int64_t)sunday(y, 4, 1) * 86400 + 2 * 3600;
        return std_local >= start || std_local < end ? c.std_off + 60 : c.std_off;
    }
    return c.std_off;
}

static UiScroll s_wc_scroll;
static bool s_wc_edit = false;
static const int WC_ROW = 84, WC_TOP = 92;

static int wc_rows() {
    if (s_wc_edit) return CITY_N;
    int n = 0;
    for (int i = 0; i < CITY_N; i++) n += (s_city_mask >> i) & 1;
    return n + 1;   // + "Edit"
}

static void wc_reset() { ui_scroll_reset(&s_wc_scroll, wc_rows() * WC_ROW - (LCD_HEIGHT - WC_TOP - 40), 0); }
static void wc_create() { s_wc_edit = false; wc_reset(); }

static void wc_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_scroll_tick(&s_wc_scroll);
    g->fillScreen(COLOR_BG);
    int off = ui_scroll_offset(&s_wc_scroll);
    if (s_utc_off == -1440 && !s_wc_edit) {
        ui_text_center(CX, CY - 20, COLOR_TEXT, "Connect the phone once", 2);
        ui_text_center(CX, CY + 16, COLOR_TEXT_DIM, "It tells the watch its time zone", 1);
        clear_header_band();
        return;
    }
    int64_t local = calendar_now();
    int64_t utc = local - (int64_t)s_utc_off * 60;
    int row = 0;
    for (int i = 0; i < CITY_N; i++) {
        bool shown = (s_city_mask >> i) & 1;
        if (!s_wc_edit && !shown) continue;
        int y = WC_TOP + row * WC_ROW - off;
        row++;
        if (y > LCD_HEIGHT || y + WC_ROW < 0) continue;
        const City &c = CITIES[i];
        if (s_wc_edit) {
            g->fillCircle(CX - 150, y + WC_ROW / 2, 16, shown ? CLOCK_ORANGE : COLOR_PANEL);
            if (shown) ui_text_center(CX - 150, y + WC_ROW / 2, COLOR_BG, "v", 1);
            ui_print(CX - 120, y + WC_ROW / 2 - 10, 2, COLOR_TEXT, c.name);
            continue;
        }
        int co = city_offset(c, utc);
        int64_t there = utc + (int64_t)co * 60;
        int dday = (int)(there / 86400 - local / 86400);
        int sod = (int)(there % 86400);
        int hh = sod / 3600, mm = sod / 60 % 60;
        char hm[8], rel[40], diff[16];
        clock_hm(hm, sizeof(hm), hh, mm);
        int dm = co - s_utc_off;
        if (dm == 0) snprintf(diff, sizeof(diff), "same time");
        else if (dm % 60) snprintf(diff, sizeof(diff), "%+d:%02dh", dm / 60, abs(dm % 60));
        else snprintf(diff, sizeof(diff), "%+dh", dm / 60);
        snprintf(rel, sizeof(rel), "%s, %s", dday > 0 ? "Tomorrow" : dday < 0 ? "Yesterday" : "Today", diff);
        bool day = hh >= 7 && hh < 19;
        g->fillCircle(CX - 160, y + 30, 6, day ? CLOCK_ORANGE : COLOR_ACCENT2);
        ui_print(CX - 144, y + 16, 2, COLOR_TEXT, c.name);
        ui_print(CX - 144, y + 48, 1, COLOR_TEXT_DIM, rel);
        int tw = ui_text_width(hm, 3);
        const char *ap = clock_ampm(hh);
        int apw = ap[0] ? ui_text_width(ap, 1) + 4 : 0;
        ui_print(CX + 168 - tw - apw, y + 18, 3, COLOR_TEXT, hm);
        if (ap[0]) ui_print(CX + 168 - apw + 4, y + 30, 1, COLOR_TEXT, ap);
    }
    if (!s_wc_edit) {
        int y = WC_TOP + row * WC_ROW - off;
        g->fillRoundRect(CX - 80, y + 14, 160, 52, 26, 0x3186);
        ui_text_center(CX, y + 40, CLOCK_ORANGE, "Edit", 2);
    } else {
        ui_text_center(CX, LCD_HEIGHT - 28, COLOR_TEXT_DIM, "Swipe right when done", 1);
    }
    clear_header_band();
}

static void wc_touch(int x, int y, bool pressed) {
    if (!ui_scroll_touch(&s_wc_scroll, y, pressed)) return;
    if (y < WC_TOP) return;
    int row = (y - WC_TOP + ui_scroll_offset(&s_wc_scroll)) / WC_ROW;
    if (s_wc_edit) {
        if (row < 0 || row >= CITY_N) return;
        s_city_mask ^= 1u << row;
        save_cities();
        return;
    }
    if (row == wc_rows() - 1) { s_wc_edit = true; wc_reset(); }
}

static void wc_gesture(Gesture g) {
    if (s_wc_edit && g == GESTURE_SWIPE_RIGHT) { s_wc_edit = false; wc_reset(); return; }
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

static void wc_destroy() { s_wc_edit = false; }

Screen worldclock_screen = {
    "World clock", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    wc_create, wc_draw, wc_touch, nullptr, wc_destroy, wc_gesture,
    1000,
    false, false, false,
    true,   // no_drag_back: swipe right ends editing first
};
