// Host simulator for the watch UI: runs the real screen code from
// firmware/ against stubbed hardware (sim_stubs.cpp) and writes frames as
// raw RGB565 files that make_screenshots.py turns into PNGs.
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
#include <vector>
#include <string>

HostSerial Serial;

uint32_t g_sim_ms = 1000;
uint32_t millis() { return g_sim_ms; }
uint32_t micros() { return g_sim_ms * 1000; }
void delay(uint32_t ms) { g_sim_ms += ms; }
void delayMicroseconds(uint32_t us) { g_sim_ms += us / 1000; }

// ---- display pipeline: two buffers, the submitted one is "on screen" ------
#define FB_PIXELS ((size_t)LCD_WIDTH * LCD_HEIGHT)
static uint16_t *s_bufs[2];
static int s_free = 0;
static uint16_t s_shown[FB_PIXELS];
int display_pipeline_start(uint16_t *first) {
    s_bufs[0] = first;
    s_bufs[1] = (uint16_t *)calloc(FB_PIXELS, 2);
    return 2;
}
uint16_t *display_fb_acquire(bool) { uint16_t *b = s_bufs[s_free]; s_free ^= 1; return b; }
void display_fb_release(uint16_t *) {}
void display_fb_submit(uint16_t *fb) { memcpy(s_shown, fb, sizeof(s_shown)); }
const uint16_t *display_fb_last_submitted() { return s_shown; }
void display_force_full_refresh() {}

// Run the UI for `ms` of simulated time.
void sim_run(uint32_t ms) {
    uint32_t end = g_sim_ms + ms;
    while (g_sim_ms < end) {
        g_sim_ms += 4;
        phone_link_update();
        notifications_update();
        ui_update();
        ui_poll_gestures();
    }
}

void sim_tap(int x, int y) {
    ui_handle_touch(x, y, true);
    sim_run(60);
    ui_handle_touch(x, y, false);
    sim_run(60);
}

void sim_save(const char *name) {
    std::string path = std::string(getenv("SIM_OUT") ? getenv("SIM_OUT") : ".") + "/" + name + ".rgb565";
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return; }
    fwrite(s_shown, 2, FB_PIXELS, f);
    fclose(f);
    fprintf(stderr, "[sim] saved %s\n", path.c_str());
}

void sim_scenarios();   // sim_scenarios.cpp

int main() {
    g_app_settings.onboarding_complete = true;
    g_app_settings.smooth_fonts = true;
    ui_fonts_set_smooth(true);
    ui_init(nullptr);
    watchface_registry_init();
    sim_scenarios();
    return 0;
}
