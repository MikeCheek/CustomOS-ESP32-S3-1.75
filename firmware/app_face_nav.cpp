// The Smart Stack and the face switcher - see app_face_nav.h.
#include "app_face_nav.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "watchface_registry.h"
#include "app_menu.h"
#include "app_battery.h"
#include "app_assistant.h"
#include "app_calendar.h"
#include "app_navigation.h"
#include "app_notifications.h"
#include "complications.h"
#include "home_gestures.h"
#include "app_pet.h"
#include "app_clock.h"
#include "app_weather.h"
#include "app_reminders.h"
#include "app_workout.h"
#include "app_life.h"
#include "fitness.h"
#include "clock_fmt.h"
#include "phone_link.h"
#include "hal_power.h"
#include "hal_imu.h"
#include "hal_audio.h"
#include "hal_sleep.h"
#include "hal_touch.h"
#include "hal_rtc.h"
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

extern Screen assistant_screen;
extern Screen nowplaying_screen;
extern Screen music_screen;
extern Screen calendar_screen;
extern Screen activity_screen;
extern Screen notifications_screen;
extern Screen battery_screen;
extern Screen settings_comp_screen;
extern Screen photo_watchface_screen;

bool face_nav_is_face(const Screen *s) {
    if (!s) return false;
    for (int i = 0; i < watchface_count(); i++) {
        const WatchfaceEntry *e = watchface_get(i);
        if (e && e->screen == s) return true;
    }
    return false;
}

// Text cut to fit w pixels, with "..." (whole UTF-8 letters).
static void fit_text(char *out, int n, const char *in, int size, int w) {
    strlcpy(out, in ? in : "", n);
    if (ui_text_width(out, size) <= w) return;
    int len = strlen(out);
    while (len > 1) {
        out[--len] = 0;
        while (len > 0 && (out[len - 1] & 0xC0) == 0x80) out[--len] = 0;
        char t[96];
        snprintf(t, sizeof(t), "%s...", out);
        if (ui_text_width(t, size) <= w) { strlcpy(out, t, n); return; }
    }
}

// =========================================================================
//  The Smart Stack
// =========================================================================

// CARD_TODAY comes after CARD_BATTERY: the tap counts kept in NVS stay as
// they were (it isn't learned - it's about the whole day, not a habit).
enum CardKind { CARD_WORKOUT, CARD_STOPWATCH, CARD_ALARM, CARD_REMINDERS, CARD_TIMER, CARD_NAV, CARD_MUSIC, CARD_EVENT, CARD_WEATHER, CARD_ACTIVITY, CARD_NOTIFS, CARD_LIFE, CARD_BATTERY, CARD_TODAY, CARD_RECAP };
struct Card {
    CardKind kind;
    Screen *open;          // tapping the card opens this (nullptr: nothing)
    uint16_t color;
    char title[24];
    char main[64];
    char sub[64];
    float progress;        // < 0: no bar
};

static Card s_cards[16];
static int  s_card_n = 0;
static float s_scroll = 0, s_scroll_vel = 0;
static bool  s_st_drag = false, s_st_moved = false;
static int   s_st_y0 = 0, s_st_last_y = 0;
static float s_st_scroll0 = 0;
static uint32_t s_st_built = 0;
static const int CARD_H = 124, CARD_STEP = 138, CARD_W = 372;

static Card &add(CardKind k, Screen *open, uint16_t color, const char *title) {
    Card &c = s_cards[s_card_n++];
    memset(&c, 0, sizeof(c));
    c.kind = k;
    c.open = open;
    c.color = color;
    c.progress = -1;
    strlcpy(c.title, title, sizeof(c.title));
    return c;
}

// ---- the order: what matters now ---------------------------------------------------------------
// Each card gets a score: running things first (a workout, a timer,
// directions, music), then what this time of day is about (weather and the
// day's events in the morning, the rings in the evening, the alarm at
// night, a low battery whenever), plus what you tend to open at this time
// of day - taps are counted per card and quarter of the day, kept in NVS.
static uint8_t s_taps[4][CARD_BATTERY + 1];
static bool s_taps_loaded = false;

static void load_taps() {
    if (s_taps_loaded) return;
    s_taps_loaded = true;
    Preferences p;
    if (!p.begin("stack", true)) return;
    p.getBytes("taps", s_taps, sizeof(s_taps));
    p.end();
}

static int quarter_now() {
    WatchTime t = rtc_now();
    return t.valid ? t.hour / 6 : 2;
}

static void count_tap(CardKind k) {
    if (k > CARD_BATTERY) return;
    uint8_t *row = s_taps[quarter_now()];
    if (row[k] == 255)
        for (int i = 0; i <= CARD_BATTERY; i++) row[i] /= 2;   // keeps learning, recent habits weigh more
    row[k]++;
    Preferences p;
    if (!p.begin("stack", false)) return;
    p.putBytes("taps", s_taps, sizeof(s_taps));
    p.end();
}

static int card_score(const Card &c) {
    WatchTime t = rtc_now();
    int h = t.valid ? t.hour : 12;
    bool morning = h >= 6 && h < 10, evening = h >= 17 && h < 22, night = h >= 21 || h < 5;
    int s = 0;
    switch (c.kind) {
    case CARD_WORKOUT:   s = 200; break;
    case CARD_TIMER:     s = 190; break;
    case CARD_NAV:       s = 185; break;
    case CARD_STOPWATCH: s = 170; break;
    case CARD_MUSIC:     s = 150; break;
    case CARD_EVENT: {
        // soon: high; later today: middling, more so in the morning
        bool soon = strstr(c.sub, "in ") != nullptr || strncmp(c.sub, "Now", 3) == 0;
        s = soon ? 160 : 60 + (morning ? 40 : 0);
        break;
    }
    case CARD_ALARM:     s = 50 + (night ? 90 : 0); break;
    case CARD_WEATHER:   s = 70 + (morning ? 60 : 0) + (strstr(c.sub, "Rain") || strstr(c.sub, "Storm") || strstr(c.sub, "Snow") ? 25 : 0); break;
    case CARD_ACTIVITY: {
        float mv = fitness_move_kcal() / fitness_move_goal();
        s = 60 + (evening ? 50 : 0) + (mv >= 0.7f && mv < 1.0f ? 30 : 0);   // nearly there: a nudge
        break;
    }
    case CARD_LIFE:      s = 58 + (h >= 7 && h < 10 ? 20 : 0) + (strstr(c.sub, " min") ? 25 : 0); break;
    case CARD_REMINDERS: s = 55 + (h >= 8 && h < 12 ? 25 : 0); break;
    case CARD_NOTIFS:    s = 50 + (int)(notifications_unread() > 6 ? 30 : notifications_unread() * 5); break;
    case CARD_BATTERY: {
        int p = power_get_battery_percent();
        s = 10 + (p >= 0 && p <= 20 && !power_is_charging() ? 140 : 0) + (power_is_charging() ? 60 : 0);
        break;
    }
    }
    if (c.kind == CARD_TODAY) return 60 + (morning ? 85 : 0) + (evening ? 0 : 10);
    if (c.kind == CARD_RECAP) return 75 + (evening ? 70 : 0);
    int taps = s_taps[quarter_now()][c.kind];
    int learned = 0;
    while (taps > 0 && learned < 40) { learned += 8; taps >>= 1; }   // ~8 x log2(taps)
    return s + learned;
}

static void sort_cards() {
    int sc[16];
    for (int i = 0; i < s_card_n; i++) sc[i] = card_score(s_cards[i]);
    for (int i = 1; i < s_card_n; i++)   // insertion sort: stable, a handful of cards
        for (int j = i; j > 0 && sc[j] > sc[j - 1]; j--) {
            Card t = s_cards[j]; s_cards[j] = s_cards[j - 1]; s_cards[j - 1] = t;
            int x = sc[j]; sc[j] = sc[j - 1]; sc[j - 1] = x;
        }
}

// What matters now first: things running, then soon, then the usual.
static void build_cards() {
    s_card_n = 0;
    s_st_built = millis();
    uint32_t now = calendar_now();
    CalEvent ev;
    bool has_ev = calendar_next(ev);
    bool ev_soon = has_ev && !ev.all_day && ev.start <= now + 3600;

    if (workout_active()) {
        Card &c = add(CARD_WORKOUT, &workout_live_screen, FIT_EXERCISE_COLOR, "WORKOUT");
        uint32_t s = workout_elapsed_s();
        snprintf(c.main, sizeof(c.main), "%lu:%02lu%s", (unsigned long)(s / 60), (unsigned long)(s % 60), workout_paused() ? " paused" : "");
        float km = workout_km();
        if (km > 0.01f) snprintf(c.sub, sizeof(c.sub), "%s - %.2f km", workout_type_name(), km);
        else strlcpy(c.sub, workout_type_name(), sizeof(c.sub));
    }
    if (assistant_timer_active()) {
        uint32_t left = assistant_timer_left_s();
        Card &c = add(CARD_TIMER, &timer_screen, 0xFC60, "TIMER");
        if (left >= 3600) snprintf(c.main, sizeof(c.main), "%lu:%02lu:%02lu", (unsigned long)(left / 3600), (unsigned long)(left / 60 % 60), (unsigned long)(left % 60));
        else snprintf(c.main, sizeof(c.main), "%lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
        snprintf(c.sub, sizeof(c.sub), assistant_timer_paused() ? "paused" : "left");
        uint32_t len = assistant_timer_length_s();
        if (len) c.progress = 1.0f - (float)assistant_timer_left_ms() / (len * 1000.0f);
    }
    if (stopwatch_running()) {
        Card &c = add(CARD_STOPWATCH, &stopwatch_screen, 0xFC60, "STOPWATCH");
        uint32_t s = stopwatch_ms() / 1000;
        snprintf(c.main, sizeof(c.main), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
        snprintf(c.sub, sizeof(c.sub), "running");
    }
    if (navigation_active()) {
        Card &c = add(CARD_NAV, &navigation_screen, COLOR_GOOD, "DIRECTIONS");
        const char *d = navigation_distance();
        snprintf(c.main, sizeof(c.main), "%s", d[0] ? d : "On route");
        snprintf(c.sub, sizeof(c.sub), "to the next turn");
    }
    const PhoneMedia &m = phone_link_media();
    if (m.valid && m.playing) {
        Card &c = add(CARD_MUSIC, &nowplaying_screen, COLOR_ACCENT3, "NOW PLAYING");
        strlcpy(c.main, m.title, sizeof(c.main));
        strlcpy(c.sub, m.artist, sizeof(c.sub));
        if (m.duration_s > 0) c.progress = (float)phone_link_media_position() / m.duration_s;
    } else if (audio_is_playing()) {
        Card &c = add(CARD_MUSIC, &music_screen, COLOR_ACCENT3, "NOW PLAYING");
        snprintf(c.main, sizeof(c.main), "On the watch");
        snprintf(c.sub, sizeof(c.sub), "Music");
    }
    auto event_card = [&]() {
        Card &c = add(CARD_EVENT, &calendar_screen, ev.color ? ev.color : COLOR_ACCENT2, "UP NEXT");
        strlcpy(c.main, ev.title, sizeof(c.main));
        if (ev.all_day) snprintf(c.sub, sizeof(c.sub), "All day");
        else if (ev.start <= now) snprintf(c.sub, sizeof(c.sub), "Now%s%s", ev.location[0] ? " - " : "", ev.location);
        else {
            char hm[16];
            uint32_t sod = ev.start % 86400;
            clock_hm_text(hm, sizeof(hm), sod / 3600, sod % 3600 / 60);
            uint32_t in = ev.start - now;
            if (in < 3600) snprintf(c.sub, sizeof(c.sub), "%s, in %lu min", hm, (unsigned long)(in / 60 + 1));
            else snprintf(c.sub, sizeof(c.sub), "%s%s%s", hm, ev.location[0] ? " - " : "", ev.location);
        }
    };
    if (ev_soon) event_card();
    uint32_t alarm_in;
    int ah, am;
    if (clock_next_alarm(alarm_in, ah, am) && alarm_in <= 12 * 60) {
        Card &c = add(CARD_ALARM, &alarms_screen, 0xFC60, "ALARM");
        clock_hm_text(c.main, sizeof(c.main), ah, am);
        if (alarm_in < 60) snprintf(c.sub, sizeof(c.sub), "in %lu min", (unsigned long)alarm_in);
        else snprintf(c.sub, sizeof(c.sub), "in %luh %02lum", (unsigned long)(alarm_in / 60), (unsigned long)(alarm_in % 60));
    }
    int t;
    char cond[32];
    if (comp_weather_now(t, cond, sizeof(cond))) {
        Card &c = add(CARD_WEATHER, &weather_screen, COLOR_ACCENT2, "WEATHER");
        snprintf(c.main, sizeof(c.main), "%d\xC2\xB0" "C", t);
        strlcpy(c.sub, cond, sizeof(c.sub));
    }
    {
        Card &c = add(CARD_ACTIVITY, &activity_screen, FIT_MOVE_COLOR, "ACTIVITY");
        snprintf(c.main, sizeof(c.main), "%d/%u kcal", (int)fitness_move_kcal(), (unsigned)fitness_move_goal());
        snprintf(c.sub, sizeof(c.sub), "%lu min exercise - %d h stand - %lu steps", (unsigned long)fitness_exercise_min(),
                 fitness_stand_hours(), (unsigned long)imu_get_step_count());
    }
    {
        char nx[48];
        bool has = life_next(nx, sizeof(nx));
        int sc = life_score();
        if (has || sc > 0) {
            Card &c = add(CARD_LIFE, &life_screen, 0x2CCA, "LIFE");
            snprintf(c.main, sizeof(c.main), "%d%% of today", sc);
            if (has) strlcpy(c.sub, nx, sizeof(c.sub));
            else snprintf(c.sub, sizeof(c.sub), "Nothing else planned");
            c.progress = sc / 100.0f;
        }
    }
    if (reminders_count() > 0) {
        Card &c = add(CARD_REMINDERS, &reminders_screen, COLOR_ACCENT3, "REMINDERS");
        strlcpy(c.main, reminders_first(), sizeof(c.main));
        if (reminders_count() > 1) snprintf(c.sub, sizeof(c.sub), "and %d more", reminders_count() - 1);
        else snprintf(c.sub, sizeof(c.sub), "Tap to tick it off");
    }
    int n = notifications_history_count();
    if (n > 0) {
        Card &c = add(CARD_NOTIFS, &notifications_screen, COLOR_BAD, "NOTIFICATIONS");
        snprintf(c.main, sizeof(c.main), "%d %s", n, n == 1 ? "message" : "messages");
        snprintf(c.sub, sizeof(c.sub), "Tap to read");
    }
    if (has_ev && !ev_soon) event_card();
    {
        // the phone's Today brief (complications.h): tap to hear it
        char head[40], line[120];
        if (comp_brief(head, sizeof(head), line, sizeof(line))) {
            Card &c = add(CARD_TODAY, nullptr, COLOR_ACCENT, "TODAY");
            strlcpy(c.main, head, sizeof(c.main));
            strlcpy(c.sub, line[0] ? line : "Tap to hear your day", sizeof(c.sub));
        }
        // tonight's recap, until the morning: tap to hear it again
        char recap[384];
        if (comp_recap(recap, sizeof(recap))) {
            Card &r = add(CARD_RECAP, nullptr, COLOR_ACCENT3, "EVENING RECAP");
            strlcpy(r.main, recap, sizeof(r.main));
            ui_fit(r.main, 2, CARD_W - 120);
            strlcpy(r.sub, "Tap to hear it", sizeof(r.sub));
        }
    }
    {
        int p = power_get_battery_percent();
        Card &c = add(CARD_BATTERY, &battery_screen, p >= 0 && p <= 20 ? COLOR_BAD : COLOR_GOOD, "BATTERY");
        if (p < 0) snprintf(c.main, sizeof(c.main), "--");
        else snprintf(c.main, sizeof(c.main), "%d%%", p);
        int pp = phone_link_battery();
        char left[32];
        if (power_is_charging()) snprintf(c.sub, sizeof(c.sub), "Charging");
        else if (battery_left_text(left, sizeof(left)))
            snprintf(c.sub, sizeof(c.sub), pp >= 0 ? "%s - phone %d%%" : "%s", left, pp);
        else if (pp >= 0) snprintf(c.sub, sizeof(c.sub), "Phone %d%%", pp);
        else snprintf(c.sub, sizeof(c.sub), "Watch");
        c.progress = p < 0 ? -1 : p / 100.0f;
    }
    sort_cards();
}

static void stack_create() {
    load_taps();
    build_cards();
    s_scroll = s_scroll_vel = 0;
    s_st_drag = false;
}

static int card_y(int i) { return 92 + i * CARD_STEP - (int)s_scroll; }   // top of card i

static void draw_card_icon(const Card &c, int cx, int cy) {
    if (c.kind == CARD_ACTIVITY) { fitness_draw_rings(cx, cy, 24, 6); return; }
    if (c.kind == CARD_RECAP) {   // a crescent moon
        Arduino_GFX *g = ui_gfx();
        g->fillCircle(cx, cy, 24, ui_dim(COLOR_ACCENT3, 0.45f));
        g->fillCircle(cx - 3, cy, 13, COLOR_TEXT);
        g->fillCircle(cx + 4, cy - 4, 11, ui_dim(COLOR_ACCENT3, 0.45f));
        return;
    }
    if (c.kind == CARD_TODAY) {   // a sparkle: the AI's
        Arduino_GFX *g = ui_gfx();
        g->fillCircle(cx, cy, 24, ui_dim(COLOR_ACCENT, 0.45f));
        g->fillTriangle(cx, cy - 15, cx - 5, cy, cx + 5, cy, COLOR_TEXT);
        g->fillTriangle(cx, cy + 15, cx - 5, cy, cx + 5, cy, COLOR_TEXT);
        g->fillTriangle(cx - 15, cy, cx, cy - 5, cx, cy + 5, COLOR_TEXT);
        g->fillTriangle(cx + 15, cy, cx, cy - 5, cx, cy + 5, COLOR_TEXT);
        g->fillCircle(cx + 11, cy - 11, 3, COLOR_TEXT);
        return;
    }
    if (c.kind == CARD_WEATHER) {
        int t; char cond[32];
        ui_gfx()->fillCircle(cx, cy, 24, 0x2A9F);
        if (comp_weather_now(t, cond, sizeof(cond))) weather_draw_icon(cx, cy, 16, cond);
        return;
    }
    if (c.open) { menu_draw_app_icon(c.kind == CARD_MUSIC ? &music_screen : c.kind == CARD_WORKOUT ? &workout_screen : c.open, cx, cy, 24); return; }
    // weather: a sun on a disc, like the app icons
    Arduino_GFX *g = ui_gfx();
    g->fillCircle(cx, cy, 24, COLOR_ACCENT2);
    g->fillCircle(cx, cy, 9, COLOR_WARN);
    for (int a = 0; a < 8; a++) {
        float r = a * 0.785398f;
        g->drawLine(cx + (int)(13 * cosf(r)), cy + (int)(13 * sinf(r)), cx + (int)(17 * cosf(r)), cy + (int)(17 * sinf(r)),
                    COLOR_WARN);
    }
}

static void stack_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (millis() - s_st_built > 1000 && !s_st_drag) build_cards();   // live: timer, steps, song
    g->fillScreen(COLOR_BG);
    const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2, R = LCD_WIDTH / 2;
    for (int i = 0; i < s_card_n; i++) {
        int y = card_y(i);
        if (y > LCD_HEIGHT || y + CARD_H < 0) continue;
        const Card &c = s_cards[i];
        // narrower where the round screen is: the widest that fits this row
        int dy = abs(y + CARD_H / 2 - CY) + CARD_H / 2;
        int w = CARD_W;
        if (dy < R) {
            int chord = 2 * (int)sqrtf((float)R * R - (float)dy * dy) - 24;
            if (chord < w) w = chord;
        } else {
            w = 200;
        }
        if (w < 200) w = 200;
        int x = CX - w / 2;
        g->fillRoundRect(x, y, w, CARD_H, 30, 0x18E3);
        draw_card_icon(c, x + 44, y + 40);
        ui_print(x + 80, y + 20, 1, c.color, c.title);
        char t[80];
        fit_text(t, sizeof(t), c.main, 2, w - 104);
        ui_print(x + 80, y + 40, 2, COLOR_TEXT, t);
        fit_text(t, sizeof(t), c.sub, 1, w - 104);
        ui_print(x + 80, y + 78, 1, COLOR_TEXT_DIM, t);
        if (c.progress >= 0) {
            int bw = w - 104, bx = x + 80, by = y + CARD_H - 18;
            float p = c.progress > 1 ? 1 : c.progress;
            g->fillRoundRect(bx, by, bw, 6, 3, COLOR_PANEL);
            if (p > 0.01f) g->fillRoundRect(bx, by, (int)(bw * p) < 6 ? 6 : (int)(bw * p), 6, 3, c.color);
        }
    }
}

static float stack_max() { return s_card_n > 1 ? (float)(s_card_n - 1) * CARD_STEP - 120 : 0; }

static void stack_touch(int x, int y, bool pressed) {
    if (pressed) {
        if (!s_st_drag) {
            s_st_drag = true;
            s_st_moved = fabsf(s_scroll_vel) > 0.5f;
            s_scroll_vel = 0;
            s_st_y0 = s_st_last_y = y;
            s_st_scroll0 = s_scroll;
            return;
        }
        if (abs(y - s_st_y0) > 8) s_st_moved = true;
        float ns = s_st_scroll0 - (y - s_st_y0);
        float mx = stack_max() < 0 ? 0 : stack_max();
        if (ns < 0) ns *= 0.4f;   // rubber band
        else if (ns > mx) ns = mx + (ns - mx) * 0.4f;
        s_scroll = ns;
        s_scroll_vel = -(float)(y - s_st_last_y);
        s_st_last_y = y;
        return;
    }
    if (!s_st_drag) return;
    s_st_drag = false;
    if (s_st_moved) {
        touch_cancel_swipes();
        if (s_scroll < -70) { s_scroll_vel = 0; ui_pop_screen(); }   // pulled down past the top: back to the face
        return;
    }
    for (int i = 0; i < s_card_n; i++) {
        int cy = card_y(i);
        if (y >= cy && y < cy + CARD_H && (s_cards[i].kind == CARD_TODAY || s_cards[i].kind == CARD_RECAP)) {
            sleep_register_activity();
            bool ok = phone_link_say(s_cards[i].kind == CARD_TODAY ? "brief" : "recap");
            ui_show_toast(ok ? "Saying it on the watch" : "Phone not reachable", 1500);
            return;
        }
        if (y >= cy && y < cy + CARD_H && s_cards[i].open) {
            sleep_register_activity();
            count_tap(s_cards[i].kind);
            ui_push(s_cards[i].open);
            return;
        }
    }
}

static void stack_tick() {
    if (s_st_drag) return;
    float mx = stack_max() < 0 ? 0 : stack_max();
    if (fabsf(s_scroll_vel) > 0.4f) { s_scroll += s_scroll_vel; s_scroll_vel *= 0.9f; }
    else s_scroll_vel = 0;
    if (s_scroll < 0) { s_scroll *= 0.7f; s_scroll_vel *= 0.5f; if (s_scroll > -0.5f) s_scroll = 0; }
    if (s_scroll > mx) { s_scroll = mx + (s_scroll - mx) * 0.7f; s_scroll_vel *= 0.5f; if (s_scroll - mx < 0.5f) s_scroll = mx; }
}

static void stack_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_DOWN && s_scroll <= 1) ui_pop_screen();
}

Screen smart_stack_screen = {
    "", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    stack_create, stack_draw, stack_touch, stack_tick, nullptr, stack_gesture,
    250,
};

// =========================================================================
//  The face switcher
// =========================================================================
//
// Each face as a 256 px preview (a still of it, drawn once), the one worn
// in the middle and its neighbours peeking in at the sides.
static const int PV = 256, PV_GAP = 290;
static uint16_t *s_pv[16];
static int   s_sel = 0;           // the face in the middle
static float s_pos = 0;           // shown position (follows s_sel, or the finger)
static bool  s_sw_drag = false, s_sw_moved = false;
static int   s_sw_x0 = 0;
static float s_sw_pos0 = 0;
static uint32_t s_sw_open_ms = 0;

static void downscale_into(uint16_t *dst, const uint16_t *fb) {
    for (int y = 0; y < PV; y++) {
        int sy = y * LCD_HEIGHT / PV;
        for (int x = 0; x < PV; x++) {
            int dx = x - PV / 2, dy = y - PV / 2;
            dst[y * PV + x] = dx * dx + dy * dy <= (PV / 2) * (PV / 2) ? fb[sy * LCD_WIDTH + x * LCD_WIDTH / PV] : 0;
        }
    }
}

static uint16_t *pv_buf(int i) {
    if (i < 0 || i >= 16) return nullptr;
    if (!s_pv[i]) s_pv[i] = (uint16_t *)heap_caps_malloc(PV * PV * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_pv[i];
}

static bool s_pv_ok[16];

// Draws face i into the frame (it's cleared after) and keeps a small copy.
static void render_preview(int i) {
    const WatchfaceEntry *e = watchface_get(i);
    uint16_t *b = pv_buf(i);
    uint16_t *fb = ui_framebuffer();
    if (!e || !e->screen || !b || !fb) return;
    Screen *s = e->screen;
    bool live = ui_screen_in_stack(s);   // already set up underneath: don't create or destroy it
    if (!live && s->on_create) s->on_create();
    ui_gfx()->fillScreen(COLOR_BG);
    if (s->on_draw) s->on_draw();
    if (!live && s->on_destroy) s->on_destroy();
    downscale_into(b, fb);
    s_pv_ok[i] = true;
}

static void blit_preview(uint16_t *fb, const uint16_t *pv, int cx, int cy) {
    for (int y = 0; y < PV; y++) {
        int sy = cy - PV / 2 + y;
        if (sy < 0 || sy >= LCD_HEIGHT) continue;
        for (int x = 0; x < PV; x++) {
            int sx = cx - PV / 2 + x;
            if (sx < 0 || sx >= LCD_WIDTH) continue;
            int dx = x - PV / 2, dy = y - PV / 2;
            if (dx * dx + dy * dy > (PV / 2) * (PV / 2)) continue;
            fb[sy * LCD_WIDTH + sx] = pv[y * PV + x];
        }
    }
}

static bool editable(int i) {
    const WatchfaceEntry *e = watchface_get(i);
    return e && e->screen != &photo_watchface_screen;   // photos are set up in the phone app
}

static void switch_create() {
    s_sel = watchface_current();
    s_pos = (float)s_sel;
    s_sw_drag = false;
    s_sw_open_ms = millis();
    // the face being worn was captured as the press opened this (face_nav_poll)
    for (int i = 0; i < 16; i++)
        if (i != s_sel) s_pv_ok[i] = false;
}

static void switch_draw() {
    Arduino_GFX *g = ui_gfx();
    uint16_t *fb = ui_framebuffer();
    if (!g || !fb) return;
    int n = watchface_count();
    // the previews in view that aren't drawn yet
    for (int i = (int)floorf(s_pos) - 1; i <= (int)ceilf(s_pos) + 1; i++)
        if (i >= 0 && i < n && i < 16 && !s_pv_ok[i]) render_preview(i);

    g->fillScreen(COLOR_BG);
    // opening: the face shrinks from full screen to its preview (drawn as a ring
    // closing in, since the still is small)
    float t = (millis() - s_sw_open_ms) / 180.0f;
    if (t > 1) t = 1;
    const int CX = LCD_WIDTH / 2, CY = 206;
    for (int i = 0; i < n && i < 16; i++) {
        float off = (i - s_pos) * PV_GAP;
        if (fabsf(off) > LCD_WIDTH) continue;
        int cx = CX + (int)off;
        if (s_pv_ok[i]) blit_preview(fb, s_pv[i], cx, CY);
        if (i == s_sel && !s_sw_drag) {
            int rr = PV / 2 + 4 + (int)((1 - t) * 90);
            ui_arc(cx, CY, rr + 3, 3, 0, 360, COLOR_TEXT_DIM, false);
        }
    }
    const WatchfaceEntry *e = watchface_get((int)lroundf(s_pos));
    if (e) ui_draw_centered_text(CY + PV / 2 + 18, COLOR_TEXT, e->name, 2);
    int near = (int)lroundf(s_pos);
    if (editable(near)) {
        int bw = 120, bh = 40, bx = CX - bw / 2, by = CY + PV / 2 + 56;
        g->fillRoundRect(bx, by, bw, bh, 20, 0x3186);
        ui_text_center(CX, by + bh / 2, COLOR_TEXT, "Edit", 2);
    }
    // dots: which of how many
    int dw = n * 14;
    for (int i = 0; i < n; i++)
        g->fillCircle(CX - dw / 2 + 7 + i * 14, 28 + 12, 3, i == near ? COLOR_TEXT : COLOR_TEXT_DIM);
}

static void wear(int i) {
    const WatchfaceEntry *e = watchface_get(i);
    if (!e) return;
    watchface_set_current(i);
    ui_go_home();
    if (e->screen && !ui_screen_on_top(e->screen)) ui_push_instant(e->screen);
}

static void switch_touch(int x, int y, bool pressed) {
    int n = watchface_count();
    if (pressed) {
        if (!s_sw_drag) { s_sw_drag = true; s_sw_moved = false; s_sw_x0 = x; s_sw_pos0 = s_pos; return; }
        if (abs(x - s_sw_x0) > 10) s_sw_moved = true;
        float p = s_sw_pos0 - (float)(x - s_sw_x0) / PV_GAP;
        if (p < 0) p *= 0.35f;
        if (p > n - 1) p = (n - 1) + (p - (n - 1)) * 0.35f;
        s_pos = p;
        return;
    }
    if (!s_sw_drag) return;
    s_sw_drag = false;
    if (s_sw_moved) {
        touch_cancel_swipes();
        int to = (int)lroundf(s_pos + (s_pos - s_sw_pos0) * 0.4f);   // a flick goes on to the next
        if (to < 0) to = 0;
        if (to > n - 1) to = n - 1;
        s_sel = to;
        return;
    }
    const int CX = LCD_WIDTH / 2, CY = 206;
    int by = CY + PV / 2 + 56;
    if (editable(s_sel) && y >= by - 6 && y <= by + 46 && abs(x - CX) < 70) {
        watchface_set_current(s_sel);
        wear(s_sel);
        ui_push(&settings_comp_screen);
        return;
    }
    int dx = x - CX, dy = y - CY;
    if (dx * dx + dy * dy <= (PV / 2) * (PV / 2)) wear(s_sel);
}

static void switch_tick() {
    if (s_sw_drag) return;
    float d = s_sel - s_pos;
    s_pos += fabsf(d) < 0.01f ? d : d * 0.3f;
}

static void switch_gesture(Gesture) {}

Screen face_switch_screen = {
    nullptr, GESTURE_MODE_FREE, UI_FRAME_MS_DEFAULT,
    switch_create, switch_draw, switch_touch, switch_tick, nullptr, switch_gesture,
    250,
    false, false, false,
    true,   // no_drag_back: sideways drags page the faces
    true,   // hide_status
};

// =========================================================================
//  Touches on a face: tap, double-tap, touch and hold (home_gestures.h)
// =========================================================================
static Screen  *s_hold_face = nullptr;
static int      s_hold_x = 0, s_hold_y = 0;
static uint32_t s_hold_ms = 0;
static const uint32_t HOLD_MS = 600;
static const uint32_t TAP_MS = 350;      // a press shorter than this that didn't move is a tap
static const uint32_t DOUBLE_MS = 300;   // a second tap within this is a double-tap
static bool     s_tap_ok = false;        // this press can still be a tap
static uint32_t s_tap_wait_ms = 0;       // a tap waiting to see if a second one comes
static int      s_tap_x = 0, s_tap_y = 0;
static uint32_t s_not_face_ms = 0;       // the last time no face was showing (just woke, just came back)

void face_nav_wear(int i) { wear(i); }

void face_nav_open_switcher() {
    int cur = watchface_current();
    uint16_t *b = pv_buf(cur);
    if (b && ui_framebuffer() && face_nav_is_face(ui_active_screen())) {
        downscale_into(b, ui_framebuffer());   // the face as it is now
        s_pv_ok[cur] = true;
    }
    sleep_register_activity();
    ui_push_instant(&face_switch_screen);
}

void face_nav_gesture(Gesture g) {
    s_tap_wait_ms = 0;
    switch (g) {
    case GESTURE_SWIPE_UP:    home_run_slot(HS_UP); break;
    case GESTURE_SWIPE_DOWN:  home_run_slot(HS_DOWN); break;
    case GESTURE_SWIPE_LEFT:  home_run_slot(HS_LEFT); break;    // from the right edge
    case GESTURE_SWIPE_RIGHT: home_run_slot(HS_RIGHT); break;   // from the left edge
    default: break;
    }
}

static void face_tap(int x, int y) {
    uint32_t now = millis();
    if (home_action(HS_DOUBLE) == HA_NONE) { home_run_slot(HS_TAP); return; }
    int dx = x - s_tap_x, dy = y - s_tap_y;
    if (s_tap_wait_ms && now - s_tap_wait_ms < DOUBLE_MS && dx * dx + dy * dy < 60 * 60) {
        s_tap_wait_ms = 0;
        sleep_register_activity();
        home_run_slot(HS_DOUBLE);
        return;
    }
    s_tap_wait_ms = now ? now : 1;   // wait: is a second one coming?
    s_tap_x = x;
    s_tap_y = y;
}

void face_nav_touch(Screen *face, int x, int y, bool pressed) {
    if (!pressed) {
        bool was_press = s_hold_face && s_hold_face != (Screen *)1;
        if (was_press && s_tap_ok && millis() - s_hold_ms < TAP_MS && face_nav_is_face(ui_active_screen()))
            face_tap(s_hold_x, s_hold_y);
        s_hold_face = nullptr;
        s_tap_ok = false;
        return;
    }
    if (!s_hold_face) {
        s_hold_face = face;
        s_hold_x = x;
        s_hold_y = y;
        s_hold_ms = millis();
        // not a tap: the finger that woke the watch, the pet being picked up
        s_tap_ok = s_hold_ms - s_not_face_ms > 400 && !pet_hit(x, y);
        return;
    }
    if (s_hold_face == (Screen *)1) return;
    int dx = x - s_hold_x, dy = y - s_hold_y;
    if (dx * dx + dy * dy > 16 * 16) { s_hold_face = (Screen *)1; s_tap_ok = false; }   // moved: a swipe or a drag
}

// The tap action for a face's own tap: true when the face should do its
// thing (the next photo) on a tap.
bool face_nav_tap_is_faces() { return home_action(HS_TAP) == HA_FACE_OWN; }

void face_nav_poll() {
    uint32_t now = millis();
    Screen *top = ui_active_screen();
    if (sleep_is_asleep() || !face_nav_is_face(top) || ui_top_panel_is_open()) { s_not_face_ms = now; s_tap_wait_ms = 0; }
    if (s_tap_wait_ms && now - s_tap_wait_ms >= DOUBLE_MS) {   // no second tap: it was one
        s_tap_wait_ms = 0;
        sleep_register_activity();
        home_run_slot(HS_TAP);
    }
    if (!s_hold_face || s_hold_face == (Screen *)1) return;
    if (now - s_hold_ms < HOLD_MS) return;
    Screen *face = s_hold_face;
    s_hold_face = (Screen *)1;   // held: not a tap, and once
    s_tap_ok = false;
    if (!ui_screen_on_top(face)) return;
    if (pet_hit(s_hold_x, s_hold_y)) return;   // holding the pet is dragging it
    // the face lets go (eyes, the pet) as if the finger had lifted
    if (face->on_touch) face->on_touch(s_hold_x, s_hold_y, false);
    touch_cancel_swipes();
    sleep_register_activity();
    if (home_action(HS_HOLD) == HA_FACES) face_nav_open_switcher();
    else home_run_slot(HS_HOLD);
}
