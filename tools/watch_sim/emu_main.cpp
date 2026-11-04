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
#include <Arduino.h>
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
#include "app_reminders.h"
#include "fitness.h"
#include "app_workout.h"
#include "hal_nvs.h"
#include "hal_sleep.h"
#include "dnd.h"
#include "complications.h"
#include <string>
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
    clock_init();                                          // alarms, world clock cities
    reminders_init();                                      // to-dos from the phone
    fitness_init();                                        // activity rings
    workout_init();                                        // saved workouts
    g_app_settings.onboarding_complete = true;   // straight to the watch, no first-boot setup
    audio_set_volume(g_app_settings.volume);
    ui_fonts_set_smooth(g_app_settings.smooth_fonts);
    ui_init(nullptr);
    watchface_registry_init();
    ui_push(watchface_get(0)->screen);
    return 1;
}

__attribute__((visibility("default"))) void emu_step() {
    if (!s_t0) return;
    phone_link_update();
    notifications_update();
    assistant_timer_update();
    clock_update();
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

}  // extern "C"
