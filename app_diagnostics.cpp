#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "diag.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <esp_heap_caps.h>

// =========================================================================
//  Settings > Diagnostics: firmware version, uptime, memory, radios and
//  the last crash report (see diag.h). Scrolls; "Clear" at the bottom
//  forgets the crash report.
// =========================================================================

static const int CX = LCD_WIDTH / 2;
static const int TEXT_W = 320, TEXT_X = (LCD_WIDTH - TEXT_W) / 2;
static const int TOP = 92, LINE_H = 22, SECTION_GAP = 16;
static const int BTN_W = 200, BTN_H = 56;

#define MAX_LINES 90
struct Line { char text[72]; uint16_t color; int y; };
static Line *s_lines = nullptr;     // PSRAM
static int   s_count = 0;
static int   s_content_h = 0;
static int   s_btn_y = -1;          // content y of the Clear button, -1 = none
static UiScroll s_scroll;
static bool  s_back = false, s_btn_down = false;
static uint32_t s_refresh_ms = 0;

static void add_line(const char *t, int len, uint16_t color, int &y) {
    if (s_count >= MAX_LINES) return;
    Line &l = s_lines[s_count++];
    if (len > (int)sizeof(l.text) - 1) len = sizeof(l.text) - 1;
    memcpy(l.text, t, len);
    l.text[len] = 0;
    ui_utf8_trim(l.text);
    l.color = color;
    l.y = y;
    y += LINE_H;
}

// Word-wraps text (with '\n' line breaks) into lines at most TEXT_W wide.
static void add_wrapped(const char *text, uint16_t color, int &y) {
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int para = eol ? (int)(eol - p) : (int)strlen(p);
        while (para > 0) {
            // longest prefix that fits, preferring a space to break at
            char buf[72];
            int take = para < 71 ? para : 71;
            memcpy(buf, p, take); buf[take] = 0;
            while (take > 1 && ui_text_width(buf, 1) > TEXT_W) { take--; buf[take] = 0; }
            if (take < para) {
                int sp = take;
                while (sp > 0 && p[sp] != ' ') sp--;
                if (sp > take / 2) take = sp;
            }
            add_line(p, take, color, y);
            p += take; para -= take;
            while (para > 0 && *p == ' ') { p++; para--; }
        }
        if (!eol) break;
        p = eol + 1;
    }
}

static void build() {
    if (!s_lines) s_lines = (Line *)heap_caps_calloc(MAX_LINES, sizeof(Line), MALLOC_CAP_SPIRAM);
    if (!s_lines) return;
    s_count = 0;
    int y = TOP;
    char status[600];
    diag_status_text(status, sizeof(status));
    add_wrapped(status, COLOR_TEXT, y);
    y += SECTION_GAP;
    const char *crash = diag_crash_text();
    if (crash[0]) {
        add_line("LAST CRASH", 10, COLOR_BAD, y);
        add_wrapped(crash, COLOR_TEXT_DIM, y);
        y += SECTION_GAP;
        s_btn_y = y;
        y += BTN_H;
    } else {
        add_line("No crashes recorded", 19, COLOR_GOOD, y);
        s_btn_y = -1;
    }
    s_content_h = y + 110;
}

static void diag_create() {
    s_back = s_btn_down = false;
    build();
    ui_scroll_reset(&s_scroll, s_content_h - LCD_HEIGHT, 0);
    s_refresh_ms = millis();
}

static void diag_tick() {
    ui_scroll_tick(&s_scroll);
    // Uptime/memory change; refresh while the list is at rest.
    if (millis() - s_refresh_ms > 2000 && !s_scroll.dragging && !ui_scroll_is_moving(&s_scroll)) {
        s_refresh_ms = millis();
        build();
        ui_scroll_set_max(&s_scroll, s_content_h - LCD_HEIGHT);
    }
}

static void diag_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g || !s_lines) return;
    int off = ui_scroll_offset(&s_scroll);
    if (TOP - 40 - off > -30) ui_text_center(CX, TOP - 30 - off, COLOR_ACCENT, "Diagnostics", 2);
    for (int i = 0; i < s_count; i++) {
        int y = s_lines[i].y - off;
        if (y < -LINE_H || y > LCD_HEIGHT) continue;
        ui_print(TEXT_X, y, 1, s_lines[i].color, s_lines[i].text);
    }
    if (s_btn_y >= 0) {
        int y = s_btn_y - off;
        if (y > -BTN_H && y < LCD_HEIGHT) {
            g->fillRoundRect(CX - BTN_W / 2, y, BTN_W, BTN_H, BTN_H / 2, s_btn_down ? ui_dim(COLOR_BAD, 0.6f) : COLOR_BAD);
            ui_text_center(CX, y + BTN_H / 2, COLOR_TEXT, "Clear report", 2);
        }
    }
}

static void diag_touch(int x, int y, bool pressed) {
    if (pressed && !s_scroll.dragging && !s_back) s_back = (x < 54 && y < 54);
    if (s_back) { if (!pressed) { s_back = false; ui_pop_screen(); } return; }
    int cy = y + ui_scroll_offset(&s_scroll);
    bool on_btn = s_btn_y >= 0 && cy >= s_btn_y && cy <= s_btn_y + BTN_H && x > CX - BTN_W / 2 && x < CX + BTN_W / 2;
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    s_btn_down = pressed && on_btn && !s_scroll.moved;
    if (tap && on_btn) {
        diag_clear_crash();
        build();
        ui_scroll_reset(&s_scroll, s_content_h - LCD_HEIGHT, 0);
        ui_show_toast("Crash report cleared", 1500);
    }
}

static void diag_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen diagnostics_screen = {
    "Diagnostics", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    diag_create, diag_draw, diag_touch, diag_tick, nullptr, diag_gesture,
    500,
};
