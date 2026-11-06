// How the watch is used - see usage_stats.h.
#include "usage_stats.h"
#include "store.h"
#include "config.h"
#include "ui.h"
#include "diag.h"
#include "phone_link.h"
#include "watchface_registry.h"
#include "app_tiles.h"
#include "app_wallet.h"
#include "app_camera.h"
#include "hal_ble.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_sd.h"
#include "hal_sleep.h"
#include "hal_audio.h"
#include "hal_espnow.h"
#include "hal_gps.h"
#include "hal_wifi.h"
#include "app_settings_state.h"
#include "hal_usb.h"
#include "voice_sr.h"
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

// What kept the watch from sleeping deeply with the screen off, in seconds
// today (keys as sent: see usage_stats.h).
enum AwakeWhy { AW_BUILD, AW_SETTING, AW_WAKEWORD, AW_AOD, AW_GPS, AW_WIFI, AW_ESPNOW, AW_AUDIO, AW_USBMODE, AW_N };
static const char *const AW_KEYS[AW_N] = {"build", "setting", "wakeword", "aod", "gps", "wifi", "espnow", "audio", "usbmode"};
static uint32_t s_aw[AW_N];
static uint32_t s_off_s = 0;   // seconds with the screen off today (unplugged)

// How long the parts that cost the most beyond the screen were in use today
// (unplugged, screen on or off): the app turns them into battery estimates.
enum FeatTime { FT_SPEAKER, FT_MIC, FT_GPS, FT_WIFI, FT_N };
static const char *const FT_KEYS[FT_N] = {"speaker", "mic", "gps", "wifi"};
static uint32_t s_ft[FT_N];

// The last 7 days, newest first (part 4): kept with the rest in the store.
struct DaySum { uint32_t date; uint16_t on_min, wakes; uint8_t drop, charges; uint16_t pad; };
static DaySum s_week[7];
static uint8_t s_drop_today = 0, s_charges_today = 0;   // battery % used unplugged, times put on charge
static uint32_t s_date_today = 0;                        // yyyymmdd

// everything above, as one file in the store (store.h)
struct UsageFile {
    uint32_t magic;
    uint8_t bat[BAT_N], chg[BAT_N];
    uint16_t on[24], ony[24];
    uint8_t wk[24];
    AppTime apps[APP_N];
    uint32_t wakes, disc, ntf;
    int16_t day;
    uint8_t drop, charges;
    uint32_t date;
    DaySum week[7];
};
static const uint32_t USAGE_MAGIC = 0x31475355;   // "USG1"

void usage_count_notification() { s_ntf++; }

static void save() {
    s_last_save_ms = millis();
    if (store_ok()) {
        UsageFile *f = (UsageFile *)heap_caps_malloc(sizeof(UsageFile), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (f) {
            f->magic = USAGE_MAGIC;
            memcpy(f->bat, s_bat, sizeof(s_bat)); memcpy(f->chg, s_chg, sizeof(s_chg));
            memcpy(f->on, s_on, sizeof(s_on)); memcpy(f->ony, s_on_y, sizeof(s_on_y));
            memcpy(f->wk, s_wk, sizeof(s_wk)); memcpy(f->apps, s_apps, sizeof(s_apps));
            f->wakes = s_wakes; f->disc = s_disc; f->ntf = s_ntf; f->day = (int16_t)s_day;
            f->drop = s_drop_today; f->charges = s_charges_today; f->date = s_date_today;
            memcpy(f->week, s_week, sizeof(s_week));
            bool ok = store_write("usage", f, sizeof(UsageFile));
            heap_caps_free(f);
            if (ok) {
                Preferences p;
                if (p.begin("usage", false)) {
                    if (p.isKey("bat")) p.clear();   // moved: frees NVS
                    p.end();
                }
                return;
            }
        }
    }
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
    p.putBytes("ft", s_ft, sizeof(s_ft));
    p.end();
}

void usage_init() {
    memset(s_bat, 255, sizeof(s_bat));
    if (store_size("usage") == (int)sizeof(UsageFile)) {
        UsageFile *f = (UsageFile *)heap_caps_malloc(sizeof(UsageFile), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (f && store_read("usage", f, sizeof(UsageFile)) == (int)sizeof(UsageFile) && f->magic == USAGE_MAGIC) {
            memcpy(s_bat, f->bat, sizeof(s_bat)); memcpy(s_chg, f->chg, sizeof(s_chg));
            memcpy(s_on, f->on, sizeof(s_on)); memcpy(s_on_y, f->ony, sizeof(s_on_y));
            memcpy(s_wk, f->wk, sizeof(s_wk)); memcpy(s_apps, f->apps, sizeof(s_apps));
            s_wakes = f->wakes; s_disc = f->disc; s_ntf = f->ntf; s_day = f->day;
            s_drop_today = f->drop; s_charges_today = f->charges; s_date_today = f->date;
            memcpy(s_week, f->week, sizeof(s_week));
            for (auto &a : s_apps) a.name[sizeof(a.name) - 1] = 0;
            heap_caps_free(f);
            s_bat[BAT_N - 1] = 255;   // the time powered off isn't known: mark the gap
            return;
        }
        if (f) heap_caps_free(f);
    }
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
    if (p.getBytesLength("ft") == sizeof(s_ft)) p.getBytes("ft", s_ft, sizeof(s_ft));
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
    if (s == &tiles_screen) { strlcpy(out, "Tiles", n); return; }
    if (s == &wallet_screen) { strlcpy(out, "Wallet", n); return; }
    if (s == &camera_screen) { strlcpy(out, "Camera", n); return; }
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
    // yesterday into the week
    if (s_date_today) {
        uint32_t on = 0;
        for (int h = 0; h < 24; h++) on += s_on[h];
        memmove(&s_week[1], &s_week[0], sizeof(DaySum) * 6);
        s_week[0] = {s_date_today, (uint16_t)(on / 60), (uint16_t)(s_wakes > 65535 ? 65535 : s_wakes), s_drop_today,
                     s_charges_today, 0};
    }
    s_drop_today = s_charges_today = 0;
    memcpy(s_on_y, s_on, sizeof(s_on));
    memset(s_on, 0, sizeof(s_on));
    memset(s_wk, 0, sizeof(s_wk));
    memset(s_apps, 0, sizeof(s_apps));
    s_wakes = s_disc = s_ntf = 0;
    memset(s_aw, 0, sizeof(s_aw));
    memset(s_ft, 0, sizeof(s_ft));
    s_off_s = 0;
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
    if (t.valid) s_date_today = (uint32_t)t.year * 10000 + t.month * 100 + t.day;
    int h = t.valid ? t.hour : 0;
    bool asleep = sleep_is_asleep();
    if (!asleep) {
        if (s_on[h] < 3600) s_on[h]++;
        char name[18];
        screen_name(name, sizeof(name));
        add_app_time(name);
    }
    if (s_was_asleep && !asleep) { s_wakes++; if (s_wk[h] < 255) s_wk[h]++; }
    static bool plugged = false;
    static uint32_t plugged_ms = 0;
    if (!plugged_ms || ms - plugged_ms > 10000) { plugged_ms = ms ? ms : 1; plugged = power_vbus_present(); }   // the PMU, every 10 s
    if (!plugged) {
#if FEATURE_AUDIO
        if (audio_is_recording()) s_ft[FT_MIC]++;
        else if (audio_is_playing()) s_ft[FT_SPEAKER]++;
#endif
#if FEATURE_GPS
        if (gps_is_enabled()) s_ft[FT_GPS]++;
#endif
#if FEATURE_WIFI
        if (wifi_is_enabled()) s_ft[FT_WIFI]++;
#endif
    }
    if (asleep && !plugged) {
        s_off_s++;
        if (!power_auto_sleep_available()) s_aw[AW_BUILD]++;
        else if (!g_app_settings.power_saving) s_aw[AW_SETTING]++;
#if FEATURE_AUDIO
        if (voice_wake_keeps_awake()) s_aw[AW_WAKEWORD]++;
        if (audio_is_playing()) s_aw[AW_AUDIO]++;
#endif
        if (g_app_settings.aod_on) s_aw[AW_AOD]++;
#if FEATURE_GPS
        if (gps_is_enabled()) s_aw[AW_GPS]++;
#endif
#if FEATURE_WIFI
        if (wifi_is_enabled()) s_aw[AW_WIFI]++;
#endif
        if (espnow_is_enabled()) s_aw[AW_ESPNOW]++;
        if (usb_mode_blocks_light_sleep()) s_aw[AW_USBMODE]++;
    }
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
        // today's totals for the week
        uint8_t a = s_bat[BAT_N - 2], b = s_bat[BAT_N - 1];
        if (a != 255 && b != 255 && !s_chg[BAT_N - 1] && !s_chg[BAT_N - 2] && b < a && s_drop_today < 250) s_drop_today += a - b;
        if (s_chg[BAT_N - 1] && !s_chg[BAT_N - 2] && s_charges_today < 255) s_charges_today++;
    }
    if (ms - s_last_save_ms > 3600000UL) save();
}

// The average drain over the unplugged part of the last 24 hours, %/hour
// (screen on and off together); < 0 without 2 hours of it.
float usage_drain_per_hour() {
    int drop = 0, n = 0;
    for (int i = 1; i < BAT_N; i++) {
        uint8_t a = s_bat[i - 1], b = s_bat[i];
        if (a == 255 || b == 255 || s_chg[i] || s_chg[i - 1] || b > a) continue;
        drop += a - b;
        n++;
    }
    if (n < 12) return -1;   // 12 x 10 min
    return drop / (n / 6.0f);
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
    {
        JsonDocument d;
        d["e"] = "usage"; d["p"] = 3;
        d["off"] = s_off_s;
        d["pm"] = power_auto_sleep_available() ? 1 : 0;
        JsonObject aw = d["aw"].to<JsonObject>();
        for (int i = 0; i < AW_N; i++)
            if (s_aw[i]) aw[AW_KEYS[i]] = s_aw[i];
        JsonObject ft = d["ft"].to<JsonObject>();
        for (int i = 0; i < FT_N; i++)
            if (s_ft[i]) ft[FT_KEYS[i]] = s_ft[i];
        phone_link_send(d);
    }
    {
        // the last 7 days: [yyyymmdd, screen-on min, wake-ups, battery % used unplugged, charges]
        JsonDocument d;
        d["e"] = "usage"; d["p"] = 4;
        JsonArray w = d["wk"].to<JsonArray>();
        for (auto &x : s_week) {
            if (!x.date) continue;
            JsonArray r = w.add<JsonArray>();
            r.add(x.date); r.add(x.on_min); r.add(x.wakes); r.add(x.drop); r.add(x.charges);
        }
        phone_link_send(d);
    }
}
