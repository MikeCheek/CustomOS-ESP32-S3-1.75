#include "app_music.h"
#include "app_media.h"
#include "media_library.h"
#include "config.h"
#include "ui_font.h"
#include "board_pins.h"
#include "hal_sd.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <stdlib.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int ROW_W = 350, ROW_X = (LCD_WIDTH - ROW_W) / 2, ROW_H = 62, GAP = 6;
static const int TOP = 84, SHUFFLE_H = 52, LIST_TOP = TOP + SHUFFLE_H + 14;
static const int MUSIC_MAX = 4000;

static MediaList   s_songs;
static MediaScanner s_scanner;
static bool        s_scanned = false;
static uint32_t    s_scan_done_ms = 0;
static UiScroll    s_scroll;
static bool        s_back = false, s_prev = false;
static int         s_press = -2;          // -1 = shuffle pill, >=0 = song row

static int audio_filter(const char *name) { return media_is_audio(name) ? 0 : -1; }

static int content_h() { return LIST_TOP + s_songs.n * (ROW_H + GAP) - GAP + 110; }

static void music_create() {
    s_back = s_prev = false;
    s_press = -2;
    if (!s_scanned || s_songs.n == 0 || millis() - s_scan_done_ms > 60000) {
        s_scanned = false;
        s_scanner.begin(&s_songs, audio_filter, MUSIC_MAX);
    }
    ui_scroll_reset(&s_scroll, 0, ROW_H + GAP);
}

static void music_tick() {
    ui_scroll_tick(&s_scroll);
    if (!s_scanned) {
        if (s_scanner.step(12)) {
            s_songs.sort_by_name();
            s_scanned = true;
            s_scan_done_ms = millis();
        }
    }
    ui_scroll_set_max(&s_scroll, content_h() - LCD_HEIGHT);
}

static void title_of(int i, char *out, size_t n) {
    const char *nm = s_songs.name(i);
    strncpy(out, nm, n - 1); out[n - 1] = 0;
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static void folder_of(int i, char *out, size_t n) {
    const char *p = s_songs.path(i);
    const char *last = strrchr(p, '/');
    if (!last || last == p) { strncpy(out, "SD card", n - 1); out[n - 1] = 0; return; }
    const char *prev = last - 1;
    while (prev > p && *prev != '/') prev--;
    if (*prev == '/') prev++;
    size_t L = (size_t)(last - prev);
    if (L >= n) L = n - 1;
    memcpy(out, prev, L); out[L] = 0;
}

static void fit(char *s, int max_chars) {
    if ((int)strlen(s) > max_chars) { s[max_chars - 2] = '.'; s[max_chars - 1] = '.'; s[max_chars] = 0; }
}

static void note_glyph(Arduino_GFX *g, int cx, int cy, uint16_t c) {
    g->fillCircle(cx - 5, cy + 7, 5, c);
    g->fillRect(cx - 1, cy - 11, 3, 18, c);
    g->fillTriangle(cx + 2, cy - 11, cx + 11, cy - 7, cx + 2, cy - 3, c);
}

static void music_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (!sd_is_mounted()) {
        ui_text_center(CX, CY - 10, COLOR_TEXT, "No SD card", 2);
        ui_text_center(CX, CY + 18, COLOR_TEXT_DIM, "Insert a card with .mp3 or .wav files", 1);
        return;
    }
    if (!s_scanned) {
        float a = (millis() % 1000) * 0.36f;
        ui_arc(CX, CY - 20, 26, 5, a, 90, COLOR_ACCENT);
        char b[32]; snprintf(b, sizeof(b), "Scanning... %d", s_songs.n);
        ui_text_center(CX, CY + 30, COLOR_TEXT_DIM, b, 2);
        return;
    }
    if (s_songs.n == 0) {
        note_glyph(g, CX, CY - 40, COLOR_TEXT_DIM);
        ui_text_center(CX, CY + 4, COLOR_TEXT, "No music found", 2);
        ui_text_center(CX, CY + 32, COLOR_TEXT_DIM, ".mp3 and .wav files, anywhere on the card", 1);
        return;
    }
    int off = -ui_scroll_offset(&s_scroll);
    const char *now = media_now_playing_path();

    // Shuffle all
    int sy = TOP + off;
    if (sy + SHUFFLE_H > 0) {
        g->fillRoundRect(CX - 120, sy, 240, SHUFFLE_H, SHUFFLE_H / 2, s_press == -1 ? COLOR_TEXT : COLOR_ACCENT);
        // two crossing arrows
        uint16_t c = s_press == -1 ? COLOR_ACCENT : COLOR_TEXT;
        int gx = CX - 72, gy = sy + SHUFFLE_H / 2;
        g->fillTriangle(gx - 12, gy - 8, gx - 12, gy - 4, gx + 8, gy + 8, c);
        g->fillTriangle(gx - 12, gy + 8, gx - 12, gy + 4, gx + 8, gy - 8, c);
        g->fillTriangle(gx + 6, gy - 12, gx + 14, gy - 7, gx + 6, gy - 2, c);
        g->fillTriangle(gx + 6, gy + 12, gx + 14, gy + 7, gx + 6, gy + 2, c);
        g->setTextSize(2); g->setTextColor(c);
        ui_print(CX - 48, gy - 8, 2, c, "Shuffle all");
        char cnt[24];
        snprintf(cnt, sizeof(cnt), "%d song%s", s_songs.n, s_songs.n == 1 ? "" : "s");
        ui_text_center(CX, sy - 12, COLOR_TEXT_DIM, cnt, 1);
    }

    char t[40], fo[40];
    for (int i = 0; i < s_songs.n; i++) {
        int y = LIST_TOP + i * (ROW_H + GAP) + off;
        if (y + ROW_H < 0) continue;
        if (y > LCD_HEIGHT) break;
        bool playing = now && !strcmp(now, s_songs.path(i));
        bool pressed = s_press == i;
        uint16_t border = playing ? COLOR_ACCENT : (pressed ? COLOR_TEXT_DIM : COLOR_PANEL);
        g->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 18, border);
        g->fillRoundRect(ROW_X + 2, y + 2, ROW_W - 4, ROW_H - 4, 16, pressed ? ui_dim(COLOR_TEXT, 0.25f) : (playing ? ui_dim(COLOR_ACCENT, 0.25f) : COLOR_PANEL));
        int ix = ROW_X + 32, iy = y + ROW_H / 2;
        g->fillCircle(ix, iy, 20, playing ? COLOR_ACCENT : COLOR_BG);
        if (playing) {      // little equaliser
            uint32_t ms = millis();
            for (int b = 0; b < 3; b++) {
                int hgt = 6 + (int)((ms / (90 + b * 37) + b * 5) % 12);
                g->fillRect(ix - 9 + b * 7, iy + 9 - hgt, 4, hgt, COLOR_TEXT);
            }
        } else note_glyph(g, ix, iy, COLOR_ACCENT);
        title_of(i, t, sizeof(t)); fit(t, 22);
        folder_of(i, fo, sizeof(fo)); fit(fo, 40);
        g->setTextSize(2); g->setTextColor(COLOR_TEXT);
        ui_print(ROW_X + 64, y + 13, 2, COLOR_TEXT, t);
        g->setTextSize(1); g->setTextColor(COLOR_TEXT_DIM);
        ui_print(ROW_X + 64, y + 38, 1, COLOR_TEXT_DIM, fo);
    }
}

static void play_shuffled() {
    int n = s_songs.n;
    int *order = (int *)malloc((size_t)n * sizeof(int));
    if (!order) return;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = n - 1; i > 0; i--) { int j = (int)(esp_random() % (uint32_t)(i + 1)); int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
    media_play_playlist(&s_songs, order, n, 0);
    free(order);
}

static void music_touch(int x, int y, bool pressed) {
    if (pressed && !s_scroll.dragging) s_back = (x < 54 && y < 54);
    if (s_back) { if (!pressed) { s_back = false; ui_pop_screen(); } return; }
    if (!s_scanned || s_songs.n == 0) return;
    bool edge = pressed && !s_prev;
    s_prev = pressed;
    int cy = y + ui_scroll_offset(&s_scroll);
    if (edge) {
        s_press = -2;
        if (cy >= TOP && cy <= TOP + SHUFFLE_H && abs(x - CX) <= 120) s_press = -1;
        else if (x >= ROW_X && x <= ROW_X + ROW_W && cy >= LIST_TOP) {
            int i = (cy - LIST_TOP) / (ROW_H + GAP);
            if (i < s_songs.n && (cy - LIST_TOP) % (ROW_H + GAP) < ROW_H) s_press = i;
        }
    }
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    if (s_scroll.moved) s_press = -2;
    if (!pressed) {
        int p = s_press;
        s_press = -2;
        if (!tap) return;
        if (p == -1) play_shuffled();
        else if (p >= 0) media_play_playlist(&s_songs, nullptr, s_songs.n, p);
    }
}

static void music_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen music_screen = {
    "Music", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    music_create, music_draw, music_touch, music_tick, nullptr, music_gesture,
    250,
};
