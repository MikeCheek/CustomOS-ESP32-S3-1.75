/*
 * app_phone.cpp
 * Companion screens backed by phone_link:
 *   phone_screen      - "Phone" app: phone battery, find my phone, shortcuts
 *   nowplaying_screen - what the phone is playing, with transport + volume
 *   call_screen       - incoming / active call, answer & decline
 */
#include "phone_link.h"
#include "phone_images.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "fx3d.h"
#include "ui_font.h"
#include "app_calendar.h"
#include "hal_vibrate.h"
#include "hal_sleep.h"
#include "hal_ble.h"
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

extern Screen notifications_screen;
extern Screen nowplaying_screen;

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

// src -> dst cut with ".." to fit max_px wide at text `size`.
static void fit(char *dst, size_t n, const char *src, int size, int max_px) {
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
    ui_utf8_trim(dst);
    ui_fit(dst, size, max_px);
}

static bool hit_circle(int x, int y, int cx, int cy, int r) {
    int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

// ---- small glyphs -----------------------------------------------------------

static void glyph_play(Arduino_GFX *g, int cx, int cy, int s, uint16_t c) {
    g->fillTriangle(cx - s / 2 + 3, cy - s, cx - s / 2 + 3, cy + s, cx + s, cy, c);
}
static void glyph_pause(Arduino_GFX *g, int cx, int cy, int s, uint16_t c) {
    g->fillRoundRect(cx - s + 2, cy - s, s * 2 / 3, s * 2, 3, c);
    g->fillRoundRect(cx + s / 3 - 2, cy - s, s * 2 / 3, s * 2, 3, c);
}
static void glyph_skip(Arduino_GFX *g, int cx, int cy, int s, bool next, uint16_t c) {
    int d = next ? 1 : -1;
    g->fillTriangle(cx - d * s, cy - s, cx - d * s, cy + s, cx + d * (s / 3), cy, c);
    g->fillRect(next ? cx + s / 3 : cx - s / 3 - 4, cy - s, 4, s * 2, c);
}
static void glyph_bell(Arduino_GFX *g, int cx, int cy, uint16_t c) {
    g->fillRoundRect(cx - 12, cy - 14, 24, 24, 11, c);
    g->fillRect(cx - 16, cy + 4, 32, 6, c);
    g->fillCircle(cx, cy + 15, 4, c);
}
static void glyph_note(Arduino_GFX *g, int cx, int cy, uint16_t c) {
    g->fillCircle(cx - 6, cy + 9, 7, c);
    g->fillRect(cx, cy - 14, 4, 24, c);
    g->fillTriangle(cx + 4, cy - 14, cx + 15, cy - 8, cx + 4, cy - 3, c);
}
static void glyph_phone(Arduino_GFX *g, int cx, int cy, uint16_t c, bool hangup) {
    if (hangup) {
        g->fillRoundRect(cx - 20, cy - 4, 40, 12, 6, c);
        g->fillRoundRect(cx - 22, cy - 2, 10, 14, 4, c);
        g->fillRoundRect(cx + 12, cy - 2, 10, 14, 4, c);
    } else {
        g->fillRoundRect(cx - 6, cy - 18, 12, 36, 6, c);
        g->fillRoundRect(cx - 14, cy - 20, 14, 10, 4, c);
        g->fillRoundRect(cx - 14, cy + 10, 14, 10, 4, c);
    }
}

// =========================================================================
//  Phone hub
// =========================================================================

static int  s_hub_press = -1;
static bool s_hub_prev = false;
static uint32_t s_hub_opened = 0;

static const int HUB_BTN_Y = CY + 92, HUB_BTN_R = 38, HUB_BTN_DX = 104;

static void phone_create() {
    s_hub_press = -1;
    s_hub_prev = false;
    s_hub_opened = millis();
    phone_link_request_state();
}

static void phone_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    if (!phone_link_active()) {
        g->fillCircle(CX, CY - 40, 40, COLOR_PANEL);
        g->fillRoundRect(CX - 16, CY - 68, 32, 56, 8, COLOR_TEXT_DIM);
        g->fillRoundRect(CX - 12, CY - 62, 24, 40, 4, COLOR_PANEL);
        ui_text_center(CX, CY + 22, COLOR_TEXT, ble_is_connected() ? "Waiting for the app" : "Phone not connected", 2);
        ui_text_center(CX, CY + 52, COLOR_TEXT_DIM, "Open the AmoledWatch app on your phone", 1);
        return;
    }

    // Phone battery ring
    int bat = phone_link_battery();
    float frac = bat >= 0 ? bat / 100.0f : 0.0f;
    uint16_t col = bat < 0 ? COLOR_TEXT_DIM : bat <= 15 ? COLOR_BAD : bat <= 35 ? COLOR_WARN : COLOR_GOOD;
    int ry = CY - 52;
    ui_arc(CX, ry, 78, 10, 0, 360, COLOR_PANEL, false);
    if (bat > 0) ui_arc(CX, ry, 78, 10, 0, 360.0f * frac, col);
    char b[16];
    if (bat >= 0) snprintf(b, sizeof(b), "%d%%", bat); else snprintf(b, sizeof(b), "--");
    ui_text_center(CX, ry - 6, COLOR_TEXT, b, 4);
    ui_text_center(CX, ry + 30, phone_link_charging() ? COLOR_GOOD : COLOR_TEXT_DIM,
                   phone_link_charging() ? "charging" : "phone", 1);

    // Now playing (or the weather, when nothing plays)
    const PhoneMedia &m = phone_link_media();
    char line[72];
    if (m.valid) {
        fit(line, sizeof(line), m.title, 1, 300);
    } else {
        snprintf(line, sizeof(line), "Phone connected");
        CalEvent ev;
        bool have_event = calendar_next(ev);
        const char *w = nullptr;
        int wl = 0;
        if (ble_get_weather(&w, &wl) && wl > 2) {
            JsonDocument wd;
            if (!deserializeJson(wd, w, wl)) {
                char cond[24];
                fit(cond, sizeof(cond), wd["condition"] | "", 2, 200);
                snprintf(line, sizeof(line), "%d C  %s", (int)lroundf(wd["tempC"] | 0.0f), cond);
            }
        }
        if (have_event) {
            // the next event beats the weather
            char when[32], t[48];
            calendar_format_when(ev, when, sizeof(when));
            snprintf(t, sizeof(t), "%s", ev.title);
            snprintf(line, sizeof(line), "%s  %s", when, t);
            ui_fit(line, 2, 320);
        }
    }
    ui_text_center(CX, CY + 42, m.valid ? COLOR_TEXT : COLOR_TEXT_DIM, line, m.valid ? 1 : 2);

    // Buttons: find phone / now playing / notifications
    bool finding = phone_link_finding_phone();
    for (int i = 0; i < 3; i++) {
        int bx = CX + (i - 1) * HUB_BTN_DX;
        bool pressed = s_hub_press == i;
        uint16_t bg = (i == 0 && finding) ? COLOR_BAD : pressed ? COLOR_ACCENT : COLOR_PANEL;
        if (i == 0 && finding) {
            float p = 0.5f + 0.5f * sinf(millis() / 160.0f);
            g->fillCircle(bx, HUB_BTN_Y, HUB_BTN_R + 4 + (int)(6 * p), ui_dim(COLOR_BAD, 0.35f));
        }
        g->fillCircle(bx, HUB_BTN_Y, HUB_BTN_R, bg);
        if (i == 0) glyph_bell(g, bx, HUB_BTN_Y - 2, COLOR_TEXT);
        else if (i == 1) glyph_note(g, bx, HUB_BTN_Y, COLOR_TEXT);
        else {
            g->fillRoundRect(bx - 18, HUB_BTN_Y - 14, 36, 26, 6, COLOR_TEXT);
            g->fillTriangle(bx - 8, HUB_BTN_Y + 12, bx + 2, HUB_BTN_Y + 12, bx - 10, HUB_BTN_Y + 20, COLOR_TEXT);
        }
    }
    ui_text_center(CX - HUB_BTN_DX, HUB_BTN_Y + HUB_BTN_R + 16, COLOR_TEXT_DIM, finding ? "Stop" : "Find", 1);
    ui_text_center(CX, HUB_BTN_Y + HUB_BTN_R + 16, COLOR_TEXT_DIM, "Music", 1);
    ui_text_center(CX + HUB_BTN_DX, HUB_BTN_Y + HUB_BTN_R + 16, COLOR_TEXT_DIM, "Alerts", 1);
}

static void phone_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_hub_prev;
    s_hub_prev = pressed;
    if (edge) {
        s_hub_press = -1;
        if (x < 54 && y < 54) { ui_pop_screen(); return; }
        if (!phone_link_active()) return;
        for (int i = 0; i < 3; i++)
            if (hit_circle(x, y, CX + (i - 1) * HUB_BTN_DX, HUB_BTN_Y, HUB_BTN_R + 8)) s_hub_press = i;
    }
    if (!pressed && s_hub_press >= 0) {
        int p = s_hub_press;
        s_hub_press = -1;
        if (p == 0) phone_link_find_phone(!phone_link_finding_phone());
        else if (p == 1) ui_push(&nowplaying_screen);
        else ui_push(&notifications_screen);
    }
}

static void phone_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen phone_screen = {
    "Phone", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    phone_create, phone_draw, phone_touch, nullptr, nullptr, phone_gesture,
    250,
};

// =========================================================================
//  Now playing (phone media remote)
// =========================================================================

static int  s_np_press = -1;
static bool s_np_prev = false;
static uint32_t s_np_vol_shown_ms = 0;

static const int NP_MAIN_R = 52, NP_SIDE_R = 36, NP_SIDE_DX = 118, NP_Y = CY + 46;
static const int NP_VOL_Y = CY + 150, NP_VOL_DX = 70, NP_VOL_R = 30;

static void np_create() {
    s_np_press = -1;
    s_np_prev = false;
    phone_link_request_state();
}

static void np_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    const PhoneMedia &m = phone_link_media();
    if (!phone_link_active()) {
        ui_text_center(CX, CY - 10, COLOR_TEXT, "Phone not connected", 2);
        return;
    }

    // Progress around the edge
    if (m.valid && m.duration_s > 0) {
        float f = (float)phone_link_media_position() / (float)m.duration_s;
        if (f > 1) f = 1;
        ui_edge_ring(0, 360, COLOR_PANEL, 8, 6, false);
        if (f > 0) ui_edge_ring(0, 360.0f * f, COLOR_ACCENT, 8, 6);
    }

    // 3D spectrum ring round the play button (far half here, near half
    // after the button). The phone's audio never reaches the watch, so it
    // moves to a made-up beat while the phone plays.
    static FxBeat beat;
    uint32_t now = millis();
    fx_beat_update(beat, -1, m.valid && m.playing, now);
    fx_eq_ring_draw(g, CX, NP_Y, FX_EQ_INNER(NP_MAIN_R), 30.0f, beat, now, COLOR_ACCENT2, COLOR_ACCENT3, 0);

    char t[72], a[72];
    if (m.valid) {
        fit(t, sizeof(t), m.title, 2, 300);
        fit(a, sizeof(a), m.artist, 2, 320);
    } else {
        snprintf(t, sizeof(t), "Nothing playing");
        snprintf(a, sizeof(a), "Start music on your phone");
    }
    ui_text_center(CX, CY - 92, COLOR_TEXT, t, 2);
    ui_text_center(CX, CY - 58, COLOR_TEXT_DIM, a, 2);
    if (m.valid && m.duration_s > 0) {
        int p = phone_link_media_position();
        char tm[24];
        snprintf(tm, sizeof(tm), "%d:%02d / %d:%02d", p / 60, p % 60, m.duration_s / 60, m.duration_s % 60);
        ui_text_center(CX, CY - 26, COLOR_TEXT_DIM, tm, 1);
    }

    // transport: play/pause, the ring's near half, then the side buttons
    static const int ORDER[3] = {1, 0, 2};
    for (int oi = 0; oi < 3; oi++) {
        int i = ORDER[oi];
        if (oi == 1) fx_eq_ring_draw(g, CX, NP_Y, FX_EQ_INNER(NP_MAIN_R), 30.0f, beat, now, COLOR_ACCENT2, COLOR_ACCENT3, 1);
        int bx = CX + (i - 1) * NP_SIDE_DX;
        int r = i == 1 ? NP_MAIN_R : NP_SIDE_R;
        bool pressed = s_np_press == i;
        // The album cover fills the play button (dimmed so the glyph reads).
        int aw = 0, ah = 0;
        const uint16_t *art = (i == 1 && m.valid && !pressed) ? phone_art(m.art, &aw, &ah) : nullptr;
        if (art) {
            phone_image_draw_round(g, art, aw, ah, bx, NP_Y, r, 18);
            g->drawCircle(bx, NP_Y, r, COLOR_ACCENT);
            g->drawCircle(bx, NP_Y, r - 1, COLOR_ACCENT);
        } else {
            g->fillCircle(bx, NP_Y, r, i == 1 ? (pressed ? COLOR_TEXT : COLOR_ACCENT) : (pressed ? COLOR_ACCENT : COLOR_PANEL));
        }
        uint16_t c = (i == 1 && pressed) ? COLOR_ACCENT : COLOR_TEXT;
        if (i == 0) glyph_skip(g, bx, NP_Y, 14, false, c);
        else if (i == 2) glyph_skip(g, bx, NP_Y, 14, true, c);
        else if (m.playing) glyph_pause(g, bx, NP_Y, 20, c);
        else glyph_play(g, bx, NP_Y, 20, c);
    }

    // volume - / bar / +
    for (int i = 0; i < 2; i++) {
        int bx = CX + (i ? NP_VOL_DX : -NP_VOL_DX);
        bool pressed = s_np_press == 3 + i;
        g->fillCircle(bx, NP_VOL_Y, NP_VOL_R, pressed ? COLOR_ACCENT : COLOR_PANEL);
        g->fillRect(bx - 11, NP_VOL_Y - 2, 22, 4, COLOR_TEXT);
        if (i) g->fillRect(bx - 2, NP_VOL_Y - 11, 4, 22, COLOR_TEXT);
    }
    if (m.volume_max > 0) {
        int w = 60, h = 6;
        int fill = w * m.volume / m.volume_max;
        g->fillRoundRect(CX - w / 2, NP_VOL_Y - h / 2, w, h, 3, COLOR_PANEL);
        if (fill > 0) g->fillRoundRect(CX - w / 2, NP_VOL_Y - h / 2, fill, h, 3, COLOR_TEXT_DIM);
    }
}

static int np_hit(int x, int y) {
    for (int i = 0; i < 3; i++)
        if (hit_circle(x, y, CX + (i - 1) * NP_SIDE_DX, NP_Y, (i == 1 ? NP_MAIN_R : NP_SIDE_R) + 8)) return i;
    for (int i = 0; i < 2; i++)
        if (hit_circle(x, y, CX + (i ? NP_VOL_DX : -NP_VOL_DX), NP_VOL_Y, NP_VOL_R + 8)) return 3 + i;
    return -1;
}

static void np_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_np_prev;
    s_np_prev = pressed;
    if (edge) {
        if (x < 54 && y < 54) { ui_pop_screen(); return; }
        s_np_press = phone_link_active() ? np_hit(x, y) : -1;
    }
    if (!pressed && s_np_press >= 0) {
        static const char *const CMDS[] = {"prev", "toggle", "next", "volDown", "volUp"};
        phone_link_media_cmd(CMDS[s_np_press]);
        vibrate_ms(25);
        s_np_press = -1;
    }
}

static void np_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen nowplaying_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    np_create, np_draw, np_touch, nullptr, nullptr, np_gesture,
    66, false, false, false, false, true, // ~15 fps when idle (the orb keeps moving); hide_status: the edge ring is the progress bar
};

// =========================================================================
//  Call
// =========================================================================

static bool s_call_open = false;
static int  s_call_press = -1;
static bool s_call_prev = false;
static uint32_t s_call_last_buzz = 0;
static uint32_t s_call_ended_ms = 0;

static const int CALL_BTN_Y = CY + 112, CALL_BTN_DX = 96, CALL_BTN_R = 46;

bool call_screen_is_open() { return s_call_open; }

static void call_create() {
    s_call_open = true;
    s_call_press = -1;
    s_call_prev = false;
    s_call_last_buzz = 0;
    s_call_ended_ms = 0;
}

static void call_destroy() {
    s_call_open = false;
}

static void call_tick() {
    const PhoneCall &c = phone_link_call();
    uint32_t now = millis();
    if (c.state == CALL_RINGING) {
        sleep_register_activity(); // keep the screen on while it rings
        if (now - s_call_last_buzz > 1400) {
            s_call_last_buzz = now;
            vibrate_ms(400);
        }
    }
    if (c.state == CALL_NONE) {
        if (!s_call_ended_ms) s_call_ended_ms = now;
        else if (now - s_call_ended_ms > 1200) ui_pop_screen();
    }
}

static void call_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    const PhoneCall &c = phone_link_call();
    uint32_t now = millis();

    // pulsing avatar
    float p = c.state == CALL_RINGING ? 0.5f + 0.5f * sinf(now / 220.0f) : 0.0f;
    int ay = CY - 70;
    if (c.state == CALL_RINGING) g->fillCircle(CX, ay, 62 + (int)(10 * p), ui_dim(COLOR_GOOD, 0.25f));
    g->fillCircle(CX, ay, 56, COLOR_PANEL);
    char ini[5];
    ui_first_char(c.name, ini);
    ui_text_center(CX, ay, COLOR_TEXT, ini, 6);

    char n[64];
    fit(n, sizeof(n), c.name[0] ? c.name : "Unknown caller", 2, 320);
    ui_text_center(CX, CY + 14, COLOR_TEXT, n, 2);

    const char *status = "Call ended";
    char dur[24];
    if (c.state == CALL_RINGING) status = "Incoming call";
    else if (c.state == CALL_ACTIVE) {
        uint32_t s = (now - c.since_ms) / 1000;
        snprintf(dur, sizeof(dur), "On call  %lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
        status = dur;
    }
    ui_text_center(CX, CY + 44, c.state == CALL_NONE ? COLOR_TEXT_DIM : COLOR_GOOD, status, 2);

    if (c.state == CALL_RINGING) {
        g->fillCircle(CX - CALL_BTN_DX, CALL_BTN_Y, CALL_BTN_R, s_call_press == 0 ? COLOR_TEXT : COLOR_BAD);
        glyph_phone(g, CX - CALL_BTN_DX, CALL_BTN_Y, s_call_press == 0 ? COLOR_BAD : COLOR_TEXT, true);
        g->fillCircle(CX + CALL_BTN_DX, CALL_BTN_Y, CALL_BTN_R, s_call_press == 1 ? COLOR_TEXT : COLOR_GOOD);
        glyph_phone(g, CX + CALL_BTN_DX, CALL_BTN_Y, s_call_press == 1 ? COLOR_GOOD : COLOR_TEXT, false);
    } else if (c.state == CALL_ACTIVE) {
        g->fillCircle(CX, CALL_BTN_Y, CALL_BTN_R, s_call_press == 0 ? COLOR_TEXT : COLOR_BAD);
        glyph_phone(g, CX, CALL_BTN_Y, s_call_press == 0 ? COLOR_BAD : COLOR_TEXT, true);
    }
}

static void call_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_call_prev;
    s_call_prev = pressed;
    const PhoneCall &c = phone_link_call();
    if (edge) {
        s_call_press = -1;
        if (c.state == CALL_RINGING) {
            if (hit_circle(x, y, CX - CALL_BTN_DX, CALL_BTN_Y, CALL_BTN_R + 10)) s_call_press = 0;
            if (hit_circle(x, y, CX + CALL_BTN_DX, CALL_BTN_Y, CALL_BTN_R + 10)) s_call_press = 1;
        } else if (c.state == CALL_ACTIVE) {
            if (hit_circle(x, y, CX, CALL_BTN_Y, CALL_BTN_R + 10)) s_call_press = 0;
        } else {
            ui_pop_screen();
        }
    }
    if (!pressed && s_call_press >= 0) {
        if (s_call_press == 0) phone_link_call_decline();
        else phone_link_call_answer();
        vibrate_ms(30);
        s_call_press = -1;
    }
}

static void call_gesture(Gesture g) {
    // Swiping away hides the screen; the call carries on on the phone.
    if (g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_DOWN) ui_pop_screen();
}

Screen call_screen = {
    "", GESTURE_MODE_FREE,
    UI_FRAME_MS_DEFAULT,
    call_create, call_draw, call_touch, call_tick, call_destroy, call_gesture,
    0, true, false, true, true, true,
};
