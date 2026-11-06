// The watch emulator inside the companion app (phase 1): the real screen
// code from firmware/, against the same stubbed hardware as the PC
// simulator (sim_stubs.cpp, shim/), on the real clock. Built as a shared
// library (companion_app/android/app/src/main/cpp/CMakeLists.txt) and
// driven from Dart through the C functions below
// (companion_app/lib/emulator/watch_emulator.dart):
//
//   emu_init(dir)      once; dir = where its files live
//   emu_step()         ~60 times a second: runs the UI loop once
//   emu_frame_rgba()   the last frame the watch "sent to the panel", as RGBA
//   emu_touch()        finger down / moving / up, in watch pixels (466x466)
//   emu_button()       BOOT / PWR presses, the way firmware.ino handles them
//
// Not here (yet): the phone link (phase 2), sensors and sound (phase 3),
// ESP-SR (chip-only libraries).
#include "store.h"
#include "game_roulette.h"
#include "game_videopoker.h"
#include "game_dice.h"
#include "casino_wallet.h"
#include <SD.h>
#include "game_audio.h"
#include "phone_images.h"
#include <ArduinoJson.h>
#include "home_gestures.h"
#include "app_weather.h"
#include "app_wallet.h"
#include <Arduino.h>
#include <atomic>
#ifdef __ANDROID__
#include <jni.h>
#endif
#include <Arduino_GFX_Library.h>
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "app_settings_state.h"
#include "watchface_registry.h"
#include "phone_link.h"
#include "app_notifications.h"
#include "app_assistant.h"
#include "app_calendar.h"
#include "app_fall.h"
#include "app_activity.h"
#include "app_sleep.h"
#include "app_photo_watchface.h"
#include "watchface_registry.h"
#include "sim_state.h"
#include "app_face_nav.h"
#include "app_clock.h"
#include "haptics.h"
#include "hal_audio.h"
#include "face_look.h"
#include "low_power.h"
#include "app_reminders.h"
#include "fitness.h"
#include "app_workout.h"
#include "app_life.h"
#include "usage_stats.h"
#include "hal_nvs.h"
#include "hal_sleep.h"
#include "dnd.h"
#include "complications.h"
#include <string>
#include <vector>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <math.h>

HostSerial Serial;
// sim_stubs.cpp
void sim_touch_track(int x, int y, bool down);
void sim_link(const char *json);
bool sim_link_pop(std::string &out);
void sim_set_weather(const char *json);
void sim_set_fitness(const char *json);
void sim_prefs_enable(const char *path);
void sim_sd_enable(const char *dir);
bool sim_take_event(std::string &out);
void sim_set_playing(bool on);
void sim_set_mic_level(int pct);
void sim_set_accel(float x, float y, float z);
int sim_take_tones(int32_t *out, int max);
void audio_set_volume(uint8_t v);

// ---- time: the real clock ------------------------------------------------------------
static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}
static uint64_t s_t0 = 0;
uint32_t millis() { return (uint32_t)((now_us() - s_t0) / 1000); }
uint32_t micros() { return (uint32_t)(now_us() - s_t0); }
// The UI code doesn't block in delay() during normal screens; when it does
// (short waits), just let the clock run.
void delay(uint32_t ms) {
    uint64_t end = now_us() + (uint64_t)ms * 1000;
    while (now_us() < end) {}
}
void delayMicroseconds(uint32_t us) {
    uint64_t end = now_us() + us;
    while (now_us() < end) {}
}

// ---- display pipeline: the submitted buffer is "on screen" --------------------------
#define FB_PIXELS ((size_t)LCD_WIDTH * LCD_HEIGHT)
static uint16_t *s_bufs[2];
static int s_free = 0;
static uint16_t s_shown[FB_PIXELS];
static volatile uint32_t s_frame_seq = 0;
static bool s_screen_off = false;

int display_pipeline_start(uint16_t *first) {
    s_bufs[0] = first;
    s_bufs[1] = (uint16_t *)calloc(FB_PIXELS, 2);
    return 2;
}
uint16_t *display_fb_acquire(bool) { uint16_t *b = s_bufs[s_free]; s_free ^= 1; return b; }
void display_fb_release(uint16_t *) {}
void display_fb_submit(uint16_t *fb) {
    memcpy(s_shown, fb, sizeof(s_shown));
    s_frame_seq++;
}
const uint16_t *display_fb_last_submitted() { return s_shown; }
void display_force_full_refresh() {}

extern "C" {

__attribute__((visibility("default"))) int emu_init(const char *dir) {
    if (s_t0) return 1;
    s_t0 = now_us();
    g_sim.live = true;
    g_sim.phone = false;    // until the app attaches (emu_set_connected)
    // The watch's NVS: settings, quick replies, watchface slots... in a file
    if (dir && dir[0]) {
        mkdir(dir, 0755);
        sim_prefs_enable((std::string(dir) + "/nvs.bin").c_str());
        sim_sd_enable((std::string(dir) + "/sd").c_str());   // the SD card
    }
    nvs_load_settings(g_app_settings);
    quick_replies_load();
    comp_load();
    store_begin();                                         // files: the emulator's SD folder
    clock_init();                                          // alarms, world clock cities
    reminders_init();                                      // to-dos from the phone
    fitness_init();                                        // activity rings
    workout_init();                                        // saved workouts
    life_init();                                           // Life: plan and today
    usage_init();                                          // usage stats for Diagnostics
    wallet_init();
    weather_init();
    g_app_settings.onboarding_complete = true;   // straight to the watch, no first-boot setup
    // the phone in your hand is held upright: "lower wrist to sleep" would
    // put the emulated watch to sleep every few seconds
    g_app_settings.wrist_down_sleep = false;
    audio_set_volume(g_app_settings.volume);
    ui_fonts_set_smooth(g_app_settings.smooth_fonts);
    ui_init(nullptr);
    watchface_registry_init();
    ui_push(watchface_get(0)->screen);
    return 1;
}

// Touches and buttons from the Android overlay (EmuOverlay.kt) come on the
// UI thread; they wait here and are handed to the watch at the next step,
// on the thread that runs it.
struct OverlayInput { int16_t a, b, c; uint8_t kind; };   // kind 0 touch (x, y, down), 1 button (which, event)
static OverlayInput s_oin[64];
static std::atomic<int> s_oin_w{0}, s_oin_r{0};
static void overlay_push(uint8_t kind, int a, int b, int c) {
    int w = s_oin_w.load(), next = (w + 1) % 64;
    if (next == s_oin_r.load()) return;   // full: dropped
    s_oin[w] = {(int16_t)a, (int16_t)b, (int16_t)c, kind};
    s_oin_w.store(next);
}
extern "C" void emu_touch(int x, int y, int down);
extern "C" void emu_button(int which, int event);
static void overlay_drain() {
    while (s_oin_r.load() != s_oin_w.load()) {
        OverlayInput in = s_oin[s_oin_r.load()];
        s_oin_r.store((s_oin_r.load() + 1) % 64);
        if (in.kind == 0) emu_touch(in.a, in.b, in.c);
        else emu_button(in.a, in.b);
    }
}

__attribute__((visibility("default"))) void emu_step() {
    if (!s_t0) return;
    overlay_drain();
    phone_link_update();
    phone_images_update();       // icons, covers, the camera view the app sent
    notifications_update();
    assistant_timer_update();
    clock_update();
    haptics_update();
    look_update();
    life_update();               // Life: the day's nudges
    usage_update();              // how the watch is used (Diagnostics)
    weather_update();
    low_power_update();
    fitness_update();
    workout_update();
    calendar_update();      // event reminders the app sent
    motion_update();        // fall detection, activity, reps, flicks - the phone's accelerometer
    sleeptrack_update();
    ui_update();
    ui_poll_gestures();
}

// Frame counter: changes whenever a new frame was "sent to the panel".
__attribute__((visibility("default"))) uint32_t emu_frame_seq() { return s_screen_off ? 0 : s_frame_seq; }

// The current frame as 466*466*4 bytes RGBA (opaque); black while the
// screen is "off" (PWR).
__attribute__((visibility("default"))) void emu_frame_rgba(uint8_t *out) {
    for (size_t i = 0; i < FB_PIXELS; i++) {
        uint16_t c = s_screen_off ? 0 : s_shown[i];
        uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        out[i * 4] = (uint8_t)((r << 3) | (r >> 2));
        out[i * 4 + 1] = (uint8_t)((g << 2) | (g >> 4));
        out[i * 4 + 2] = (uint8_t)((b << 3) | (b >> 2));
        out[i * 4 + 3] = 255;
    }
}

__attribute__((visibility("default"))) int emu_width() { return LCD_WIDTH; }
__attribute__((visibility("default"))) int emu_height() { return LCD_HEIGHT; }

// down: 1 while the finger touches (send moves with down = 1), 0 to lift.
__attribute__((visibility("default"))) void emu_touch(int x, int y, int down) {
    if (s_screen_off) {
        if (!down) s_screen_off = false;   // tap to wake
        return;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= LCD_WIDTH) x = LCD_WIDTH - 1;
    if (y >= LCD_HEIGHT) y = LCD_HEIGHT - 1;
    sleep_register_activity();   // like firmware.ino: a touch keeps the screen at full frame rate
    sim_touch_track(x, y, down != 0);
    ui_handle_touch(x, y, down != 0);
}

// which: 0 = BOOT, 1 = PWR; event: 1 short, 2 double, 3 long - the same
// meaning as on the watch (firmware.ino).
__attribute__((visibility("default"))) void emu_button(int which, int event) {
    sleep_register_activity();
    if (which == 1) {
        if (event == 1) s_screen_off = !s_screen_off;   // the screen sleeps / wakes
        else if (event == 2) ui_pop_screen();
        s_frame_seq++;
        return;
    }
    if (s_screen_off) { s_screen_off = false; s_frame_seq++; return; }
    if (event == 1) {
        if (ui_top_panel_is_open()) ui_top_panel_close();
        else if (face_nav_is_face(ui_active_screen())) ui_top_panel_open();   // as on the watch
        else ui_pop_screen();
    } else if (event == 3) {
        if (ui_top_panel_is_open()) ui_top_panel_close();
        ui_go_home();
    } else if (event == 2) {
        if (ui_top_panel_is_open()) ui_top_panel_close();
        if (!ui_screen_on_top(&assistant_screen)) ui_push(&assistant_screen);
    }
}

// ---- the phone link, in-process (phase 2) ----------------------------------------------
// The app attaches as the watch's phone: its messages arrive as if over
// Bluetooth, the watch's come back through emu_link_pop.
__attribute__((visibility("default"))) void emu_set_connected(int on) { g_sim.phone = on != 0; }

__attribute__((visibility("default"))) void emu_link_push(const char *json) { if (json) sim_link(json); }

// An image for the watch (phone_images.h): 'i' app icon, 'a' album cover,
// 'p' the camera remote's live view - one JPEG, as the Bluetooth frames
// would carry it.
__attribute__((visibility("default"))) void emu_image(int kind, uint32_t hash, const uint8_t *jpeg, int len) {
    if (!jpeg || len <= 0) return;
    std::vector<uint8_t> f(14 + (size_t)len);
    f[0] = 0x03; f[1] = (uint8_t)kind;
    for (int i = 0; i < 4; i++) {
        f[2 + i] = (uint8_t)(hash >> (8 * i));
        f[6 + i] = (uint8_t)((uint32_t)len >> (8 * i));
        f[10 + i] = 0;
    }
    memcpy(f.data() + 14, jpeg, (size_t)len);
    phone_images_on_frame(f.data(), (int)f.size());
}

// The live view's size once decoded (0 when there's none) - for emu_host_test.
__attribute__((visibility("default"))) int emu_preview_width() {
    int w = 0, h = 0;
    return phone_preview(&w, &h, nullptr) ? w : 0;
}

// The next message from the watch into buf (NUL-terminated); its length,
// 0 when there's none, or -needed when buf is too small (it stays queued).
static std::string s_pending;
static bool s_has_pending = false;
__attribute__((visibility("default"))) int emu_link_pop(char *buf, int cap) {
    if (!s_has_pending) {
        if (!sim_link_pop(s_pending)) return 0;
        s_has_pending = true;
    }
    int n = (int)s_pending.size();
    if (n + 1 > cap) return -(n + 1);
    memcpy(buf, s_pending.data(), n);
    buf[n] = 0;
    s_has_pending = false;
    return n;
}

// kind 2 = weather, 3 = fitness: the JSON the app sends on the file
// characteristic after its prefix byte.
__attribute__((visibility("default"))) void emu_set_data(int kind, const char *json) {
    if (!json) return;
    if (kind == 2) sim_set_weather(json);
    else if (kind == 3) sim_set_fitness(json);
}

// ---- sensors and sound (phase 3) ----------------------------------------------------
// The phone's accelerometer in g (x right, y up, z out of the screen - the
// watch's axes when it lies the same way), ~62.5 times a second.
__attribute__((visibility("default"))) void emu_set_accel(float x, float y, float z) { sim_set_accel(x, y, z); }

// Tones the watch played since the last call: (frequency Hz, length ms,
// kind | volume << 8) per tone, kind 0 = beep / effect, 1 = piano note.
// Returns how many.
__attribute__((visibility("default"))) int emu_take_sounds(int32_t *out, int max) { return sim_take_tones(out, max); }

// ---- SD card, microphone, playback (phase 4) -----------------------------------------
// Events for the app, JSON, like emu_link_pop:
//   {"ev":"rec","on":1,"mono":1,"path":"<file>"}  record the phone's microphone into <file>
//   {"ev":"rec","on":0}                          stop it
//   {"ev":"play","path":"<file>"}  /  {"ev":"stop"}   play a WAV/MP3 / stop
static std::string s_ev;
static bool s_has_ev = false;
__attribute__((visibility("default"))) int emu_take_event(char *buf, int cap) {
    if (!s_has_ev) {
        if (!sim_take_event(s_ev)) return 0;
        s_has_ev = true;
    }
    int n = (int)s_ev.size();
    if (n + 1 > cap) return -(n + 1);
    memcpy(buf, s_ev.data(), n);
    buf[n] = 0;
    s_has_ev = false;
    return n;
}

// The phone finished (0) playing what "play" asked for.
__attribute__((visibility("default"))) void emu_set_playing(int on) { sim_set_playing(on != 0); }

// The phone microphone's level while recording, 0..100 (the watch's meters).
__attribute__((visibility("default"))) void emu_set_mic_level(int pct) { sim_set_mic_level(pct); }

// A few settings by name, for tests and tools ("fall", "sleep"): 1 on, 0 off.
// "clock24": 0 = 12-hour clock.
__attribute__((visibility("default"))) void emu_set_option(const char *name, int on) {
    if (!name) return;
    if (!strcmp(name, "fall")) g_app_settings.fall_detect = on != 0;
    else if (!strcmp(name, "sleep")) g_app_settings.sleep_track = on != 0;
    else if (!strcmp(name, "clock24")) g_app_settings.clock_24h = on != 0;
}

// The app wrote <sd>/faces/photos/<name>.pwf ("pf:<name>.pwf"): show it,
// like the watch does after a transfer, and tell the app the new list.
__attribute__((visibility("default"))) void emu_photo_face(const char *name) {
    photo_face_reload();
    if (name && photo_face_show(name) && photo_face_available()) {
        for (int i = 0; i < watchface_count(); i++)
            if (watchface_get(i)->screen == &photo_watchface_screen) watchface_set_current(i);
        ui_go_home();
        if (!ui_screen_on_top(&photo_watchface_screen)) ui_push(&photo_watchface_screen);
    }
    phone_link_send_photo_faces();
}

// Tests: what the wake word hands the assistant when it wasn't a command -
// `ms` of a 220 Hz tone as the question (voice_sr.cpp on the watch).
__attribute__((visibility("default"))) int emu_ask_recorded(int ms) {
    static int16_t pcm[16000 * 5];
    int n = 16 * ms;
    if (n > (int)(sizeof(pcm) / 2)) n = sizeof(pcm) / 2;
    for (int i = 0; i < n; i++) pcm[i] = (int16_t)(8000 * sin(2 * M_PI * 220 * i / 16000.0));
    return assistant_ask_recorded(pcm, n, 16000) ? 1 : 0;
}

// Checks of the firmware's own logic, for emu_host_test (and so CI): the
// store, Wallet's link protocol, the home gestures' styles and link.
// Returns the number of failures; each is printed.
static int s_fail = 0;
extern "C" int emu_take_sounds(int32_t *out, int max);
extern "C" int emu_take_event(char *buf, int cap);
static void check(bool ok, const char *what) {
    if (!ok) { printf("SELFTEST FAIL: %s\n", what); s_fail++; }
}
static int pop_all(char *last, int n, const char *want) {   // drains the link; the last message with `want`
    char m[1024];
    int found = 0;
    last[0] = 0;
    while (emu_link_pop(m, sizeof(m)) > 0)
        if (strstr(m, want)) { strlcpy(last, m, n); found++; }
    return found;
}
static void feed(const char *json) {
    JsonDocument d;
    if (deserializeJson(d, json)) { check(false, "bad test JSON"); return; }
    const char *t = d["t"] | "";
    if (!strcmp(t, "wal")) wallet_on_link(d);
    else if (!strcmp(t, "home")) home_on_link(d);
}

__attribute__((visibility("default"))) int emu_selftest() {
    s_fail = 0;
    char m[1024];
    // ---- the store: write, read back, replace, folders, remove
    {
        const char a[] = "kept across a restart", b2[] = "replaced";
        char back[64] = {0};
        check(store_ok(), "store: somewhere to keep files");
        check(store_write("test/roundtrip", a, sizeof(a)), "store: write");
        check(store_read("test/roundtrip", back, sizeof(back)) == (int)sizeof(a) && !strcmp(back, a), "store: read back");
        check(store_write("test/roundtrip", b2, sizeof(b2)) && store_size("test/roundtrip") == (int)sizeof(b2), "store: replace");
        check(store_remove("test/roundtrip") && !store_exists("test/roundtrip"), "store: remove");
        check(store_read("test/none", back, sizeof(back)) == -1, "store: a missing file reads -1");
    }
    // ---- Wallet: a 3x3 code in two parts, listed with its crc, then deleted
    {
        pop_all(m, sizeof(m), "\"e\":\"wal\"");
        // 9 modules: 101 010 101 -> 0xAA 0x80, in two parts of one byte (base64 "qg==" / "gA==")
        feed("{\"t\":\"wal\",\"op\":\"put\",\"i\":5,\"n\":\"Test pass\",\"s\":\"x\",\"k\":\"qr\",\"w\":3,\"h\":3,\"r\":10,"
             "\"crc\":4242,\"tot\":2,\"o\":0,\"b\":\"qg==\"}");
        check(pop_all(m, sizeof(m), "\"e\":\"wal\"") == 0, "wallet: no list before the last part");
        feed("{\"t\":\"wal\",\"op\":\"put\",\"i\":5,\"tot\":2,\"o\":1,\"b\":\"gA==\"}");
        check(pop_all(m, sizeof(m), "\"e\":\"wal\"") == 1 && strstr(m, "[\"Test pass\",4242]"), "wallet: put in parts, listed");
        check(store_exists("wallet/5"), "wallet: kept in the store");
        feed("{\"t\":\"wal\",\"op\":\"put\",\"i\":6,\"n\":\"Bad\",\"k\":\"qr\",\"w\":99,\"h\":99,\"tot\":2,\"o\":0,\"b\":\"qg==\"}");
        pop_all(m, sizeof(m), "\"e\":\"wal\"");
        check(!strstr(m, "Bad"), "wallet: more modules than bytes is refused");
        feed("{\"t\":\"wal\",\"op\":\"del\",\"i\":5}");
        check(pop_all(m, sizeof(m), "\"e\":\"wal\"") == 1 && !strstr(m, "Test pass") && !store_exists("wallet/5"), "wallet: deleted");
    }
    // ---- home gestures: styles, custom, the face's own only for a tap, the link
    {
        uint8_t keep[HS_COUNT];
        memcpy(keep, g_app_settings.home_act, sizeof(keep));
        home_apply_style(1);
        check(home_style() == 1 && home_action(HS_DOWN) == HA_NOTIFS, "home: Apple-like style");
        home_set_action(HS_TAP, HA_TORCH);
        check(home_style() == home_style_count() && home_action(HS_TAP) == HA_TORCH, "home: one change makes it Custom");
        check(!home_slot_allows(HS_UP, HA_FACE_OWN) && home_slot_allows(HS_TAP, HA_FACE_OWN), "home: the face's own is tap only");
        check(!home_slot_allows(HS_HOLD, HA_NONE), "home: hold is never nothing");
        pop_all(m, sizeof(m), "\"e\":\"home\"");
        feed("{\"t\":\"home\",\"style\":\"galaxy\"}");
        check(home_action(HS_UP) == HA_APPS && pop_all(m, sizeof(m), "\"e\":\"home\"") == 1 && strstr(m, "\"style\":\"galaxy\""),
              "home: a style from the app, and the answer");
        feed("{\"t\":\"home\",\"a\":{\"up\":\"face\",\"left\":\"wallet\"},\"tiles\":[\"timer\",\"bogus\",\"timer\",\"music\"]}");
        uint8_t tl[TILES_MAX];
        int n = tiles_list(tl);
        check(home_action(HS_UP) == HA_APPS, "home: a refused action leaves the old one");
        check(home_action(HS_LEFT) == HA_WALLET, "home: an action from the app");
        check(n == 2 && tl[0] == TL_TIMER && tl[1] == TL_MUSIC, "home: tiles from the app, unknown and repeated ones dropped");
        memcpy(g_app_settings.home_act, keep, sizeof(keep));
        tiles_defaults(g_app_settings.tiles);
        pop_all(m, sizeof(m), "\"e\":\"home\"");
    }
    // ---- the Casino: roulette pays, the shared wallet
    {
        int32_t out[9] = {}, num[37] = {};
        out[0] = 10;                                   // 10 on red
        check(roulette_win(out, num, 1) == 20 && roulette_win(out, num, 2) == 0, "roulette: red pays 1:1");
        check(roulette_win(out, num, 0) == 0, "roulette: zero loses the outside bets");
        out[0] = 0; out[8] = 10;                       // 10 on the 3rd dozen
        check(roulette_win(out, num, 30) == 30 && roulette_win(out, num, 12) == 0, "roulette: a dozen pays 2:1");
        out[8] = 0; num[17] = 5; num[0] = 5;           // straight up on 17 and on 0
        check(roulette_win(out, num, 17) == 180 && roulette_win(out, num, 0) == 180, "roulette: a number pays 35:1");
        // video poker: the hands, with the ace low and high
        auto ph = [](const char *cards) {   // "AS KS QS JS TS": rank then suit (S H D C)
            uint8_t r[5], s[5];
            for (int i = 0; i < 5; i++) {
                const char *c = cards + i * 3;
                const char *R = "A23456789TJQK", *S = "SHDC";
                r[i] = (uint8_t)(strchr(R, c[0]) - R + 1);
                s[i] = (uint8_t)(strchr(S, c[1]) - S);
            }
            return poker_hand(r, s);
        };
        check(ph("AS KS QS JS TS") == PH_ROYAL && poker_pays(PH_ROYAL) == 250, "poker: royal flush pays 250");
        check(ph("9H 8H 7H 6H 5H") == PH_STRAIGHT_FLUSH, "poker: straight flush");
        check(ph("AD 2C 3H 4S 5D") == PH_STRAIGHT && ph("TD JC QH KS AD") == PH_STRAIGHT, "poker: the ace low and high");
        check(ph("QD KC AH 2S 3D") == PH_NOTHING, "poker: no wrapping round the ace");
        check(ph("7S 7H 7D 7C 2S") == PH_FOUR && ph("7S 7H 7D 2C 2S") == PH_FULL_HOUSE, "poker: four of a kind, full house");
        check(ph("2H 9H 4H JH KH") == PH_FLUSH && ph("5S 5H 5D 2C 9S") == PH_THREE, "poker: flush, three of a kind");
        check(ph("5S 5H 9D 9C 2S") == PH_TWO_PAIR, "poker: two pair");
        check(ph("JS JH 4D 9C 2S") == PH_JACKS && ph("AS AH 4D 9C 2S") == PH_JACKS, "poker: jacks or better");
        check(ph("TS TH 4D 9C 2S") == PH_NOTHING && poker_pays(PH_NOTHING) == 0, "poker: a low pair pays nothing");
        // hi-lo dice
        check(dice_win(10, 0, 0, 6) == 20 && dice_win(10, 0, 0, 7) == 0, "dice: under 7 pays 1:1, not on 7");
        check(dice_win(0, 10, 0, 7) == 50 && dice_win(0, 0, 10, 12) == 20 && dice_win(0, 0, 10, 7) == 0,
              "dice: seven pays 4:1, over 7 pays 1:1");
        int32_t keep = casino_credit();
        casino_set(3);
        check(casino_top_up() == CASINO_TOP_UP, "casino: the cashier tops up an empty wallet");
        casino_set(800);
        check(casino_top_up() == 800, "casino: no top-up while there's credit");
        casino_set(keep);
    }
    // ---- the sound switches: All sounds, Notification sounds, Game music
    {
        AppSettings keep = g_app_settings;
        int32_t t[192];
        auto tones = [&]() {   // what the speaker played since the last call
            for (int i = 0; i < 20; i++) { haptics_update(); delay(40); }
            return emu_take_sounds(t, 64);
        };
        auto played = [&]() {  // a file started (music, game WAVs) since the last call
            int n = 0;
            while (emu_take_event(m, sizeof(m)) > 0) if (strstr(m, "\"ev\":\"play\"")) n++;
            return n;
        };
        tones(); played();
        g_app_settings.sfx_enabled = true; g_app_settings.sound_cues = true;
        haptic(HAP_NOTIFY);
        check(tones() > 0, "sound: a message plays its tune");
        g_app_settings.sound_cues = false;
        haptic(HAP_NOTIFY);
        check(tones() == 0, "sound: Notification sounds off - no tune");
        g_app_settings.sound_cues = true; g_app_settings.sfx_enabled = false;
        haptic(HAP_NOTIFY);
        check(tones() == 0, "sound: All sounds off - no message tune");
        game_sfx_score();
        check(tones() == 0, "sound: All sounds off - no game effects");
        g_app_settings.sfx_enabled = true;
        game_sfx_score();
        check(tones() > 0, "sound: game effects with All sounds on");
        // Game music: nothing while it's off, it starts when turned on, stops when off again
        SD.mkdir("/music");
        { File f = SD.open("/music/test.mp3", FILE_WRITE); if (f) { f.write((const uint8_t *)"ID3", 3); f.close(); } }
        g_app_settings.music_enabled = false;
        game_music_start("/music/test.mp3");
        game_music_poll();
        check(played() == 0, "sound: Game music off - no music");
        g_app_settings.music_enabled = true;
        delay(600);
        game_music_poll();
        check(played() == 1, "sound: Game music on - the music starts");
        g_app_settings.music_enabled = false;
        game_music_poll();
        check(!audio_is_playing(), "sound: Game music off mid-game - it stops");
        // Breathe: a chime to start, notes as each breath turns - and none with All sounds off
        for (int pass = 0; pass < 2; pass++) {
            g_app_settings.sfx_enabled = pass == 0;
            tones();
            ui_push(&breathe_screen);
            for (int i = 0; i < 5; i++) emu_step();
            emu_touch(233, 392, 1); emu_step(); emu_touch(233, 392, 0);   // Start
            int n = 0;
            for (int i = 0; i < 400; i++) { emu_step(); delay(20); n += emu_take_sounds(t, 64); }   // ~8 s: start, in, out
            ui_pop_screen();
            emu_step();
            printf("breathe pass %d: %d tones\n", pass, n);
            if (pass == 0) check(n >= 7, "sound: Breathe - a start chime and notes for in and out");
            else check(n == 0, "sound: Breathe - silent with All sounds off");
        }
        g_app_settings.sfx_enabled = true;
        game_music_stop();
        SD.remove("/music/test.mp3");
        SD.rmdir("/music");
        g_app_settings = keep;
        tones(); played();
    }
    return s_fail;
}

#ifdef __ANDROID__
// ---- the floating watch over other apps (EmuOverlay.kt), through JNI -----------------
// The frame into a direct ByteBuffer (RGBA, the order Bitmap.copyPixelsFromBuffer
// takes) when it changed since `seq`; returns the frame's number.
JNIEXPORT jint JNICALL Java_com_amoledwatch_amoled_1companion_EmuOverlay_nativeFrame(JNIEnv *env, jclass, jobject buf, jint seq) {
    uint32_t now = emu_frame_seq();
    if ((jint)now == seq && now != 0) return seq;
    void *p = env->GetDirectBufferAddress(buf);
    if (!p || env->GetDirectBufferCapacity(buf) < (jlong)FB_PIXELS * 4) return seq;
    emu_frame_rgba((uint8_t *)p);
    return (jint)now;
}
JNIEXPORT void JNICALL Java_com_amoledwatch_amoled_1companion_EmuOverlay_nativeTouch(JNIEnv *, jclass, jint x, jint y, jint down) {
    overlay_push(0, x, y, down);
}
JNIEXPORT void JNICALL Java_com_amoledwatch_amoled_1companion_EmuOverlay_nativeButton(JNIEnv *, jclass, jint which, jint ev) {
    overlay_push(1, which, ev, 0);
}
JNIEXPORT jint JNICALL Java_com_amoledwatch_amoled_1companion_EmuOverlay_nativeSize(JNIEnv *, jclass) { return LCD_WIDTH; }
#endif

}  // extern "C"
