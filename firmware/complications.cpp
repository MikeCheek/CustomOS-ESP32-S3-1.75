#include "complications.h"
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
    "none", "steps", "battery", "phone_battery", "next_event", "weather", "notifications", "navigation", "todos", "seconds",
};
static const char *const LABELS[COMP_COUNT] = {
    "Empty", "Steps", "Battery", "Phone battery", "Next event", "Weather", "Notifications", "Navigation", "To-dos", "Seconds",
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

static uint32_t s_step_goal = 8000;

// Weather from the last "weather" push: temperature + a short condition.
static bool weather_now(int &temp, char *cond, int n) {
    const char *wdata = nullptr;
    int wlen = 0;
    if (!ble_get_weather(&wdata, &wlen) || wlen < 3) return false;
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
    if (label && label[0]) ui_text_center(cx, cy + r / 2 - 2, COLOR_TEXT_DIM, label, 1);
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
        if (weather_now(t, cond, sizeof(cond))) {
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
