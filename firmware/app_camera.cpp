// The camera remote - see app_camera.h.
#include "app_camera.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "haptics.h"
#include "phone_images.h"
#include "phone_link.h"
#include "hal_sleep.h"
#include "hal_touch.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int PV_Y = 200, PV_R = 140;   // the preview disc

// what the phone says
static bool s_on = false;        // the app's camera is open
static bool s_ext = false;       // another app's camera is open
static int  s_timer = 0;         // 0, 3, 10 s
static char s_flash[8] = "off";  // off / auto / on
static bool s_front = false, s_video = false, s_rec = false;
static int  s_zoom10 = 10;       // zoom x10
static int  s_count = 0;         // the self-timer counting down (seconds left)
static uint32_t s_rec_since = 0, s_flash_ms = 0, s_count_ms = 0, s_open_asked = 0;
static int  s_press = -1;

static void send(const char *a) {
    JsonDocument d;
    d["e"] = "cam";
    d["a"] = a;
    phone_link_send(d);
}

void camera_on_link(JsonDocument &doc) {
    if (doc["on"].is<int>()) {
        bool on = (doc["on"] | 0) != 0;
        bool ext = (doc["ext"] | 0) != 0;
        bool was = s_on || s_ext;
        s_on = on && !ext;
        s_ext = on && ext;
        if ((s_on || s_ext) && !was) {
            // like directions: the remote comes up by itself
            sleep_register_activity();
            if (!ui_screen_on_top(&camera_screen)) ui_push(&camera_screen);
        } else if (!s_on && !s_ext && ui_screen_on_top(&camera_screen)) {
            ui_pop_screen();
        }
    }
    if (doc["tm"].is<int>()) s_timer = doc["tm"] | 0;
    if (doc["fl"].is<const char *>()) strlcpy(s_flash, doc["fl"] | "off", sizeof(s_flash));
    if (doc["fr"].is<int>()) s_front = (doc["fr"] | 0) != 0;
    if (doc["vm"].is<int>()) s_video = (doc["vm"] | 0) != 0;
    if (doc["z"].is<int>()) s_zoom10 = doc["z"] | 10;
    if (doc["rec"].is<int>()) {
        bool r = (doc["rec"] | 0) != 0;
        if (r && !s_rec) { s_rec_since = millis(); haptic(HAP_START); }
        if (!r && s_rec) haptic(HAP_STOP);
        s_rec = r;
    }
    if (doc["cd"].is<int>()) { s_count = doc["cd"] | 0; s_count_ms = millis(); }
    if ((doc["ok"] | 0) != 0) {   // a photo was taken
        s_flash_ms = millis();
        s_count = 0;
        haptic(HAP_SUCCESS);
    }
}

// ---- the screen -----------------------------------------------------------------------------
// buttons: 0 shutter, 1 flip, 2 photo/video, 3 timer, 4 flash, 5 zoom -, 6 zoom +, 7 open on the phone
struct Btn { int x, y, r; };
static const Btn BTN[8] = {
    {CX, 398, 40}, {CX - 112, 380, 28}, {CX + 112, 380, 28}, {CX - 92, 70, 24},
    {CX + 92, 70, 24}, {50, PV_Y, 24}, {LCD_WIDTH - 50, PV_Y, 24}, {CX, 300, 60},
};

static int hit(int x, int y) {
    for (int i = 0; i < 8; i++) {
        if (i == 7 && !s_ext) continue;
        if (i < 7 && !s_on) continue;
        int dx = x - BTN[i].x, dy = y - BTN[i].y, r = BTN[i].r + 8;
        if (dx * dx + dy * dy <= r * r) return i;
    }
    return -1;
}

static void chip(int i, const char *text, bool lit) {
    const Btn &b = BTN[i];
    uint16_t bg = s_press == i ? COLOR_ACCENT : lit ? COLOR_TEXT : 0x31A6;
    ui_gfx()->fillCircle(b.x, b.y, b.r, bg);
    ui_text_center(b.x, b.y, lit && s_press != i ? COLOR_BG : COLOR_TEXT, text, 1);
}

static void cam_create() {
    s_press = -1;
    if (!s_on && !s_ext && phone_link_active()) { send("open"); s_open_asked = millis(); }   // from the menu: ask the phone
}

static void cam_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    uint32_t now = millis();

    if (!s_on) {
        // nothing to control (yet): say why and what to do
        ui_text_center(CX, 110, COLOR_TEXT, "Camera remote", 2);
        if (!phone_link_active()) {
            ui_text_center(CX, 200, COLOR_TEXT_DIM, "Connect the phone", 2);
        } else if (s_ext) {
            ui_text_center(CX, 170, COLOR_TEXT_DIM, "The phone's camera is open", 1);
            ui_text_center(CX, 192, COLOR_TEXT_DIM, "The remote works with the app's camera", 1);
            g->fillCircle(BTN[7].x, BTN[7].y, BTN[7].r, s_press == 7 ? COLOR_ACCENT : 0x31A6);
            ui_text_center(BTN[7].x, BTN[7].y - 8, COLOR_TEXT, "Open it", 2);
            ui_text_center(BTN[7].x, BTN[7].y + 16, COLOR_TEXT_DIM, "on the phone", 1);
        } else {
            ui_text_center(CX, 200, COLOR_TEXT_DIM,
                           now - s_open_asked < 15000 ? "Opening on the phone..." : "Open Camera remote in the app", 1);
        }
        return;
    }

    // the live view
    int w = 0, h = 0;
    const uint16_t *pv = phone_preview(&w, &h, nullptr);
    if (pv) phone_image_draw_round(g, pv, w, h, CX, PV_Y, PV_R, 255);
    else {
        g->fillCircle(CX, PV_Y, PV_R, 0x18E3);
        ui_text_center(CX, PV_Y, COLOR_TEXT_DIM, "Live view...", 1);
    }
    g->drawCircle(CX, PV_Y, PV_R, 0x4208);
    // a photo was just taken: a white flash over the view
    if (now - s_flash_ms < 180) g->fillCircle(CX, PV_Y, PV_R, COLOR_TEXT);
    // the self-timer counting down (the phone sends each second)
    if (s_count > 0 && now - s_count_ms < 1500) {
        char b[8];
        snprintf(b, sizeof(b), "%d", s_count);
        g->fillCircle(CX, PV_Y, 54, 0x0000);
        ui_text_center(CX, PV_Y, COLOR_TEXT, b, 6);
    }
    if (s_rec) {   // recording: a red dot and the time
        uint32_t sec = (now - s_rec_since) / 1000;
        char b[16];
        snprintf(b, sizeof(b), "%lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));
        g->fillRoundRect(CX - 52, PV_Y + PV_R - 46, 104, 30, 15, 0x0000);
        if ((now / 500) % 2) g->fillCircle(CX - 32, PV_Y + PV_R - 31, 7, COLOR_BAD);
        ui_print(CX - 18, PV_Y + PV_R - 41, 2, COLOR_TEXT, b);
    }

    // the top: self-timer and flash
    char t[8];
    snprintf(t, sizeof(t), s_timer ? "%ds" : "Timer", s_timer);
    chip(3, t, s_timer > 0);
    chip(4, strcmp(s_flash, "off") ? (strcmp(s_flash, "auto") ? "Flash" : "Auto") : "No fl.", strcmp(s_flash, "off") != 0);
    // zoom, at the sides of the view
    chip(5, "-", false);
    chip(6, "+", false);
    snprintf(t, sizeof(t), "%d.%dx", s_zoom10 / 10, s_zoom10 % 10);
    ui_text_center(CX, PV_Y - PV_R + 22, COLOR_TEXT, t, 1);
    // the bottom: flip, the shutter, photo/video
    chip(1, s_front ? "Front" : "Back", false);
    chip(2, s_video ? "Video" : "Photo", false);
    const Btn &sh = BTN[0];
    g->fillCircle(sh.x, sh.y, sh.r, COLOR_TEXT);
    g->fillCircle(sh.x, sh.y, sh.r - 6, COLOR_BG);
    uint16_t inner = s_video ? COLOR_BAD : COLOR_TEXT;
    if (s_press == 0) inner = ui_dim(inner, 0.6f);
    if (s_video && s_rec) g->fillRoundRect(sh.x - 15, sh.y - 15, 30, 30, 6, inner);   // stop
    else g->fillCircle(sh.x, sh.y, sh.r - 11, inner);
}

static void cam_touch(int x, int y, bool pressed) {
    static int armed = -1;
    int h = hit(x, y);
    if (pressed) {
        if (armed < 0) { armed = h; s_press = h; }
        return;
    }
    s_press = -1;
    if (armed < 0 || armed != h) { armed = -1; return; }
    armed = -1;
    sleep_register_activity();
    touch_cancel_swipes();
    static const char *const A[8] = {"shot", "flip", "mode", "timer", "flash", "zoom-", "zoom+", "open"};
    send(A[h]);
    if (h == 7) s_open_asked = millis();
}

static void cam_gesture(Gesture) {}

static void cam_destroy() { s_press = -1; }

Screen camera_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_SMOOTH,
    cam_create, cam_draw, cam_touch, nullptr, cam_destroy, cam_gesture,
    0,
    true,    // suppress_idle: the phone is propped up somewhere, the watch stays on
};
