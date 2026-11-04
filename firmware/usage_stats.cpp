// How the watch is used - see usage_stats.h.
#include "usage_stats.h"
#include "config.h"
#include "ui.h"
#include "diag.h"
#include "phone_link.h"
#include "watchface_registry.h"
#include "hal_ble.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_sd.h"
#include "hal_sleep.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <string.h>

static const int BAT_N = 144;                 // 24 h at one sample every 10 minutes
static uint8_t s_bat[BAT_N];                  // oldest first, 255 = none
static uint8_t s_chg[BAT_N];
static uint16_t s_on[24], s_on_y[24];         // screen-on seconds per hour, today / yesterday
static uint8_t s_wk[24];                      // wake-ups per hour today
struct AppTime { char name[18]; uint32_t secs; };
static const int APP_N = 16;
static AppTime s_apps[APP_N];
static uint32_t s_wakes = 0, s_disc = 0, s_ntf = 0;
static uint32_t s_min_heap_kb = 0xFFFFFFFF;
static int s_day = -1;
static uint32_t s_last_bat_ms = 0, s_last_save_ms = 0, s_last_sec_ms = 0;
static bool s_was_asleep = false, s_was_conn = false;

void usage_count_notification() { s_ntf++; }

static void save() {
    Preferences p;
    if (!p.begin("usage", false)) return;
    p.putBytes("bat", s_bat, sizeof(s_bat));
    p.putBytes("chg", s_chg, sizeof(s_chg));
    p.putBytes("on", s_on, sizeof(s_on));
    p.putBytes("ony", s_on_y, sizeof(s_on_y));
    p.putBytes("wk", s_wk, sizeof(s_wk));
    p.putBytes("apps", s_apps, sizeof(s_apps));
    p.putUInt("wakes", s_wakes);
    p.putUInt("disc", s_disc);
    p.putUInt("ntf", s_ntf);
    p.putShort("day", (int16_t)s_day);
    p.end();
    s_last_save_ms = millis();
}

void usage_init() {
    memset(s_bat, 255, sizeof(s_bat));
    Preferences p;
    if (!p.begin("usage", true)) return;
    if (p.getBytesLength("bat") == sizeof(s_bat)) p.getBytes("bat", s_bat, sizeof(s_bat));
    if (p.getBytesLength("chg") == sizeof(s_chg)) p.getBytes("chg", s_chg, sizeof(s_chg));
    if (p.getBytesLength("on") == sizeof(s_on)) p.getBytes("on", s_on, sizeof(s_on));
    if (p.getBytesLength("ony") == sizeof(s_on_y)) p.getBytes("ony", s_on_y, sizeof(s_on_y));
    if (p.getBytesLength("wk") == sizeof(s_wk)) p.getBytes("wk", s_wk, sizeof(s_wk));
    if (p.getBytesLength("apps") == sizeof(s_apps)) p.getBytes("apps", s_apps, sizeof(s_apps));
    s_wakes = p.getUInt("wakes", 0);
    s_disc = p.getUInt("disc", 0);
    s_ntf = p.getUInt("ntf", 0);
    s_day = p.getShort("day", -1);
    p.end();
    // the time asleep while powered off isn't known: mark the gap
    s_bat[BAT_N - 1] = 255;
}

// What to call the screen on top: its title, else the face's name, else "Other".
static void screen_name(char *out, int n) {
    Screen *s = ui_active_screen();
    out[0] = 0;
    if (!s) return;
    for (int i = 0; i < watchface_count(); i++)
        if (watchface_get(i)->screen == s) { strlcpy(out, "Watch face", n); return; }
    if (s->title && s->title[0]) { strlcpy(out, s->title, n); return; }
    strlcpy(out, "Other", n);
}

static void add_app_time(const char *name) {
    if (!name[0]) return;
    int free_i = -1, min_i = 0;
    for (int i = 0; i < APP_N; i++) {
        if (!strcmp(s_apps[i].name, name)) { s_apps[i].secs++; return; }
        if (!s_apps[i].name[0] && free_i < 0) free_i = i;
        if (s_apps[i].secs < s_apps[min_i].secs) min_i = i;
    }
    int i = free_i >= 0 ? free_i : min_i;   // full: the least used one makes room
    strlcpy(s_apps[i].name, name, sizeof(s_apps[i].name));
    s_apps[i].secs = 1;
}

static void new_day() {
    memcpy(s_on_y, s_on, sizeof(s_on));
    memset(s_on, 0, sizeof(s_on));
    memset(s_wk, 0, sizeof(s_wk));
    memset(s_apps, 0, sizeof(s_apps));
    s_wakes = s_disc = s_ntf = 0;
}

void usage_update() {
    uint32_t ms = millis();
    if (ms - s_last_sec_ms < 1000) return;
    s_last_sec_ms = ms;
    WatchTime t = rtc_now();
    if (t.valid && t.day != s_day) {
        if (s_day >= 0) new_day();
        s_day = t.day;
    }
    int h = t.valid ? t.hour : 0;
    bool asleep = sleep_is_asleep();
    if (!asleep) {
        if (s_on[h] < 3600) s_on[h]++;
        char name[18];
        screen_name(name, sizeof(name));
        add_app_time(name);
    }
    if (s_was_asleep && !asleep) { s_wakes++; if (s_wk[h] < 255) s_wk[h]++; }
    s_was_asleep = asleep;
    bool conn = ble_is_connected();
    if (s_was_conn && !conn) s_disc++;
    s_was_conn = conn;
    uint32_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
    if (heap < s_min_heap_kb) s_min_heap_kb = heap;
    // the battery, every 10 minutes
    if (!s_last_bat_ms || ms - s_last_bat_ms >= 600000UL) {
        s_last_bat_ms = ms ? ms : 1;
        int pct = power_get_battery_percent();
        memmove(s_bat, s_bat + 1, BAT_N - 1);
        memmove(s_chg, s_chg + 1, BAT_N - 1);
        s_bat[BAT_N - 1] = pct < 0 ? 255 : (uint8_t)pct;
        s_chg[BAT_N - 1] = power_is_charging() ? 1 : 0;
    }
    if (ms - s_last_save_ms > 3600000UL) save();
}

void usage_send() {
    {
        JsonDocument d;
        d["e"] = "usage"; d["p"] = 0; d["iv"] = 10;
        // two hex digits a sample (a JSON array of 144 numbers won't fit one message)
        static const char HEX_D[] = "0123456789abcdef";
        char bat[BAT_N * 2 + 1];
        for (int i = 0; i < BAT_N; i++) { bat[i * 2] = HEX_D[s_bat[i] >> 4]; bat[i * 2 + 1] = HEX_D[s_bat[i] & 15]; }
        bat[BAT_N * 2] = 0;
        d["bat"] = bat;
        char chg[BAT_N + 1];
        for (int i = 0; i < BAT_N; i++) chg[i] = s_chg[i] ? '1' : '0';
        chg[BAT_N] = 0;
        d["chg"] = chg;
        phone_link_send(d);
    }
    {
        JsonDocument d;
        d["e"] = "usage"; d["p"] = 1;
        JsonArray on = d["on"].to<JsonArray>(), wk = d["wk"].to<JsonArray>(), ony = d["ony"].to<JsonArray>();
        for (int i = 0; i < 24; i++) { on.add(s_on[i]); wk.add(s_wk[i]); ony.add(s_on_y[i]); }
        phone_link_send(d);
    }
    {
        JsonDocument d;
        d["e"] = "usage"; d["p"] = 2;
        JsonArray apps = d["apps"].to<JsonArray>();
        // the 12 used most, most first
        bool used[APP_N] = {false};
        for (int k = 0; k < 12; k++) {
            int best = -1;
            for (int i = 0; i < APP_N; i++)
                if (!used[i] && s_apps[i].name[0] && (best < 0 || s_apps[i].secs > s_apps[best].secs)) best = i;
            if (best < 0) break;
            used[best] = true;
            JsonArray a = apps.add<JsonArray>();
            a.add(s_apps[best].name);
            a.add(s_apps[best].secs);
        }
        d["wake"] = s_wakes;
        d["disc"] = s_disc;
        d["ntf"] = s_ntf;
        d["boot"] = diag_boot_count();
        d["crash"] = diag_crash_count();
        d["heap"] = s_min_heap_kb == 0xFFFFFFFF ? 0 : s_min_heap_kb;
        d["psram"] = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
        if (sd_is_mounted()) {
            JsonArray sd = d["sd"].to<JsonArray>();
            sd.add((uint32_t)sd_card_size_mb());
            sd.add((uint32_t)sd_used_mb());
        }
        d["up"] = (uint32_t)(millis() / 1000);
        d["rr"] = diag_reset_reason();
        phone_link_send(d);
    }
}
