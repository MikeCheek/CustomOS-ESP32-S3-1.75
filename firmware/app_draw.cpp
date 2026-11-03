/*
 * app_draw.cpp - "Draw to open": write a letter with one stroke and the app
 * opens (stroke_recog.h). Tools > Draw.
 */
#include "stroke_recog.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_audio.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

extern Screen assistant_screen, calendar_screen, music_screen, notifications_screen, phone_screen, settings_screen,
    recorder_screen, sleep_screen;

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int MAXP = 400;

struct Target { char c; const char *name; Screen *s; };
static const Target TARGETS[] = {
    {'A', "Assistant", &assistant_screen}, {'C', "Calendar", &calendar_screen},
    {'M', "Music", &music_screen},         {'N', "Notifications", &notifications_screen},
    {'P', "Phone", &phone_screen},         {'S', "Settings", &settings_screen},
    {'V', "Voice recorder", &recorder_screen}, {'Z', "Sleep", &sleep_screen},
};

static StrokePt s_pts[MAXP];
static int s_n = 0;
static bool s_down = false;
static char s_msg[40] = "";
static uint32_t s_msg_ms = 0;

static void draw_create() { s_n = 0; s_down = false; s_msg[0] = 0; }

static void draw_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_n == 0 && !s_down) {
        ui_text_center(CX, 92, COLOR_TEXT, "Draw a letter", 2);
        // the letters and their apps, two columns
        for (int i = 0; i < 8; i++) {
            int x = i < 4 ? CX - 150 : CX + 10, y = 160 + (i % 4) * 40;
            char b[24];
            snprintf(b, sizeof(b), "%c", TARGETS[i].c);
            ui_print(x, y, 2, COLOR_ACCENT2, b);
            ui_print(x + 30, y + 4, 1, COLOR_TEXT_DIM, TARGETS[i].name);
        }
    }
    // the stroke so far
    for (int i = 1; i < s_n; i++) {
        float dx = s_pts[i].x - s_pts[i - 1].x, dy = s_pts[i].y - s_pts[i - 1].y;
        int steps = (int)(sqrtf(dx * dx + dy * dy) / 3) + 1;
        for (int k = 0; k <= steps; k++)
            g->fillCircle((int)(s_pts[i - 1].x + dx * k / steps), (int)(s_pts[i - 1].y + dy * k / steps), 5, COLOR_ACCENT);
    }
    if (s_msg[0] && millis() - s_msg_ms < 1500) ui_text_center(CX, LCD_HEIGHT - 70, COLOR_TEXT_DIM, s_msg, 1);
}

static void draw_touch(int x, int y, bool pressed) {
    if (pressed) {
        if (!s_down) { s_n = 0; s_down = true; s_msg[0] = 0; }
        if (s_n < MAXP && (s_n == 0 || fabsf(s_pts[s_n - 1].x - x) + fabsf(s_pts[s_n - 1].y - y) >= 3))
            s_pts[s_n++] = {(float)x, (float)y};
        return;
    }
    if (!s_down) return;
    s_down = false;
    float score = 0;
    char c = s_n >= 8 ? stroke_recognize(s_pts, s_n, &score) : 0;
    DEBUG_PRINTF("[draw] %d points -> %c (%.2f)\n", s_n, c ? c : '-', score);
    for (const Target &t : TARGETS) {
        if (t.c != c) continue;
        audio_play_sfx(1319, 50);
        ui_pop_screen();
        ui_push(t.s);
        return;
    }
    snprintf(s_msg, sizeof(s_msg), s_n < 8 ? "Draw the whole letter in one stroke" : "Didn't recognise that - try again");
    s_msg_ms = millis();
    s_n = 0;
}

static void draw_gesture(Gesture gs) {
    if (gs == GESTURE_SWIPE_LEFT || gs == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen draw_screen = {
    "Draw", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    draw_create, draw_draw, draw_touch, nullptr, nullptr, draw_gesture,
};
