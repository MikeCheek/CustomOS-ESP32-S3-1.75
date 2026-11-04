// What each gesture on the watch face does - see home_gestures.h.
#include "home_gestures.h"
#include "config.h"
#include "app_settings_state.h"
#include "hal_nvs.h"
#include "phone_link.h"
#include "ui.h"
#include "apps.h"
#include "app_menu.h"
#include "app_face_nav.h"
#include "app_torch.h"
#include "app_clock.h"
#include "app_life.h"
#include "app_weather.h"
#include "app_workout.h"
#include "app_assistant.h"
#include "watchface_registry.h"
#include <Arduino.h>
#include <string.h>

extern Screen notifications_screen, settings_screen, activity_screen, calendar_screen, music_screen,
    nowplaying_screen, tiles_screen;

struct ActionInfo { const char *key, *name; };
static const ActionInfo ACTIONS[HA_COUNT] = {
    {"none", "Nothing"},          {"apps", "Apps"},
    {"notifs", "Notifications"},  {"stack", "Smart Stack"},
    {"control", "Control Center"}, {"tiles", "Tiles"},
    {"faces", "Faces"},           {"nextface", "Next face"},
    {"prevface", "Previous face"}, {"assistant", "Assistant"},
    {"music", "Music"},           {"playpause", "Play / pause"},
    {"timer", "Timer"},           {"stopwatch", "Stopwatch"},
    {"workout", "Workout"},       {"torch", "Flashlight"},
    {"recorder", "Voice note"},   {"life", "Life"},
    {"activity", "Activity"},     {"weather", "Weather"},
    {"calendar", "Calendar"},     {"settings", "Settings"},
    {"face", "The face's own"},
};
static const char *const SLOT_KEYS[HS_COUNT] = {"tap", "double", "hold", "up", "down", "left", "right", "boot"};
static const char *const SLOT_NAMES[HS_COUNT] = {"Tap", "Double-tap", "Touch and hold", "Swipe up",
                                                 "Pull down", "Swipe from the right", "Swipe from the left",
                                                 "BOOT button"};

// tap, double, hold, up, down, from the right, from the left, BOOT
struct Style { const char *key, *name; uint8_t a[HS_COUNT]; };
static const Style STYLES[] = {
    {"standard", "Standard",
     {HA_APPS, HA_PLAYPAUSE, HA_FACES, HA_STACK, HA_CONTROL, HA_TILES, HA_NOTIFS, HA_CONTROL}},
    // watchOS 10: Smart Stack up, notifications down, edge swipes change the face
    {"apple", "Apple-like",
     {HA_APPS, HA_PLAYPAUSE, HA_FACES, HA_STACK, HA_NOTIFS, HA_NEXT_FACE, HA_PREV_FACE, HA_CONTROL}},
    // Wear OS: quick settings down, notifications up, tiles to the side, the crown opens apps
    {"wearos", "Wear OS-like",
     {HA_APPS, HA_NONE, HA_FACES, HA_NOTIFS, HA_CONTROL, HA_TILES, HA_ASSISTANT, HA_APPS}},
    // One UI Watch: apps up, quick panel down, tiles and notifications to the sides
    {"galaxy", "Galaxy-like",
     {HA_FACE_OWN, HA_NONE, HA_FACES, HA_APPS, HA_CONTROL, HA_TILES, HA_NOTIFS, HA_APPS}},
    // this watch before the gestures could be changed
    {"classic", "Classic",
     {HA_FACE_OWN, HA_NONE, HA_FACES, HA_STACK, HA_CONTROL, HA_APPS, HA_NOTIFS, HA_CONTROL}},
};
static const int STYLE_N = sizeof(STYLES) / sizeof(STYLES[0]);

struct TileInfo { const char *key, *name; };
static const TileInfo TILES[TL_COUNT] = {
    {"activity", "Activity"}, {"weather", "Weather"},     {"next", "Up next"},  {"music", "Music"},
    {"timer", "Timer"},       {"life", "Life"},           {"world", "World clock"}, {"alarm", "Alarm"},
    {"reminders", "Reminders"}, {"battery", "Battery"},
};

const char *home_action_name(HomeAction a) { return a < HA_COUNT ? ACTIONS[a].name : "Nothing"; }
const char *home_action_key(HomeAction a) { return a < HA_COUNT ? ACTIONS[a].key : "none"; }
const char *home_slot_name(HomeSlot s) { return s < HS_COUNT ? SLOT_NAMES[s] : ""; }
const char *home_slot_key(HomeSlot s) { return s < HS_COUNT ? SLOT_KEYS[s] : ""; }
bool home_slot_allows(HomeSlot s, HomeAction a) {
    if (a >= HA_COUNT) return false;
    if (a == HA_FACE_OWN) return s == HS_TAP;   // a face's own tap (the next photo...)
    if (s == HS_HOLD && a == HA_NONE) return false;   // the faces must stay reachable somewhere
    return true;
}

HomeAction home_action(HomeSlot s) {
    if (s >= HS_COUNT) return HA_NONE;
    uint8_t a = g_app_settings.home_act[s];
    return a < HA_COUNT ? (HomeAction)a : HA_NONE;
}

void home_set_action(HomeSlot s, HomeAction a) {
    if (s >= HS_COUNT || !home_slot_allows(s, a)) return;
    g_app_settings.home_act[s] = a;
    nvs_save_settings(g_app_settings);
}

void home_cycle_action(HomeSlot s) {
    int a = home_action(s);
    for (int k = 0; k < HA_COUNT; k++) {
        a = (a + 1) % HA_COUNT;
        if (home_slot_allows(s, (HomeAction)a)) break;
    }
    home_set_action(s, (HomeAction)a);
}

void home_defaults(uint8_t *acts) { memcpy(acts, STYLES[0].a, HS_COUNT); }

int home_style_count() { return STYLE_N; }
const char *home_style_name(int i) { return i >= 0 && i < STYLE_N ? STYLES[i].name : "Custom"; }
int home_style() {
    for (int i = 0; i < STYLE_N; i++)
        if (!memcmp(STYLES[i].a, g_app_settings.home_act, HS_COUNT)) return i;
    return STYLE_N;
}
void home_apply_style(int i) {
    if (i < 0 || i >= STYLE_N) return;
    memcpy(g_app_settings.home_act, STYLES[i].a, HS_COUNT);
    nvs_save_settings(g_app_settings);
}

const char *tile_name(TileKind k) { return k < TL_COUNT ? TILES[k].name : ""; }
const char *tile_key(TileKind k) { return k < TL_COUNT ? TILES[k].key : ""; }
int tiles_list(uint8_t *out) {
    int n = 0;
    bool seen[TL_COUNT] = {false};
    for (int i = 0; i < TILES_MAX; i++) {
        uint8_t k = g_app_settings.tiles[i];
        if (k == TILE_END) break;
        if (k >= TL_COUNT || seen[k]) continue;
        seen[k] = true;
        out[n++] = k;
    }
    return n;
}
void tiles_defaults(uint8_t *t) {
    static const uint8_t D[TILES_MAX] = {TL_ACTIVITY, TL_WEATHER, TL_NEXT, TL_MUSIC, TL_TIMER, TL_LIFE, TL_WORLD,
                                         TILE_END, TILE_END, TILE_END};
    memcpy(t, D, TILES_MAX);
}

// ---- running them --------------------------------------------------------------------

void home_run_slot(HomeSlot s) { home_run(home_action(s)); }

static void open(Screen *s) {
    if (s && !ui_screen_on_top(s)) ui_push(s);
}

void home_run(HomeAction a) {
    switch (a) {
    case HA_NONE: case HA_FACE_OWN: case HA_COUNT: break;
    case HA_APPS:      open(&menu_screen); break;
    case HA_NOTIFS:    open(&notifications_screen); break;
    case HA_STACK:     open(&smart_stack_screen); break;
    case HA_CONTROL:   ui_top_panel_open(); break;
    case HA_TILES:     open(&tiles_screen); break;
    case HA_FACES:     face_nav_open_switcher(); break;
    case HA_NEXT_FACE: face_nav_wear((watchface_current() + 1) % watchface_count()); break;
    case HA_PREV_FACE: face_nav_wear((watchface_current() + watchface_count() - 1) % watchface_count()); break;
    case HA_ASSISTANT: open(&assistant_screen); break;
    case HA_MUSIC:     open(phone_link_media().valid ? &nowplaying_screen : &music_screen); break;
    case HA_PLAYPAUSE: {
        const PhoneMedia &m = phone_link_media();
        if (!phone_link_active() || !m.valid) { ui_show_toast("Nothing playing on the phone", 1500); break; }
        phone_link_media_cmd("toggle");
        ui_show_toast(m.playing ? "Paused" : "Playing", 1000);
        break;
    }
    case HA_TIMER:     open(&timer_screen); break;
    case HA_STOPWATCH: open(&stopwatch_screen); break;
    case HA_WORKOUT:   open(&workout_screen); break;
    case HA_TORCH:     open(&torch_screen); break;
    case HA_RECORDER:  open(&recorder_screen); break;
    case HA_LIFE:      open(&life_screen); break;
    case HA_ACTIVITY:  open(&activity_screen); break;
    case HA_WEATHER:   open(&weather_screen); break;
    case HA_CALENDAR:  open(&calendar_screen); break;
    case HA_SETTINGS:  open(&settings_screen); break;
    }
}

// ---- the app --------------------------------------------------------------------------

static int find_action(const char *k) {
    if (!k) return -1;
    for (int i = 0; i < HA_COUNT; i++)
        if (!strcmp(ACTIONS[i].key, k)) return i;
    return -1;
}
static int find_tile(const char *k) {
    if (!k) return -1;
    for (int i = 0; i < TL_COUNT; i++)
        if (!strcmp(TILES[i].key, k)) return i;
    return -1;
}

static uint32_t s_sent_hash = 0;
static uint32_t home_hash() {
    uint32_t h = 2166136261u;
    for (int i = 0; i < HS_COUNT; i++) h = (h ^ g_app_settings.home_act[i]) * 16777619u;
    for (int i = 0; i < TILES_MAX; i++) h = (h ^ g_app_settings.tiles[i]) * 16777619u;
    return h;
}

void home_on_link(JsonDocument &doc) {
    bool changed = false;
    if (const char *st = doc["style"]) {
        for (int i = 0; i < STYLE_N; i++)
            if (!strcmp(STYLES[i].key, st)) { memcpy(g_app_settings.home_act, STYLES[i].a, HS_COUNT); changed = true; }
    }
    JsonObject a = doc["a"];
    if (!a.isNull()) {
        for (int s = 0; s < HS_COUNT; s++) {
            int v = find_action(a[SLOT_KEYS[s]] | (const char *)nullptr);
            if (v >= 0 && home_slot_allows((HomeSlot)s, (HomeAction)v)) { g_app_settings.home_act[s] = v; changed = true; }
        }
    }
    JsonArray t = doc["tiles"];
    if (!t.isNull()) {
        uint8_t nt[TILES_MAX];
        memset(nt, TILE_END, sizeof(nt));
        int n = 0;
        for (JsonVariant v : t) {
            int k = find_tile(v.as<const char *>());
            bool dup = false;
            for (int i = 0; i < n; i++) dup |= nt[i] == k;
            if (k >= 0 && !dup && n < TILES_MAX) nt[n++] = k;
        }
        memcpy(g_app_settings.tiles, nt, sizeof(nt));
        changed = true;
    }
    if (changed) nvs_save_settings(g_app_settings);
    home_send();
}

void home_send() {
    JsonDocument d;
    d["e"] = "home";
    JsonObject a = d["a"].to<JsonObject>();
    for (int s = 0; s < HS_COUNT; s++) a[SLOT_KEYS[s]] = ACTIONS[home_action((HomeSlot)s)].key;
    int st = home_style();
    d["style"] = st < STYLE_N ? STYLES[st].key : "custom";
    JsonArray t = d["tiles"].to<JsonArray>();
    uint8_t l[TILES_MAX];
    int n = tiles_list(l);
    for (int i = 0; i < n; i++) t.add(TILES[l[i]].key);
    s_sent_hash = home_hash();
    phone_link_send(d);
}

void home_update() {
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 1500) return;
    last = now;
    if (!phone_link_active()) return;
    if (home_hash() != s_sent_hash) home_send();
}
