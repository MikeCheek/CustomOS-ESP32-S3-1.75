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
#include "sim_state.h"
#include "hal_nvs.h"
#include "dnd.h"
#include "complications.h"
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

HostSerial Serial;
// sim_stubs.cpp
void sim_touch_track(int x, int y, bool down);
void sim_link(const char *json);
bool sim_link_pop(std::string &out);
void sim_set_weather(const char *json);
void sim_set_fitness(const char *json);
void sim_prefs_enable(const char *path);

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
    }
    nvs_load_settings(g_app_settings);
    quick_replies_load();
    comp_load();
    g_app_settings.onboarding_complete = true;   // straight to the watch, no first-boot setup
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
    sim_touch_track(x, y, down != 0);
    ui_handle_touch(x, y, down != 0);
}

// which: 0 = BOOT, 1 = PWR; event: 1 short, 2 double, 3 long - the same
// meaning as on the watch (firmware.ino).
__attribute__((visibility("default"))) void emu_button(int which, int event) {
    if (which == 1) {
        if (event == 1) s_screen_off = !s_screen_off;   // the screen sleeps / wakes
        else if (event == 2) ui_pop_screen();
        s_frame_seq++;
        return;
    }
    if (s_screen_off) { s_screen_off = false; s_frame_seq++; return; }
    if (event == 1) {
        if (ui_top_panel_is_open()) ui_top_panel_close();
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

}  // extern "C"
