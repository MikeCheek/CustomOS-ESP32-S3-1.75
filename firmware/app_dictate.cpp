#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_audio.h"
#include "hal_sd.h"
#include "hal_ble.h"
#include "phone_link.h"
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <math.h>
#include <string.h>

// =========================================================================
//  Voice reply: record a short message, the phone transcribes it (whisper,
//  on the phone) and sends the text back, the user checks it and sends it
//  as the notification's reply.
//
//   watch: records /Recordings/_reply.wav (mono, not listed as a memo)
//   watch -> phone  {"e":"dict","id":<notif id>,"f":"_reply.wav"}
//   phone downloads it over the notes characteristic and transcribes
//   phone -> watch  {"t":"dict","id":..,"tx":"text"}  or  {"t":"dict","id":..,"err":"why"}
// =========================================================================

#define DICT_FILE     "_reply.wav"
#define DICT_MAX_S    20        // ~640 KB mono - about half a minute to transfer
#define DICT_WAIT_MS  180000

enum DictState { D_RECORDING, D_WAITING, D_CONFIRM, D_ERROR };

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static DictState s_state = D_ERROR;
static uint32_t  s_notif_id = 0;
static char      s_to[48] = "";
static char      s_text[400] = "";
static char      s_error[96] = "";
static uint32_t  s_state_ms = 0;
static int       s_press = -1;
static uint32_t  s_seq = 0;            // attempt number - a late answer to an old take is ignored
static bool      s_open = false;
static int       s_confirm_by = 320;   // button row y, laid out by the draw

static void set_state(DictState st) { s_state = st; s_state_ms = millis(); s_press = -1; }

static void fail(const char *why) {
    snprintf(s_error, sizeof(s_error), "%s", why);
    if (audio_is_recording()) audio_stop_record();
    set_state(D_ERROR);
}

static void start_recording() {
    s_text[0] = 0;
    s_error[0] = 0;
    if (!phone_link_active()) { fail("Connect the companion app to use voice replies"); return; }
    if (!sd_is_mounted()) { fail("Voice replies need the SD card"); return; }
    if (audio_is_playing()) audio_stop_playback();
    SD.remove("/Recordings/" DICT_FILE);
    if (!audio_start_record_mono(DICT_FILE)) { fail("Microphone busy"); return; }
    set_state(D_RECORDING);
}

static void stop_and_send() {
    audio_stop_record();
    if (millis() - s_state_ms < 700) { fail("Too short - hold on a bit longer"); return; }
    char msg[112];
    s_seq++;
    snprintf(msg, sizeof(msg), "{\"e\":\"dict\",\"id\":%lu,\"n\":%lu,\"f\":\"" DICT_FILE "\"}",
             (unsigned long)s_notif_id, (unsigned long)s_seq);
    if (!ble_link_send(msg)) { fail("Phone not reachable"); return; }
    set_state(D_WAITING);
}

// ---- called from phone_link.cpp -----------------------------------------------------

void dictate_on_result(uint32_t id, uint32_t seq, const char *text, const char *err) {
    if (!s_open || s_state != D_WAITING || id != s_notif_id) return;
    if (seq != 0 && seq != s_seq) return;   // answer to an earlier take
    if (err && err[0]) {
        snprintf(s_error, sizeof(s_error), "%s", err);
        ble_fold_utf8(s_error);
        ui_utf8_trim(s_error);
        set_state(D_ERROR);
        return;
    }
    snprintf(s_text, sizeof(s_text), "%s", text ? text : "");
    ble_fold_utf8(s_text);
    ui_utf8_trim(s_text);
    if (!s_text[0]) { snprintf(s_error, sizeof(s_error), "No words recognized - try again"); set_state(D_ERROR); return; }
    set_state(D_CONFIRM);
}

// ---- screen -------------------------------------------------------------------

extern Screen dictate_screen;

void dictate_open(uint32_t notif_id, const char *to) {
    s_notif_id = notif_id;
    snprintf(s_to, sizeof(s_to), "%s", to ? to : "");
    ui_utf8_trim(s_to);
    ui_push(&dictate_screen);
}

static void dict_create() {
    s_open = true;
    start_recording();
}

static void dict_destroy() {
    s_open = false;
    if (audio_is_recording()) audio_stop_record();
}

static void dict_tick() {
    if (s_state == D_RECORDING) {
        if (!audio_is_recording()) { fail("Recording stopped"); return; }
        if (millis() - s_state_ms > DICT_MAX_S * 1000UL) stop_and_send();
    } else if (s_state == D_WAITING) {
        if (!ble_is_connected()) fail("Phone disconnected");
        else if (millis() - s_state_ms > DICT_WAIT_MS) fail("The phone didn't answer");
    }
}

// Two pill buttons side by side at y; returns hit index or -1.
static const int BTN_W = 140, BTN_H = 54, BTN_GAP = 16;
static void draw_buttons(Arduino_GFX *g, int y, const char *l, const char *r, uint16_t rc) {
    int x0 = CX - BTN_W - BTN_GAP / 2, x1 = CX + BTN_GAP / 2;
    g->fillRoundRect(x0, y, BTN_W, BTN_H, BTN_H / 2, s_press == 0 ? ui_dim(COLOR_PANEL, 0.6f) : COLOR_PANEL);
    ui_text_center(x0 + BTN_W / 2, y + BTN_H / 2, COLOR_TEXT, l, 2);
    g->fillRoundRect(x1, y, BTN_W, BTN_H, BTN_H / 2, s_press == 1 ? ui_dim(rc, 0.6f) : rc);
    ui_text_center(x1 + BTN_W / 2, y + BTN_H / 2, COLOR_TEXT, r, 2);
}
static int hit_buttons(int x, int y, int by) {
    if (y < by || y > by + BTN_H) return -1;
    if (x >= CX - BTN_W - BTN_GAP / 2 && x <= CX - BTN_GAP / 2) return 0;
    if (x >= CX + BTN_GAP / 2 && x <= CX + BTN_GAP / 2 + BTN_W) return 1;
    return -1;
}

static int wrap_center(const char *text, int y, int width, int size, uint16_t color, int max_lines) {
    const char *p = text;
    char line[100];
    int lh = 8 * size + 10, lines = 0;
    while (*p && lines < max_lines) {
        while (*p == ' ') p++;
        int n = 0, best = 0;
        while (p[n] && n < (int)sizeof(line) - 1) {
            int e = n;
            while (p[e] == ' ') e++;
            while (p[e] && p[e] != ' ' && e < (int)sizeof(line) - 1) e++;
            memcpy(line, p, e); line[e] = 0;
            if (ui_text_width(line, size) > width) break;
            best = e; n = e;
            if (!p[e]) break;
        }
        if (best == 0) { best = n > 0 ? n : (int)strlen(p); if (best > (int)sizeof(line) - 1) best = sizeof(line) - 1; }
        memcpy(line, p, best); line[best] = 0;
        ui_utf8_trim(line);
        int used = (int)strlen(line);
        if (used == 0) break;
        if (lines == max_lines - 1 && p[used]) { strncat(line, " .......", sizeof(line) - strlen(line) - 1); ui_fit(line, size, width); }
        ui_text_center(CX, y, color, line, size);
        y += lh; p += used; lines++;
    }
    return y;
}

static void dict_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    char t[64];
    if (s_to[0]) {
        snprintf(t, sizeof(t), "Reply to %s", s_to);
        ui_fit(t, 1, 280);
        ui_text_center(CX, 70, COLOR_TEXT_DIM, t, 1);
    }

    switch (s_state) {
    case D_RECORDING: {
        uint32_t el = millis() - s_state_ms;
        int lvl = audio_mic_level_percent();
        float pulse = 0.5f + 0.5f * sinf(el / 180.0f);
        int r = 62 + (int)(lvl * 0.5f) + (int)(pulse * 4);
        g->fillCircle(CX, CY - 20, r + 14, ui_dim(COLOR_BAD, 0.25f));
        g->fillCircle(CX, CY - 20, r, COLOR_BAD);
        // microphone glyph
        g->fillRoundRect(CX - 13, CY - 58, 26, 46, 13, COLOR_TEXT);
        g->fillRect(CX - 3, CY - 6, 6, 16, COLOR_TEXT);
        g->fillRoundRect(CX - 16, CY + 8, 32, 5, 2, COLOR_TEXT);
        snprintf(t, sizeof(t), "0:%02lu", (unsigned long)(el / 1000));
        ui_text_center(CX, CY + 84, COLOR_TEXT, t, 3);
        ui_text_center(CX, CY + 122, COLOR_TEXT_DIM, "Speak, then tap to finish", 1);
        ui_edge_ring(0, 360.0f * el / (DICT_MAX_S * 1000.0f), COLOR_BAD, 6, 6, true);
        break;
    }
    case D_WAITING: {
        float a = (millis() % 1200) * 0.3f;
        ui_arc(CX, CY - 20, 46, 8, a, 270, COLOR_ACCENT, true);
        ui_text_center(CX, CY + 60, COLOR_TEXT, "Transcribing", 2);
        ui_text_center(CX, CY + 90, COLOR_TEXT_DIM, "on your phone...", 1);
        break;
    }
    case D_CONFIRM: {
        int y = wrap_center(s_text, 112, 330, 2, COLOR_TEXT, 7);
        int by = y + 16 < 320 ? 320 : y + 16;
        if (by > LCD_HEIGHT - 90) by = LCD_HEIGHT - 90;
        s_confirm_by = by;
        draw_buttons(g, by, "Retry", "Send", COLOR_ACCENT);
        break;
    }
    case D_ERROR:
        ui_text_center(CX, CY - 70, COLOR_BAD, "Couldn't dictate", 2);
        wrap_center(s_error, CY - 30, 320, 1, COLOR_TEXT, 3);
        draw_buttons(g, CY + 60, "Close", "Retry", COLOR_ACCENT);
        break;
    }
}

static int confirm_btn_y() { return s_confirm_by; }

static void dict_touch(int x, int y, bool pressed) {
    static bool prev = false;
    bool edge = pressed && !prev;
    prev = pressed;
    if (s_state == D_RECORDING) {
        if (!pressed && s_press == 9) stop_and_send();
        else if (edge) s_press = 9;
        return;
    }
    if (s_state == D_WAITING) return;
    int by = s_state == D_CONFIRM ? confirm_btn_y() : CY + 60;
    if (edge) s_press = hit_buttons(x, y, by);
    if (pressed) return;
    int p = s_press;
    s_press = -1;
    if (p < 0) return;
    if (s_state == D_CONFIRM) {
        if (p == 0) start_recording();
        else {
            if (phone_link_notif_reply(s_notif_id, s_text)) {
                ui_show_toast("Reply sent", 1500);
                ui_pop_screen();
            } else {
                ui_show_toast("Phone not reachable - try again", 2000);
            }
        }
    } else { // error
        if (p == 0) ui_pop_screen();
        else start_recording();
    }
}

static void dict_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT && s_state != D_RECORDING) ui_pop_screen();
}

Screen dictate_screen = {
    "Voice reply", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    dict_create, dict_draw, dict_touch, dict_tick, dict_destroy, dict_gesture,
    0,
    true,   // suppress_idle while recording / waiting
};
