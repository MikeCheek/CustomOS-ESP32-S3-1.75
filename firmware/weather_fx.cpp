/*
 * weather_fx.cpp
 * Weather behind the main watchface - see weather_fx.h.
 *
 * The condition string comes from the phone (WeatherService.describe():
 * Clear, Partly cloudy, Cloudy, Fog, Drizzle, Rain, Showers, Snow,
 * Snow showers, Storm); it's parsed once every few seconds, not per frame.
 * Particles live in one static pool and move by real elapsed time, so the
 * speed doesn't depend on the frame rate.
 */
#include "weather_fx.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "hal_ble.h"
#include "hal_rtc.h"
#include "fx3d.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

enum WxKind : uint8_t {
    WX_NONE, WX_CLEAR, WX_HOT, WX_NIGHT, WX_PARTLY, WX_CLOUDY, WX_FOG,
    WX_DRIZZLE, WX_RAIN, WX_SHOWERS, WX_SNOW, WX_STORM,
};

#define MAX_P 70
struct Particle { float x, y, v, s, ph; };
static Particle s_p[MAX_P];
static WxKind s_kind = WX_NONE, s_seeded = WX_NONE;
static uint32_t s_parse_ms = 0, s_last_ms = 0;
static uint32_t s_flash_until = 0, s_next_flash = 0;
static int s_bolt_x = 0;

static const int W = LCD_WIDTH, H = LCD_HEIGHT, CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

static float frand(float lo, float hi) { return lo + (hi - lo) * (float)random(10001) / 10000.0f; }

static WxKind classify() {
    const char *wdata = nullptr;
    int wlen = 0;
    if (!ble_get_weather(&wdata, &wlen) || wlen < 3) return WX_NONE;
    JsonDocument d;
    if (deserializeJson(d, wdata, wlen)) return WX_NONE;
    const char *c = d["condition"] | "";
    float t = d["tempC"] | 15.0f;
    WatchTime now = rtc_now();
    bool night = now.valid && (now.hour >= 20 || now.hour < 6);
    if (strstr(c, "Storm")) return WX_STORM;
    if (strstr(c, "Snow")) return WX_SNOW;
    if (strstr(c, "Showers")) return WX_SHOWERS;
    if (strstr(c, "Rain")) return WX_RAIN;
    if (strstr(c, "Drizzle")) return WX_DRIZZLE;
    if (strstr(c, "Fog")) return WX_FOG;
    if (!strcmp(c, "Cloudy")) return WX_CLOUDY;
    if (strstr(c, "Partly")) return night ? WX_NIGHT : WX_PARTLY;
    if (strstr(c, "Clear")) return night ? WX_NIGHT : (t >= 28.0f ? WX_HOT : WX_CLEAR);
    return WX_NONE;
}

static int count_for(WxKind k) {
    switch (k) {
    case WX_DRIZZLE: return 28;
    case WX_RAIN: return 50;
    case WX_SHOWERS: case WX_STORM: return MAX_P;
    case WX_SNOW: return 45;
    case WX_NIGHT: return 40;
    case WX_CLOUDY: return 7;
    case WX_PARTLY: return 4;
    case WX_FOG: return 9;
    case WX_HOT: return 18;
    default: return 0;
    }
}

static void seed(WxKind k) {
    for (int i = 0; i < MAX_P; i++) {
        Particle &p = s_p[i];
        p.x = frand(0, W);
        p.y = frand(0, H);
        p.ph = frand(0, 6.2831853f);
        switch (k) {
        case WX_SNOW: p.v = frand(18, 45); p.s = frand(1.0f, 2.6f); break;
        case WX_NIGHT: p.v = 0; p.s = frand(0.6f, 1.8f); break;
        case WX_CLOUDY: case WX_PARTLY: p.v = frand(4, 10); p.s = frand(26, 46); p.y = frand(40, H * 0.55f); break;
        case WX_FOG: p.v = frand(6, 16); p.s = frand(90, 220); p.y = 60 + i * (H - 120) / 9.0f; break;
        case WX_HOT: p.v = frand(12, 30); p.s = frand(14, 30); break;
        default: p.v = frand(260, 420); p.s = frand(10, 20); break;       // rain: speed, length
        }
    }
    s_seeded = k;
}

static void rain(Arduino_GFX *g, int n, float dt, float slant, uint16_t c) {
    for (int i = 0; i < n; i++) {
        Particle &p = s_p[i];
        p.y += p.v * dt;
        p.x += p.v * slant * dt;
        if (p.y > H) { p.y = frand(-30, 0); p.x = frand(-40, W); }
        if (p.x > W) p.x -= W + 40;
        g->drawLine((int)p.x, (int)p.y, (int)(p.x - p.s * slant), (int)(p.y - p.s), c);
    }
}

static void cloud(Arduino_GFX *g, int x, int y, int r, uint16_t c) {
    g->fillCircle(x, y, r, c);
    g->fillCircle(x - r * 9 / 10, y + r / 4, r * 2 / 3, c);
    g->fillCircle(x + r, y + r / 5, r * 3 / 4, c);
    g->fillRect(x - r * 9 / 10, y + r / 4, r * 19 / 10, r * 2 / 3, c);
}

static void sun(Arduino_GFX *g, uint32_t now, uint16_t core, uint16_t ray) {
    const int sx = CX + 120, sy = CY - 135;
    float rot = (now % 24000) / 24000.0f * 6.2831853f;
    for (int i = 0; i < 12; i++) {
        float a = rot + i * 0.5235988f;
        float pulse = 0.85f + 0.15f * sinf(now * 0.002f + i);
        int r0 = 34, r1 = (int)(58 * pulse);
        g->drawLine(sx + (int)(cosf(a) * r0), sy + (int)(sinf(a) * r0), sx + (int)(cosf(a) * r1), sy + (int)(sinf(a) * r1), ray);
    }
    g->fillCircle(sx, sy, 24, core);
}

void weather_fx_draw(Arduino_GFX *g) {
    if (!g || !g_app_settings.weather_fx) return;
    uint32_t now = millis();
    if (now - s_parse_ms > 5000 || s_parse_ms == 0) {
        s_parse_ms = now;
        s_kind = classify();
    }
    if (s_kind == WX_NONE) return;
    if (s_seeded != s_kind) { seed(s_kind); s_last_ms = now; }
    float dt = (now - s_last_ms) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;                      // after a pause, don't jump
    s_last_ms = now;
    int n = count_for(s_kind);

    switch (s_kind) {
    case WX_CLEAR:
    case WX_HOT: {
        bool hot = s_kind == WX_HOT;
        sun(g, now, hot ? 0x7A00 : 0x6B20, hot ? 0x5100 : 0x39E0);
        if (hot) {   // heat shimmer: wavy lines rising from the bottom
            for (int i = 0; i < n; i++) {
                Particle &p = s_p[i];
                p.y -= p.v * dt;
                if (p.y < H * 0.45f) { p.y = H + frand(0, 40); p.x = frand(60, W - 60); }
                float k = (p.y - H * 0.45f) / (H * 0.55f);
                uint16_t c = fx_scale565(0x9A40, (uint32_t)(10 * k));
                for (int j = 0; j < (int)p.s; j += 2) {
                    int x = (int)(p.x + j - p.s / 2), y = (int)(p.y + 3 * sinf(j * 0.4f + now * 0.006f + p.ph));
                    g->drawPixel(x, y, c);
                }
            }
        }
        break;
    }
    case WX_NIGHT:
        for (int i = 0; i < n; i++) {
            Particle &p = s_p[i];
            float tw = 0.35f + 0.65f * (0.5f + 0.5f * sinf(now * 0.0015f * (1.0f + p.s) + p.ph));
            uint16_t c = fx_scale565(0xBDF7, (uint32_t)(14 * tw));
            if (p.s > 1.4f) g->fillCircle((int)p.x, (int)p.y, 1, c);
            else g->drawPixel((int)p.x, (int)p.y, c);
        }
        // a crescent moon
        g->fillCircle(CX + 118, CY - 132, 20, 0x4228);
        g->fillCircle(CX + 128, CY - 140, 18, COLOR_BG);
        break;
    case WX_PARTLY:
        sun(g, now, 0x6B20, 0x39E0);
        // fall through: plus a few clouds
    case WX_CLOUDY: {
        uint16_t c = s_kind == WX_CLOUDY ? 0x2965 : 0x2124;
        for (int i = 0; i < n; i++) {
            Particle &p = s_p[i];
            p.x += p.v * dt;
            if (p.x - 2 * p.s > W) { p.x = -2 * p.s; p.y = frand(40, H * 0.55f); }
            cloud(g, (int)p.x, (int)p.y, (int)p.s, c);
        }
        break;
    }
    case WX_FOG:
        for (int i = 0; i < n; i++) {
            Particle &p = s_p[i];
            p.x += p.v * dt * (i & 1 ? 1 : -1);
            if (p.x > W + p.s) p.x = -p.s;
            if (p.x < -p.s) p.x = W + p.s;
            float a = 0.5f + 0.5f * sinf(now * 0.0007f + p.ph);
            uint16_t c = fx_scale565(0x8C71, (uint32_t)(4 + 6 * a));
            for (int k = 0; k < 4; k++)
                g->drawFastHLine((int)(p.x - p.s / 2 + k * 9), (int)p.y + k * 5, (int)p.s - k * 18, c);
        }
        break;
    case WX_DRIZZLE: rain(g, n, dt, 0.08f, 0x31A9); break;
    case WX_RAIN: rain(g, n, dt, 0.12f, 0x4A6F); break;
    case WX_SHOWERS: rain(g, n, dt, 0.18f, 0x4A6F); break;
    case WX_STORM: {
        if (now >= s_next_flash) {
            s_flash_until = now + 120;
            s_next_flash = now + (uint32_t)frand(3500, 9000);
            s_bolt_x = (int)frand(110, W - 110);
        }
        bool flash = now < s_flash_until;
        if (flash) g->fillScreen(0x18E3);
        rain(g, n, dt, 0.22f, 0x3A2D);
        if (flash) {   // a jagged bolt from the top
            int x = s_bolt_x, y = 30;
            randomSeed(s_bolt_x);
            while (y < CY + 40) {
                int nx = x + (int)frand(-18, 18), ny = y + (int)frand(18, 34);
                g->drawLine(x, y, nx, ny, 0xCE7F);
                g->drawLine(x + 1, y, nx + 1, ny, 0x8C9F);
                x = nx;
                y = ny;
            }
            randomSeed(micros());
        }
        break;
    }
    case WX_SNOW:
        for (int i = 0; i < n; i++) {
            Particle &p = s_p[i];
            p.y += p.v * dt;
            p.x += 12.0f * sinf(now * 0.0012f + p.ph) * dt;
            if (p.y > H) { p.y = frand(-10, 0); p.x = frand(0, W); }
            g->fillCircle((int)p.x, (int)p.y, (int)(p.s + 0.5f), fx_scale565(0xDEFB, 14));
        }
        break;
    default: break;
    }
}
