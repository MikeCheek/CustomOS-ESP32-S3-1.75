/*
 * app_bambu_printer.cpp
 * Bambu Lab printer monitor. Discovery/MQTT live in hal_bambu.*; the two
 * printer-side prerequisites (LAN Only + Developer Mode, and the 8-digit
 * Access Code) are explained there. First entry asks for the Access
 * Code on a compact number pad; after that this shows live status as
 * four pages - swipe left/right (or tap) to flip between them:
 *   1. Print      - progress ring, state, time left + finish time,
 *                   layer, file, current stage, error/warning banner
 *   2. Temps      - nozzle / bed / chamber vs. target, with bars
 *   3. Fans+Speed - part / aux / chamber / heatbreak fans, speed mode,
 *                   chamber light, printer WiFi signal
 *   4. Filament   - every AMS slot's colour, type and remaining %,
 *                   active slot highlighted (external spool if no AMS)
 * Which of these views is showing is decided by current_view(), shared
 * by drawing AND touch so the two can never disagree about what's on
 * screen. Dead ends the first version had are handled here: a wrong
 * Access Code offers "Re-enter Code" (it used to be permanent), and
 * "WiFi not connected" offers a direct route into WiFi Setup.
 *
 * Drawing note: only fillRect/fillRoundRect/fillCircle/fillTriangle
 * are used - no drawRoundRect (documented elsewhere in this project
 * as unreliable on this display's CO5300 driver), no drawCircle
 * outline, no drawBitmap. The progress ring stays inside radius 184 on
 * purpose: the always-on battery readout (drawn by ui.cpp at x 348-404,
 * y 64-76) sits 194+ px from centre, so a bigger ring would collide.
 * Every other position was checked against the display's safe radius.
 *
 * Touch handling: number-pad taps are rising-edge gated (see this
 * session's project-wide touch fixes). Status views track the whole
 * press so a horizontal swipe and a plain tap can be told apart on
 * release.
 */
#include <new>
#include "app_bambu_printer.h"
#include "config.h"
#include "board_pins.h"
#include "hal_bambu.h"
#include "hal_wifi.h"
#include "hal_rtc.h"
#include "hal_vibrate.h"
#include "hal_imu.h"
#include "app_wifi_setup.h"
#include "ui.h"
#include "ui_font.h"
#include <Arduino_GFX_Library.h>
#include <JPEGDEC.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

enum BambuScreenState { BS_ENTER_CODE, BS_STATUS };
enum BambuView { V_NO_WIFI, V_SEARCHING, V_NO_DEVMODE, V_CONNECTING, V_WRONG_CODE, V_WAITING, V_PAGES };
#define NUM_PAGES 6 // 0 Print, 1 Temps, 2 Fans+Speed, 3 Filament, 4 Camera, 5 Files

static BambuScreenState s_screen_state;
#define CODE_MAX_LEN 16
static char s_code_buf[CODE_MAX_LEN + 1];
static int s_code_len;
static int s_page;
static bool s_prev_pressed;
static int s_down_x, s_down_y, s_last_x, s_last_y; // swipe-vs-tap tracking
static int s_prev_page = -1; // detects page 4 (camera) enter/exit so the stream opens/closes with it

// Camera
static uint8_t *s_cam_buf = nullptr;
#define CAM_BUF_SIZE (160 * 1024)
// JPEGDEC is ~18 KB of state - allocated on the heap (lands in PSRAM)
// when the camera view is first used, instead of sitting in internal RAM.
static JPEGDEC *s_cam_jpeg_p = nullptr;
#define s_cam_jpeg (*s_cam_jpeg_p)
static bool s_cam_have_frame = false;
static uint32_t s_cam_frame_count = 0;
static uint32_t s_cam_last_frame_ms = 0;
static uint16_t *s_cam_pixels = nullptr; // decoded RGB565 cache - drawn every frame from draw_page_camera(),
static int s_cam_pix_w = 0, s_cam_pix_h = 0; // since the display clears every frame, so the image can't just be blitted once when a new JPEG arrives
#define CAM_PIX_MAX_W 280
#define CAM_PIX_MAX_H 200

// Files (FTPS browser)
#define FILES_MAX 30
static BambuFileEntry s_files[FILES_MAX];
static int s_files_count = -2; // -2 = never loaded, -1 = load failed, >=0 = entry count
static char s_files_dir[64] = "";
static bool s_files_pending_load = false; // draw "Loading..." for one frame before the blocking fetch runs
static int s_files_scroll_y = 0;
static bool s_files_scrolling = false;
static int s_files_drag_start_y, s_files_scroll_start;
static bool s_files_moved;
static char s_confirm_print_file[64] = ""; // non-empty while the "print this?" dialog is showing
static bool s_confirm_stop = false;

// Access code entry: letters toggle (codes are documented as always
// 8-digit numeric - see hal_bambu.h - but this covers the rare/unknown case)
static bool s_code_letters_mode = false;

// Liquid-bar tilt (Temps page) - raw accelerometer, deliberately not run
// through the user's game-tilt calibration, same reasoning ui.cpp uses
// for auto-rotate: a liquid surface should track gravity directly, not
// whatever a person has calibrated game steering to feel like.
static float s_tilt_x = 0, s_tilt_y = 0;
static void update_tilt() {
    ImuSample smp = imu_read();
    if (!smp.valid) return;
    s_tilt_x = s_tilt_x * 0.85f + smp.ax * 0.15f;
    s_tilt_y = s_tilt_y * 0.85f + smp.ay * 0.15f;
}


// Shared "action button" spot for the message views (Set Up WiFi /
// Change Code) - drawing and touch both use these so they can't drift.
#define MSG_PILL_CX (LCD_WIDTH / 2)
#define MSG_PILL_Y 300
#define MSG_PILL_W 180
#define MSG_PILL_H 44

#define PAD_KEY_W 80
#define PAD_KEY_H 56
#define PAD_GAP 10
#define PAD_ROW0_Y 140
#define PAD_ROW1_Y 208
#define PAD_ROW2_Y 276
#define PAD_ROW3_Y 344

static void key_rect(int col, int row_y, int *ox, int *oy, int *ow, int *oh) {
    int total_w = 3 * PAD_KEY_W + 2 * PAD_GAP;
    int x0 = LCD_WIDTH / 2 - total_w / 2;
    *ox = x0 + col * (PAD_KEY_W + PAD_GAP);
    *oy = row_y;
    *ow = PAD_KEY_W;
    *oh = PAD_KEY_H;
}

static void draw_key(Arduino_GFX *g, int x, int y, int w, int h, uint16_t color, const char *label) {
    // Inset double-fill, not drawRoundRect - see file header.
    g->fillRoundRect(x, y, w, h, 10, COLOR_TEXT_DIM);
    g->fillRoundRect(x + 2, y + 2, w - 4, h - 4, 8, color);
    ui_draw_centered_text_at(x + w / 2, y + h / 2 - 8, COLOR_TEXT, label, 2);
}

// Letters mode reuses app_wifi_setup.cpp's exact keyboard geometry
// (KB_KEY_W/H, row Y positions) verbatim - already verified safe on this
// round display, and this screen's own field ends only 6px higher than
// that screen's does, so the same clearance holds without re-deriving it.
#define KB_KEY_W 38
#define KB_KEY_H 36
#define KB_KEY_GAP 3
#define KB_ROW1_Y 150
#define KB_ROW2_Y 192
#define KB_ROW3_Y 234
static const char *CODE_ROW1 = "QWERTYUIOP";
static const char *CODE_ROW2 = "ASDFGHJKL";
static const char *CODE_ROW3 = "ZXCVBNM";

static void kb_key_rect(int index, int count, int row_y, int *ox, int *oy, int *ow, int *oh) {
    int total_w = count * KB_KEY_W + (count - 1) * KB_KEY_GAP;
    int x0 = LCD_WIDTH / 2 - total_w / 2;
    *ox = x0 + index * (KB_KEY_W + KB_KEY_GAP);
    *oy = row_y;
    *ow = KB_KEY_W;
    *oh = KB_KEY_H;
}

static bool code_ready() {
    return s_code_letters_mode ? s_code_len > 0 : s_code_len == 8;
}

static void draw_enter_code(Arduino_GFX *g) {
    ui_draw_centered_text(40, COLOR_TEXT_DIM, "Printer Access Code", 1);

    int fw = 260, fh = 44;
    int fx = LCD_WIDTH / 2 - fw / 2, fy = 84; // below the always-on battery readout (y 64-76, x 348+) - at y=66 the field ran underneath it
    g->fillRoundRect(fx, fy, fw, fh, 8, COLOR_TEXT_DIM);
    g->fillRoundRect(fx + 2, fy + 2, fw - 4, fh - 4, 6, COLOR_PANEL);
    if (s_code_len > 0) {
        ui_draw_centered_text_at(LCD_WIDTH / 2, fy + fh / 2 - 8, COLOR_TEXT, s_code_buf, 2);
    } else {
        ui_draw_centered_text_at(LCD_WIDTH / 2, fy + fh / 2 - 8, COLOR_TEXT_DIM, "8 digits", 1);
    }

    if (!s_code_letters_mode) {
        const char *labels[9] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };
        for (int row = 0; row < 3; row++) {
            int row_y = PAD_ROW0_Y + row * (PAD_ROW1_Y - PAD_ROW0_Y);
            for (int col = 0; col < 3; col++) {
                int x, y, w, h;
                key_rect(col, row_y, &x, &y, &w, &h);
                draw_key(g, x, y, w, h, COLOR_PANEL, labels[row * 3 + col]);
            }
        }
    } else {
        for (int i = 0; i < 10; i++) {
            int x, y, w, h; kb_key_rect(i, 10, KB_ROW1_Y, &x, &y, &w, &h);
            char lbl[2] = { CODE_ROW1[i], 0 };
            g->fillRoundRect(x, y, w, h, 6, COLOR_TEXT_DIM);
            g->fillRoundRect(x + 1, y + 1, w - 2, h - 2, 5, COLOR_PANEL);
            g->setTextSize(1); g->setTextColor(COLOR_TEXT); ui_print(x + w / 2 - 3, y + h / 2 - 4, 1, COLOR_TEXT, lbl);
        }
        for (int i = 0; i < 9; i++) {
            int x, y, w, h; kb_key_rect(i, 9, KB_ROW2_Y, &x, &y, &w, &h);
            char lbl[2] = { CODE_ROW2[i], 0 };
            g->fillRoundRect(x, y, w, h, 6, COLOR_TEXT_DIM);
            g->fillRoundRect(x + 1, y + 1, w - 2, h - 2, 5, COLOR_PANEL);
            g->setTextSize(1); g->setTextColor(COLOR_TEXT); ui_print(x + w / 2 - 3, y + h / 2 - 4, 1, COLOR_TEXT, lbl);
        }
        for (int i = 0; i < 7; i++) {
            int x, y, w, h; kb_key_rect(i, 7, KB_ROW3_Y, &x, &y, &w, &h);
            char lbl[2] = { CODE_ROW3[i], 0 };
            g->fillRoundRect(x, y, w, h, 6, COLOR_TEXT_DIM);
            g->fillRoundRect(x + 1, y + 1, w - 2, h - 2, 5, COLOR_PANEL);
            g->setTextSize(1); g->setTextColor(COLOR_TEXT); ui_print(x + w / 2 - 3, y + h / 2 - 4, 1, COLOR_TEXT, lbl);
        }
    }

    int x, y, w, h;
    key_rect(0, PAD_ROW3_Y, &x, &y, &w, &h);
    draw_key(g, x, y, w, h, COLOR_BAD, "<-");
    key_rect(1, PAD_ROW3_Y, &x, &y, &w, &h);
    draw_key(g, x, y, w, h, COLOR_PANEL, "0");
    key_rect(2, PAD_ROW3_Y, &x, &y, &w, &h);
    draw_key(g, x, y, w, h, code_ready() ? COLOR_GOOD : COLOR_TEXT_DIM, "OK");

    // Mode toggle pill, below the numeric row (checked safe at this y for
    // the round display - well inside the margin the corner keys already use).
    int pw = 130, ph = 30, px = LCD_WIDTH / 2 - pw / 2, py = 412;
    g->fillRoundRect(px, py, pw, ph, ph / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(px + 2, py + 2, pw - 4, ph - 4, (ph - 4) / 2, COLOR_PANEL);
    ui_draw_centered_text_at(LCD_WIDTH / 2, py + ph / 2 - 6, COLOR_ACCENT2, s_code_letters_mode ? "123" : "Has letters? ABC", 1);
}

// ---- Small drawing helpers ------------------------------------------------

static int text_w(const char *t, int size) { return ui_text_width(t, size); }

static void put_text(Arduino_GFX *g, int x, int y, uint16_t color, const char *t, int size) {
    g->setTextSize(size);
    g->setTextColor(color);
    ui_print(x, y, size, color, t);
}

static void put_right(Arduino_GFX *g, int x_right, int y, uint16_t color, const char *t, int size) {
    put_text(g, x_right - text_w(t, size), y, color, t, size);
}

static void draw_pill(Arduino_GFX *g, int cx, int by, int bw, int bh, uint16_t fill, uint16_t text_color, const char *label) {
    int bx = cx - bw / 2;
    g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, fill);
    put_text(g, cx - text_w(label, 1) / 2, by + (bh - 8) / 2, text_color, label, 1);
}

static bool hit_rect(int x, int y, int cx, int by, int bw, int bh) {
    int bx = cx - bw / 2;
    return x >= bx && x <= bx + bw && y >= by && y <= by + bh;
}

// Horizontal bar with an optional target tick (target_frac < 0 = none).
static void draw_bar(Arduino_GFX *g, int x, int y, int w, int h, float frac, uint16_t color, float target_frac) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    g->fillRoundRect(x, y, w, h, h / 2, COLOR_PANEL);
    int fw = (int)(w * frac);
    if (fw > 0) {
        int r = fw / 2 < h / 2 ? fw / 2 : h / 2;
        g->fillRoundRect(x, y, fw, h, r, color);
    }
    if (target_frac >= 0) {
        if (target_frac > 1) target_frac = 1;
        int tx = x + (int)(w * target_frac);
        g->fillRect(tx - 1, y - 3, 3, h + 6, COLOR_TEXT);
    }
}

// Ring of small dots, starting at 12 o'clock and running clockwise.
static void draw_progress_ring(Arduino_GFX *g, int cx, int cy, int R, int pct, uint16_t color) {
    const int N = 72;
    static float cs[N], sn[N];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < N; i++) {
            float a = -1.5707963f + i * 6.2831853f / N;
            cs[i] = cosf(a);
            sn[i] = sinf(a);
        }
        init = true;
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int lit = (pct * N + 50) / 100;
    if (pct > 0 && lit == 0) lit = 1;
    for (int i = 0; i < N; i++) {
        int x = cx + (int)lroundf(R * cs[i]);
        int y = cy + (int)lroundf(R * sn[i]);
        if (i < lit) g->fillCircle(x, y, i == lit - 1 ? 6 : 4, color);
        else g->fillCircle(x, y, 3, COLOR_PANEL);
    }
}

static void draw_page_dots(Arduino_GFX *g) {
    const int gap = 18, y = 388;
    int x0 = LCD_WIDTH / 2 - (NUM_PAGES - 1) * gap / 2;
    for (int i = 0; i < NUM_PAGES; i++)
        g->fillCircle(x0 + i * gap, y, i == s_page ? 5 : 3, i == s_page ? COLOR_TEXT : COLOR_TEXT_DIM);
}

static void truncate_to(const char *src, char *dst, size_t dst_len, size_t max_chars) {
    size_t len = strlen(src);
    if (len <= max_chars) { snprintf(dst, dst_len, "%s", src); return; }
    snprintf(dst, dst_len, "%.*s...", (int)(max_chars - 3), src);
}

// ---- Which view is showing -------------------------------------------------

static BambuView current_view() {
    if (!wifi_is_connected()) return V_NO_WIFI;
    if (!bambu_printer_found()) return V_SEARCHING;
    if (!bambu_printer_dev_mode_on()) return V_NO_DEVMODE;
    if (!bambu_mqtt_connected()) {
        int rc = bambu_mqtt_last_error();
        return (rc == 4 || rc == 5) ? V_WRONG_CODE : V_CONNECTING;
    }
    if (!bambu_get_state().valid) return V_WAITING;
    return V_PAGES;
}

// ---- Message views (everything before live data exists) --------------------

static void draw_dots(Arduino_GFX *g, int y) {
    int lit = (millis() / 350) % 4;
    for (int i = 0; i < 3; i++)
        g->fillCircle(LCD_WIDTH / 2 - 20 + i * 20, y, 5, i < lit ? COLOR_ACCENT3 : COLOR_TEXT_DIM);
}

static void draw_message_view(Arduino_GFX *g, BambuView v) {
    char buf[48];
    switch (v) {
        case V_NO_WIFI:
            ui_draw_centered_text(140, COLOR_WARN, "WiFi not connected", 2);
            ui_draw_centered_text(184, COLOR_TEXT_DIM, "The printer link needs WiFi", 1);
            draw_pill(g, MSG_PILL_CX, MSG_PILL_Y, MSG_PILL_W, MSG_PILL_H, COLOR_ACCENT2, COLOR_TEXT, "Set Up WiFi");
            break;
        case V_SEARCHING:
            ui_draw_centered_text(160, COLOR_TEXT, "Looking for your printer", 1);
            draw_dots(g, 196);
            ui_draw_centered_text(236, COLOR_TEXT_DIM, "It must be on the same network", 1);
            ui_draw_centered_text(254, COLOR_TEXT_DIM, "with LAN Only mode enabled", 1);
            break;
        case V_NO_DEVMODE:
            snprintf(buf, sizeof(buf), "Found %s", bambu_printer_model());
            ui_draw_centered_text(150, COLOR_TEXT, buf, 1);
            ui_draw_centered_text(190, COLOR_WARN, "LAN Only + Developer Mode", 1);
            ui_draw_centered_text(212, COLOR_WARN, "isn't enabled on the printer", 1);
            ui_draw_centered_text(244, COLOR_TEXT_DIM, "Enable it in the printer's", 1);
            ui_draw_centered_text(266, COLOR_TEXT_DIM, "network settings", 1);
            break;
        case V_CONNECTING:
            snprintf(buf, sizeof(buf), "Connecting to %s", bambu_printer_model());
            ui_draw_centered_text(170, COLOR_TEXT, buf, 1);
            draw_dots(g, 206);
            ui_draw_centered_text(246, COLOR_TEXT_DIM, "Taking a while? The access", 1);
            ui_draw_centered_text(264, COLOR_TEXT_DIM, "code may need changing", 1);
            draw_pill(g, MSG_PILL_CX, MSG_PILL_Y, MSG_PILL_W, MSG_PILL_H, COLOR_PANEL, COLOR_TEXT, "Change Code");
            break;
        case V_WRONG_CODE:
            ui_draw_centered_text(150, COLOR_BAD, "Wrong access code", 2);
            ui_draw_centered_text(196, COLOR_TEXT_DIM, "Check the 8-digit code in the", 1);
            ui_draw_centered_text(214, COLOR_TEXT_DIM, "printer's LAN Only settings", 1);
            draw_pill(g, MSG_PILL_CX, MSG_PILL_Y, MSG_PILL_W, MSG_PILL_H, COLOR_ACCENT2, COLOR_TEXT, "Re-enter Code");
            break;
        case V_WAITING:
            ui_draw_centered_text(190, COLOR_GOOD, "Connected", 2);
            ui_draw_centered_text(230, COLOR_TEXT_DIM, "Waiting for status...", 1);
            break;
        default: break;
    }
}

// ---- Page 1: Print ---------------------------------------------------------

static const char *state_label(const char *gs, uint16_t *color) {
    if (!strcmp(gs, "RUNNING")) { *color = COLOR_GOOD; return "PRINTING"; }
    if (!strcmp(gs, "PAUSE")) { *color = COLOR_WARN; return "PAUSED"; }
    if (!strcmp(gs, "FINISH")) { *color = COLOR_ACCENT2; return "FINISHED"; }
    if (!strcmp(gs, "FAILED")) { *color = COLOR_BAD; return "FAILED"; }
    if (!strcmp(gs, "PREPARE")) { *color = COLOR_ACCENT3; return "PREPARING"; }
    if (!strcmp(gs, "SLICING")) { *color = COLOR_ACCENT3; return "SLICING"; }
    if (!strcmp(gs, "INIT")) { *color = COLOR_ACCENT3; return "STARTING"; }
    if (!strcmp(gs, "IDLE")) { *color = COLOR_TEXT_DIM; return "IDLE"; }
    *color = COLOR_TEXT_DIM;
    return gs[0] ? gs : "...";
}

static void draw_page_print(Arduino_GFX *g, const BambuPrintState &st) {
    const int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2;
    uint16_t col;
    const char *label = state_label(st.gcode_state, &col);
    bool finished = !strcmp(st.gcode_state, "FINISH");
    bool active = !strcmp(st.gcode_state, "RUNNING") || !strcmp(st.gcode_state, "PAUSE");
    int pct = finished ? 100 : st.percent;

    draw_progress_ring(g, cx, cy, 176, pct, col);

    int pw = text_w(label, 1) + 28;
    // Dark blue needs light text; every other state colour is bright enough for dark text.
    uint16_t pill_text = (col == COLOR_ACCENT2) ? COLOR_TEXT : COLOR_BG;
    g->fillRoundRect(cx - pw / 2, 82, pw, 22, 11, col);
    put_text(g, cx - text_w(label, 1) / 2, 82 + 7, pill_text, label, 1);
    ui_draw_centered_text(114, COLOR_TEXT_DIM, bambu_printer_model(), 1);

    char buf[40];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    ui_draw_centered_text(138, COLOR_TEXT, buf, 5);

    if (active && st.remaining_min > 0) {
        if (st.remaining_min >= 60) snprintf(buf, sizeof(buf), "%dh %02dm left", st.remaining_min / 60, st.remaining_min % 60);
        else snprintf(buf, sizeof(buf), "%d min left", st.remaining_min);
        ui_draw_centered_text(192, COLOR_TEXT, buf, 2);
        WatchTime t = rtc_now();
        if (t.valid) {
            int total = t.hour * 60 + t.minute + st.remaining_min;
            int days = total / 1440, m = total % 1440;
            if (days == 0) snprintf(buf, sizeof(buf), "Done at %02d:%02d", m / 60, m % 60);
            else snprintf(buf, sizeof(buf), "Done at %02d:%02d +%dd", m / 60, m % 60, days);
            ui_draw_centered_text(218, COLOR_TEXT_DIM, buf, 1);
        }
    } else if (finished) {
        ui_draw_centered_text(192, COLOR_GOOD, "Print complete", 2);
    } else if (!strcmp(st.gcode_state, "FAILED")) {
        ui_draw_centered_text(192, COLOR_BAD, "Print failed", 2);
    }

    if (st.total_layer_num > 0) {
        snprintf(buf, sizeof(buf), "Layer %d / %d", st.layer_num, st.total_layer_num);
        ui_draw_centered_text(242, COLOR_TEXT_DIM, buf, 1);
    }
    if (st.filename[0]) {
        char shown[32];
        truncate_to(st.filename, shown, sizeof(shown), 26);
        ui_draw_centered_text(262, COLOR_TEXT, shown, 1);
    }
    const char *stage = bambu_stage_text(st.stage);
    if (stage[0]) ui_draw_centered_text(286, COLOR_ACCENT3, stage, 1);

    if (st.print_error != 0 || st.hms_count > 0) {
        if (st.print_error != 0) snprintf(buf, sizeof(buf), "Error %04X-%04X", (unsigned)((uint32_t)st.print_error >> 16), (unsigned)((uint32_t)st.print_error & 0xFFFF));
        else snprintf(buf, sizeof(buf), "%d warning%s", st.hms_count, st.hms_count == 1 ? "" : "s");
        int bw = text_w(buf, 1) + 28;
        g->fillRoundRect(cx - bw / 2, 314, bw, 22, 11, COLOR_BAD);
        put_text(g, cx - text_w(buf, 1) / 2, 314 + 7, COLOR_TEXT, buf, 1);
    }

    bool paused = !strcmp(st.gcode_state, "PAUSE");
    if (active) {
        int bw = 130, bh = 32, gap = 12, y0 = 358;
        int x0 = cx - bw - gap / 2;
        draw_pill(g, x0 + bw / 2, y0, bw, bh, COLOR_PANEL, COLOR_TEXT, paused ? "Resume" : "Pause");
        draw_pill(g, x0 + bw + gap + bw / 2, y0, bw, bh, COLOR_BAD, COLOR_TEXT, "Stop");
    }
}

// ---- Page 2: Temperatures --------------------------------------------------

// A "liquid in a tube" bar for temperatures: the fill's leading edge
// tilts with the watch's own roll (s_tilt_x, updated once per tick in
// update_tilt() from the raw accelerometer - see that function for why
// raw and not the user's game-tilt calibration), plus a drifting
// highlight streak and a couple of small bubbles for some life. Falls
// back to a plain flat edge when frac is too small for the tilt to have
// anywhere to go without drawing outside the tube.
static void draw_liquid_bar(Arduino_GFX *g, int x, int y, int w, int h, float frac, uint16_t color, float target_frac) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    g->fillRoundRect(x, y, w, h, h / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(x + 2, y + 2, w - 4, h - 4, (h - 4) / 2, COLOR_BG);

    int fw = (int)((w - 4) * frac);
    if (fw > 4) {
        int fx = x + 2, fy = y + 2, fh = h - 4;
        int max_x = x + w - 2; // never draw past the tube's own inner wall
        // A real diagonal boundary: the top and bottom corners of the fill
        // shift in OPPOSITE directions as roll increases, so the edge
        // genuinely slants top-to-bottom rather than just moving as a whole
        // (a single shared x for both corners would just translate the
        // same flat edge sideways, not tilt it - that was the earlier bug).
        float tilt_px = s_tilt_x * (fh * 2.4f); // exaggerated well past 1:1 with real g-force - a literal physical tilt would be barely visible at this bar's 10px height, this is for a readable effect, not a accelerometer readout
        int top_edge_x = fx + fw + (int)tilt_px;
        int bot_edge_x = fx + fw - (int)tilt_px;
        if (top_edge_x < fx) top_edge_x = fx;
        if (top_edge_x > max_x) top_edge_x = max_x;
        if (bot_edge_x < fx) bot_edge_x = fx;
        if (bot_edge_x > max_x) bot_edge_x = max_x;
        int safe_x = top_edge_x < bot_edge_x ? top_edge_x : bot_edge_x;
        // Square edge here (not fillRoundRect) - rounding this corner blurred
        // the slant into an ambiguous blob at this bar's small height; the
        // tube's own rounded ends already read as a "pill" without it.
        if (safe_x > fx) g->fillRect(fx, fy, safe_x - fx, fh, color);
        // Wedge connecting the solid body up to the two slanted corners
        g->fillTriangle(safe_x, fy, safe_x, fy + fh, bot_edge_x, fy + fh, color);
        g->fillTriangle(safe_x, fy, bot_edge_x, fy + fh, top_edge_x, fy, color);

        float t = (millis() % 2600) / 2600.0f;
        int shimmer_x = fx + (int)(t * fw);
        if (shimmer_x >= fx + 2 && shimmer_x < fx + fw - 2) {
            g->fillRect(shimmer_x, fy + 1, 2, fh - 2, COLOR_TEXT);
        }
        for (int b = 0; b < 2; b++) {
            float bt = ((millis() + (uint32_t)(b * 1300)) % 2000) / 2000.0f;
            int bx = fx + (int)(bt * (fw > 12 ? fw - 12 : fw));
            if (bx >= fx + 3 && bx < fx + fw - 3) {
                g->fillCircle(bx, fy + fh / 2 + (b == 0 ? -3 : 3), 2, COLOR_TEXT);
            }
        }
    }
    if (target_frac >= 0) {
        if (target_frac > 1) target_frac = 1;
        int tx = x + 2 + (int)((w - 4) * target_frac);
        g->fillRect(tx - 1, y - 3, 3, h + 6, COLOR_TEXT);
    }
}

static void temp_row(Arduino_GFX *g, int y, const char *label, float cur, float tgt, float maxv, uint16_t color, bool trend) {
    const int L = 93, R = 373, W = R - L;
    put_text(g, L, y + 4, COLOR_TEXT_DIM, label, 1);
    const char *note = "";
    if (trend) {
        if (tgt > 0 && cur < tgt - 4) note = "heating";
        else if (tgt <= 0 && cur > 45) note = "cooling";
    }
    if (*note) put_text(g, L + text_w(label, 1) + 8, y + 4, color, note, 1);
    char v[24];
    if (tgt > 0) snprintf(v, sizeof(v), "%d / %d C", (int)(cur + 0.5f), (int)(tgt + 0.5f));
    else snprintf(v, sizeof(v), "%d C", (int)(cur + 0.5f));
    put_right(g, R, y, COLOR_TEXT, v, 2);
    draw_liquid_bar(g, L, y + 26, W, 14, cur / maxv, color, tgt > 0 ? tgt / maxv : -1.0f);
}

static void draw_page_temps(Arduino_GFX *g, const BambuPrintState &st) {
    ui_draw_centered_text(60, COLOR_TEXT_DIM, "TEMPERATURES", 1);
    int y = 92;
    temp_row(g, y, "Nozzle", st.nozzle_temp, st.nozzle_target, 300.0f, COLOR_ACCENT3, true);
    y += 68;
    temp_row(g, y, "Bed", st.bed_temp, st.bed_target, 120.0f, COLOR_WARN, true);
    y += 68;
    if (st.chamber_temp > 0.5f) { // printers without a chamber sensor report nothing useful here
        temp_row(g, y, "Chamber", st.chamber_temp, 0, 65.0f, COLOR_ACCENT2, false);
        y += 68;
    }
    if (st.nozzle_diameter[0]) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Nozzle %s mm", st.nozzle_diameter);
        ui_draw_centered_text(y + 4, COLOR_TEXT_DIM, buf, 1);
    }
}

// ---- Page 3: Fans & speed --------------------------------------------------

// Shared by draw and touch so the speed-selector / light-toggle
// positions - which shift depending on how many fans this printer
// reports - can never disagree between what's drawn and what's tappable.
static int fans_page_content_y(const BambuPrintState &st) {
    int fan_pcts[4] = { st.fan_part, st.fan_aux, st.fan_chamber, st.fan_heatbreak };
    int y = 92, shown = 0;
    for (int i = 0; i < 4; i++) { if (fan_pcts[i] >= 0) { y += 42; shown++; } }
    if (shown == 0) y = 150;
    return y + 8;
}

static const char *SPEED_LABELS[5] = { "", "Sil", "Std", "Spt", "Lud" };

static void draw_page_fans(Arduino_GFX *g, const BambuPrintState &st) {
    ui_draw_centered_text(60, COLOR_TEXT_DIM, "FANS & SPEED", 1);
    const int L = 93, R = 373, W = R - L;
    struct { const char *name; int pct; } fans[4] = {
        { "Part cooling", st.fan_part }, { "Aux fan", st.fan_aux },
        { "Chamber fan", st.fan_chamber }, { "Heatbreak", st.fan_heatbreak },
    };
    int y = 92;
    for (int i = 0; i < 4; i++) {
        if (fans[i].pct < 0) continue; // not reported by this model
        put_text(g, L, y + 2, COLOR_TEXT_DIM, fans[i].name, 1);
        char v[8];
        snprintf(v, sizeof(v), "%d%%", fans[i].pct);
        put_right(g, R, y + 2, COLOR_TEXT, v, 1);
        draw_bar(g, L, y + 16, W, 10, fans[i].pct / 100.0f, COLOR_ACCENT2, -1.0f);
        y += 42;
    }
    if (y == 92) ui_draw_centered_text(120, COLOR_TEXT_DIM, "No fan data yet", 1);

    int cy = fans_page_content_y(st);
    ui_draw_centered_text(cy, COLOR_TEXT_DIM, "PRINT SPEED - tap to change", 1);
    int bw = 78, gap = 6, total = 4 * bw + 3 * gap;
    int x0 = LCD_WIDTH / 2 - total / 2, by = cy + 16, bh = 32;
    for (int lvl = 1; lvl <= 4; lvl++) {
        bool active = (st.speed_level == lvl);
        int bx = x0 + (lvl - 1) * (bw + gap);
        g->fillRoundRect(bx, by, bw, bh, 8, COLOR_TEXT_DIM);
        g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, 6, active ? COLOR_ACCENT2 : COLOR_PANEL);
        put_text(g, bx + bw / 2 - text_w(SPEED_LABELS[lvl], 1) / 2, by + bh / 2 - 4, COLOR_TEXT, SPEED_LABELS[lvl], 1);
    }
    if (st.speed_level >= 1 && st.speed_level <= 4 && st.speed_pct > 0) {
        char pctbuf[16];
        snprintf(pctbuf, sizeof(pctbuf), "%d%%", st.speed_pct);
        ui_draw_centered_text(by + bh + 6, COLOR_TEXT_DIM, pctbuf, 1);
    }

    int ly = by + bh + 26;
    const char *light_label = st.light_known ? (st.chamber_light ? "Light: On" : "Light: Off") : "Light: --";
    int lw = 140, lh = 32;
    draw_pill(g, LCD_WIDTH / 2, ly, lw, lh, st.chamber_light ? COLOR_ACCENT3 : COLOR_PANEL, st.chamber_light ? COLOR_BG : COLOR_TEXT, light_label);

    if (st.wifi_dbm != 0) {
        char wbuf[24];
        snprintf(wbuf, sizeof(wbuf), "Printer WiFi %d dBm", st.wifi_dbm);
        ui_draw_centered_text(ly + lh + 16, COLOR_TEXT_DIM, wbuf, 1);
    }
}

static void touch_page_fans(int x, int y_tap, const BambuPrintState &st) {
    int cy = fans_page_content_y(st);
    int bw = 78, gap = 6, total = 4 * bw + 3 * gap;
    int x0 = LCD_WIDTH / 2 - total / 2, by = cy + 16, bh = 32;
    if (y_tap >= by && y_tap <= by + bh) {
        for (int lvl = 1; lvl <= 4; lvl++) {
            int bx = x0 + (lvl - 1) * (bw + gap);
            if (x >= bx && x <= bx + bw) { vibrate_buzz(); bambu_set_speed(lvl); return; }
        }
    }
    int ly = by + bh + 26, lw = 140, lh = 32;
    if (hit_rect(x, y_tap, LCD_WIDTH / 2, ly, lw, lh)) {
        vibrate_buzz();
        bambu_set_light(!st.chamber_light);
    }
}

// ---- Page 4: Filament ------------------------------------------------------

static void draw_swatch(Arduino_GFX *g, int cx, int cy, int r, const BambuTray &t, bool active) {
    if (active) g->fillCircle(cx, cy, r + 5, COLOR_TEXT);
    g->fillCircle(cx, cy, r + 2, COLOR_TEXT_DIM); // grey rim so black filament still reads on the black background
    g->fillCircle(cx, cy, r, t.present ? t.color565 : COLOR_PANEL);
    if (!t.present) put_text(g, cx - 3, cy - 4, COLOR_TEXT_DIM, "-", 1);
}

static void tray_label(const BambuTray &t, char *buf, size_t n) {
    if (!t.present) snprintf(buf, n, "empty");
    else if (t.remain > 0) snprintf(buf, n, "%s %d%%", t.type, t.remain);
    else snprintf(buf, n, "%s", t.type);
}

static void draw_page_filament(Arduino_GFX *g, const BambuPrintState &st) {
    ui_draw_centered_text(60, COLOR_TEXT_DIM, "FILAMENT", 1);
    char buf[24];
    if (st.ams_units == 0) {
        bool ext_active = (st.active_tray == 254);
        draw_swatch(g, LCD_WIDTH / 2, 190, 34, st.ext_spool, ext_active);
        tray_label(st.ext_spool, buf, sizeof(buf));
        ui_draw_centered_text(246, COLOR_TEXT, st.ext_spool.present ? buf : "No spool detected", 1);
        ui_draw_centered_text(268, COLOR_TEXT_DIM, "No AMS connected", 1);
        return;
    }
    int n = st.ams_units;
    int gap = (n <= 2) ? 84 : 70;
    int y0 = LCD_HEIGHT / 2 - (n - 1) * gap / 2 - (n >= 3 ? 12 : 0); // lifted a little with 3-4 units so the last label row clears the page dots
    const int R = 24, DX = 76;
    for (int u = 0; u < n; u++) {
        int cy = y0 + u * gap;
        if (n > 1) {
            snprintf(buf, sizeof(buf), "A%d", u + 1);
            put_text(g, LCD_WIDTH / 2 - (DX * 3) / 2 - R - 30, cy - 4, COLOR_TEXT_DIM, buf, 1);
        }
        for (int t = 0; t < 4; t++) {
            int cx = LCD_WIDTH / 2 - (DX * 3) / 2 + t * DX;
            const BambuTray &tr = st.trays[u * 4 + t];
            draw_swatch(g, cx, cy, R, tr, st.active_tray == u * 4 + t);
            tray_label(tr, buf, sizeof(buf));
            put_text(g, cx - text_w(buf, 1) / 2, cy + R + 8, COLOR_TEXT_DIM, buf, 1);
        }
    }
    if (st.active_tray == 254) ui_draw_centered_text(366, COLOR_ACCENT3, "Using external spool", 1);
}

// ---- Camera (page 4) -------------------------------------------------------

// Mirrors app_media.cpp's jpg_cache_callback exactly (same per-pixel
// bounds clamp) - that's this project's own proven-correct pattern for
// writing JPEGDEC's decoded MCU blocks into a persistent RGB565 cache.
static int cam_jpeg_cb(JPEGDRAW *pDraw) {
    if (!s_cam_pixels) return 0;
    uint16_t *src = pDraw->pPixels;
    for (int row = 0; row < pDraw->iHeight; row++) {
        int dy = pDraw->y + row;
        if (dy < 0 || dy >= s_cam_pix_h) continue;
        for (int col = 0; col < pDraw->iWidth; col++) {
            int dx = pDraw->x + col;
            if (dx < 0 || dx >= s_cam_pix_w) continue;
            s_cam_pixels[dy * s_cam_pix_w + dx] = src[row * pDraw->iWidth + col];
        }
    }
    return 1;
}

static void decode_camera_frame(uint8_t *jpg, size_t len) {
    if (!s_cam_jpeg_p) s_cam_jpeg_p = new (std::nothrow) JPEGDEC();
    if (!s_cam_jpeg_p) return;
    s_cam_jpeg.setPixelType(RGB565_BIG_ENDIAN);
    // openRAM(), not open() - JPEGDEC's plain open() only has file/custom-
    // handle overloads; openRAM/openFLASH are the ones for a RAM buffer,
    // and per the library's own docs are specifically what ESP32/ESP8266
    // (Harvard architecture) should use. Caught by an actual build error
    // against the installed library, not assumed.
    if (!s_cam_jpeg.openRAM(jpg, (int)len, cam_jpeg_cb)) return;
    int w = s_cam_jpeg.getWidth(), h = s_cam_jpeg.getHeight();
    if (w <= 0 || h <= 0) { s_cam_jpeg.close(); return; }
    // Same scale ladder as app_media.cpp's image viewer (JPEGDEC only
    // supports these four) - pick the smallest scale that still fits the cache.
    int scales[] = { 0, 2, 4, 8 };
    int scale = 8;
    for (int i = 0; i < 4; i++) {
        int sh = scales[i];
        if ((w >> sh) <= CAM_PIX_MAX_W && (h >> sh) <= CAM_PIX_MAX_H) { scale = sh; break; }
    }
    int out_w = w >> scale, out_h = h >> scale;
    if (out_w < 1) out_w = 1;
    if (out_h < 1) out_h = 1;
    if (out_w > CAM_PIX_MAX_W) out_w = CAM_PIX_MAX_W;
    if (out_h > CAM_PIX_MAX_H) out_h = CAM_PIX_MAX_H;

    if (!s_cam_pixels) s_cam_pixels = (uint16_t *)ps_malloc((size_t)CAM_PIX_MAX_W * CAM_PIX_MAX_H * 2);
    if (!s_cam_pixels) { s_cam_jpeg.close(); return; }
    s_cam_pix_w = out_w;
    s_cam_pix_h = out_h;
    memset(s_cam_pixels, 0, (size_t)CAM_PIX_MAX_W * CAM_PIX_MAX_H * 2);

    if (s_cam_jpeg.decode(0, 0, scale)) {
        s_cam_have_frame = true;
        s_cam_frame_count++;
        s_cam_last_frame_ms = millis();
    }
    s_cam_jpeg.close();
}

static void draw_page_camera(Arduino_GFX *g) {
    if (!bambu_camera_supported()) {
        ui_draw_centered_text(60, COLOR_TEXT_DIM, "CAMERA", 1);
        ui_draw_centered_text(180, COLOR_TEXT_DIM, "Not available for this", 1);
        ui_draw_centered_text(198, COLOR_TEXT_DIM, "printer over this app", 1);
        ui_draw_centered_text(230, COLOR_TEXT_DIM, "(X1/H2-series use RTSP,", 1);
        ui_draw_centered_text(248, COLOR_TEXT_DIM, "not implemented here)", 1);
        return;
    }
    if (!s_cam_have_frame) {
        ui_draw_centered_text(60, COLOR_TEXT_DIM, "CAMERA", 1);
        ui_draw_centered_text(210, COLOR_TEXT_DIM, bambu_camera_is_open() ? "Waiting for stream..." : "Connecting...", 1);
        int dots = (millis() / 350) % 4;
        for (int i = 0; i < 3; i++)
            g->fillCircle(LCD_WIDTH / 2 - 20 + i * 20, 250, 5, i < dots ? COLOR_ACCENT3 : COLOR_TEXT_DIM);
        ui_draw_centered_text(300, COLOR_TEXT_DIM, "Some models only stream", 1);
        ui_draw_centered_text(316, COLOR_TEXT_DIM, "while actively printing", 1);
        return;
    }
    int ix = LCD_WIDTH / 2 - s_cam_pix_w / 2, iy = 120 - s_cam_pix_h / 2 + 100;
    g->draw16bitRGBBitmap(ix, iy, s_cam_pixels, s_cam_pix_w, s_cam_pix_h);

    bool fresh = (millis() - s_cam_last_frame_ms) < 3000;
    int dot_pulse = (millis() / 500) % 2;
    g->fillCircle(150, 62, (fresh && dot_pulse) ? 6 : 5, fresh ? COLOR_BAD : COLOR_TEXT_DIM);
    put_text(g, 162, 56, COLOR_TEXT_DIM, fresh ? "LIVE" : "STALLED", 1);
}

// ---- Files (page 5, FTPS browser) ------------------------------------------

static bool is_printable(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".3mf") == 0;
}

static int files_content_bottom() {
    int rows = s_files_count > 0 ? s_files_count : 1;
    if (s_files_dir[0]) rows++; // ".." row
    return 92 + rows * (56 + 6);
}

static void draw_files_row(Arduino_GFX *g, int rowy, const char *label, bool is_dir, uint32_t size, bool up) {
    int x = (LCD_WIDTH - 320) / 2, w = 320, h = 56;
    g->fillRoundRect(x, rowy, w, h, 14, COLOR_TEXT_DIM);
    g->fillRoundRect(x + 2, rowy + 2, w - 4, h - 4, 12, COLOR_PANEL);
    char shown[40];
    truncate_to(up ? ".. (up one level)" : label, shown, sizeof(shown), 30);
    uint16_t color = up ? COLOR_ACCENT2 : (is_dir ? COLOR_TEXT : (is_printable(label) ? COLOR_TEXT : COLOR_TEXT_DIM));
    put_text(g, x + 14, rowy + 10, color, shown, 1);
    if (!up) {
        if (is_dir) put_text(g, x + 14, rowy + 30, COLOR_TEXT_DIM, "Folder", 1);
        else if (is_printable(label)) put_text(g, x + 14, rowy + 30, COLOR_ACCENT2, "Tap to print", 1);
        else {
            char sz[24];
            snprintf(sz, sizeof(sz), "%lu KB", (unsigned long)(size / 1024));
            put_text(g, x + 14, rowy + 30, COLOR_TEXT_DIM, sz, 1);
        }
    }
}

static void draw_page_files(Arduino_GFX *g) {
    ui_draw_centered_text(60, COLOR_TEXT_DIM, s_files_dir[0] ? s_files_dir : "SD CARD", 1);

    if (s_files_pending_load) {
        ui_draw_centered_text(220, COLOR_TEXT_DIM, "Loading...", 1);
        return;
    }
    if (s_files_count == -1) {
        ui_draw_centered_text(200, COLOR_BAD, "Couldn't load files", 1);
        ui_draw_centered_text(222, COLOR_TEXT_DIM, "Tap to retry", 1);
        return;
    }
    if (s_files_count == 0 && !s_files_dir[0]) {
        ui_draw_centered_text(220, COLOR_TEXT_DIM, "No files found", 1);
        return;
    }

    // Safe Y window for a 320px-wide row on this round display: half-width
    // 160, so a row needs sqrt(233^2 - 160^2) =~ 169px of vertical clearance
    // from centre (233) either way - rows outside [64, 346] get clipped here
    // rather than drawn into the corner, same principle as every other
    // round-safety check in this file, just applied to a scrollable list.
    const int SAFE_TOP = 64, SAFE_BOTTOM = 346;
    int off = -s_files_scroll_y;
    int rowy = 92 + off;
    if (s_files_dir[0]) {
        if (rowy >= SAFE_TOP && rowy + 56 <= SAFE_BOTTOM) draw_files_row(g, rowy, "", false, 0, true);
        rowy += 62;
    }
    for (int i = 0; i < s_files_count; i++) {
        if (rowy >= SAFE_TOP && rowy + 56 <= SAFE_BOTTOM) draw_files_row(g, rowy, s_files[i].name, s_files[i].is_dir, s_files[i].size, false);
        rowy += 62;
    }
}

static void start_files_load() {
    s_files_pending_load = true; // draw the loading message for one frame before the blocking fetch (see bambu_printer_tick)
}

static void touch_files(int x, int y, bool pressed) {
    if (s_files_count == -1) { // failed - any tap retries
        if (!pressed) start_files_load();
        return;
    }
    if (!pressed) {
        if (!s_files_moved) {
            int cy = y + s_files_scroll_y;
            int rowy = 92;
            int rx = (LCD_WIDTH - 320) / 2, rw = 320;
            bool hit_row = (x >= rx && x <= rx + rw);
            if (s_files_dir[0]) {
                if (hit_row && cy >= rowy && cy <= rowy + 56) {
                    vibrate_buzz();
                    char *slash = strrchr(s_files_dir, '/');
                    if (slash) *slash = 0; else s_files_dir[0] = 0;
                    s_files_count = -2;
                    s_files_scroll_y = 0;
                    start_files_load();
                    s_files_scrolling = false;
                    return;
                }
                rowy += 62;
            }
            for (int i = 0; i < s_files_count; i++) {
                if (hit_row && cy >= rowy && cy <= rowy + 56) {
                    vibrate_buzz();
                    if (s_files[i].is_dir) {
                        size_t cur = strlen(s_files_dir);
                        snprintf(s_files_dir + cur, sizeof(s_files_dir) - cur, "%s%s", cur ? "/" : "", s_files[i].name);
                        s_files_count = -2;
                        s_files_scroll_y = 0;
                        start_files_load();
                    } else if (is_printable(s_files[i].name)) {
                        snprintf(s_confirm_print_file, sizeof(s_confirm_print_file), "%s", s_files[i].name);
                    }
                    s_files_scrolling = false;
                    return;
                }
                rowy += 62;
            }
        }
        s_files_scrolling = false;
        return;
    }
    if (!s_files_scrolling) {
        s_files_scrolling = true;
        s_files_drag_start_y = y;
        s_files_scroll_start = s_files_scroll_y;
        s_files_moved = false;
        return;
    }
    int dy = y - s_files_drag_start_y;
    s_files_scroll_y = s_files_scroll_start - dy;
    int max_scroll = files_content_bottom() - LCD_HEIGHT;
    if (max_scroll < 0) max_scroll = 0;
    if (s_files_scroll_y < 0) s_files_scroll_y = 0;
    if (s_files_scroll_y > max_scroll) s_files_scroll_y = max_scroll;
    if (abs(dy) > 8) s_files_moved = true;
}

static void draw_print_confirm(Arduino_GFX *g) {
    int cx = LCD_WIDTH / 2;
    ui_draw_centered_text(180, COLOR_TEXT, "Print this file?", 2);
    char shown[32];
    truncate_to(s_confirm_print_file, shown, sizeof(shown), 26);
    ui_draw_centered_text(214, COLOR_ACCENT2, shown, 1);
    ui_draw_centered_text(246, COLOR_TEXT_DIM, "Multi-plate files print", 1);
    ui_draw_centered_text(262, COLOR_TEXT_DIM, "plate 1 only", 1);
    g->fillRoundRect(cx - 90, 300, 80, 40, 20, COLOR_GOOD);
    ui_draw_centered_text_at(cx - 50, 300 + 12, COLOR_BG, "Print", 1);
    g->fillRoundRect(cx + 10, 300, 80, 40, 20, COLOR_PANEL);
    ui_draw_centered_text_at(cx + 50, 300 + 12, COLOR_TEXT, "Cancel", 1);
}

static void draw_stop_confirm(Arduino_GFX *g) {
    int cx = LCD_WIDTH / 2;
    ui_draw_centered_text(200, COLOR_BAD, "Stop this print?", 2);
    ui_draw_centered_text(232, COLOR_TEXT_DIM, "This can't be undone", 1);
    g->fillRoundRect(cx - 90, 330, 80, 40, 20, COLOR_BAD);
    ui_draw_centered_text_at(cx - 50, 330 + 12, COLOR_TEXT, "Stop", 1);
    g->fillRoundRect(cx + 10, 330, 80, 40, 20, COLOR_PANEL);
    ui_draw_centered_text_at(cx + 50, 330 + 12, COLOR_TEXT, "Cancel", 1);
}

static void draw_status(Arduino_GFX *g) {
    BambuView v = current_view();
    if (v != V_PAGES) { draw_message_view(g, v); return; }
    if (s_confirm_stop) { draw_stop_confirm(g); return; }
    if (s_confirm_print_file[0]) { draw_print_confirm(g); return; }
    const BambuPrintState &st = bambu_get_state();
    switch (s_page) {
        case 0: draw_page_print(g, st); break;
        case 1: draw_page_temps(g, st); break;
        case 2: draw_page_fans(g, st); break;
        case 3: draw_page_filament(g, st); break;
        case 4: draw_page_camera(g); break;
        default: draw_page_files(g); break;
    }
    draw_page_dots(g);
}

static void bambu_printer_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    // No WiFi means nothing below can work (and there's no point typing a
    // code yet), so say so first - whichever step this screen is on.
    if (!wifi_is_connected()) { draw_message_view(g, V_NO_WIFI); return; }
    if (s_screen_state == BS_ENTER_CODE) draw_enter_code(g);
    else draw_status(g);
}

// ---- Touch --------------------------------------------------------------

static void code_insert(char c) {
    if (s_code_len < CODE_MAX_LEN) { s_code_buf[s_code_len++] = c; s_code_buf[s_code_len] = 0; vibrate_buzz(); }
}

static void touch_enter_code(int x, int y) {
    if (!s_code_letters_mode) {
        for (int row = 0; row < 3; row++) {
            int row_y = PAD_ROW0_Y + row * (PAD_ROW1_Y - PAD_ROW0_Y);
            if (y < row_y || y > row_y + PAD_KEY_H) continue;
            for (int col = 0; col < 3; col++) {
                int kx, ky, kw, kh;
                key_rect(col, row_y, &kx, &ky, &kw, &kh);
                if (x >= kx && x <= kx + kw) { code_insert((char)('1' + (row * 3 + col))); return; }
            }
        }
    } else {
        struct { const char *table; int n; int row_y; } rows[3] = {
            { CODE_ROW1, 10, KB_ROW1_Y }, { CODE_ROW2, 9, KB_ROW2_Y }, { CODE_ROW3, 7, KB_ROW3_Y },
        };
        for (auto &r : rows) {
            if (y < r.row_y || y > r.row_y + KB_KEY_H) continue;
            for (int i = 0; i < r.n; i++) {
                int kx, ky, kw, kh;
                kb_key_rect(i, r.n, r.row_y, &kx, &ky, &kw, &kh);
                if (x >= kx && x <= kx + kw) { code_insert(r.table[i]); return; }
            }
        }
    }

    if (y >= PAD_ROW3_Y && y <= PAD_ROW3_Y + PAD_KEY_H) {
        int kx, ky, kw, kh;
        key_rect(0, PAD_ROW3_Y, &kx, &ky, &kw, &kh);
        if (x >= kx && x <= kx + kw) {
            if (s_code_len > 0) s_code_buf[--s_code_len] = 0;
            vibrate_buzz();
            return;
        }
        key_rect(1, PAD_ROW3_Y, &kx, &ky, &kw, &kh);
        if (x >= kx && x <= kx + kw) { code_insert('0'); return; }
        key_rect(2, PAD_ROW3_Y, &kx, &ky, &kw, &kh);
        if (x >= kx && x <= kx + kw && code_ready()) {
            bambu_set_access_code(s_code_buf);
            bambu_enable();
            s_screen_state = BS_STATUS;
            return;
        }
    }

    int pw = 130, ph = 30, px = LCD_WIDTH / 2 - pw / 2, py = 412;
    if (x >= px && x <= px + pw && y >= py && y <= py + ph) {
        s_code_letters_mode = !s_code_letters_mode;
        vibrate_buzz();
    }
}

// A tap on a message view's action pill.
static void status_tap(int x, int y) {
    BambuView v = current_view();
    if (v == V_PAGES) {
        if (s_confirm_print_file[0]) {
            int cx = LCD_WIDTH / 2;
            if (hit_rect(x, y, cx - 50, 300, 80, 40)) {
                vibrate_buzz();
                bambu_print_file(s_confirm_print_file);
                s_confirm_print_file[0] = 0;
            } else if (hit_rect(x, y, cx + 50, 300, 80, 40)) {
                s_confirm_print_file[0] = 0;
            }
            return;
        }
        if (s_confirm_stop) {
            if (hit_rect(x, y, LCD_WIDTH / 2 - 90, 330, 80, 40)) { bambu_stop(); s_confirm_stop = false; }
            else if (hit_rect(x, y, LCD_WIDTH / 2 + 10, 330, 80, 40)) { s_confirm_stop = false; }
            return;
        }
        if (s_page == 0) {
            const BambuPrintState &st = bambu_get_state();
            bool active = !strcmp(st.gcode_state, "RUNNING") || !strcmp(st.gcode_state, "PAUSE");
            if (active) {
                int bw = 130, bh = 32, gap = 12, y0 = 358, cx = LCD_WIDTH / 2;
                int x0 = cx - bw - gap / 2;
                if (hit_rect(x, y, x0 + bw / 2, y0, bw, bh)) {
                    vibrate_buzz();
                    if (!strcmp(st.gcode_state, "PAUSE")) bambu_resume(); else bambu_pause();
                    return;
                }
                if (hit_rect(x, y, x0 + bw + gap + bw / 2, y0, bw, bh)) { vibrate_buzz(); s_confirm_stop = true; return; }
            }
        } else if (s_page == 2) {
            const BambuPrintState &st = bambu_get_state();
            int cy = fans_page_content_y(st);
            int bw = 78, gap = 6, total = 4 * bw + 3 * gap;
            int x0 = LCD_WIDTH / 2 - total / 2, by = cy + 16, bh = 32;
            int ly = by + bh + 26, lw = 140, lh = 32;
            bool hit_speed = (y >= by && y <= by + bh && x >= x0 && x <= x0 + total);
            bool hit_light = hit_rect(x, y, LCD_WIDTH / 2, ly, lw, lh);
            if (hit_speed || hit_light) { touch_page_fans(x, y, st); return; }
        }
        // tap anywhere else flips to the next page
        s_page = (s_page + 1) % NUM_PAGES;
        return;
    }
    if (!hit_rect(x, y, MSG_PILL_CX, MSG_PILL_Y, MSG_PILL_W, MSG_PILL_H)) return;
    if (v == V_NO_WIFI) {
        // WiFi Setup takes over this press and swallows its release, so
        // this screen would be left thinking the finger is still down and
        // would drop the first tap after we return - reset that now.
        s_prev_pressed = false;
        ui_push(&wifi_setup_screen);
    } else if (v == V_CONNECTING || v == V_WRONG_CODE) {
        s_code_buf[0] = 0;
        s_code_len = 0;
        s_screen_state = BS_ENTER_CODE;
    }
}

static void bambu_printer_touch(int x, int y, bool pressed) {
    if (s_screen_state == BS_ENTER_CODE) {
        bool edge = pressed && !s_prev_pressed;
        s_prev_pressed = pressed;
        if (!wifi_is_connected()) return;
        if (!edge) return;
        if (x < 54 && y < 54) { ui_pop_screen(); return; }
        touch_enter_code(x, y);
        return;
    }

    // The files browser needs the whole press (drag-to-scroll), not just
    // tap-on-release like every other page here - same reason
    // app_wifi_setup.cpp's network list handles touch differently from
    // its keyboard. Confirm dialogs still take priority over it.
    if (s_page == 5 && current_view() == V_PAGES && !s_confirm_print_file[0] && !s_confirm_stop) {
        touch_files(x, y, pressed);
        s_prev_pressed = pressed;
        return;
    }

    bool edge = pressed && !s_prev_pressed;
    bool release = !pressed && s_prev_pressed;
    s_prev_pressed = pressed;
    if (pressed) {
        if (edge) { s_down_x = x; s_down_y = y; }
        s_last_x = x;
        s_last_y = y;
    }

    if (edge && x < 54 && y < 54) { ui_pop_screen(); return; }
    if (!release) return;

    int dx = s_last_x - s_down_x, dy = s_last_y - s_down_y;
    if (abs(dx) > 50 && abs(dx) > abs(dy) && current_view() == V_PAGES && !s_confirm_print_file[0] && !s_confirm_stop) {
        s_page = (s_page + (dx < 0 ? 1 : NUM_PAGES - 1)) % NUM_PAGES; // swipe left = next
        return;
    }
    if (abs(dx) < 20 && abs(dy) < 20) status_tap(s_down_x, s_down_y);
}

static void bambu_printer_tick() {
    update_tilt();

    if (s_screen_state != BS_STATUS || current_view() != V_PAGES) return;

    if (s_page != s_prev_page) {
        if (s_prev_page == 4 && s_page != 4) bambu_camera_close();
        if (s_page == 4 && s_prev_page != 4 && bambu_camera_supported()) {
            s_cam_have_frame = false;
            bambu_camera_open();
        }
        s_prev_page = s_page;
    }

    if (s_page == 4 && bambu_camera_is_open()) {
        if (!s_cam_buf) s_cam_buf = (uint8_t *)ps_malloc(CAM_BUF_SIZE);
        if (s_cam_buf) {
            size_t len = 0;
            if (bambu_camera_poll_frame(s_cam_buf, CAM_BUF_SIZE, &len)) decode_camera_frame(s_cam_buf, len);
        }
    }

    if (s_page == 5 && s_files_pending_load) {
        s_files_pending_load = false;
        int n = bambu_ftp_list(s_files_dir, s_files, FILES_MAX);
        s_files_count = n; // -1 on failure, propagated straight through
    }
}

static void bambu_printer_create() {
    s_code_buf[0] = 0;
    s_code_len = 0;
    s_code_letters_mode = false;
    s_page = 0;
    s_prev_page = -1;
    s_prev_pressed = false;
    s_confirm_print_file[0] = 0;
    s_confirm_stop = false;
    s_files_count = -2;
    s_files_dir[0] = 0;
    s_files_scroll_y = 0;
    s_files_scrolling = false;
    s_cam_have_frame = false;
    if (bambu_has_access_code()) {
        s_screen_state = BS_STATUS;
        if (!bambu_is_enabled()) bambu_enable();
    } else {
        s_screen_state = BS_ENTER_CODE;
    }
}

static void bambu_printer_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

static void bambu_printer_destroy() {
    // Leaving the app entirely - release the camera TLS connection rather
    // than leaving it open in the background using resources for a view
    // nobody's looking at.
    if (bambu_camera_is_open()) bambu_camera_close();
}

Screen bambu_printer_screen = {
    "3D Printer", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    bambu_printer_create, bambu_printer_draw, bambu_printer_touch, bambu_printer_tick, bambu_printer_destroy, bambu_printer_gesture,
    200, // idle_frame_ms - live values change at most about once a second, no need to redraw the ring at 30 fps while nobody's touching it
};
