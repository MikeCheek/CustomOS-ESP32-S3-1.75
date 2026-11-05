// Emergency SOS - see app_sos.h.
#include "app_sos.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "haptics.h"
#include "phone_link.h"
#include "hal_sleep.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
enum SosState { SOS_COUNT, SOS_SENT, SOS_FAILED, SOS_DONE };
static SosState s_state = SOS_COUNT;
static uint32_t s_start = 0, s_beep_s = 0;
static char s_status[96] = "";

void sos_on_reply(bool ok, const char *why) {
    if (s_state != SOS_SENT) return;
    s_state = ok ? SOS_DONE : SOS_FAILED;
    strlcpy(s_status, ok ? "Your emergency contact was texted with your location" : (why && why[0] ? why : "The phone couldn't send it"),
            sizeof(s_status));
    haptic(ok ? HAP_SUCCESS : HAP_FAILURE);
}

static void sos_create() {
    s_state = SOS_COUNT;
    s_start = millis();
    s_beep_s = 99;
    s_status[0] = 0;
}

static void send_now() {
    JsonDocument d;
    d["e"] = "sos";
    if (phone_link_active() && phone_link_send(d)) {
        s_state = SOS_SENT;
        strlcpy(s_status, "Asking your phone to send it...", sizeof(s_status));
    } else {
        s_state = SOS_FAILED;
        strlcpy(s_status, "The phone isn't connected - the app sends SOS texts", sizeof(s_status));
        haptic(HAP_FAILURE);
    }
}

static void sos_tick() {
    sleep_register_activity();
    if (s_state != SOS_COUNT) return;
    uint32_t el = millis() - s_start;
    uint32_t sec = el / 1000;
    if (sec != s_beep_s && sec < 5) { s_beep_s = sec; haptic(HAP_URGENT); }
    if (el >= 5000) send_now();
}

static void sos_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    if (s_state == SOS_COUNT) {
        uint32_t el = millis() - s_start;
        int left = 5 - (int)(el / 1000);
        float f = (el % 1000) / 1000.0f;
        g->fillCircle(CX, CY - 40, 110 + (int)(8 * sinf(f * 3.14159f)), ui_dim(COLOR_BAD, 0.3f));
        g->fillCircle(CX, CY - 40, 96, COLOR_BAD);
        char b[4];
        snprintf(b, sizeof(b), "%d", left < 1 ? 1 : left);
        ui_text_center(CX, CY - 40, COLOR_TEXT, b, 6);
        ui_text_center(CX, CY + 92, COLOR_TEXT, "Emergency SOS", 2);
        g->fillRoundRect(CX - 90, CY + 120, 180, 56, 28, COLOR_PANEL);
        ui_text_center(CX, CY + 148, COLOR_TEXT, "Cancel", 2);
        return;
    }
    uint16_t c = s_state == SOS_FAILED ? COLOR_WARN : s_state == SOS_DONE ? COLOR_GOOD : COLOR_BAD;
    g->fillCircle(CX, CY - 60, 60, ui_dim(c, 0.3f));
    ui_text_center(CX, CY - 60, c, "SOS", 4);
    const char *head = s_state == SOS_DONE ? "Help is on the way" : s_state == SOS_FAILED ? "Not sent" : "Sending...";
    ui_text_center(CX, CY + 24, COLOR_TEXT, head, 3);
    char line[96];
    strlcpy(line, s_status, sizeof(line));
    ui_fit(line, 1, 340);
    ui_text_center(CX, CY + 62, COLOR_TEXT_DIM, line, 1);
    g->fillRoundRect(CX - 80, CY + 110, 160, 52, 26, COLOR_PANEL);
    ui_text_center(CX, CY + 136, COLOR_TEXT, s_state == SOS_FAILED ? "Close" : "OK", 2);
}

static void sos_touch(int x, int y, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (!down) return;
    down = false;
    bool on_btn = abs(x - CX) < 100 && y > CY + 104 && y < CY + 182;
    if (!on_btn) return;
    if (s_state == SOS_COUNT) { haptic(HAP_STOP); ui_pop_screen(); return; }
    ui_pop_screen();
}

Screen sos_screen = {
    nullptr, GESTURE_MODE_NONE, UI_FRAME_MS_SMOOTH,
    sos_create, sos_draw, sos_touch, sos_tick, nullptr, nullptr,
    0, true,
    false, false,
    true,   // no_drag_back
};
