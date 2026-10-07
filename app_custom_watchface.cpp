#include <esp_heap_caps.h>
#include "complications.h"
#include "app_custom_watchface.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "apps.h"
#include "app_menu.h"
#include "watchface_registry.h"
#include "hal_rtc.h"
#include "hal_power.h"
#include "hal_imu.h"
#include "hal_ble.h"
#include "hal_sd.h"
#include "app_pet.h"
#if FEATURE_NVS
#include "hal_nvs.h"
#endif
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>

#define MAX_WIDGETS 12

struct CachedWidget {
    char type[20];
    int16_t x, y;
    uint16_t radius;
    uint8_t fs;
    uint16_t color;
    char text[32];
    char dataSrc[32];
};

static CachedWidget s_widgets[MAX_WIDGETS];
static int s_widget_count = 0;
static uint16_t s_bg_color = 0;
static bool s_has_layout = false;
static int s_last_parsed_len = -1;

static uint16_t parse_hex_color(const char *hex) {
    if (!hex || hex[0] != '#') return COLOR_TEXT;
    uint32_t v = 0;
    const char *p = hex + 1;
    while (*p) {
        char c = *p++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
    }
    uint8_t r = (v >> 16) & 0xFF;
    uint8_t g = (v >> 8) & 0xFF;
    uint8_t b = v & 0xFF;
    return COLOR565(r, g, b);
}

static bool parse_widgets_from_json(const char *json, int len) {
    DynamicJsonDocument doc(len * 3);
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        DEBUG_PRINTF("[custom_wf] JSON parse error: %s (len=%d)\n", err.c_str(), len);
        return false;
    }

    s_bg_color = parse_hex_color(doc["bg"] | "#000000");
    s_widget_count = 0;

    JsonArray widgets = doc["widgets"].as<JsonArray>();
    for (JsonObject w : widgets) {
        if (s_widget_count >= MAX_WIDGETS) break;
        CachedWidget &cw = s_widgets[s_widget_count++];

        const char *type = w["type"] | "text";
        strncpy(cw.type, type, sizeof(cw.type) - 1);
        cw.type[sizeof(cw.type) - 1] = '\0';

        cw.x = (int16_t)w["x"].as<int>();
        cw.y = (int16_t)w["y"].as<int>();
        cw.radius = (uint16_t)w["radius"].as<int>();
        cw.fs = (uint8_t)w["fontSize"].as<int>();
        cw.color = parse_hex_color(w["color"] | "#FFFFFF");

        const char *text = w["text"] | "";
        strncpy(cw.text, text, sizeof(cw.text) - 1);
        cw.text[sizeof(cw.text) - 1] = '\0';

        const char *dataSrc = w["dataSrc"] | "";
        strncpy(cw.dataSrc, dataSrc, sizeof(cw.dataSrc) - 1);
        cw.dataSrc[sizeof(cw.dataSrc) - 1] = '\0';

        DEBUG_PRINTF("[custom_wf] widget[%d]: type=%s x=%d y=%d r=%d fs=%d\n",
                     s_widget_count - 1, cw.type, cw.x, cw.y, cw.radius, cw.fs);
    }

    s_has_layout = (s_widget_count > 0);
    s_last_parsed_len = len;
    DEBUG_PRINTF("[custom_wf] parsed %d widgets, bg=0x%04X\n", s_widget_count, s_bg_color);
    return s_has_layout;
}

static void parse_layout() {
    const char *data = nullptr;
    int dlen = 0;
    if (!ble_get_watchface_data(&data, &dlen) || dlen < 2) {
        s_has_layout = false;
        return;
    }

    if (parse_widgets_from_json(data, dlen)) {
        ble_consume_watchface();
    }
}

static void custom_wf_create() {
    // Try BLE data first
    if (ble_watchface_ready()) {
        parse_layout();
    }
    // Fall back to NVS-persisted watchface
    if (!s_has_layout) {
#if FEATURE_NVS
        static char *nvs_buf = (char *)heap_caps_malloc(4096, (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); // PSRAM, not internal RAM
        int nvs_len = 0;
        if (nvs_buf && nvs_load_watchface_json(nvs_buf, 4096, &nvs_len) && nvs_len > 2) {
            if (parse_widgets_from_json(nvs_buf, nvs_len)) {
                DEBUG_PRINTF("[custom_wf] loaded from NVS\n");
            }
        }
#endif
    }
}

static void draw_text_centered(Arduino_GFX *g, int16_t x, int16_t y, const char *txt, uint8_t fontSize, uint16_t color) {
    uint8_t sz = fontSize / 6;
    if (sz < 1) sz = 1;
    if (sz > 8) sz = 8;
    g->setTextSize(sz);
    g->setTextColor(color);
    int tw = ui_text_width(txt, sz);
    ui_print(x - tw / 2, y - sz * 4, sz, color, txt);
}

static void draw_widget(Arduino_GFX *g, const CachedWidget &w, const WatchTime &t) {
    if (strcmp(w.type, "analog_clock") == 0) {
        g->drawCircle(w.x, w.y, w.radius, w.color);
        for (int i = 0; i < 12; i++) {
            float a = (i * 30 - 90) * 3.14159f / 180.0f;
            int ox = w.x + (int)(cosf(a) * w.radius * 0.85f);
            int oy = w.y + (int)(sinf(a) * w.radius * 0.85f);
            int ix = w.x + (int)(cosf(a) * w.radius * 0.72f);
            int iy = w.y + (int)(sinf(a) * w.radius * 0.72f);
            g->drawLine(ix, iy, ox, oy, w.color);
        }
        g->fillCircle(w.x, w.y, 4, w.color);
        float ha = ((t.hour % 12) * 30 + t.minute * 0.5f - 90) * 3.14159f / 180.0f;
        g->drawLine(w.x, w.y,
                    w.x + (int)(cosf(ha) * w.radius * 0.45f),
                    w.y + (int)(sinf(ha) * w.radius * 0.45f), w.color);
        float ma = (t.minute * 6 - 90) * 3.14159f / 180.0f;
        g->drawLine(w.x, w.y,
                    w.x + (int)(cosf(ma) * w.radius * 0.65f),
                    w.y + (int)(sinf(ma) * w.radius * 0.65f), w.color);
    }
    else if (strcmp(w.type, "complication") == 0) {
        comp_draw(g, comp_from_key(w.dataSrc), w.x, w.y, w.radius > 10 ? w.radius : 40, w.color);
    }
    else if (strcmp(w.type, "digital_time") == 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
        draw_text_centered(g, w.x, w.y, buf, w.fs, w.color);
    }
    else if (strcmp(w.type, "date") == 0) {
        static const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                     "Jul","Aug","Sep","Oct","Nov","Dec"};
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %d",
                 (t.month >= 1 && t.month <= 12) ? mons[t.month - 1] : "???",
                 t.day);
        draw_text_centered(g, w.x, w.y, buf, w.fs, w.color);
    }
    else if (strcmp(w.type, "battery") == 0) {
        int pct = power_get_battery_percent();
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%%", pct >= 0 ? pct : 0);
        draw_text_centered(g, w.x, w.y, buf, w.fs, w.color);
    }
    else if (strcmp(w.type, "steps") == 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", imu_get_step_count());
        draw_text_centered(g, w.x, w.y, buf, w.fs, w.color);
    }
    else if (strcmp(w.type, "weather") == 0) {
        const char *wdata = nullptr;
        int wlen = 0;
        if (ble_get_weather(&wdata, &wlen) && wlen > 2) {
            DynamicJsonDocument wdoc(wlen + 64);
            if (!deserializeJson(wdoc, wdata, wlen)) {
                float tempC = wdoc["tempC"] | 0.0f;
                int tempF = (int)(tempC * 9.0f / 5.0f + 32.0f);
                const char *cond = wdoc["condition"] | "";
                char buf[32];
                if (w.dataSrc[0] == 'f' || w.dataSrc[0] == 'F') {
                    snprintf(buf, sizeof(buf), "%dF", tempF);
                } else {
                    snprintf(buf, sizeof(buf), "%dC", (int)tempC);
                }
                draw_text_centered(g, w.x, w.y, buf, w.fs, w.color);
            }
        } else {
            draw_text_centered(g, w.x, w.y, "--", w.fs, w.color);
        }
    }
    else if (strcmp(w.type, "heart_rate") == 0) {
        // This board has no PPG/heart-rate sensor of its own - the only
        // source for a real number is whatever the phone pushes via
        // sendFitness() (see ble_get_fitness()/BleProtocol.encodeFitness
        // in the companion app). This used to just hardcode 72 here,
        // silently ignoring that data even though the phone-side push
        // pipeline and this getter both already existed and worked -
        // they just never got connected to each other.
        const char *fdata = nullptr;
        int flen = 0;
        char buf[16];
        if (ble_get_fitness(&fdata, &flen) && flen > 2) {
            DynamicJsonDocument fdoc(flen + 64);
            if (!deserializeJson(fdoc, fdata, flen)) {
                int hr = fdoc["heartRate"] | 0;
                if (hr > 0) {
                    snprintf(buf, sizeof(buf), "%d", hr);
                    draw_text_centered(g, w.x, w.y, buf, w.fs, w.color);
                } else {
                    draw_text_centered(g, w.x, w.y, "--", w.fs, w.color);
                }
            } else {
                draw_text_centered(g, w.x, w.y, "--", w.fs, w.color);
            }
        } else {
            draw_text_centered(g, w.x, w.y, "--", w.fs, w.color);
        }
    }
    else if (strcmp(w.type, "digital_seconds") == 0) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02d", t.second);
        draw_text_centered(g, w.x, w.y, buf, w.fs, w.color);
    }
    else if (strcmp(w.type, "weekday") == 0) {
        static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        int dow = t.weekday;
        if (dow < 0 || dow > 6) dow = 0;
        draw_text_centered(g, w.x, w.y, days[dow], w.fs, w.color);
    }
    else if (strcmp(w.type, "ampm") == 0) {
        draw_text_centered(g, w.x, w.y, t.hour >= 12 ? "PM" : "AM", w.fs, w.color);
    }
    else if (strcmp(w.type, "text") == 0) {
        const char *txt = w.text[0] ? w.text : "Text";
        draw_text_centered(g, w.x, w.y, txt, w.fs, w.color);
    }
    else if (strcmp(w.type, "seconds_ring") == 0) {
        g->drawCircle(w.x, w.y, w.radius, w.color);
        float sa = (t.second / 60.0f) * 2.0f * 3.14159f - 1.5708f;
        int sx = w.x + (int)(cosf(sa) * w.radius);
        int sy = w.y + (int)(sinf(sa) * w.radius);
        g->fillCircle(sx, sy, 6, w.color);
    }
}

static void custom_wf_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    if (ble_watchface_ready()) {
        // ble_watchface_ready() already tracks "is there new, unconsumed
        // data" correctly (hal_ble.cpp's ble_consume_watchface() is what
        // clears it, called from parse_layout() below on a successful
        // parse) - that's the right signal to act on by itself.
        //
        // This used to also require dlen != s_last_parsed_len before
        // re-parsing, using the payload's byte length as a proxy for
        // "did the content change." That's unreliable - plenty of edits
        // (a color tweak, a widget moved a few pixels) leave the JSON's
        // total length unchanged - and it was worse than just skipping
        // one redraw: since parse_layout() is the only thing that calls
        // ble_consume_watchface(), a same-length update that got
        // skipped here never got consumed either, so the same stale
        // length comparison kept blocking it on every subsequent frame
        // too. A same-length watchface update from the phone would
        // never apply at all, not just late - only a later,
        // different-length update (or a reboot) would ever get through.
        parse_layout();
    }

    if (!s_has_layout) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_TEXT_DIM, "No custom", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 20, COLOR_TEXT_DIM, "watchface set", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 50, COLOR_TEXT_DIM, "Use companion app", 2);
        pet_step_and_draw(g);
        return;
    }

    g->fillScreen(s_bg_color);

    WatchTime t = rtc_now();
    if (!t.valid) { pet_step_and_draw(g); return; }

    for (int i = 0; i < s_widget_count; i++) {
        draw_widget(g, s_widgets[i], t);
    }
    pet_step_and_draw(g); // drawn last so widgets never paint over it
}

static void custom_wf_gesture(Gesture g) {
    switch (g) {
        case GESTURE_SWIPE_LEFT:
            ui_push(&menu_screen);
            break;
        case GESTURE_SWIPE_RIGHT:
            ui_push(&notifications_screen);
            break;
        case GESTURE_SWIPE_UP:
            watchface_cycle_next();
            break;
        default:
            break;
    }
}

static void custom_wf_touch(int x, int y, bool pressed) {
    pet_handle_touch(x, y, pressed);
}

Screen custom_watchface_screen = {
    nullptr, GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    custom_wf_create, custom_wf_draw, custom_wf_touch, nullptr, nullptr, custom_wf_gesture,
    1000, // idle_frame_ms - same reasoning as watchface_minimal_screen
};
