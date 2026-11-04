#include "app_photo_watchface.h"
#include "watchface_registry.h"
#include "app_assistant.h"
#include "phone_link.h"
#include "app_clock.h"
#include "app_weather.h"
#include "app_reminders.h"
#include "app_workout.h"
#include "app_sos.h"
#include "face_look.h"
#include "home_gestures.h"
#include "app_wallet.h"
#include "app_life.h"
#include "usage_stats.h"
#include "fitness.h"
#include "complications.h"
#include "dnd.h"
#include "hal_nvs.h"
#include "config.h"
#include "hal_ble.h"
#include "hal_power.h"
#include "hal_imu.h"
#include "hal_sleep.h"
#include "app_notifications.h"
#include "app_settings_state.h"
#include "ui.h"
#include "ui_font.h"
#include "diag.h"
#include "app_calendar.h"
#include "app_navigation.h"
#include "hal_wifi_xfer.h"
#include "wifi_cfg.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <esp_heap_caps.h>

extern Screen call_screen;
bool call_screen_is_open();
void dictate_on_result(uint32_t id, uint32_t seq, const char *text, const char *err);

static bool       s_active = false;
static bool       s_was_connected = false;
static uint32_t   s_connected_ms = 0;
static uint32_t   s_last_report_ms = 0;
static int        s_phone_bat = -1;
static bool       s_phone_chg = false;
static PhoneMedia s_media = {};
static PhoneCall  s_call = {};
static bool       s_finding = false;

static void copy_str(char *dst, size_t n, const char *src) {
    if (!src) src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
    ble_fold_utf8(dst);
    ui_utf8_trim(dst);
}

static bool send_doc(JsonDocument &doc) {
    char buf[720];   // replies (dictated text) can be a few hundred bytes
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return ble_link_send(buf);
}

bool phone_link_send(JsonDocument &doc) { return send_doc(doc); }

// Watch status for the phone: battery, steps, charging.
static void send_report(const char *ev) {
    JsonDocument doc;
    doc["e"] = ev;
    doc["bat"] = power_get_battery_percent();
    doc["chg"] = power_is_charging() ? 1 : 0;
    doc["st"] = (uint32_t)imu_get_step_count();
    // activity rings today, and their goals
    doc["mv"] = (int)fitness_move_kcal();
    doc["ex"] = fitness_exercise_min();
    doc["sd"] = fitness_stand_hours();
    doc["gm"] = fitness_move_goal();
    doc["ge"] = fitness_exercise_goal();
    doc["gs"] = fitness_stand_goal();
    doc["kg"] = g_app_settings.weight_kg;
    doc["cm"] = g_app_settings.height_cm;
    if (!strcmp(ev, "hi")) {
        // Once per connection: firmware version, why it last reset, and
        // whether there's a crash report the app hasn't fetched yet.
        doc["fw"] = diag_fw_version();
        doc["rr"] = diag_reset_reason();
        doc["cr"] = diag_crash_id();
        doc["up"] = (uint32_t)(millis() / 1000);
        // the last days' rings: [date yyyymmdd, kcal, exercise min, stand h, steps]
        FitDay h[7];
        int n = fitness_history(h, 7);
        JsonArray fh = doc["fh"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonArray d = fh.add<JsonArray>();
            d.add(h[i].date); d.add(h[i].move_kcal); d.add(h[i].exercise_min); d.add(h[i].stand_hours); d.add(h[i].steps);
        }
    }
    send_doc(doc);
    s_last_report_ms = millis();
}

static void wake_screen() {
    sleep_register_activity();
}

static void handle(const char *msg) {
    JsonDocument doc;
    if (deserializeJson(doc, msg)) {
        DEBUG_PRINTF("[link] bad JSON: %.60s\n", msg);
        return;
    }
    const char *t = doc["t"] | "";

    if (!strcmp(t, "hi")) {
        s_active = true;
        send_report("hi");
        workout_send_pending();   // workouts recorded while the phone was away
        look_send();              // the app's Faces tab shows what the watch shows
        home_send();              // ...and what each gesture on the face does
        wallet_send();            // the passes the watch has (the app sends what's missing)
        life_send(true);          // the Life screen: today and the last days
    } else if (!strcmp(t, "bat")) {
        s_active = true;
        s_phone_bat = doc["l"] | -1;
        s_phone_chg = (doc["c"] | 0) != 0;
    } else if (!strcmp(t, "med")) {
        s_active = true;
        copy_str(s_media.title, sizeof(s_media.title), doc["ti"] | "");
        copy_str(s_media.artist, sizeof(s_media.artist), doc["ar"] | "");
        s_media.playing = (doc["pl"] | 0) != 0;
        s_media.position_s = doc["po"] | 0;
        s_media.duration_s = doc["du"] | 0;
        s_media.volume = doc["v"] | 0;
        s_media.volume_max = doc["vm"] | 15;
        s_media.art = doc["ah"] | 0u;
        s_media.valid = s_media.title[0] != 0;
        s_media.updated_ms = millis();
    } else if (!strcmp(t, "ntf")) {
        s_active = true;
        notifications_add(doc["id"] | 0u, doc["ap"] | "Phone", doc["ti"] | "", doc["tx"] | "",
                          (doc["rp"] | 0) != 0, doc["ic"] | 0u);
    } else if (!strcmp(t, "sr")) {
        // Smart replies for one message: written by the phone's LLM.
        const char *items[NOTIF_SMART_MAX];
        int n = 0;
        for (JsonVariant v : doc["l"].as<JsonArray>()) {
            if (n < NOTIF_SMART_MAX && v.is<const char *>()) items[n++] = v.as<const char *>();
        }
        notifications_set_smart_replies(doc["id"] | 0u, items, n);
    } else if (!strcmp(t, "tmr")) {
        // a timer asked for on the phone's assistant
        assistant_timer_set((uint32_t)(doc["s"] | 0));
    } else if (!strcmp(t, "digest")) {
        notifications_on_digest(doc["tx"] | "", doc["err"] | "");
    } else if (!strcmp(t, "tr")) {
        notifications_on_translation(doc["id"] | 0u, doc["tx"] | "", doc["err"] | "");
    } else if (!strcmp(t, "nrm")) {
        notifications_remove(doc["id"] | 0u);
    } else if (!strcmp(t, "call")) {
        s_active = true;
        const char *st = doc["st"] | "end";
        PhoneCallState ns = !strcmp(st, "ring") ? CALL_RINGING : !strcmp(st, "active") ? CALL_ACTIVE : CALL_NONE;
        if (ns != s_call.state) s_call.since_ms = millis();
        s_call.state = ns;
        s_call.id = doc["id"] | 0u;
        if (doc["n"].is<const char *>()) copy_str(s_call.name, sizeof(s_call.name), doc["n"]);
        if (ns == CALL_RINGING && !call_screen_is_open()) {
            wake_screen();
            ui_push(&call_screen);
        }
    } else if (!strcmp(t, "find")) {
        s_finding = (doc["on"] | 0) != 0;
    } else if (!strcmp(t, "req")) {
        send_report("st");
    } else if (!strcmp(t, "cal")) {
        s_active = true;
        calendar_on_link(doc);
    } else if (!strcmp(t, "nav")) {
        s_active = true;
        navigation_on_link(doc, false);
    } else if (!strcmp(t, "navi")) {
        navigation_on_link(doc, true);
    } else if (!strcmp(t, "wifi")) {
        wifi_xfer_request((doc["on"] | 0) != 0);
    } else if (!strcmp(t, "wcfg")) {
        wifi_cfg_handle(doc);
    } else if (!strcmp(t, "comp")) {
        JsonArray l = doc["s"].as<JsonArray>();
        int i = 0;
        for (JsonVariant v : l) {
            if (i >= COMP_SLOTS) break;
            comp_set_slot(i++, comp_from_key(v.as<const char *>()));
        }
    } else if (!strcmp(t, "usage")) {
        usage_send();   // the app's Diagnostics: battery, screen time, apps
    } else if (!strcmp(t, "body")) {
        // weight and height from the app's Activity screen
        int kg = doc["kg"] | 0, cm = doc["cm"] | 0;
        if (kg >= 30 && kg <= 250) g_app_settings.weight_kg = (uint8_t)kg;
        if (cm >= 100 && cm <= 230) g_app_settings.height_cm = (uint8_t)cm;
        nvs_save_settings(g_app_settings);
        send_report("st");
    } else if (!strcmp(t, "life")) {
        life_on_plan(doc);
    } else if (!strcmp(t, "meals")) {
        life_on_meals(doc);
    } else if (!strcmp(t, "look")) {
        look_on_link(doc);
    } else if (!strcmp(t, "home")) {
        home_on_link(doc);
    } else if (!strcmp(t, "wal")) {
        wallet_on_link(doc);
    } else if (!strcmp(t, "sos")) {
        sos_on_reply((doc["ok"] | 0) != 0, doc["why"] | "");
    } else if (!strcmp(t, "woack")) {
        workout_on_ack(doc["st"] | 0u);
    } else if (!strcmp(t, "tz")) {
        clock_set_utc_offset(doc["o"] | 0);
    } else if (!strcmp(t, "wxh")) {
        weather_on_hourly(doc);
    } else if (!strcmp(t, "todos")) {
        reminders_on_link(doc);
    } else if (!strcmp(t, "todo")) {
        comp_set_todos(doc["n"] | 0, doc["top"] | "");
    } else if (!strcmp(t, "dnd")) {
        if (doc["on"].is<int>()) dnd_set((doc["on"] | 0) != 0, true);
        if (doc["bed"].is<int>()) {            // bedtime schedule from the app
            g_app_settings.dnd_bedtime = (doc["bed"] | 0) != 0;
            int f = doc["from"] | (int)g_app_settings.dnd_from_min;
            int to = doc["to"] | (int)g_app_settings.dnd_to_min;
            if (f >= 0 && f < 1440) g_app_settings.dnd_from_min = (uint16_t)f;
            if (to >= 0 && to < 1440) g_app_settings.dnd_to_min = (uint16_t)to;
            nvs_save_settings(g_app_settings);
        }
    } else if (!strcmp(t, "qr")) {
        JsonArray l = doc["l"].as<JsonArray>();
        const char *items[10];
        int n = 0;
        for (JsonVariant v : l) {
            if (n >= 10) break;
            items[n++] = v.as<const char *>();
        }
        quick_replies_set(items, n);
    } else if (!strcmp(t, "dict")) {
        dictate_on_result(doc["id"] | 0u, doc["n"] | 0u, doc["tx"] | "", doc["err"] | "");
    } else if (!strcmp(t, "ask")) {
        uint32_t n = doc["n"] | 0u;
        if (doc["st"].is<const char *>()) assistant_on_status(n, doc["st"]);
        else assistant_on_result(n, doc["q"] | "", doc["a"] | "", doc["do"] | "", doc["s"] | 0, doc["err"] | "");
    } else if (!strcmp(t, "pf")) {
        // Photo faces: {"a":"list"} / {"a":"show","n":..} / {"a":"del","n":..} / {"a":"rot","m":0|1|2}
        const char *a = doc["a"] | "list";
        const char *n = doc["n"] | "";
        if (!strcmp(a, "show") && photo_face_show(n)) {
            for (int i = 0; i < watchface_count(); i++)
                if (watchface_get(i)->screen == &photo_watchface_screen) watchface_set_current(i);
            ui_go_home();
            if (!ui_screen_on_top(&photo_watchface_screen)) ui_push(&photo_watchface_screen);
        } else if (!strcmp(a, "del")) {
            photo_face_delete(n);
        } else if (!strcmp(a, "rot")) {
            int m = doc["m"] | 0;
            if (m >= 0 && m <= 2) { g_app_settings.photo_rotate = (uint8_t)m; nvs_save_settings(g_app_settings); }
        }
        phone_link_send_photo_faces();
    } else if (!strcmp(t, "crash")) {
        // Crash report + status text for the app's Diagnostics page.
        if (doc["clr"] | 0) diag_clear_crash();
        // Scratch space in PSRAM - internal RAM is kept for the BLE stack.
        const size_t STATUS_N = 600, BIG_N = 2400;
        char *status = (char *)heap_caps_malloc(STATUS_N + BIG_N, MALLOC_CAP_SPIRAM);
        if (status) {
            char *big = status + STATUS_N;
            diag_status_text(status, STATUS_N);
            JsonDocument out;
            out["e"] = "crash";
            out["id"] = diag_crash_id();
            out["txt"] = diag_crash_text();
            out["sys"] = status;
            size_t n = serializeJson(out, big, BIG_N);
            if (n > 0 && n < BIG_N) ble_link_send(big);
            heap_caps_free(status);
        }
    }
}

// The photo faces on the card, which one is shown and how they change.
void phone_link_send_photo_faces() {
    JsonDocument doc;
    doc["e"] = "pf";
    JsonArray l = doc["l"].to<JsonArray>();
    for (int i = 0; i < photo_face_count(); i++) l.add(photo_face_name(i));
    doc["cur"] = photo_face_current();
    doc["rot"] = g_app_settings.photo_rotate;
    doc["max"] = PHOTO_FACE_MAX;
    send_doc(doc);
}

void phone_link_update() {
    bool conn = ble_is_connected();
    if (conn && !s_was_connected) {
        s_connected_ms = millis();
        s_last_report_ms = 0;
    }
    if (!conn && s_was_connected) {
        s_active = false;
        s_phone_bat = -1;
        s_media.valid = false;
        s_finding = false;
        if (s_call.state != CALL_NONE) s_call.state = CALL_NONE;
    }
    s_was_connected = conn;
    if (!conn) return;

    char msg[1024];
    int budget = 6; // bounded work per loop
    while (budget-- > 0 && ble_link_recv(msg, sizeof(msg))) handle(msg);

    // Say hello shortly after connecting (the app subscribes first), then
    // report steps/battery every 5 minutes.
    uint32_t now = millis();
    if (s_last_report_ms == 0 ? now - s_connected_ms > 2500 : now - s_last_report_ms > 300000) {
        send_report(s_last_report_ms == 0 ? "hi" : "st");
    }
}

bool phone_link_active() { return s_active && ble_is_connected(); }
int  phone_link_battery() { return s_phone_bat; }
bool phone_link_charging() { return s_phone_chg; }
const PhoneMedia &phone_link_media() { return s_media; }
const PhoneCall &phone_link_call() { return s_call; }
bool phone_link_finding_phone() { return s_finding; }

int phone_link_media_position() {
    int p = s_media.position_s;
    if (s_media.playing) p += (int)((millis() - s_media.updated_ms) / 1000);
    if (s_media.duration_s > 0 && p > s_media.duration_s) p = s_media.duration_s;
    return p;
}

static void send_simple(const char *ev, const char *key, const char *val) {
    JsonDocument doc;
    doc["e"] = ev;
    doc[key] = val;
    send_doc(doc);
}

void phone_link_media_cmd(const char *cmd) {
    send_simple("med", "a", cmd);
    // Optimistic UI: flip play state now, the phone confirms within ~1 s.
    if (!strcmp(cmd, "toggle")) {
        s_media.position_s = phone_link_media_position();
        s_media.updated_ms = millis();
        s_media.playing = !s_media.playing;
    }
}

void phone_link_call_answer() {
    JsonDocument doc;
    doc["e"] = "call"; doc["a"] = "answer"; doc["id"] = s_call.id;
    send_doc(doc);
}

void phone_link_call_decline() {
    JsonDocument doc;
    doc["e"] = "call"; doc["a"] = "decline"; doc["id"] = s_call.id;
    send_doc(doc);
}

void phone_link_find_phone(bool on) {
    JsonDocument doc;
    doc["e"] = "find"; doc["on"] = on ? 1 : 0;
    send_doc(doc);
    s_finding = on;
}

void phone_link_notif_dismiss(uint32_t id) {
    JsonDocument doc;
    doc["e"] = "ntf"; doc["a"] = "dismiss"; doc["id"] = id;
    send_doc(doc);
}

bool phone_link_notif_reply(uint32_t id, const char *text) {
    JsonDocument doc;
    doc["e"] = "ntf"; doc["a"] = "reply"; doc["id"] = id; doc["tx"] = text;
    return send_doc(doc);
}

bool phone_link_notif_digest() {
    JsonDocument doc;
    doc["e"] = "digest";
    return send_doc(doc);
}

bool phone_link_notif_translate(uint32_t id) {
    JsonDocument doc;
    doc["e"] = "tr"; doc["id"] = id;
    return send_doc(doc);
}

bool phone_link_fall(bool ok) {
    JsonDocument doc;
    doc["e"] = "fall";
    if (ok) doc["ok"] = 1;
    return send_doc(doc);
}

void phone_link_request_state() {
    JsonDocument doc;
    doc["e"] = "req";
    send_doc(doc);
}
