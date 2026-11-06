#include "complications.h"
#include "app_weather.h"
#include "fitness.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_rtc.h"
#include "hal_power.h"
#include "hal_imu.h"
#include "hal_ble.h"
#include "phone_link.h"
#include "app_calendar.h"
#include "app_navigation.h"
#include "app_notifications.h"
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>

static const char *const KEYS[COMP_COUNT] = {
    "none", "steps", "battery", "phone_battery", "next_event", "weather", "notifications", "navigation", "todos", "seconds", "rings", "brief",
};
static const char *const LABELS[COMP_COUNT] = {
    "Empty", "Steps", "Battery", "Phone battery", "Next event", "Weather", "Notifications", "Navigation", "To-dos", "Seconds", "Activity rings", "Today",
};

const char *comp_key(CompId id) { return id < COMP_COUNT ? KEYS[id] : "none"; }
const char *comp_label(CompId id) { return id < COMP_COUNT ? LABELS[id] : "Empty"; }

CompId comp_from_key(const char *k) {
    if (!k) return COMP_NONE;
    for (int i = 0; i < COMP_COUNT; i++) if (!strcmp(k, KEYS[i])) return (CompId)i;
    return COMP_NONE;
}

// ---- slots ----------------------------------------------------------------------------

static CompId s_slots[COMP_SLOTS] = { COMP_STEPS, COMP_NEXT_EVENT, COMP_BATTERY };

void comp_load() {
    Preferences p;
    if (!p.begin("comp", true)) return;
    for (int i = 0; i < COMP_SLOTS; i++) {
        char k[4];
        snprintf(k, sizeof(k), "s%d", i);
        uint8_t v = p.getUChar(k, (uint8_t)s_slots[i]);
        if (v < COMP_COUNT) s_slots[i] = (CompId)v;
    }
    p.end();
}

CompId comp_slot(int i) { return (i >= 0 && i < COMP_SLOTS) ? s_slots[i] : COMP_NONE; }

void comp_set_slot(int i, CompId id) {
    if (i < 0 || i >= COMP_SLOTS || id >= COMP_COUNT) return;
    s_slots[i] = id;
    Preferences p;
    if (!p.begin("comp", false)) return;
    char k[4];
    snprintf(k, sizeof(k), "s%d", i);
    p.putUChar(k, (uint8_t)id);
    p.end();
}

// ---- data ---------------------------------------------------------------------------------

static int s_todo_n = -1;
static char s_todo_top[40] = "";

void comp_set_todos(int count, const char *top) {
    s_todo_n = count;
    snprintf(s_todo_top, sizeof(s_todo_top), "%s", top ? top : "");
    ble_fold_utf8(s_todo_top);
}

static char s_brief_v[8] = "";
static char s_brief_l[40] = "";
static char s_brief_x[120] = "";

void comp_set_brief(const char *value, const char *label, const char *line) {
    snprintf(s_brief_v, sizeof(s_brief_v), "%s", value ? value : "");
    snprintf(s_brief_l, sizeof(s_brief_l), "%s", label ? label : "");
    snprintf(s_brief_x, sizeof(s_brief_x), "%s", line ? line : "");
    ble_fold_utf8(s_brief_l);
    ble_fold_utf8(s_brief_x);
}

static char s_recap[384] = "";
static int s_recap_day = -1;   // yyyymmdd of the evening it belongs to

// The evening a moment is in: before 4:00 it's still the day before.
static int evening_of(const WatchTime &t) {
    if (!t.valid) return -1;
    int d = t.year * 10000 + t.month * 100 + t.day;
    if (t.hour >= 4) return d;
    // yesterday, roughly (the 1st after midnight counts as the last of the month)
    return t.day > 1 ? d - 1 : d - 100 + 30;
}

void comp_set_recap(const char *text) {
    snprintf(s_recap, sizeof(s_recap), "%s", text ? text : "");
    ble_fold_utf8(s_recap);
    s_recap_day = evening_of(rtc_now());
}

bool comp_recap(char *text, int n) {
    if (!s_recap[0] || s_recap_day < 0 || evening_of(rtc_now()) != s_recap_day) return false;
    snprintf(text, n, "%s", s_recap);
    return true;
}

bool comp_brief(char *head, int hn, char *line, int ln) {
    if (!s_brief_l[0]) return false;
    snprintf(head, hn, "%s", s_brief_l);
    snprintf(line, ln, "%s", s_brief_x);
    return true;
}

static uint32_t s_step_goal = 8000;
uint32_t comp_step_goal() { return s_step_goal; }

// Weather from the last "weather" push: temperature + a short condition.
bool comp_weather_now(int &temp, char *cond, int n) {
    const char *wdata = nullptr;
    int wlen = 0;
    if (!ble_get_weather(&wdata, &wlen) || wlen < 3) return false;
    if (weather_age_min() > 24 * 60) return false;   // kept from before, but a day old: too stale to show
    JsonDocument d;
    if (deserializeJson(d, wdata, wlen)) return false;
    float t = d["tempC"] | -999.0f;
    if (t < -200) return false;
    temp = (int)lroundf(t);
    snprintf(cond, n, "%s", (const char *)(d["condition"] | ""));
    return true;
}

// ---- drawing --------------------------------------------------------------------------------

static void ring(int cx, int cy, int r, float frac, uint16_t c) {
    ui_arc(cx, cy, r, 5, 0, 360, COLOR_PANEL, false);
    if (frac > 0.001f) ui_arc(cx, cy, r, 5, 0, 360.0f * (frac > 1 ? 1 : frac), c, true);
}

static void value_label(int cx, int cy, int r, const char *value, const char *label, uint16_t vc) {
    int vsize = r >= 44 ? 3 : 2;
    if (ui_text_width(value, vsize) > 2 * r - 14) vsize = vsize > 1 ? vsize - 1 : 1;
    ui_text_center(cx, cy - (label && label[0] ? 6 : 0), vc, value, vsize);
    if (label && label[0]) {
        // The label sits where the ring is narrower: keep it inside, with "..." if it's long
        // ("Design review" -> "Design...").
        char fit[40];
        strlcpy(fit, label, sizeof(fit));
        int maxw = (int)(1.6f * r) - 10;
        if (ui_text_width(fit, 1) > maxw) {
            int n = strlen(fit);
            while (n > 1) {
                fit[--n] = 0;
                while (n > 0 && (fit[n - 1] & 0xC0) == 0x80) fit[--n] = 0;   // whole UTF-8 letters
                char t[44];
                snprintf(t, sizeof(t), "%s...", fit);
                if (ui_text_width(t, 1) <= maxw) { strlcpy(fit, t, sizeof(fit)); break; }
            }
        }
        ui_text_center(cx, cy + r / 2 - 2, COLOR_TEXT_DIM, fit, 1);
    }
}

static void short_time(uint32_t secs_from_now, char *out, int n) {
    if (secs_from_now < 60) snprintf(out, n, "now");
    else if (secs_from_now < 3600) snprintf(out, n, "%lum", (unsigned long)(secs_from_now / 60));
    else if (secs_from_now < 86400) snprintf(out, n, "%luh", (unsigned long)(secs_from_now / 3600));
    else snprintf(out, n, "%lud", (unsigned long)(secs_from_now / 86400));
}

void comp_draw(Arduino_GFX *g, CompId id, int cx, int cy, int r, uint16_t accent) {
    if (!g || id == COMP_NONE) return;
    char v[24], l[24];
    v[0] = l[0] = 0;
    switch (id) {
    case COMP_STEPS: {
        uint32_t st = imu_get_step_count();
        ring(cx, cy, r, (float)st / (float)s_step_goal, st >= s_step_goal ? COLOR_GOOD : accent);
        if (st >= 10000) snprintf(v, sizeof(v), "%lu.%luk", (unsigned long)(st / 1000), (unsigned long)(st % 1000 / 100));
        else snprintf(v, sizeof(v), "%lu", (unsigned long)st);
        value_label(cx, cy, r, v, "steps", COLOR_TEXT);
        break;
    }
    case COMP_BATTERY: {
        int p = power_get_battery_percent();
        uint16_t c = p < 0 ? COLOR_TEXT_DIM : p <= 15 ? COLOR_BAD : p <= 35 ? COLOR_WARN : COLOR_GOOD;
        ring(cx, cy, r, p < 0 ? 0 : p / 100.0f, c);
        snprintf(v, sizeof(v), p < 0 ? "--" : "%d%%", p);
        value_label(cx, cy, r, v, power_is_charging() ? "charging" : "watch", COLOR_TEXT);
        break;
    }
    case COMP_PHONE_BAT: {
        int p = phone_link_battery();
        ring(cx, cy, r, p < 0 ? 0 : p / 100.0f, p < 0 ? COLOR_TEXT_DIM : p <= 15 ? COLOR_BAD : accent);
        snprintf(v, sizeof(v), p < 0 ? "--" : "%d%%", p);
        value_label(cx, cy, r, v, "phone", COLOR_TEXT);
        break;
    }
    case COMP_NEXT_EVENT: {
        CalEvent e;
        ring(cx, cy, r, 0, accent);
        if (calendar_next(e)) {
            uint32_t now = calendar_now();
            if (e.all_day) snprintf(v, sizeof(v), "today");
            else if (e.start <= now) snprintf(v, sizeof(v), "now");
            else short_time(e.start - now, v, sizeof(v));
            snprintf(l, sizeof(l), "%.10s", e.title);
            ui_arc(cx, cy, r, 5, 0, 360, e.color ? e.color : accent, false);
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(l, sizeof(l), "no events");
        }
        value_label(cx, cy, r, v, l, COLOR_TEXT);
        break;
    }
    case COMP_WEATHER: {
        int t;
        char cond[24];
        ring(cx, cy, r, 0, accent);
        if (comp_weather_now(t, cond, sizeof(cond))) {
            snprintf(v, sizeof(v), "%dC", t);
            snprintf(l, sizeof(l), "%.9s", cond);
        } else {
            snprintf(v, sizeof(v), "--");
            snprintf(l, sizeof(l), "weather");
        }
        value_label(cx, cy, r, v, l, COLOR_TEXT);
        break;
    }
    case COMP_NOTIFS: {
        int n = notifications_history_count();
        ring(cx, cy, r, n > 0 ? 1.0f : 0, n > 0 ? accent : COLOR_PANEL);
        snprintf(v, sizeof(v), "%d", n);
        value_label(cx, cy, r, v, n == 1 ? "message" : "messages", COLOR_TEXT);
        break;
    }
    case COMP_NAV: {
        bool on = navigation_active();
        ring(cx, cy, r, on ? 1.0f : 0, on ? COLOR_GOOD : COLOR_PANEL);
        const char *d = navigation_distance();
        value_label(cx, cy, r, on && d[0] ? d : "--", on ? "next turn" : "no route", COLOR_TEXT);
        break;
    }
    case COMP_TODOS: {
        ring(cx, cy, r, s_todo_n > 0 ? 1.0f : 0, s_todo_n > 0 ? COLOR_WARN : COLOR_PANEL);
        if (s_todo_n < 0) snprintf(v, sizeof(v), "--");
        else snprintf(v, sizeof(v), "%d", s_todo_n);
        value_label(cx, cy, r, v, s_todo_n > 0 && s_todo_top[0] ? s_todo_top : "to-dos", COLOR_TEXT);
        break;
    }
    case COMP_BRIEF: {
        bool some = s_brief_v[0] && strcmp(s_brief_v, "-") != 0;
        ring(cx, cy, r, some ? 1.0f : 0, some ? accent : COLOR_PANEL);
        value_label(cx, cy, r, s_brief_v[0] ? s_brief_v : "--", s_brief_l[0] ? s_brief_l : "today", COLOR_TEXT);
        break;
    }
    case COMP_RINGS: {
        int th = r / 5 > 4 ? r / 5 : 4;
        fitness_draw_rings(cx, cy, r + 2, th);
        break;
    }
    case COMP_SECONDS: {
        WatchTime t = rtc_now();
        ring(cx, cy, r, t.valid ? t.second / 60.0f : 0, accent);
        snprintf(v, sizeof(v), "%02d", t.valid ? t.second : 0);
        value_label(cx, cy, r, v, "sec", COLOR_TEXT);
        break;
    }
    default:
        break;
    }
}

void comp_draw_slots(Arduino_GFX *g, int y, int r) {
    const int gap = 2 * r + 18;
    for (int i = 0; i < COMP_SLOTS; i++) {
        int cx = LCD_WIDTH / 2 + (i - 1) * gap;
        comp_draw(g, s_slots[i], cx, y, r, i == 1 ? COLOR_ACCENT : COLOR_ACCENT2);
    }
}
