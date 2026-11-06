/*
 * app_assistant.cpp
 * Voice assistant - see app_assistant.h.
 */
#include "app_assistant.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_audio.h"
#include "hal_sd.h"
#include "hal_ble.h"
#include "hal_sleep.h"
#include "phone_link.h"
#include "app_settings_state.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <math.h>
#include <string.h>

#define ASK_FILE      "_ask.wav"
#define ASK_MAX_S     10          // a question, not a memo - and it has to cross Bluetooth
#define ASK_WAIT_MS   120000
#define QUIET_STOP_MS 1300        // this long quiet after speaking ends the question
#define FOLLOW_WAIT_MS 4000       // a follow-up: this long to start talking
#define FOLLOW_MAX    3           // follow-ups in a row before it waits for a tap

enum AskState { A_LISTEN, A_WAIT, A_ANSWER, A_ERROR };

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static AskState s_state = A_ERROR;
static uint32_t s_state_ms = 0;
static uint32_t s_seq = 0;
static bool s_open = false;
static int s_press = -1;
static char s_question[200] = "";
static char s_answer[400] = "";
static char s_status[48] = "";
static char s_chip[40] = "";        // what was done ("Timer 5:00")
// voice activity: the noise floor while listening, and when speech was last heard
static int s_floor = 100;
static bool s_spoke = false;
static uint32_t s_last_voice_ms = 0;
// follow-ups: the answer is spoken, then the watch listens again by itself
static bool s_follow = false;        // this listen is a follow-up
static bool s_speaks = false, s_follow_ok = false, s_heard_say = false;
static int s_followups = 0;
static uint32_t s_answer_ms = 0;
static uint32_t s_expect_until = 0;

void assistant_expect_speech() { s_expect_until = millis() + 60000; }
bool assistant_speech_expected() { return s_expect_until && (int32_t)(s_expect_until - millis()) > 0; }

static void set_state(AskState st) { s_state = st; s_state_ms = millis(); s_press = -1; }

static void fail(const char *why) {
    snprintf(s_answer, sizeof(s_answer), "%s", why);
    if (audio_is_recording()) audio_stop_record();
    set_state(A_ERROR);
}

// ---- timer -----------------------------------------------------------------------------
static uint32_t s_timer_end_ms = 0, s_timer_len_s = 0;
static uint32_t s_timer_paused_left_ms = 0;   // > 0: paused with this much left
extern Screen timer_alert_screen;

void assistant_timer_set(uint32_t seconds) {
    s_timer_len_s = seconds;
    s_timer_paused_left_ms = 0;
    s_timer_end_ms = seconds ? millis() + seconds * 1000UL : 0;
    if (!s_timer_end_ms && seconds) s_timer_end_ms = 1;   // millis() wrapped to 0
}
bool assistant_timer_active() { return s_timer_end_ms != 0 || s_timer_paused_left_ms != 0; }
bool assistant_timer_paused() { return s_timer_paused_left_ms != 0; }
uint32_t assistant_timer_length_s() { return s_timer_len_s; }
uint32_t assistant_timer_left_ms() {
    if (s_timer_paused_left_ms) return s_timer_paused_left_ms;
    if (!s_timer_end_ms) return 0;
    int32_t left = (int32_t)(s_timer_end_ms - millis());
    return left > 0 ? (uint32_t)left : 0;
}
uint32_t assistant_timer_left_s() { return (assistant_timer_left_ms() + 999) / 1000; }
void assistant_timer_pause(bool pause) {
    if (pause && s_timer_end_ms) {
        s_timer_paused_left_ms = assistant_timer_left_ms();
        if (!s_timer_paused_left_ms) s_timer_paused_left_ms = 1;
        s_timer_end_ms = 0;
    } else if (!pause && s_timer_paused_left_ms) {
        s_timer_end_ms = millis() + s_timer_paused_left_ms;
        if (!s_timer_end_ms) s_timer_end_ms = 1;
        s_timer_paused_left_ms = 0;
    }
}

void assistant_timer_update() {
    if (!s_timer_end_ms || (int32_t)(millis() - s_timer_end_ms) < 0) return;
    s_timer_end_ms = 0;
    sleep_register_activity();          // wakes the screen
    if (!ui_screen_on_top(&timer_alert_screen)) ui_push(&timer_alert_screen);
}

static void fmt_dur(char *b, int n, uint32_t s) {
    if (s >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// ---- asking ------------------------------------------------------------------------------
static void start_listening(bool follow = false) {
    s_follow = follow;
    if (!follow) { s_question[0] = s_answer[0] = s_chip[0] = s_status[0] = 0; s_followups = 0; }
    s_speaks = false;
    if (!phone_link_active()) { fail("Connect the companion app to use the assistant"); return; }
    if (!sd_is_mounted()) { fail("The assistant needs the SD card"); return; }
    if (audio_is_playing()) audio_stop_playback();
    SD.remove("/Recordings/" ASK_FILE);
    if (!audio_start_record_mono(ASK_FILE)) { fail("Microphone busy"); return; }
    s_floor = 100;
    s_spoke = false;
    s_last_voice_ms = millis();
    set_state(A_LISTEN);
}

static void send_ask() {
    char msg[80];
    s_seq++;
    snprintf(msg, sizeof(msg), "{\"e\":\"ask\",\"n\":%lu,\"f\":\"" ASK_FILE "\"%s}", (unsigned long)s_seq,
             s_follow ? ",\"fu\":1" : "");
    if (!ble_link_send(msg)) { fail("Phone not reachable"); return; }
    snprintf(s_status, sizeof(s_status), "Sending");
    set_state(A_WAIT);
}

// A follow-up nobody asked: back to the answer, quietly.
static void follow_give_up() {
    audio_stop_record();
    s_follow = false;
    s_speaks = false;
    set_state(A_ANSWER);
}

static void stop_and_send() {
    if (s_follow && !s_spoke) { follow_give_up(); return; }
    audio_stop_record();
    if (!s_spoke && millis() - s_state_ms < 900) { fail("Didn't hear anything - tap and ask again"); return; }
    send_ask();
}

// ---- a question that's already recorded (after the wake word) -------------------------
static bool s_prerecorded = false;   // the next open sends _ask.wav instead of recording

static void put_le(uint8_t *p, uint32_t v, int n) { for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

bool assistant_ask_recorded(const int16_t *pcm, int samples, int rate) {
    if (!phone_link_active() || !sd_is_mounted() || samples <= 0 || audio_is_recording()) return false;
    SD.mkdir("/Recordings");
    SD.remove("/Recordings/" ASK_FILE);
    File f = SD.open("/Recordings/" ASK_FILE, FILE_WRITE);
    if (!f) return false;
    uint32_t bytes = (uint32_t)samples * 2;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); put_le(h + 4, 36 + bytes, 4); memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4); put_le(h + 20, 1, 2); put_le(h + 22, 1, 2);        // PCM, mono
    put_le(h + 24, rate, 4); put_le(h + 28, rate * 2, 4); put_le(h + 32, 2, 2); put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4); put_le(h + 40, bytes, 4);
    bool ok = f.write(h, 44) == 44 && f.write((const uint8_t *)pcm, bytes) == bytes;   // little-endian already
    f.close();
    if (!ok) { SD.remove("/Recordings/" ASK_FILE); return false; }
    s_prerecorded = true;
    if (ui_screen_on_top(&assistant_screen)) ui_pop_screen();
    ui_push(&assistant_screen);
    return true;
}

// ---- called from phone_link.cpp ---------------------------------------------------------
static void clean(char *s) { ble_fold_utf8(s); ui_utf8_trim(s); }

void assistant_on_status(uint32_t seq, const char *status) {
    if (!s_open || s_state != A_WAIT || seq != s_seq) return;
    snprintf(s_status, sizeof(s_status), "%s", status ? status : "");
    clean(s_status);
}

void assistant_on_result(uint32_t seq, const char *question, const char *answer, const char *action,
                         int32_t seconds, const char *err, bool spoken, bool follow) {
    // The watch-side action happens even if the screen was closed meanwhile.
    if (action && !strcmp(action, "timer") && seq == s_seq) assistant_timer_set(seconds > 0 ? seconds : 0);
    if (!s_open || s_state != A_WAIT || seq != s_seq) return;
    snprintf(s_question, sizeof(s_question), "%s", question ? question : "");
    clean(s_question);
    if (err && err[0]) {
        snprintf(s_answer, sizeof(s_answer), "%s", err);
        clean(s_answer);
        set_state(A_ERROR);
        return;
    }
    snprintf(s_answer, sizeof(s_answer), "%s", answer ? answer : "");
    clean(s_answer);
    s_chip[0] = 0;
    if (action && !strcmp(action, "timer")) {
        if (seconds > 0) { char d[16]; fmt_dur(d, sizeof(d), seconds); snprintf(s_chip, sizeof(s_chip), "Timer %s", d); }
        else snprintf(s_chip, sizeof(s_chip), "Timer cancelled");
    }
    s_speaks = spoken;
    s_follow_ok = spoken && follow;
    s_heard_say = false;
    s_answer_ms = millis();
    s_follow = false;
    set_state(A_ANSWER);
}

// ---- drawing -------------------------------------------------------------------------------
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
        if (lines == max_lines - 1 && p[used]) { strncat(line, " ...", sizeof(line) - strlen(line) - 1); ui_fit(line, size, width); }
        ui_text_center(CX, y, color, line, size);
        y += lh; p += used; lines++;
    }
    return y;
}

static const int MIC_Y = LCD_HEIGHT - 92, MIC_R = 36;

static void draw_mic_button(Arduino_GFX *g, int y, int r, uint16_t c) {
    g->fillCircle(CX, y, r, c);
    g->fillRoundRect(CX - 7, y - 18, 14, 24, 7, COLOR_BG);
    g->drawCircle(CX, y - 2, 12, COLOR_BG);
    g->fillRect(CX - 13, y - 14, 26, 12, c);                 // keep only the lower half of the ring
    g->fillRect(CX - 1, y + 10, 3, 6, COLOR_BG);
    g->fillRect(CX - 7, y + 15, 15, 2, COLOR_BG);
}

static void assist_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    uint32_t now = millis();
    switch (s_state) {
    case A_LISTEN: {
        int lvl = audio_mic_level_percent();
        // a soft ring that breathes with the voice
        int r = 58 + lvl / 2;
        g->fillCircle(CX, CY - 10, r + 10, ui_dim(COLOR_ACCENT, 0.18f));
        g->fillCircle(CX, CY - 10, r, ui_dim(COLOR_ACCENT, 0.45f));
        draw_mic_button(g, CY - 10, 44, COLOR_ACCENT);
        ui_text_center(CX, CY + 92, COLOR_TEXT, s_follow ? "Anything else?" : "Listening", 2);
        ui_text_center(CX, CY + 122, COLOR_TEXT_DIM, s_follow ? "Just ask - or tap to stop" : "Ask, then pause - or tap", 1);
        if (s_follow && !s_spoke)
            ui_edge_ring(0, 360.0f * (1.0f - (now - s_state_ms) / (float)FOLLOW_WAIT_MS), COLOR_ACCENT2, 4, 6, true);
        else
            ui_edge_ring(0, 360.0f * (now - s_state_ms) / (ASK_MAX_S * 1000.0f), COLOR_ACCENT, 4, 6, true);
        break;
    }
    case A_WAIT: {
        float a = (now % 1200) * 0.3f;
        ui_arc(CX, CY - 10, 46, 6, a, 270, COLOR_ACCENT, true);
        ui_text_center(CX, CY + 70, COLOR_TEXT, s_status[0] ? s_status : "Thinking", 2);
        ui_text_center(CX, CY + 100, COLOR_TEXT_DIM, "on your phone", 1);
        break;
    }
    case A_ANSWER: {
        int y = 112;
        if (s_question[0]) y = wrap_center(s_question, y, 290, 1, COLOR_TEXT_DIM, 2) + 8;
        y = wrap_center(s_answer, y + 8, 340, 2, COLOR_TEXT, 5);
        if (s_chip[0]) {
            char c[40];
            if (assistant_timer_active() && !strncmp(s_chip, "Timer ", 6) && strcmp(s_chip, "Timer cancelled")) {
                char d[16];
                fmt_dur(d, sizeof(d), assistant_timer_left_s());
                snprintf(c, sizeof(c), "Timer  %s", d);
            } else {
                snprintf(c, sizeof(c), "%s", s_chip);
            }
            int w = ui_text_width(c, 1) + 32;
            g->fillRoundRect(CX - w / 2, y + 2, w, 30, 15, COLOR_PANEL);
            ui_text_center(CX, y + 17, COLOR_ACCENT, c, 1);
        }
        draw_mic_button(g, MIC_Y, MIC_R, s_press == 0 ? ui_dim(COLOR_ACCENT, 0.6f) : COLOR_ACCENT);
        break;
    }
    case A_ERROR:
        ui_text_center(CX, CY - 70, COLOR_TEXT, "Couldn't help with that", 2);
        wrap_center(s_answer, CY - 30, 320, 1, COLOR_TEXT_DIM, 4);
        draw_mic_button(g, MIC_Y, MIC_R, s_press == 0 ? ui_dim(COLOR_ACCENT, 0.6f) : COLOR_ACCENT);
        break;
    }
}

// ---- screen ------------------------------------------------------------------------------
static void assist_create() {
    s_open = true;
    if (s_prerecorded) {
        s_prerecorded = false;
        s_question[0] = s_answer[0] = s_chip[0] = s_status[0] = 0;
        send_ask();
        return;
    }
    start_listening();
}

static void assist_destroy() {
    s_open = false;
    if (audio_is_recording()) audio_stop_record();
}

static void assist_tick() {
    uint32_t now = millis();
    if (s_state == A_LISTEN) {
        if (!audio_is_recording()) { fail("Recording stopped"); return; }
        // Voice activity: learn the room's level for the first 300 ms, then
        // speech is clearly above it; a pause after speaking ends the question.
        int lvl = audio_mic_level_percent();
        if (now - s_state_ms < 300) { if (lvl < s_floor) s_floor = lvl; return; }
        if (lvl > s_floor + 10) { s_spoke = true; s_last_voice_ms = now; }
        if (s_spoke && now - s_last_voice_ms > QUIET_STOP_MS) { stop_and_send(); return; }
        if (s_follow && !s_spoke && now - s_state_ms > FOLLOW_WAIT_MS) { follow_give_up(); return; }
        if (!s_spoke && now - s_state_ms > 5000) { fail("Didn't hear anything - tap and ask again"); return; }
        if (now - s_state_ms > ASK_MAX_S * 1000UL) stop_and_send();
    } else if (s_state == A_ANSWER && s_follow_ok) {
        // the spoken answer: once it has played, listen for a follow-up
        if (audio_is_playing()) s_heard_say = true;
        else if (s_heard_say && s_followups < FOLLOW_MAX && phone_link_active()) {
            s_follow_ok = false;
            s_followups++;
            start_listening(true);
        } else if (!s_heard_say && now - s_answer_ms > 25000) s_follow_ok = false;   // the speech never came
    } else if (s_state == A_WAIT) {
        if (!ble_is_connected()) fail("Phone disconnected");
        else if (now - s_state_ms > ASK_WAIT_MS) fail("The phone didn't answer");
    }
}

static void assist_touch(int x, int y, bool pressed) {
    static bool prev = false;
    bool edge = pressed && !prev;
    prev = pressed;
    if (s_state == A_LISTEN) {          // tap anywhere: done talking
        if (!pressed && s_press == 9) stop_and_send();
        else if (edge) s_press = 9;
        return;
    }
    if (s_state == A_WAIT) return;
    bool on_mic = (x - CX) * (x - CX) + (y - MIC_Y) * (y - MIC_Y) < (MIC_R + 14) * (MIC_R + 14);
    if (edge) s_press = on_mic ? 0 : -1;
    if (pressed) return;
    int p = s_press;
    s_press = -1;
    if (p == 0 && on_mic) { s_follow_ok = false; start_listening(); }
}

static void assist_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT && s_state != A_LISTEN) ui_pop_screen();
}

Screen assistant_screen = {
    "Assistant", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    assist_create, assist_draw, assist_touch, assist_tick, assist_destroy, assist_gesture,
    0, true,        // suppress_idle: don't dim or lock while it listens or waits
};

// ---- the timer rings -----------------------------------------------------------------------
static uint32_t s_ring_ms = 0, s_beep_ms = 0;

static void ring_create() { s_ring_ms = s_beep_ms = millis() - 1000; }

static void ring_tick() {
    uint32_t now = millis();
    if (now - s_ring_ms > 60000) { ui_pop_screen(); return; }   // gives up after a minute
    if (now - s_beep_ms >= 1000) {
        s_beep_ms = now;
        audio_play_sfx(988, 120);
        audio_play_sfx(1319, 160);
    }
}

static void ring_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    uint32_t a = millis() - s_ring_ms;
    float pu = 0.5f + 0.5f * sinf(a * 0.008f);
    g->fillCircle(CX, CY - 30, 70 + (int)(pu * 8), ui_dim(COLOR_ACCENT, 0.25f));
    g->fillCircle(CX, CY - 30, 64, COLOR_ACCENT);
    char d[16];
    fmt_dur(d, sizeof(d), s_timer_len_s);
    ui_text_center(CX, CY - 30, COLOR_BG, d, 3);
    ui_text_center(CX, CY + 66, COLOR_TEXT, "Timer done", 2);
    g->fillRoundRect(CX - 80, CY + 100, 160, 50, 25, COLOR_PANEL);
    ui_text_center(CX, CY + 125, COLOR_TEXT, "Stop", 2);
}

static void ring_touch(int, int, bool pressed) {
    static bool prev = false;
    if (!pressed && prev) ui_pop_screen();   // any tap stops it
    prev = pressed;
}

Screen timer_alert_screen = {
    "Timer", GESTURE_MODE_NONE,
    UI_FRAME_MS_SMOOTH,
    ring_create, ring_draw, ring_touch, ring_tick, nullptr, nullptr,
    0, true,
};
