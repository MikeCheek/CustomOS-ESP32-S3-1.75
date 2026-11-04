// Weather app - see app_weather.h.
#include "app_weather.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "clock_fmt.h"
#include "hal_ble.h"
#include "hal_rtc.h"
#include "app_calendar.h"
#include "store.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

struct Hour { int8_t h; int8_t t; uint8_t code; };
static void save_hours();
static Hour s_hours[12];
static int  s_hour_n = 0;
static int  s_hi = -999, s_lo = -999;
static char s_place[32] = "";
static uint32_t s_hourly_ms = 0;

// Open-Meteo's WMO codes, the same words as the app's WeatherService.describe().
static const char *wmo_text(int code) {
    if (code == 0) return "Clear";
    if (code <= 2) return "Partly cloudy";
    if (code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code >= 85 && code <= 86) return "Snow showers";
    if (code >= 95) return "Storm";
    return "";
}

void weather_on_hourly(JsonDocument &doc) {
    strlcpy(s_place, doc["pl"] | "", sizeof(s_place));
    s_hi = doc["hi"].is<float>() ? (int)lroundf(doc["hi"].as<float>()) : -999;
    s_lo = doc["lo"].is<float>() ? (int)lroundf(doc["lo"].as<float>()) : -999;
    s_hour_n = 0;
    for (JsonVariant v : doc["h"].as<JsonArray>()) {
        if (s_hour_n >= 12) break;
        JsonArray a = v.as<JsonArray>();
        if (a.size() < 3) continue;
        s_hours[s_hour_n++] = {(int8_t)(a[0] | 0), (int8_t)lroundf(a[1] | 0.0f), (uint8_t)(a[2] | 0)};
    }
    s_hourly_ms = millis();
    save_hours();
}

// ---- kept across restarts (store.h) -----------------------------------------------------
// "weather_now": [epoch u32][the phone's JSON]; "weather_hours": HoursFile.
struct HoursFile { uint32_t epoch; Hour h[12]; int32_t n, hi, lo; char place[32]; };
static uint32_t s_now_epoch = 0, s_now_hash = 0;

static uint32_t hash_of(const char *d, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ (uint8_t)d[i]) * 16777619u;
    return h;
}

static void save_hours() {
    HoursFile f;
    memset(&f, 0, sizeof(f));
    f.epoch = calendar_now();
    memcpy(f.h, s_hours, sizeof(s_hours));
    f.n = s_hour_n; f.hi = s_hi; f.lo = s_lo;
    strlcpy(f.place, s_place, sizeof(f.place));
    store_write("weather_hours", &f, sizeof(f));
}

void weather_init() {
    HoursFile f;
    if (store_read("weather_hours", &f, sizeof(f)) == (int)sizeof(f) && f.n >= 0 && f.n <= 12) {
        memcpy(s_hours, f.h, sizeof(s_hours));
        s_hour_n = f.n; s_hi = f.hi; s_lo = f.lo;
        f.place[sizeof(f.place) - 1] = 0;
        strlcpy(s_place, f.place, sizeof(s_place));
    }
    int sz = store_size("weather_now");
    if (sz > 4 && sz < 4096) {
        char *buf = (char *)malloc(sz);
        if (buf && store_read("weather_now", buf, sz) == sz) {
            memcpy(&s_now_epoch, buf, 4);
            ble_restore_weather(buf + 4, sz - 4);
            s_now_hash = hash_of(buf + 4, sz - 4);
        }
        free(buf);
    }
}

// loop(): a new push from the phone is saved, with when it came
void weather_update() {
    static uint32_t last = 0;
    uint32_t ms = millis();
    if (ms - last < 2000) return;
    last = ms;
    const char *d;
    int n;
    if (!ble_get_weather(&d, &n) || n <= 0) return;
    uint32_t h = hash_of(d, n);
    if (h == s_now_hash) return;
    s_now_hash = h;
    s_now_epoch = calendar_now();
    char *buf = (char *)malloc(n + 4);
    if (!buf) return;
    memcpy(buf, &s_now_epoch, 4);
    memcpy(buf + 4, d, n);
    store_write("weather_now", buf, n + 4);
    free(buf);
}

int weather_age_min() {
    uint32_t now = calendar_now();
    if (!s_now_epoch || !now || now < s_now_epoch) return -1;
    return (int)((now - s_now_epoch) / 60);
}

// ---- symbols ---------------------------------------------------------------------------------------
static void cloud(Arduino_GFX *g, int cx, int cy, int r, uint16_t c) {
    g->fillCircle(cx - r * 4 / 10, cy + r / 10, r * 4 / 10, c);
    g->fillCircle(cx + r / 10, cy - r * 2 / 10, r / 2, c);
    g->fillCircle(cx + r * 5 / 10, cy + r / 10, r * 35 / 100, c);
    g->fillRoundRect(cx - r * 8 / 10, cy + r / 10, r * 165 / 100, r * 4 / 10, r / 5, c);
}

void weather_draw_icon(int cx, int cy, int r, const char *c, bool night) {
    Arduino_GFX *g = ui_gfx();
    if (!g || !c) return;
    const uint16_t SUN = COLOR_WARN, CLOUD = 0xC618, DARK = 0x7BEF, DROP = COLOR_ACCENT2, MOON = 0xEF5D;
    bool rain = strstr(c, "Rain") || strstr(c, "Drizzle") || (strstr(c, "Showers") && !strstr(c, "Snow"));
    bool snow = strstr(c, "Snow");
    bool storm = strstr(c, "Storm");
    bool fog = strstr(c, "Fog");
    bool partly = strstr(c, "Partly");
    bool clear = strstr(c, "Clear");
    if (clear || partly) {
        int sx = partly ? cx - r / 3 : cx, sy = partly ? cy - r / 3 : cy, sr = partly ? r / 2 : r * 6 / 10;
        if (night) {
            g->fillCircle(sx, sy, sr, MOON);
            g->fillCircle(sx + sr / 2, sy - sr / 3, sr * 8 / 10, COLOR_BG);
        } else {
            g->fillCircle(sx, sy, sr * 7 / 10, SUN);
            for (int a = 0; a < 8; a++) {
                float t = a * 0.785398f;
                g->drawLine(sx + (int)(sr * 0.9f * cosf(t)), sy + (int)(sr * 0.9f * sinf(t)), sx + (int)(sr * 1.25f * cosf(t)),
                            sy + (int)(sr * 1.25f * sinf(t)), SUN);
            }
        }
        if (partly) cloud(g, cx + r / 6, cy + r / 5, r * 7 / 10, CLOUD);
        return;
    }
    if (fog) {
        for (int i = 0; i < 4; i++) g->fillRoundRect(cx - r + (i % 2) * r / 4, cy - r / 2 + i * r / 3, r * 7 / 4, r / 7 + 1, r / 14 + 1, CLOUD);
        return;
    }
    cloud(g, cx, cy - (rain || snow || storm ? r / 4 : 0), r, rain || storm ? DARK : CLOUD);
    if (rain)
        for (int i = 0; i < 3; i++) g->fillRoundRect(cx - r / 2 + i * r / 2 - 2, cy + r * 4 / 10, 4, r / 3, 2, DROP);
    if (snow)
        for (int i = 0; i < 3; i++) g->fillCircle(cx - r / 2 + i * r / 2, cy + r / 2, r / 9 + 1, COLOR_TEXT);
    if (storm) {
        g->fillTriangle(cx, cy + r / 6, cx - r / 4, cy + r * 6 / 10, cx + r / 12, cy + r * 6 / 10, SUN);
        g->fillTriangle(cx + r / 12, cy + r * 45 / 100, cx - r / 8, cy + r * 95 / 100, cx + r / 4, cy + r * 45 / 100, SUN);
    }
}

// ---- the app ----------------------------------------------------------------------------------------
static UiScroll s_scroll;
static const int CONTENT_H = 760;

static void wx_create() { ui_scroll_reset(&s_scroll, CONTENT_H - LCD_HEIGHT, 0); }

static void wx_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_scroll_tick(&s_scroll);
    g->fillScreen(COLOR_BG);
    int off = ui_scroll_offset(&s_scroll);
    const char *wd = nullptr;
    int wl = 0;
    JsonDocument d;
    bool have = ble_get_weather(&wd, &wl) && wl > 2 && !deserializeJson(d, wd, wl);
    if (!have) {
        weather_draw_icon(CX, CY - 50, 60, "Partly cloudy");
        ui_text_center(CX, CY + 40, COLOR_TEXT, "No weather yet", 2);
        ui_text_center(CX, CY + 74, COLOR_TEXT_DIM, "The phone sends it when connected", 1);
        return;
    }
    WatchTime now = rtc_now();
    bool night = now.valid && (now.hour >= 20 || now.hour < 6);
    const char *cond = d["condition"] | "";
    float t = d["tempC"] | 0.0f;

    // now
    int y = 70 - off;
    if (s_place[0]) ui_text_center(CX, y + 20, COLOR_TEXT, s_place, 2);
    char b[40];
    snprintf(b, sizeof(b), "%d\xC2\xB0", (int)lroundf(t));
    weather_draw_icon(CX - 86, y + 100, 46, cond, night);
    ui_text_center(CX + 50, y + 100, COLOR_TEXT, b, 6);
    ui_text_center(CX, y + 162, COLOR_TEXT, cond, 2);
    if (s_hi > -999) snprintf(b, sizeof(b), "H:%d\xC2\xB0  L:%d\xC2\xB0  -  %d%% humidity", s_hi, s_lo, d["humidity"] | 0);
    else snprintf(b, sizeof(b), "%d%% humidity", d["humidity"] | 0);
    ui_text_center(CX, y + 194, COLOR_TEXT_DIM, b, 1);
    // kept from before (a restart, the phone away): say how old it is
    int age = weather_age_min();
    if (age >= 60) {
        if (age < 48 * 60) snprintf(b, sizeof(b), "Updated %d h ago", age / 60);
        else snprintf(b, sizeof(b), "Updated %d days ago", age / 1440);
        ui_text_center(CX, y + 214, COLOR_WARN, b, 1);
    }

    // the next hours
    y = 300 - off;
    if (s_hour_n > 0) {
        g->fillRoundRect(28, y, LCD_WIDTH - 56, 132, 26, 0x18E3);
        int n = s_hour_n > 6 ? 6 : s_hour_n;
        int step = (LCD_WIDTH - 96) / n;
        for (int i = 0; i < n; i++) {
            const Hour &h = s_hours[i * s_hour_n / n];
            int x = 48 + step / 2 + i * step;
            char hb[8];
            if (i == 0) snprintf(hb, sizeof(hb), "Now");
            else if (clock_24h()) snprintf(hb, sizeof(hb), "%02d", h.h);
            else snprintf(hb, sizeof(hb), "%d%s", h.h % 12 == 0 ? 12 : h.h % 12, h.h < 12 ? "a" : "p");
            ui_text_center(x, y + 24, COLOR_TEXT_DIM, hb, 1);
            weather_draw_icon(x, y + 66, 18, wmo_text(h.code), h.h >= 20 || h.h < 6);
            snprintf(hb, sizeof(hb), "%d\xC2\xB0", h.t);
            ui_text_center(x, y + 108, COLOR_TEXT, hb, 2);
        }
    }

    // the coming days, with a bar from the low to the high
    y = 456 - off;
    JsonArray days = d["forecast"].as<JsonArray>();
    float lo_all = 99, hi_all = -99;
    for (JsonVariant v : days) { lo_all = fminf(lo_all, v["low"] | 0.0f); hi_all = fmaxf(hi_all, v["high"] | 0.0f); }
    int row = 0;
    for (JsonVariant v : days) {
        int ry = y + row * 66;
        row++;
        if (ry > LCD_HEIGHT || ry + 66 < 0) continue;
        float lo = v["low"] | 0.0f, hi = v["high"] | 0.0f;
        ui_print(64, ry + 22, 2, COLOR_TEXT, v["day"] | "");
        weather_draw_icon(160, ry + 32, 18, v["cond"] | "");
        char lb[8], hb2[8];
        snprintf(lb, sizeof(lb), "%d\xC2\xB0", (int)lroundf(lo));
        snprintf(hb2, sizeof(hb2), "%d\xC2\xB0", (int)lroundf(hi));
        ui_print(200, ry + 22, 2, COLOR_TEXT_DIM, lb);
        int bx = 252, bw = 120;
        g->fillRoundRect(bx, ry + 28, bw, 8, 4, COLOR_PANEL);
        float span = hi_all - lo_all > 1 ? hi_all - lo_all : 1;
        int x0 = bx + (int)((lo - lo_all) / span * bw), x1 = bx + (int)((hi - lo_all) / span * bw);
        if (x1 < x0 + 8) x1 = x0 + 8;
        g->fillRoundRect(x0, ry + 28, x1 - x0, 8, 4, COLOR_WARN);
        ui_print(bx + bw + 10, ry + 22, 2, COLOR_TEXT, hb2);
    }
}

static void wx_touch(int, int y, bool pressed) { ui_scroll_touch(&s_scroll, y, pressed); }

static void wx_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen weather_screen = {
    "", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    wx_create, wx_draw, wx_touch, nullptr, nullptr, wx_gesture,
    500,
};
