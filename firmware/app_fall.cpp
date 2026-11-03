#include "app_fall.h"
#include "fall_detect.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "hal_imu.h"
#include "hal_audio.h"
#include "hal_sleep.h"
#include "hal_vibrate.h"
#include "phone_link.h"
#include "ui_font.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>

static FallDetector s_det;
static bool s_alert = false;
static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const uint32_t ASK_MS = 30000;

void fall_update() {
    static uint32_t last_ms = 0;
    bool on = g_app_settings.fall_detect;
    imu_fifo_enable(on);
    if (!on) return;
    uint32_t now = millis();
    if (now - last_ms < 400) return;   // the FIFO holds 2 s
    last_ms = now;
    static float xyz[128 * 3];
    int n = imu_fifo_read(xyz, 128);
    for (int i = 0; i < n; i++) {
        if (s_det.feed(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]) && !s_alert) {
            DEBUG_PRINTF("[fall] fall detected\n");
            sleep_register_activity();       // wakes the screen
            ui_push(&fall_alert_screen);
        }
    }
}

bool fall_alert_active() { return s_alert; }

// ---- "Did you fall?" ------------------------------------------------------------------------
static uint32_t s_start = 0, s_tick_ms = 0;
static bool s_asked = false, s_prev = false;

static void alert_create() {
    s_alert = true;
    s_asked = false;
    s_start = s_tick_ms = millis();
    vibrate_ms(600);
}

static void alert_destroy() { s_alert = false; }

static void alert_tick() {
    uint32_t now = millis();
    if (!s_asked && now - s_start >= ASK_MS) {
        s_asked = true;
        phone_link_fall(false);
        vibrate_ms(800);
    }
    if (!s_asked && now - s_tick_ms >= 1000) {
        s_tick_ms = now;
        vibrate_ms(250);
        audio_play_sfx(1319, 140);
        audio_play_sfx(1760, 140);
    }
}

static void alert_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    uint32_t a = millis() - s_start;
    if (!s_asked) {
        // countdown ring
        float left = 1.0f - (float)a / ASK_MS;
        if (left < 0) left = 0;
        int segs = (int)(120 * left);
        for (int i = 0; i < 120; i++) {
            float ang = -1.5708f + i * 6.2832f / 120;
            uint16_t c = i < segs ? COLOR_BAD : ui_dim(COLOR_BAD, 0.2f);
            g->fillCircle(CX + (int)(cosf(ang) * 206), CY + (int)(sinf(ang) * 206), 4, c);
        }
        ui_text_center(CX, CY - 92, COLOR_TEXT, "Did you fall?", 3);
        char s[48];
        snprintf(s, sizeof(s), "Asking your phone for help in %lus", (unsigned long)((ASK_MS - a + 999) / 1000));
        ui_text_center(CX, CY - 46, COLOR_TEXT_DIM, s, 1);
    } else {
        ui_text_center(CX, CY - 92, COLOR_BAD, "Help requested", 3);
        ui_text_center(CX, CY - 46, COLOR_TEXT_DIM,
                       phone_link_active() ? "Your phone is alerting your contact" : "Phone not connected", 1);
    }
    g->fillRoundRect(CX - 120, CY + 6, 240, 84, 42, s_prev ? ui_dim(COLOR_GOOD, 0.6f) : COLOR_GOOD);
    ui_text_center(CX, CY + 48, COLOR_BG, "I'm OK", 3);
}

static void alert_touch(int x, int y, bool pressed) {
    bool on_btn = abs(x - CX) < 130 && y > CY && y < CY + 100;
    if (!pressed && s_prev && on_btn) {
        if (s_asked) phone_link_fall(true);   // tell the phone it's fine after all
        ui_pop_screen();
    }
    s_prev = pressed && on_btn;
}

Screen fall_alert_screen = {
    "Fall", GESTURE_MODE_NONE,
    UI_FRAME_MS_SMOOTH,
    alert_create, alert_draw, alert_touch, alert_tick, alert_destroy, nullptr,
    0, true,
};
