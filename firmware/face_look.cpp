// The watch's look, from the app - see face_look.h.
#include "face_look.h"
#include "config.h"
#include "app_settings_state.h"
#include "complications.h"
#include "watchface_registry.h"
#include "face_look.h"
#include "hal_display.h"
#include "hal_nvs.h"
#include "phone_link.h"
#include "ui.h"
#include "ui_font.h"
#include <Arduino.h>

// violet (the default), blue, cyan, green, yellow, orange, pink, white
static const uint16_t ACCENTS[FACE_ACCENT_COUNT] = {COLOR_ACCENT, 0x2A9F, 0x1F5D, 0x9F45, 0xFF60, 0xFC60, 0xF8AA, 0xFFFF};

uint16_t face_accent() { return ACCENTS[g_app_settings.face_accent % FACE_ACCENT_COUNT]; }

static uint32_t look_hash() {
    const AppSettings &s = g_app_settings;
    uint32_t h = watchface_current();
    h = h * 31 + s.face_accent;
    h = h * 31 + (s.face_seconds | s.face_eyes << 1 | s.face_date << 2 | s.face_pet << 3 | s.weather_fx << 4 | s.aod_on << 5 |
                  s.clock_24h << 6 | s.menu_list << 7);
    h = h * 31 + s.text_size;
    h = h * 31 + s.brightness;
    for (int i = 0; i < COMP_SLOTS; i++) h = h * 31 + comp_slot(i);
    return h;
}
static uint32_t s_sent_hash = 0;

void look_send() {
    const AppSettings &s = g_app_settings;
    JsonDocument d;
    d["e"] = "look";
    d["wf"] = watchface_current();
    JsonArray f = d["faces"].to<JsonArray>();
    for (int i = 0; i < watchface_count(); i++) f.add(watchface_get(i)->name);
    d["acc"] = s.face_accent;
    d["sec"] = s.face_seconds ? 1 : 0;
    d["eyes"] = s.face_eyes ? 1 : 0;
    d["date"] = s.face_date ? 1 : 0;
    d["pet"] = s.face_pet ? 1 : 0;
    d["wx"] = s.weather_fx ? 1 : 0;
    d["aod"] = s.aod_on ? 1 : 0;
    d["h24"] = s.clock_24h ? 1 : 0;
    d["ts"] = s.text_size;
    d["ml"] = s.menu_list ? 1 : 0;
    d["br"] = s.brightness;
    JsonArray c = d["comp"].to<JsonArray>();
    for (int i = 0; i < COMP_SLOTS; i++) c.add(comp_key(comp_slot(i)));
    if (phone_link_send(d)) s_sent_hash = look_hash();
}

static void flag(JsonDocument &doc, const char *k, bool &v) {
    if (doc[k].is<int>()) v = (doc[k].as<int>()) != 0;
}

void look_on_link(JsonDocument &doc) {
    AppSettings &s = g_app_settings;
    if (doc["acc"].is<int>()) s.face_accent = (uint8_t)(doc["acc"].as<int>() % FACE_ACCENT_COUNT);
    flag(doc, "sec", s.face_seconds);
    flag(doc, "eyes", s.face_eyes);
    flag(doc, "date", s.face_date);
    flag(doc, "pet", s.face_pet);
    flag(doc, "wx", s.weather_fx);
    flag(doc, "aod", s.aod_on);
    flag(doc, "h24", s.clock_24h);
    flag(doc, "ml", s.menu_list);
    if (doc["ts"].is<int>()) s.text_size = (uint8_t)(doc["ts"].as<int>() % 3);
    if (doc["br"].is<int>()) {
        int b = doc["br"].as<int>();
        s.brightness = (uint8_t)(b < 8 ? 8 : b > 255 ? 255 : b);
        display_set_brightness(s.brightness);
    }
    nvs_save_settings(s);
    if (doc["wf"].is<int>()) {
        int i = doc["wf"].as<int>();
        const WatchfaceEntry *e = watchface_get(i);
        if (e && i != watchface_current()) {
            watchface_set_current(i);
            // wearing a face already: show the new one now
            Screen *top = ui_active_screen();
            bool on_face = false;
            for (int k = 0; k < watchface_count(); k++) on_face |= top == watchface_get(k)->screen;
            if (on_face) {
                ui_go_home();
                if (e->screen && !ui_screen_on_top(e->screen)) ui_push_instant(e->screen);
            }
        }
    }
    look_send();   // what it is now (the app's controls follow it)
}

void look_update() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 1500) return;
    last = now;
    if (!phone_link_active()) return;
    if (look_hash() != s_sent_hash) look_send();
}
