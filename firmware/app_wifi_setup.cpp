/*
 * app_wifi_setup.cpp
 * See app_wifi_setup.h. Four steps: WS_LIST (scanned networks, tap one
 * or "Enter Manually"), WS_KEYBOARD (SSID/password entry on the shared
 * T9 keypad, ui_keypad.h - password fields get a Show/Hide key),
 * WS_CONNECTING (polls wifi_is_connected() with a timeout), WS_RESULT
 * (success/fail).
 *
 * The network list uses the shared kinetic scroller (UiScroll in ui.h,
 * same as Settings): finger-tracking drag, fling with momentum, snap to
 * rows, tap-on-release. The keypad gets raw touch events (holds and
 * backspace repeat need them); the remaining steps are one-shot taps,
 * rising-edge gated as elsewhere in this project.
 */

#include "app_wifi_setup.h"
#include "config.h"
#include "board_pins.h"
#include "hal_wifi.h"
#include "hal_ntp.h"
#include "hal_nvs.h"
#include "hal_vibrate.h"
#include "app_settings_state.h"
#include "ui.h"
#include "ui_font.h"
#include "ui_keypad.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#if FEATURE_WIFI
#include <WiFi.h>

// ---- Steps ------------------------------------------------------------
enum WSStep { WS_LIST, WS_KEYBOARD, WS_CONNECTING, WS_RESULT };
enum KbTarget { KB_SSID, KB_PASSWORD };

static WSStep s_step;
static KbTarget s_kb_target;

// ---- Network list -------------------------------------------------------
struct WifiNet {
    char ssid[33];
    int32_t rssi;
    bool open;
};
#define MAX_NETS 20
static WifiNet s_nets[MAX_NETS];
static int s_net_count;
static bool s_scan_started;
static bool s_scan_done;

static const int NET_ROW_TOP = 80;   // identical to Settings' own row list - already
static const int NET_ROW_W = 340;    // verified safe for the round display at this
static const int NET_ROW_H = 78;     // geometry, see app_settings.cpp's matching note
static const int NET_ROW_GAP = 8;
static const int NET_ROW_PITCH = NET_ROW_H + NET_ROW_GAP;
// Bottom padding lets the last row scroll up clear of the round
// screen's bottom curve (MUSE pads its lists the same way).
static const int NET_LIST_BOTTOM_PAD = 110;
static int list_max_scroll() {
    int content_bottom = NET_ROW_TOP + (s_net_count + 1) * NET_ROW_PITCH - NET_ROW_GAP + NET_LIST_BOTTOM_PAD;
    int m = content_bottom - LCD_HEIGHT;
    return m > 0 ? m : 0;
}

static UiScroll s_list_scroll;

// ---- Keyboard -------------------------------------------------------------
// The shared T9 keypad (ui_keypad.h) edits s_ssid_buf / s_pass_buf directly.
static char s_ssid_buf[33];
static int s_ssid_len;
static char s_pass_buf[64];
static int s_pass_len;
static bool s_prev_pressed;

// ---- Connect / result -----------------------------------------------------
static bool s_wifi_was_on_at_entry;
static bool s_connect_success;
static uint32_t s_connect_start_ms;
#define CONNECT_TIMEOUT_MS 15000

// =========================================================================
//  Shared small helpers
// =========================================================================

// Thick line via two fillTriangle calls (a quad split down its length) -
// consistent with this project's established no-outline-primitives
// convention, used here to draw a simple checkmark/X badge.
static void draw_thick_line(Arduino_GFX *g, float x0, float y0, float x1, float y1, float thickness, uint16_t color) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * thickness / 2.0f;
    float ny = dx / len * thickness / 2.0f;
    g->fillTriangle((int)(x0 + nx), (int)(y0 + ny), (int)(x1 + nx), (int)(y1 + ny), (int)(x1 - nx), (int)(y1 - ny), color);
    g->fillTriangle((int)(x0 + nx), (int)(y0 + ny), (int)(x1 - nx), (int)(y1 - ny), (int)(x0 - nx), (int)(y0 - ny), color);
}

static const char *truncate_ssid(const char *ssid, char *buf, size_t buf_len) {
    size_t len = strlen(ssid);
    const size_t MAX_SHOWN = 18;
    if (len <= MAX_SHOWN) { snprintf(buf, buf_len, "%s", ssid); return buf; }
    snprintf(buf, buf_len, "%.*s...", (int)(MAX_SHOWN - 3), ssid);
    return buf;
}

// =========================================================================
//  Scanning
// =========================================================================

static void start_scan() {
    wifi_enable(); // through the hal, so radio-coordination + light-sleep policy stay correct
    WiFi.scanNetworks(true); // async
    s_scan_started = true;
    s_scan_done = false;
    s_net_count = 0;
}

static void poll_scan() {
    if (!s_scan_started || s_scan_done) return;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    if (n < 0) { // WIFI_SCAN_FAILED or similar
        DEBUG_PRINTF("[wifi] scan failed (%d)%s%s\n", n, wifi_last_error() ? ": " : "",
                     wifi_last_error() ? wifi_last_error() : "");
        s_scan_done = true;
        s_net_count = 0;
        return;
    }
    s_net_count = 0;
    for (int i = 0; i < n && s_net_count < MAX_NETS; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        int32_t rssi = WiFi.RSSI(i);
        bool open = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        int existing = -1;
        for (int j = 0; j < s_net_count; j++) {
            if (strcmp(s_nets[j].ssid, ssid.c_str()) == 0) { existing = j; break; }
        }
        if (existing >= 0) {
            if (rssi > s_nets[existing].rssi) { s_nets[existing].rssi = rssi; s_nets[existing].open = open; }
        } else {
            snprintf(s_nets[s_net_count].ssid, sizeof(s_nets[s_net_count].ssid), "%s", ssid.c_str());
            s_nets[s_net_count].rssi = rssi;
            s_nets[s_net_count].open = open;
            s_net_count++;
        }
    }
    // Simple insertion sort by signal strength - small N, no need for anything fancier.
    for (int i = 1; i < s_net_count; i++) {
        WifiNet key = s_nets[i];
        int j = i - 1;
        while (j >= 0 && s_nets[j].rssi < key.rssi) { s_nets[j + 1] = s_nets[j]; j--; }
        s_nets[j + 1] = key;
    }
    WiFi.scanDelete();
    s_scan_done = true;
}

// =========================================================================
//  WS_LIST
// =========================================================================

static void draw_signal_bars(Arduino_GFX *g, int bx, int by_bottom, int32_t rssi, uint16_t on_color, uint16_t off_color) {
    int strength = 1;
    if (rssi >= -55) strength = 4;
    else if (rssi >= -65) strength = 3;
    else if (rssi >= -75) strength = 2;
    for (int i = 0; i < 4; i++) {
        int bar_h = 8 + i * 6;
        int bar_x = bx + i * 10;
        int bar_y = by_bottom - bar_h;
        g->fillRect(bar_x, bar_y, 7, bar_h, i < strength ? on_color : off_color);
    }
}

static void draw_lock_icon(Arduino_GFX *g, int lx, int ly_top, uint16_t color, uint16_t bg_color) {
    // Body + a shackle faked as a filled circle with the row's own
    // background painted back over its lower half - same cutout-via-
    // overpaint technique used for Crystal Cavern's Moon Crystal this
    // session (see game_cavern.cpp).
    g->fillCircle(lx + 7, ly_top, 5, color);
    g->fillRect(lx, ly_top, 14, 5, bg_color);
    g->fillRoundRect(lx, ly_top + 3, 14, 11, 2, color);
}

static void draw_list_row(Arduino_GFX *g, int y, int index) {
    int x = (LCD_WIDTH - NET_ROW_W) / 2;
    g->fillRoundRect(x, y, NET_ROW_W, NET_ROW_H, 18, COLOR_TEXT_DIM);
    g->fillRoundRect(x + 2, y + 2, NET_ROW_W - 4, NET_ROW_H - 4, 16, COLOR_PANEL);

    if (index == s_net_count) {
        // "Enter Manually" row
        g->setTextSize(2);
        g->setTextColor(COLOR_ACCENT2);
        ui_print(x + 18, y + 30, 2, COLOR_ACCENT2, "+ Enter Manually");
        return;
    }

    WifiNet &n = s_nets[index];
    char shown[24];
    truncate_ssid(n.ssid, shown, sizeof(shown));
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    ui_print(x + 18, y + 14, 2, COLOR_TEXT, shown);

    int bars_x = x + NET_ROW_W - 18 - 40;
    draw_signal_bars(g, bars_x, y + 60, n.rssi, COLOR_ACCENT2, COLOR_TEXT_DIM);
    if (!n.open) {
        draw_lock_icon(g, bars_x - 24, y + 32, COLOR_TEXT_DIM, COLOR_PANEL);
    }
}

static void draw_list(Arduino_GFX *g) {
    ui_draw_centered_text(30, COLOR_TEXT_DIM, "Select a Network", 1);

    if (!s_scan_done) {
        ui_draw_centered_text(220, COLOR_TEXT_DIM, "Scanning...", 2);
        return;
    }
    if (s_net_count == 0) {
        const char *err = wifi_last_error();
        ui_draw_centered_text(180, err ? COLOR_BAD : COLOR_TEXT_DIM, err ? err : "No networks found", 1);
    }

    // Rows are drawn in full while scrolling too - the old "simplified"
    // placeholder rows existed because every frame used to block on a
    // full-screen flush; the background panel sender removed that cost.
    int off = -ui_scroll_offset(&s_list_scroll);
    int total_rows = s_net_count + 1; // +1 for "Enter Manually"
    for (int i = 0; i < total_rows; i++) {
        int y = NET_ROW_TOP + i * NET_ROW_PITCH + off;
        if (y + NET_ROW_H < 0 || y > LCD_HEIGHT) continue;
        draw_list_row(g, y, i);
    }
}

static void enter_keyboard(KbTarget target) {
    s_kb_target = target;
    if (target == KB_SSID) { s_ssid_buf[0] = 0; s_ssid_len = 0; }
    s_pass_buf[0] = 0;
    s_pass_len = 0;
    if (target == KB_SSID) keypad_begin(s_ssid_buf, sizeof(s_ssid_buf), false, "Next");
    else keypad_begin(s_pass_buf, sizeof(s_pass_buf), true, "Join");
    s_step = WS_KEYBOARD;
}

static void start_connect() {
    wifi_set_credentials(s_ssid_buf, s_pass_buf);
    // wifi_enable() no-ops if the radio's already on (e.g. left on from
    // scanning, or already connected to a different network) - force a
    // fresh attempt with the new credentials rather than silently
    // keeping whatever connection was already up.
    if (wifi_is_enabled()) wifi_disable();
    wifi_enable();
    s_connect_start_ms = millis();
    s_step = WS_CONNECTING;
}

static void touch_list(int x, int y, bool pressed) {
    if (!s_scan_done) return;
    if (!ui_scroll_touch(&s_list_scroll, y, pressed)) return;
    // A tap (released without scrolling, list not coasting): which row?
    int cy = y + ui_scroll_offset(&s_list_scroll);
    int row_x = (LCD_WIDTH - NET_ROW_W) / 2;
    int total_rows = s_net_count + 1;
    for (int i = 0; i < total_rows; i++) {
        int ry = NET_ROW_TOP + i * NET_ROW_PITCH;
        if (cy >= ry && cy <= ry + NET_ROW_H && x >= row_x && x <= row_x + NET_ROW_W) {
            vibrate_buzz();
            if (i == s_net_count) {
                enter_keyboard(KB_SSID);
            } else if (s_nets[i].open) {
                snprintf(s_ssid_buf, sizeof(s_ssid_buf), "%s", s_nets[i].ssid);
                s_ssid_len = strlen(s_ssid_buf);
                s_pass_buf[0] = 0; s_pass_len = 0;
                start_connect();
            } else {
                snprintf(s_ssid_buf, sizeof(s_ssid_buf), "%s", s_nets[i].ssid);
                s_ssid_len = strlen(s_ssid_buf);
                enter_keyboard(KB_PASSWORD);
            }
            break;
        }
    }
}

// =========================================================================
//  WS_KEYBOARD (shared by SSID entry and password entry)
// =========================================================================

static void draw_keyboard(Arduino_GFX *g) {
    if (s_kb_target == KB_SSID) {
        ui_draw_centered_text(44, COLOR_TEXT_DIM, "Enter Network Name", 1);
    } else {
        char label[40];
        char shown[20];
        truncate_ssid(s_ssid_buf, shown, sizeof(shown));
        snprintf(label, sizeof(label), "Password for %s", shown);
        ui_draw_centered_text(44, COLOR_TEXT_DIM, label, 1);
    }
    keypad_draw_field(g, 84, s_kb_target == KB_SSID ? "network name" : "password");
    keypad_draw(g);
}

// Raw (not edge-gated) events: the keypad needs holds and repeats.
static void touch_keyboard(int x, int y, bool pressed) {
    KeypadEvent ev = keypad_touch(x, y, pressed);
    s_ssid_len = (int)strlen(s_ssid_buf);
    s_pass_len = (int)strlen(s_pass_buf);
    if (ev != KP_DONE) return;
    if (s_kb_target == KB_SSID) {
        if (s_ssid_len == 0) return; // nothing to join yet
        enter_keyboard(KB_PASSWORD);
    } else {
        start_connect();
    }
}

// =========================================================================
//  WS_CONNECTING
// =========================================================================

static void draw_connecting(Arduino_GFX *g) {
    char shown[20];
    truncate_ssid(s_ssid_buf, shown, sizeof(shown));
    ui_draw_centered_text(180, COLOR_TEXT_DIM, "Connecting to", 1);
    ui_draw_centered_text(210, COLOR_TEXT, shown, 2);

    int dots = ((millis() / 300) % 4);
    int cx0 = LCD_WIDTH / 2 - 24;
    for (int i = 0; i < 3; i++) {
        uint16_t c = (i < dots) ? COLOR_ACCENT2 : COLOR_TEXT_DIM;
        g->fillCircle(cx0 + i * 24, 260, 6, c);
    }

    int bw = 140, bh = 40;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 320;
    g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, COLOR_PANEL);
    ui_draw_centered_text_at(LCD_WIDTH / 2, by + bh / 2 - 8, COLOR_TEXT, "Cancel", 1);
}

static void touch_connecting(int x, int y) {
    int bw = 140, bh = 40;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 320;
    if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
        wifi_disable();
        vibrate_buzz();
        s_step = WS_LIST;
        start_scan();
    }
}

// =========================================================================
//  WS_RESULT
// =========================================================================

static void draw_result(Arduino_GFX *g) {
    int cx = LCD_WIDTH / 2, cy = 180;
    char shown[20];
    truncate_ssid(s_ssid_buf, shown, sizeof(shown));
    if (s_connect_success) {
        g->fillCircle(cx, cy, 50, COLOR_GOOD);
        draw_thick_line(g, cx - 20, cy, cx - 6, cy + 16, 8, COLOR_TEXT);
        draw_thick_line(g, cx - 6, cy + 16, cx + 22, cy - 18, 8, COLOR_TEXT);
        ui_draw_centered_text(250, COLOR_TEXT, "Connected!", 2);
        ui_draw_centered_text(280, COLOR_TEXT_DIM, shown, 1);
    } else {
        g->fillCircle(cx, cy, 50, COLOR_BAD);
        draw_thick_line(g, cx - 18, cy - 18, cx + 18, cy + 18, 8, COLOR_TEXT);
        draw_thick_line(g, cx - 18, cy + 18, cx + 18, cy - 18, 8, COLOR_TEXT);
        ui_draw_centered_text(250, COLOR_TEXT, "Couldn't Connect", 2);
        ui_draw_centered_text(280, COLOR_TEXT_DIM, "Check the password and try again", 1);
    }

    int bw = 180, bh = 44;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 326;
    g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, s_connect_success ? COLOR_GOOD : COLOR_ACCENT2);
    ui_draw_centered_text_at(LCD_WIDTH / 2, by + bh / 2 - 8, COLOR_TEXT, s_connect_success ? "Done" : "Try Again", 1);
}

static void touch_result(int x, int y) {
    int bw = 180, bh = 44;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 326;
    if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
        vibrate_buzz();
        if (s_connect_success) {
            ui_pop_screen();
        } else {
            s_step = WS_LIST;
            start_scan();
        }
    }
}

// =========================================================================
//  Screen glue
// =========================================================================

static void wifi_setup_create() {
    s_step = WS_LIST;
    s_wifi_was_on_at_entry = g_app_settings.wifi_on;
    s_connect_success = false;
    ui_scroll_reset(&s_list_scroll, 0, NET_ROW_PITCH);
    s_prev_pressed = false;
    s_ssid_buf[0] = 0; s_ssid_len = 0;
    s_pass_buf[0] = 0; s_pass_len = 0;
    start_scan();
}

static void wifi_setup_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    switch (s_step) {
        case WS_LIST: draw_list(g); break;
        case WS_KEYBOARD: draw_keyboard(g); break;
        case WS_CONNECTING: draw_connecting(g); break;
        case WS_RESULT: draw_result(g); break;
    }
}

static void wifi_setup_touch(int x, int y, bool pressed) {
    if (s_step == WS_LIST) {
        touch_list(x, y, pressed);
        return;
    }
    if (s_step == WS_KEYBOARD) {
        touch_keyboard(x, y, pressed);
        s_prev_pressed = pressed;
        return;
    }
    // Every other step is one-shot taps, not drag - rising-edge gated.
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;
    switch (s_step) {
        case WS_CONNECTING: touch_connecting(x, y); break;
        case WS_RESULT: touch_result(x, y); break;
        default: break;
    }
}

static void wifi_setup_tick() {
    if (s_step == WS_LIST) {
        poll_scan();
        ui_scroll_set_max(&s_list_scroll, list_max_scroll());
        ui_scroll_tick(&s_list_scroll);
        return;
    }
    if (s_step == WS_KEYBOARD) {
        keypad_tick();
        return;
    }
    if (s_step != WS_CONNECTING) return;
    if (wifi_is_connected()) {
        s_connect_success = true;
        g_app_settings.wifi_on = true;
        nvs_save_settings(g_app_settings);
        ntp_sync(); // same as every other WiFi entry point - without this the clock stays unsynced until next boot
        s_step = WS_RESULT;
        return;
    }
    wl_status_t st = WiFi.status();
    bool timed_out = (millis() - s_connect_start_ms) > CONNECT_TIMEOUT_MS;
    bool definite_fail = (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL);
    if (timed_out || definite_fail) {
        s_connect_success = false;
        s_step = WS_RESULT;
    }
}

static void wifi_setup_destroy() {
    // If we never landed a successful connection this visit and WiFi
    // wasn't already on when we entered, restore the pre-entry state
    // instead of leaving the radio on from scanning/an aborted attempt.
    if (!s_connect_success && !s_wifi_was_on_at_entry) {
        wifi_disable();
    }
}

static void wifi_setup_gesture(Gesture g) {
    if (g != GESTURE_SWIPE_LEFT && g != GESTURE_SWIPE_RIGHT) return;
    if (s_step == WS_KEYBOARD) { s_step = WS_LIST; }
    else if (s_step == WS_RESULT) { s_step = WS_LIST; start_scan(); }
    else if (s_step == WS_LIST) { ui_pop_screen(); }
    // WS_CONNECTING: ignore - let it finish or time out, Cancel handles early exit.
}

Screen wifi_setup_screen = {
    "WiFi Setup", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    wifi_setup_create, wifi_setup_draw, wifi_setup_touch, wifi_setup_tick, wifi_setup_destroy, wifi_setup_gesture,
    0,     // idle_frame_ms - no idle throttling, this screen is short-lived
    true,  // suppress_idle - typing a password shouldn't risk a mid-entry auto-lock
    false, // needs_tilt_calibration
    false, // no_transition
    true,  // no_drag_back - swipe right steps back *inside* this screen (keyboard -> list)
};

#else // !FEATURE_WIFI

static void wifi_setup_draw_stub() {
    ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "WiFi not available", 1);
}
static void wifi_setup_gesture_stub(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}
Screen wifi_setup_screen = {
    "WiFi Setup", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, wifi_setup_draw_stub, nullptr, nullptr, nullptr, wifi_setup_gesture_stub
};

#endif
