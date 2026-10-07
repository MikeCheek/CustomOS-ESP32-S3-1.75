/*
 * app_onboarding.cpp
 * First-boot-only welcome and setup flow: an animated title reveal
 * followed by a short wizard (WiFi, optional phone pairing, name,
 * brightness, a couple of core preferences) that ends by marking g_app_settings.onboarding_complete
 * so this never shows again on a device that's already been set up.
 * This is an original design in the general spirit of "clean,
 * minimal, animated" first-run experiences - fade/reveal timing and
 * an expanding ring flourish, not a reproduction of any specific
 * company's actual onboarding screens, copy, or trademarked visuals.
 *
 * Drawing note: only fillRect/fillRoundRect/fillCircle/fillTriangle
 * are used - no drawRoundRect (documented elsewhere in this project
 * as unreliable on this display's CO5300 driver), no drawCircle
 * outline, no drawBitmap. The welcome screen's ring flourish is the
 * same "many small fillCircle dots at a fixed radius" technique
 * already used safely in game_cavern.cpp/game_blackjack.cpp. Every
 * row/button/key position below was checked against this display's
 * actual safe-area radius before being chosen.
 *
 * Keyboard note: the name step uses the shared T9 keypad (ui_keypad.h),
 * whose draw and hit-testing share one key_rect() helper, so
 * the two can't silently drift out of sync the way separately-typed
 * coordinates have elsewhere in this project's history.
 *
 * Touch handling: every tap is rising-edge gated (tap_edge), not raw
 * `pressed` - see this session's project-wide touch-handling fixes
 * for why.
 */
#include "app_onboarding.h"
#include "config.h"
#include "board_pins.h"
#include "hal_nvs.h"
#include "hal_display.h"
#include "hal_vibrate.h"
#include "hal_wifi.h"
#include "hal_ble.h"
#include "app_wifi_setup.h"
#include "app_settings_state.h"
#include "ui.h"
#include "ui_font.h"
#include "ui_keypad.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <math.h>

// OB_WIFI..OB_PREFS must stay contiguous - draw_step_dots() derives the
// progress indicator from their enum distance.
enum OBStep { OB_WELCOME, OB_WIFI, OB_PHONE, OB_NAME, OB_BRIGHTNESS, OB_PREFS, OB_DONE };
static OBStep s_step;
static uint32_t s_step_enter_ms;
static bool s_prev_pressed;
static bool s_ble_was_on_at_entry; // so Skip can put Bluetooth back how it was

static char s_name_buf[24];
static int s_name_len;

static bool s_brightness_dragging;

// ---- Shared layout helpers -----------------------------------------------

static uint16_t blend_to_black(uint16_t c, float t) {
    // t=0 -> full color, t=1 -> black. Same channel-wise RGB565
    // interpolation used throughout this project's fade effects.
    if (t <= 0.0f) return c;
    if (t > 1.0f) t = 1.0f;
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (int)(r * (1.0f - t));
    g = (int)(g * (1.0f - t));
    b = (int)(b * (1.0f - t));
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_ring(Arduino_GFX *g, int cx, int cy, int radius, int dot_count, uint16_t color, float fraction) {
    // fraction 0..1 - how much of the full circle to draw, for the
    // welcome screen's expanding-ring reveal.
    int lit = (int)(dot_count * fraction);
    for (int i = 0; i < lit; i++) {
        float ang = (360.0f * i / dot_count) * 0.0174533f;
        int dx = cx + (int)(cosf(ang) * radius);
        int dy = cy + (int)(sinf(ang) * radius);
        g->fillCircle(dx, dy, 4, color);
    }
}

static void enter_step(OBStep step) {
    s_step = step;
    s_step_enter_ms = millis();
    if (step == OB_PHONE) s_ble_was_on_at_entry = ble_is_enabled();
    if (step == OB_NAME) {
        keypad_begin(s_name_buf, sizeof(s_name_buf), false, "Next", true);
        keypad_ignore_current_touch(); // we got here from a press - don't let it type
    }
}

// ---- Drawing --------------------------------------------------------------

static void draw_welcome(Arduino_GFX *g) {
    uint32_t elapsed = millis() - s_step_enter_ms;
    float reveal = elapsed / 900.0f;
    if (reveal > 1.0f) reveal = 1.0f;

    uint16_t title_color = blend_to_black(COLOR_TEXT, 1.0f - reveal);
    ui_draw_centered_text(200, title_color, "Welcome", 3);

    draw_ring(g, LCD_WIDTH / 2, LCD_HEIGHT / 2, 130, 28, COLOR_ACCENT3, reveal);

    if (elapsed > 1400) {
        uint32_t sub_elapsed = elapsed - 1400;
        float sub_reveal = sub_elapsed / 500.0f;
        if (sub_reveal > 1.0f) sub_reveal = 1.0f;
        uint16_t sub_color = blend_to_black(COLOR_TEXT_DIM, 1.0f - sub_reveal);
        ui_draw_centered_text(280, sub_color, "Let's get set up", 1);
        if (sub_reveal >= 1.0f) {
            int bw = 160, bh = 44;
            int bx = LCD_WIDTH / 2 - bw / 2, by = 340;
            g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
            g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, COLOR_GOOD);
            ui_draw_centered_text_at(LCD_WIDTH / 2, by + bh / 2 - 8, COLOR_TEXT, "Get Started", 1);
        }
    }
}

static void draw_name(Arduino_GFX *g) {
    ui_draw_centered_text(44, COLOR_TEXT_DIM, "What should we call you?", 1);
    keypad_draw_field(g, 84, "(optional)");
    keypad_draw(g);
}

static void draw_brightness(Arduino_GFX *g) {
    ui_draw_centered_text(70, COLOR_TEXT_DIM, "Set your brightness", 2);

    int slider_left = 80, slider_right = LCD_WIDTH - 80;
    int slider_y = LCD_HEIGHT / 2 - 5, slider_h = 10;
    g->fillRoundRect(slider_left, slider_y, slider_right - slider_left, slider_h, 5, COLOR_PANEL);
    int fill = (int)g_app_settings.brightness * (slider_right - slider_left) / 255;
    g->fillRoundRect(slider_left, slider_y, fill, slider_h, 5, COLOR_ACCENT);
    g->fillCircle(slider_left + fill, slider_y + slider_h / 2, 14, COLOR_ACCENT);
    g->fillCircle(slider_left + fill, slider_y + slider_h / 2, 9, COLOR_BG);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)g_app_settings.brightness * 100 / 255);
    ui_draw_centered_text(slider_y + 40, COLOR_TEXT_DIM, buf, 2);

    int bw = 140, bh = 44;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 330;
    g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, COLOR_GOOD);
    ui_draw_centered_text_at(LCD_WIDTH / 2, by + bh / 2 - 8, COLOR_TEXT, "Next", 2);
}

static void draw_toggle_row(Arduino_GFX *g, int y, const char *label, bool on) {
    int w = 320, h = 56;
    int x = LCD_WIDTH / 2 - w / 2;
    g->fillRoundRect(x, y, w, h, 14, COLOR_TEXT_DIM);
    g->fillRoundRect(x + 2, y + 2, w - 4, h - 4, 12, COLOR_PANEL);
    g->setTextSize(1);
    g->setTextColor(COLOR_TEXT);
    ui_print(x + 16, y + h / 2 - 4, 1, COLOR_TEXT, label);

    int sw = 50, sh = 26;
    int sx = x + w - 16 - sw, sy = y + h / 2 - sh / 2;
    uint16_t track_c = on ? COLOR_GOOD : COLOR_PANEL;
    uint16_t border_c = on ? COLOR_GOOD : COLOR_TEXT_DIM;
    g->fillRoundRect(sx, sy, sw, sh, sh / 2, border_c);
    g->fillRoundRect(sx + 2, sy + 2, sw - 4, sh - 4, (sh - 4) / 2, track_c);
    int thumb_r = 10;
    int thumb_x = on ? sx + sw - thumb_r - 3 : sx + thumb_r + 3;
    g->fillCircle(thumb_x, sy + sh / 2, thumb_r, COLOR_TEXT);
}

static void draw_prefs(Arduino_GFX *g) {
    ui_draw_centered_text(50, COLOR_TEXT_DIM, "A couple of preferences", 1);
    draw_toggle_row(g, 90, "Haptic on touch", g_app_settings.haptic_on_touch);
    draw_toggle_row(g, 156, "Wake on touch", g_app_settings.wake_on_touch);
    draw_toggle_row(g, 222, "Wake on wrist motion", g_app_settings.wake_on_motion);

    int bw = 140, bh = 44;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 300;
    g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, COLOR_GOOD);
    ui_draw_centered_text_at(LCD_WIDTH / 2, by + bh / 2 - 8, COLOR_TEXT, "Next", 2);
}

static void draw_done(Arduino_GFX *g) {
    uint32_t elapsed = millis() - s_step_enter_ms;
    float reveal = elapsed / 500.0f;
    if (reveal > 1.0f) reveal = 1.0f;
    uint16_t color = blend_to_black(COLOR_GOOD, 1.0f - reveal);

    if (s_name_len > 0) {
        char buf[40];
        snprintf(buf, sizeof(buf), "All set, %s!", s_name_buf);
        ui_draw_centered_text(200, color, buf, 2);
    } else {
        ui_draw_centered_text(200, color, "All set!", 3);
    }
    draw_ring(g, LCD_WIDTH / 2, LCD_HEIGHT / 2, 130, 28, COLOR_GOOD, reveal);

    if (reveal >= 1.0f) {
        int bw = 160, bh = 44;
        int bx = LCD_WIDTH / 2 - bw / 2, by = 340;
        g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
        g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, COLOR_GOOD);
        ui_draw_centered_text_at(LCD_WIDTH / 2, by + bh / 2 - 8, COLOR_TEXT, "Start", 1);
    }
}

// ---- WiFi + phone steps ----------------------------------------------------

static void draw_pill(Arduino_GFX *g, int cx, int by, int bw, int bh, uint16_t fill, const char *label) {
    int bx = cx - bw / 2;
    g->fillRoundRect(bx, by, bw, bh, bh / 2, COLOR_TEXT_DIM);
    g->fillRoundRect(bx + 2, by + 2, bw - 4, bh - 4, (bh - 4) / 2, fill);
    ui_draw_centered_text_at(cx, by + bh / 2 - 8, COLOR_TEXT, label, 1);
}

static bool hit_pill(int x, int y, int cx, int by, int bw, int bh) {
    int bx = cx - bw / 2;
    return x >= bx && x <= bx + bw && y >= by && y <= by + bh;
}

// Thick line as two triangles - same primitive-only technique as
// app_wifi_setup.cpp (self-contained copy, per this project's convention).
static void draw_thick_line(Arduino_GFX *g, float x0, float y0, float x1, float y1, float t, uint16_t color) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * t / 2.0f, ny = dx / len * t / 2.0f;
    g->fillTriangle((int)(x0 + nx), (int)(y0 + ny), (int)(x1 + nx), (int)(y1 + ny), (int)(x1 - nx), (int)(y1 - ny), color);
    g->fillTriangle((int)(x0 + nx), (int)(y0 + ny), (int)(x1 - nx), (int)(y1 - ny), (int)(x0 - nx), (int)(y0 - ny), color);
}

static void draw_wifi_bars(Arduino_GFX *g, int cx, int bottom_y, uint16_t color) {
    for (int i = 0; i < 4; i++) {
        int h = 14 + i * 12;
        int x = cx - 33 + i * 18;
        g->fillRoundRect(x, bottom_y - h, 12, h, 3, color);
    }
}

static void draw_wifi(Arduino_GFX *g) {
    bool up = wifi_is_connected();
    ui_draw_centered_text(64, COLOR_TEXT, "WiFi", 2);
    ui_draw_centered_text(112, COLOR_TEXT_DIM, "Sets your clock automatically", 1);
    ui_draw_centered_text(128, COLOR_TEXT_DIM, "and unlocks online features", 1);
    draw_wifi_bars(g, LCD_WIDTH / 2, 230, up ? COLOR_GOOD : COLOR_TEXT_DIM);
    if (up) {
        ui_draw_centered_text(250, COLOR_GOOD, "Connected", 2);
        draw_pill(g, LCD_WIDTH / 2, 300, 180, 44, COLOR_GOOD, "Next");
    } else {
        ui_draw_centered_text(256, COLOR_TEXT_DIM, "Not connected", 1);
        draw_pill(g, LCD_WIDTH / 2, 300, 180, 44, COLOR_ACCENT2, "Set Up WiFi");
        draw_pill(g, LCD_WIDTH / 2, 356, 120, 36, COLOR_PANEL, "Skip");
    }
}

static void draw_phone(Arduino_GFX *g) {
    bool en = ble_is_enabled();
    bool conn = en && ble_is_connected();
    ui_draw_centered_text(60, COLOR_TEXT, "Pair Phone", 2);
    ui_draw_centered_text(104, COLOR_TEXT_DIM, "Optional - for notifications, time", 1);
    ui_draw_centered_text(120, COLOR_TEXT_DIM, "and the companion app", 1);

    if (conn) {
        int cx = LCD_WIDTH / 2, cy = 190;
        g->fillCircle(cx, cy, 40, COLOR_GOOD);
        draw_thick_line(g, cx - 16, cy, cx - 5, cy + 13, 7, COLOR_TEXT);
        draw_thick_line(g, cx - 5, cy + 13, cx + 18, cy - 14, 7, COLOR_TEXT);
        ui_draw_centered_text(252, COLOR_GOOD, "Connected", 2);
        const char *name = ble_get_connected_device_name();
        ui_draw_centered_text(282, COLOR_TEXT_DIM, (name && name[0]) ? name : "Phone", 1);
        draw_pill(g, LCD_WIDTH / 2, 316, 180, 44, COLOR_GOOD, "Next");
    } else if (!en) {
        ui_draw_centered_text(190, COLOR_TEXT_DIM, "Bluetooth is off to save battery", 1);
        ui_draw_centered_text(210, COLOR_TEXT_DIM, "You can also do this later in Settings", 1);
        draw_pill(g, LCD_WIDTH / 2, 300, 200, 44, COLOR_ACCENT2, "Enable Bluetooth");
        draw_pill(g, LCD_WIDTH / 2, 356, 120, 36, COLOR_PANEL, "Skip");
    } else {
        ui_draw_centered_text(168, COLOR_TEXT_DIM, "Open the app and look for", 1);
        ui_draw_centered_text(186, COLOR_TEXT, "AmoledWatch", 2);
        // fw 3.0: the code is random and appears (full screen) when the
        // phone starts pairing.
        ui_draw_centered_text(224, COLOR_TEXT_DIM, "A pairing code will appear", 1);
        ui_draw_centered_text(242, COLOR_TEXT_DIM, "here - type it on the phone", 1);
        int dots = (millis() / 350) % 4;
        for (int i = 0; i < 3; i++)
            g->fillCircle(LCD_WIDTH / 2 - 20 + i * 20, 298, 5, i < dots ? COLOR_ACCENT2 : COLOR_TEXT_DIM);
        draw_pill(g, LCD_WIDTH / 2, 336, 140, 40, COLOR_PANEL, "Skip");
    }
}

static void draw_step_dots(Arduino_GFX *g) {
    if (s_step < OB_WIFI || s_step > OB_PREFS) return;
    const int n = (int)OB_PREFS - (int)OB_WIFI + 1;
    int idx = (int)s_step - (int)OB_WIFI;
    const int gap = 18, y = 420;
    int x0 = LCD_WIDTH / 2 - (n - 1) * gap / 2;
    for (int i = 0; i < n; i++)
        g->fillCircle(x0 + i * gap, y, i == idx ? 5 : 3, i == idx ? COLOR_TEXT : COLOR_TEXT_DIM);
}

static void onboarding_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    switch (s_step) {
        case OB_WELCOME: draw_welcome(g); break;
        case OB_WIFI: draw_wifi(g); break;
        case OB_PHONE: draw_phone(g); break;
        case OB_NAME: draw_name(g); break;
        case OB_BRIGHTNESS: draw_brightness(g); break;
        case OB_PREFS: draw_prefs(g); break;
        case OB_DONE: draw_done(g); break;
    }
    draw_step_dots(g);
}

// ---- Touch / tick -----------------------------------------------------

static void finish_onboarding() {
    strncpy(g_app_settings.user_name, s_name_buf, sizeof(g_app_settings.user_name) - 1);
    g_app_settings.user_name[sizeof(g_app_settings.user_name) - 1] = '\0';
    g_app_settings.onboarding_complete = true;
    nvs_save_settings(g_app_settings);
    ui_pop_screen();
}

static void touch_welcome(int x, int y) {
    uint32_t elapsed = millis() - s_step_enter_ms;
    if (elapsed <= 1400 + 500) return; // not fully revealed yet
    int bw = 160, bh = 44;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 340;
    if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
        enter_step(OB_WIFI);
    }
}

static void touch_wifi(int x, int y) {
    const int cx = LCD_WIDTH / 2;
    if (wifi_is_connected()) {
        if (hit_pill(x, y, cx, 300, 180, 44)) enter_step(OB_PHONE);
        return;
    }
    if (hit_pill(x, y, cx, 300, 180, 44)) {
        // WiFi Setup takes over this press and swallows its release (see
        // ui_handle_touch's screen-change handling), so this screen would
        // never see the finger lift and its edge detector would stay
        // stuck "down" - dropping the first tap after we return.
        s_prev_pressed = false;
        ui_push(&wifi_setup_screen);
    } else if (hit_pill(x, y, cx, 356, 120, 36)) {
        enter_step(OB_PHONE);
    }
}

static void touch_phone(int x, int y) {
    const int cx = LCD_WIDTH / 2;
    bool en = ble_is_enabled();
    bool conn = en && ble_is_connected();
    if (conn) {
        if (hit_pill(x, y, cx, 316, 180, 44)) {
            g_app_settings.ble_on = true;
            nvs_save_settings(g_app_settings);
            enter_step(OB_NAME);
        }
        return;
    }
    if (!en) {
        if (hit_pill(x, y, cx, 300, 200, 44)) {
            g_app_settings.ble_on = ble_enable();
            if (!g_app_settings.ble_on) ui_show_toast("Not enough memory for Bluetooth", 2500);
            nvs_save_settings(g_app_settings);
        } else if (hit_pill(x, y, cx, 356, 120, 36)) {
            enter_step(OB_NAME);
        }
        return;
    }
    // Enabled, waiting for a phone
    if (hit_pill(x, y, cx, 336, 140, 40)) {
        if (!s_ble_was_on_at_entry) {
            ble_disable();
            g_app_settings.ble_on = false;
            nvs_save_settings(g_app_settings);
        }
        enter_step(OB_NAME);
    }
}

// Raw (not edge-gated) events: the keypad needs holds and repeats.
static void touch_name(int x, int y, bool pressed) {
    KeypadEvent ev = keypad_touch(x, y, pressed);
    s_name_len = (int)strlen(s_name_buf);
    if (ev == KP_DONE) {
        // Trailing spaces from the "space 0" key aren't part of a name.
        while (s_name_len > 0 && s_name_buf[s_name_len - 1] == ' ') s_name_buf[--s_name_len] = 0;
        enter_step(OB_BRIGHTNESS);
    }
}

static void touch_prefs(int x, int y) {
    struct Row { int y; bool *field; };
    Row rows[3] = {
        { 90, &g_app_settings.haptic_on_touch },
        { 156, &g_app_settings.wake_on_touch },
        { 222, &g_app_settings.wake_on_motion },
    };
    int w = 320, h = 56;
    int rx = LCD_WIDTH / 2 - w / 2;
    for (int i = 0; i < 3; i++) {
        if (y >= rows[i].y && y <= rows[i].y + h && x >= rx && x <= rx + w) {
            *rows[i].field = !*rows[i].field;
            return;
        }
    }
    int bw = 140, bh = 44;
    int bx = LCD_WIDTH / 2 - bw / 2, by = 300;
    if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
        enter_step(OB_DONE);
    }
}

static void onboarding_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    if (s_step == OB_NAME) {
        touch_name(x, y, pressed);
        return;
    }

    if (s_step == OB_BRIGHTNESS) {
        // Slider needs continuous `pressed`, unlike every other step
        // here which is one-shot taps - handled before the tap_edge
        // gate below on purpose.
        int slider_left = 80, slider_right = LCD_WIDTH - 80;
        int slider_y = LCD_HEIGHT / 2 - 5, slider_h = 10;
        if (pressed) {
            if (y >= slider_y - 30 && y <= slider_y + slider_h + 30 && x >= slider_left - 14 && x <= slider_right + 14) {
                s_brightness_dragging = true;
                int v = (x - slider_left) * 255 / (slider_right - slider_left);
                if (v < 0) v = 0; if (v > 255) v = 255;
                g_app_settings.brightness = v;
                display_set_brightness(v);
                return;
            }
        } else {
            s_brightness_dragging = false;
        }
        if (!tap_edge) return;
        int bw = 140, bh = 44;
        int bx = LCD_WIDTH / 2 - bw / 2, by = 330;
        if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) {
            nvs_save_settings(g_app_settings);
            enter_step(OB_PREFS);
        }
        return;
    }

    if (!tap_edge) return;

    switch (s_step) {
        case OB_WELCOME: touch_welcome(x, y); break;
        case OB_WIFI: touch_wifi(x, y); break;
        case OB_PHONE: touch_phone(x, y); break;
        case OB_PREFS: touch_prefs(x, y); break;
        case OB_DONE: {
            uint32_t elapsed = millis() - s_step_enter_ms;
            if (elapsed < 500) return;
            int bw = 160, bh = 44;
            int bx = LCD_WIDTH / 2 - bw / 2, by = 340;
            if (x >= bx && x <= bx + bw && y >= by && y <= by + bh) finish_onboarding();
            break;
        }
        default: break;
    }
}

static void onboarding_tick() {
    if (s_step == OB_NAME) keypad_tick();
}

static void onboarding_create() {
    enter_step(OB_WELCOME);
    s_name_buf[0] = 0;
    s_name_len = 0;
    s_brightness_dragging = false;
    s_prev_pressed = false;
}

Screen onboarding_screen = {
    nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_DEFAULT,
    onboarding_create, onboarding_draw, onboarding_touch, onboarding_tick, nullptr, nullptr,
    0,    // idle_frame_ms - no idle throttling needed, this screen is short-lived
    true, // suppress_idle - a first-run setup flow (especially typing a name) shouldn't risk an awkward mid-setup auto-lock from the default 2-minute idle timeout
    false, // needs_tilt_calibration
    true,  // no_transition - has its own welcome reveal
};
