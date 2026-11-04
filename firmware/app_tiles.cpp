// Tiles - see app_tiles.h.
#include "app_tiles.h"
#include "config.h"
#include "board_pins.h"
#include "home_gestures.h"
#include "app_settings_state.h"
#include "ui_font.h"
#include "fitness.h"
#include "complications.h"
#include "app_weather.h"
#include "app_calendar.h"
#include "app_clock.h"
#include "app_life.h"
#include "app_reminders.h"
#include "app_assistant.h"
#include "app_menu.h"
#include "app_battery.h"
#include "clock_fmt.h"
#include "phone_link.h"
#include "hal_audio.h"
#include "hal_imu.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_sleep.h"
#include "hal_touch.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

extern Screen activity_screen, calendar_screen, music_screen, nowplaying_screen, battery_screen;

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static uint8_t s_list[TILES_MAX];
static int   s_n = 0;
static float s_pos = 0;          // the page shown, fractional while moving (0 = the first)
static float s_target = 0;
static bool  s_drag = false, s_moved = false;
static int   s_x0 = 0, s_y0 = 0, s_lastx = 0;
static float s_pos0 = 0, s_vel = 0;
static int   s_page = 0;         // remembered between visits

// ---- helpers ----------------------------------------------------------------------------

// text that fits w pixels at this size ("..." when cut)
static void fit(char *out, int n, const char *in, int size, int w) {
    strlcpy(out, in, n);
    if (ui_text_width(out, size) <= w) return;
    int len = strlen(out);
    while (len > 1) {
        len--;
        while (len > 0 && (out[len] & 0xC0) == 0x80) len--;   // whole UTF-8 characters
        char t[96];
        snprintf(t, sizeof(t), "%.*s...", len, in);
        if (ui_text_width(t, size) <= w) { strlcpy(out, t, n); return; }
    }
}

static void center(int cx, int cy, uint16_t c, const char *t, int size, int w = 330) {
    char b[96];
    fit(b, sizeof(b), t, size, w);
    ui_text_center(cx, cy, c, b, size);
}

static void title(int cx, uint16_t c, const char *t) { ui_text_center(cx, 74, c, t, 1); }

static void button(int cx, int cy, int r, uint16_t bg, const char *label, int size = 2) {
    ui_gfx()->fillCircle(cx, cy, r, bg);
    ui_text_center(cx, cy, COLOR_TEXT, label, size);
}

// ---- the tiles ---------------------------------------------------------------------------

static void draw_activity(int cx) {
    title(cx, FIT_MOVE_COLOR, "ACTIVITY");
    fitness_draw_rings(cx, CY - 26, 96, 20);
    char b[64];
    snprintf(b, sizeof(b), "%d/%u kcal", (int)fitness_move_kcal(), (unsigned)fitness_move_goal());
    ui_text_center(cx, CY + 100, COLOR_TEXT, b, 2);
    snprintf(b, sizeof(b), "%lu min  -  %d h  -  %lu steps", (unsigned long)fitness_exercise_min(), fitness_stand_hours(),
             (unsigned long)imu_get_step_count());
    center(cx, CY + 136, COLOR_TEXT_DIM, b, 1, 300);
}

static void draw_weather(int cx) {
    title(cx, COLOR_ACCENT2, "WEATHER");
    int t;
    char cond[32];
    if (!comp_weather_now(t, cond, sizeof(cond))) {
        ui_text_center(cx, CY - 10, COLOR_TEXT, "No weather yet", 2);
        ui_text_center(cx, CY + 26, COLOR_TEXT_DIM, "It comes from the phone", 1);
        return;
    }
    WatchTime now = rtc_now();
    bool night = now.valid && (now.hour < 6 || now.hour >= 20);
    weather_draw_icon(cx, CY - 50, 56, cond, night);
    char b[24];
    snprintf(b, sizeof(b), "%d\xC2\xB0", t);
    ui_text_center(cx, CY + 52, COLOR_TEXT, b, 4);
    center(cx, CY + 106, COLOR_TEXT_DIM, cond, 2);
    int age = weather_age_min();
    if (age >= 60) {
        char a[32];
        snprintf(a, sizeof(a), "updated %d h ago", age / 60);
        ui_text_center(cx, CY + 136, COLOR_WARN, a, 1);
    }
}

static void draw_next(int cx) {
    title(cx, COLOR_ACCENT2, "UP NEXT");
    CalEvent ev;
    if (!calendar_next(ev)) {
        ui_text_center(cx, CY - 10, COLOR_TEXT, "Nothing planned", 2);
        ui_text_center(cx, CY + 26, COLOR_TEXT_DIM, "Events come from the phone's calendar", 1);
        return;
    }
    ui_gfx()->fillRoundRect(cx - 40, CY - 92, 80, 8, 4, ev.color ? ev.color : COLOR_ACCENT2);
    center(cx, CY - 40, COLOR_TEXT, ev.title, 3, 340);
    char when[40];
    calendar_format_when(ev, when, sizeof(when));
    center(cx, CY + 14, COLOR_ACCENT2, when, 2);
    if (ev.location[0]) center(cx, CY + 54, COLOR_TEXT_DIM, ev.location, 1, 300);
}

// music: the buttons
static const int MB_Y = CY + 92, MB_DX = 96;
static void draw_music(int cx) {
    title(cx, COLOR_ACCENT3, "NOW PLAYING");
    const PhoneMedia &m = phone_link_media();
    if (m.valid && m.title[0]) {
        center(cx, CY - 46, COLOR_TEXT, m.title, 3, 330);
        center(cx, CY - 4, COLOR_TEXT_DIM, m.artist, 2, 300);
        if (m.duration_s > 0) {
            float p = (float)phone_link_media_position() / m.duration_s;
            if (p > 1) p = 1;
            ui_gfx()->fillRoundRect(cx - 120, CY + 30, 240, 6, 3, COLOR_PANEL);
            ui_gfx()->fillRoundRect(cx - 120, CY + 30, 6 + (int)(234 * p), 6, 3, COLOR_ACCENT3);
        }
        button(cx - MB_DX, MB_Y, 32, COLOR_PANEL, "<<");
        button(cx, MB_Y, 42, COLOR_ACCENT3, m.playing ? "||" : ">", 3);
        button(cx + MB_DX, MB_Y, 32, COLOR_PANEL, ">>");
        return;
    }
    ui_text_center(cx, CY - 10, COLOR_TEXT, audio_is_playing() ? "On the watch" : "Nothing playing", 2);
    ui_text_center(cx, CY + 26, COLOR_TEXT_DIM, "Tap for Music", 1);
}

static const uint16_t TIMER_C = 0xFC60;
static const int QUICK[4] = {1, 3, 5, 10};   // minutes
static void quick_pos(int i, int cx, int &x, int &y) { x = cx + (i % 2 ? 62 : -62); y = CY - 10 + (i / 2 ? 62 : -62) + 20; }
static void draw_timer(int cx) {
    title(cx, TIMER_C, "TIMER");
    if (assistant_timer_active()) {
        uint32_t left = assistant_timer_left_s(), len = assistant_timer_length_s();
        float p = len ? 1.0f - (float)assistant_timer_left_ms() / (len * 1000.0f) : 0;
        ui_arc(cx, CY + 6, 150, 14, 0, 360, COLOR_PANEL, false);
        if (p < 1) ui_arc(cx, CY + 6, 150, 14, 360 * p, 360 * (1 - p), TIMER_C);
        char b[16];
        if (left >= 3600) snprintf(b, sizeof(b), "%lu:%02lu:%02lu", (unsigned long)(left / 3600), (unsigned long)(left / 60 % 60), (unsigned long)(left % 60));
        else snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
        ui_text_center(cx, CY, COLOR_TEXT, b, 4);
        ui_text_center(cx, CY + 52, COLOR_TEXT_DIM, assistant_timer_paused() ? "paused - tap to go on" : "tap to pause", 1);
        return;
    }
    for (int i = 0; i < 4; i++) {
        int x, y;
        quick_pos(i, cx, x, y);
        char b[8];
        snprintf(b, sizeof(b), "%d", QUICK[i]);
        ui_gfx()->fillCircle(x, y, 50, ui_dim(TIMER_C, 0.5f));
        ui_text_center(x, y - 6, COLOR_TEXT, b, 3);
        ui_text_center(x, y + 24, COLOR_TEXT_DIM, "min", 1);
    }
}

static void draw_life(int cx) {
    title(cx, 0x2CCA, "LIFE");
    int sc = life_score();
    ui_arc(cx, CY - 20, 92, 16, 0, 360, COLOR_PANEL, false);
    if (sc > 0) ui_arc(cx, CY - 20, 92, 16, 0, 3.6f * (sc > 100 ? 100 : sc), 0x2CCA);
    char b[48];
    snprintf(b, sizeof(b), "%d%%", sc);
    ui_text_center(cx, CY - 28, COLOR_TEXT, b, 4);
    ui_text_center(cx, CY + 10, COLOR_TEXT_DIM, "of today", 1);
    if (life_next(b, sizeof(b))) center(cx, CY + 106, COLOR_TEXT, b, 2, 300);
    else ui_text_center(cx, CY + 106, COLOR_TEXT_DIM, "Nothing else planned", 1);
}

static void draw_world(int cx) {
    title(cx, COLOR_ACCENT, "WORLD CLOCK");
    const char *names[4];
    int hh[4], mm[4];
    int n = clock_world_times(names, hh, mm, 4);
    if (!n) {
        ui_text_center(cx, CY - 10, COLOR_TEXT, "No cities yet", 2);
        ui_text_center(cx, CY + 26, COLOR_TEXT_DIM, "Connect the phone once", 1);
        return;
    }
    int y0 = CY - (n - 1) * 34;
    for (int i = 0; i < n; i++) {
        int y = y0 + i * 68;
        char hm[16];
        clock_hm_text(hm, sizeof(hm), hh[i], mm[i]);
        ui_print(cx - 150, y - 10, 2, COLOR_TEXT, names[i]);
        ui_print(cx + 150 - ui_text_width(hm, 2), y - 10, 2, COLOR_ACCENT, hm);
        if (i < n - 1) ui_gfx()->drawFastHLine(cx - 150, y + 34, 300, COLOR_PANEL);
    }
}

static void draw_alarm(int cx) {
    title(cx, TIMER_C, "ALARM");
    uint32_t in;
    int h, m;
    if (!clock_next_alarm(in, h, m)) {
        ui_text_center(cx, CY - 10, COLOR_TEXT, "No alarm set", 2);
        ui_text_center(cx, CY + 26, COLOR_TEXT_DIM, "Tap to add one", 1);
        return;
    }
    char b[24];
    clock_hm_text(b, sizeof(b), h, m);
    ui_text_center(cx, CY - 16, COLOR_TEXT, b, 4);
    if (in < 60) snprintf(b, sizeof(b), "in %lu min", (unsigned long)in);
    else snprintf(b, sizeof(b), "in %luh %02lum", (unsigned long)(in / 60), (unsigned long)(in % 60));
    ui_text_center(cx, CY + 44, COLOR_TEXT_DIM, b, 2);
}

static void draw_reminders(int cx) {
    title(cx, COLOR_ACCENT3, "REMINDERS");
    int n = reminders_count();
    if (!n) {
        ui_text_center(cx, CY - 10, COLOR_TEXT, "All done", 2);
        ui_text_center(cx, CY + 26, COLOR_TEXT_DIM, "Nothing to remember", 1);
        return;
    }
    center(cx, CY - 20, COLOR_TEXT, reminders_first(), 3, 330);
    char b[32];
    if (n > 1) snprintf(b, sizeof(b), "and %d more", n - 1);
    else snprintf(b, sizeof(b), "Tap to tick it off");
    ui_text_center(cx, CY + 36, COLOR_TEXT_DIM, b, 2);
}

static void draw_battery(int cx) {
    int p = power_get_battery_percent();
    uint16_t c = p >= 0 && p <= 20 ? COLOR_BAD : COLOR_GOOD;
    title(cx, c, "BATTERY");
    ui_arc(cx, CY - 14, 110, 18, 0, 360, COLOR_PANEL, false);
    if (p > 0) ui_arc(cx, CY - 14, 110, 18, 0, 3.6f * p, c);
    char b[24];
    if (p < 0) snprintf(b, sizeof(b), "--");
    else snprintf(b, sizeof(b), "%d%%", p);
    ui_text_center(cx, CY - 22, COLOR_TEXT, b, 4);
    ui_text_center(cx, CY + 20, COLOR_TEXT_DIM, power_is_charging() ? "charging" : "watch", 1);
    char left[32];
    if (battery_left_text(left, sizeof(left))) ui_text_center(cx, CY + 92, COLOR_TEXT_DIM, left, 1);
    int pp = phone_link_battery();
    if (pp >= 0) {
        snprintf(b, sizeof(b), "Phone %d%%", pp);
        ui_text_center(cx, CY + 130, COLOR_TEXT, b, 2);
    }
}

static void draw_tile(int k, int cx) {
    switch (k) {
    case TL_ACTIVITY:  draw_activity(cx); break;
    case TL_WEATHER:   draw_weather(cx); break;
    case TL_NEXT:      draw_next(cx); break;
    case TL_MUSIC:     draw_music(cx); break;
    case TL_TIMER:     draw_timer(cx); break;
    case TL_LIFE:      draw_life(cx); break;
    case TL_WORLD:     draw_world(cx); break;
    case TL_ALARM:     draw_alarm(cx); break;
    case TL_REMINDERS: draw_reminders(cx); break;
    case TL_BATTERY:   draw_battery(cx); break;
    }
}

static Screen *tile_app(int k) {
    switch (k) {
    case TL_ACTIVITY:  return &activity_screen;
    case TL_WEATHER:   return &weather_screen;
    case TL_NEXT:      return &calendar_screen;
    case TL_MUSIC:     return phone_link_media().valid ? &nowplaying_screen : &music_screen;
    case TL_TIMER:     return &timer_screen;
    case TL_LIFE:      return &life_screen;
    case TL_WORLD:     return &worldclock_screen;
    case TL_ALARM:     return &alarms_screen;
    case TL_REMINDERS: return &reminders_screen;
    case TL_BATTERY:   return &battery_screen;
    }
    return nullptr;
}

static bool in_circle(int x, int y, int cx, int cy, int r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }

static void tile_tap(int k, int x, int y) {
    sleep_register_activity();
    if (k == TL_MUSIC && phone_link_media().valid && phone_link_media().title[0]) {
        if (in_circle(x, y, CX - MB_DX, MB_Y, 40)) { phone_link_media_cmd("prev"); return; }
        if (in_circle(x, y, CX, MB_Y, 50)) { phone_link_media_cmd("toggle"); return; }
        if (in_circle(x, y, CX + MB_DX, MB_Y, 40)) { phone_link_media_cmd("next"); return; }
    }
    if (k == TL_TIMER) {
        if (assistant_timer_active()) {
            if (in_circle(x, y, CX, CY, 120)) { assistant_timer_pause(!assistant_timer_paused()); return; }
        } else {
            for (int i = 0; i < 4; i++) {
                int qx, qy;
                quick_pos(i, CX, qx, qy);
                if (in_circle(x, y, qx, qy, 54)) {
                    assistant_timer_set(QUICK[i] * 60);
                    char b[24];
                    snprintf(b, sizeof(b), "Timer: %d min", QUICK[i]);
                    ui_show_toast(b, 1200);
                    return;
                }
            }
            return;   // between the buttons: nothing
        }
    }
    Screen *s = tile_app(k);
    if (s) ui_push(s);
}

// ---- the screen ---------------------------------------------------------------------------

static void tiles_create() {
    s_n = tiles_list(s_list);
    if (s_page >= s_n) s_page = s_n ? s_n - 1 : 0;
    s_pos = s_target = s_page;
    s_drag = false;
    s_vel = 0;
}

static void tiles_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    if (!s_n) {
        ui_text_center(CX, CY - 14, COLOR_TEXT, "No tiles", 2);
        ui_text_center(CX, CY + 22, COLOR_TEXT_DIM, "Choose them in the app's Faces tab", 1);
        return;
    }
    // the tile under the finger and its neighbour, sliding
    int i0 = (int)floorf(s_pos);
    for (int i = i0; i <= i0 + 1; i++) {
        if (i < 0 || i >= s_n) continue;
        int ox = (int)lroundf((i - s_pos) * LCD_WIDTH);
        if (ox <= -LCD_WIDTH || ox >= LCD_WIDTH) continue;
        draw_tile(s_list[i], CX + ox);
    }
    // which of how many
    if (s_n > 1) {
        int dw = s_n * 14;
        int near = (int)lroundf(s_pos);
        for (int i = 0; i < s_n; i++)
            g->fillCircle(CX - dw / 2 + 7 + i * 14, LCD_HEIGHT - 34, i == near ? 4 : 3, i == near ? COLOR_TEXT : COLOR_TEXT_DIM);
    }
}

static void tiles_touch(int x, int y, bool pressed) {
    if (pressed) {
        if (!s_drag) {
            s_drag = true;
            s_moved = fabsf(s_pos - s_target) > 0.05f;
            s_x0 = s_lastx = x;
            s_y0 = y;
            s_pos0 = s_pos;
            s_vel = 0;
            return;
        }
        if (abs(x - s_x0) > 10) s_moved = true;
        float np = s_pos0 - (float)(x - s_x0) / LCD_WIDTH;
        if (np < 0) np *= 0.35f;                                 // rubber band: past the first...
        else if (np > s_n - 1) np = (s_n - 1) + (np - (s_n - 1)) * 0.35f;   // ...and the last
        s_pos = np;
        s_vel = -(float)(x - s_lastx) / LCD_WIDTH;
        s_lastx = x;
        return;
    }
    if (!s_drag) return;
    s_drag = false;
    touch_cancel_swipes();
    if (!s_moved) {
        int k = (int)lroundf(s_pos);
        if (k >= 0 && k < s_n) tile_tap(s_list[k], x, y);
        return;
    }
    // pulled right past the first tile: back to the face
    if (s_pos < -0.22f) { s_target = 0; s_page = 0; ui_pop_screen(); return; }
    int t = (int)lroundf(s_pos + s_vel * 6);   // a flick goes on to the next one
    int base = (int)lroundf(s_pos0);
    if (t > base + 1) t = base + 1;
    if (t < base - 1) t = base - 1;
    if (t < 0) t = 0;
    if (t > s_n - 1) t = s_n - 1;
    s_target = t;
    s_page = t;
}

static void tiles_tick() {
    if (s_drag) return;
    float d = s_target - s_pos;
    if (fabsf(d) < 0.002f) s_pos = s_target;
    else s_pos += d * 0.28f;
}

void tiles_set_page(int i) { s_page = i < 0 ? 0 : i; }

static void tiles_gesture(Gesture) {}

Screen tiles_screen = {
    nullptr, GESTURE_MODE_FREE, UI_FRAME_MS_SMOOTH,   // no header: each tile has its title
    tiles_create, tiles_draw, tiles_touch, tiles_tick, nullptr, tiles_gesture,
    500,     // idle: the timer, the song, steps
    false, false, false,
    true,    // no_drag_back: sideways drags page the tiles (past the first: back)
    false,   // the time stays in the status bar
};
