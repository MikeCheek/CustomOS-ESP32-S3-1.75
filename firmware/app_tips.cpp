// Tips: every gesture and button, shown - see app_tips.h.
#include "app_tips.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "haptics.h"
#include "hal_nvs.h"
#include "hal_touch.h"
#include "app_settings_state.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <stdlib.h>

static const int CX = LCD_WIDTH / 2;
static const int WX = CX, WY = 168, WR = 86;   // the little watch

enum TipKind : uint8_t { T_SWIPE, T_HOLD, T_EDGE, T_BUTTON, T_PAN, T_FLICK };
struct Tip {
    const char *title, *line1, *line2;
    TipKind kind;
    int8_t dx, dy;          // swipe direction
    uint8_t button;         // 0 BOOT, 1 PWR
    const char *press;      // the button's three ways, or nullptr
};

static const Tip TIPS[] = {
    {"Swipe left", "Your apps, in a honeycomb", "Drag around, tap one to open", T_SWIPE, -1, 0, 0, nullptr},
    {"Swipe up", "The Smart Stack", "What matters now, on cards", T_SWIPE, 0, -1, 0, nullptr},
    {"Swipe right", "Your notifications", "Swipe one left to clear it", T_SWIPE, 1, 0, 0, nullptr},
    {"Swipe down", "Control Center", "Quiet, torch, brightness...", T_SWIPE, 0, 1, 0, nullptr},
    {"Touch and hold", "Change the watch face", "Swipe between them, tap to wear", T_HOLD, 0, 0, 0, nullptr},
    {"From the left edge", "Back, in any app", "Drag it, and let go", T_EDGE, 1, 0, 0, nullptr},
    {"Side button", "On a face: Control Center", "In an app: back", T_BUTTON, 0, 0, 0, "Hold: home   Twice: assistant"},
    {"Power button", "Press: lock the screen", "Hold: power off or Emergency SOS", T_BUTTON, 0, 0, 1, nullptr},
    {"Flick twice", "Two quick wrist flicks: back", "Settings > Wake & quiet", T_FLICK, 0, 0, 0, nullptr},
};
static const int TIP_N = sizeof(TIPS) / sizeof(TIPS[0]);

static int s_page = 0;
static float s_pos = 0;            // shown page (follows s_page or the finger)
static uint32_t s_t0 = 0;          // the page's animation clock
static bool s_drag = false, s_moved = false;
static int s_x0 = 0;
static float s_pos0 = 0;

void tips_show_if_new() {
    if (g_app_settings.tips_seen) return;
    ui_push(&tips_screen);
}

static void finish() {
    if (!g_app_settings.tips_seen) {
        g_app_settings.tips_seen = true;
        nvs_save_settings(g_app_settings);
    }
    ui_pop_screen();
}

static void tips_create() {
    s_page = 0;
    s_pos = 0;
    s_t0 = millis();
    s_drag = false;
}

// A finger: a soft disc with a ring, pressed or lifted.
static void finger(Arduino_GFX *g, int x, int y, bool down) {
    g->fillCircle(x, y, down ? 15 : 13, down ? 0xFFFF : 0xBDF7);
    g->fillCircle(x, y, 7, down ? COLOR_ACCENT : (uint16_t)0x8410);
}

static void draw_demo(Arduino_GFX *g, const Tip &t, int ox) {
    int cx = WX + ox;
    // the watch: a dark round screen with a face's time on it
    g->fillCircle(cx, WY, WR + 6, 0x2104);
    g->fillCircle(cx, WY, WR, COLOR_BG);
    float ph = ((millis() - s_t0) % 1800) / 1800.0f;   // a loop
    if (t.kind == T_BUTTON) {
        // the button on the case, pressing in and out
        int by = t.button ? WY + 34 : WY - 34;
        bool in = ph > 0.25f && ph < 0.45f;
        g->fillRoundRect(cx + WR + 4 + (in ? -2 : 0), by - 16, 12, 32, 5, in ? COLOR_ACCENT : (uint16_t)0x8C71);
        ui_text_center(cx, WY - 8, COLOR_TEXT, "10:09", 3);
        ui_text_center(cx, WY + 24, COLOR_TEXT_DIM, t.button ? "PWR" : "BOOT", 1);
        return;
    }
    ui_text_center(cx, WY - 6, ui_dim(COLOR_TEXT, 0.5f), "10:09", 3);
    if (t.kind == T_HOLD) {
        // the finger rests, a ring fills, the face shrinks into the switcher
        bool down = ph > 0.1f && ph < 0.8f;
        float f = ph < 0.1f ? 0 : ph > 0.6f ? 1 : (ph - 0.1f) / 0.5f;
        if (down) ui_arc(cx, WY + 30, 26, 4, 0, 360 * f, COLOR_ACCENT, true);
        finger(g, cx, WY + 30, down);
        return;
    }
    if (t.kind == T_FLICK) {
        // the watch rocking, twice
        float a = sinf(ph * 2 * 3.14159f * 2) * (ph < 0.5f ? 1 : 0);
        int k = (int)(a * 14);
        g->fillRoundRect(cx - 40 + k, WY + 40, 80, 10, 5, COLOR_ACCENT);
        return;
    }
    // a swipe: the finger travels across, pressed
    float m = ph < 0.15f ? 0 : ph > 0.75f ? 1 : (ph - 0.15f) / 0.6f;
    float e = m * m * (3 - 2 * m);
    int span = WR - 26;
    int sx, sy;
    if (t.kind == T_EDGE) { sx = cx - WR + 14; sy = WY + 20; }
    else { sx = cx - t.dx * span; sy = WY - t.dy * span; }
    if (t.kind != T_EDGE && t.dx == 0) sx = cx;
    if (t.kind != T_EDGE && t.dy == 0) sy = WY + 20;
    int ex = t.kind == T_EDGE ? cx + 30 : cx + t.dx * span;
    int ey = t.kind == T_EDGE ? sy : (t.dy ? WY + t.dy * span : sy);
    int fx = sx + (int)((ex - sx) * e), fy = sy + (int)((ey - sy) * e);
    // a trail behind it
    for (int i = 1; i <= 4; i++) {
        float te = e - i * 0.06f;
        if (te <= 0) break;
        g->fillCircle(sx + (int)((ex - sx) * te), sy + (int)((ey - sy) * te), 6 - i, ui_dim(COLOR_ACCENT, 0.9f - i * 0.18f));
    }
    finger(g, fx, fy, ph > 0.1f && ph < 0.8f);
}

static void tips_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    int base = (int)floorf(s_pos);
    for (int p = base; p <= base + 1; p++) {
        if (p < 0 || p >= TIP_N) continue;
        int ox = (int)((p - s_pos) * LCD_WIDTH);
        if (abs(ox) >= LCD_WIDTH) continue;
        const Tip &t = TIPS[p];
        draw_demo(g, t, ox);
        ui_text_center(CX + ox, 292, COLOR_TEXT, t.title, 3);
        ui_text_center(CX + ox, 330, COLOR_TEXT, t.line1, 2);
        ui_text_center(CX + ox, 356, COLOR_TEXT_DIM, t.line2, 1);
        if (t.press) ui_text_center(CX + ox, 376, COLOR_ACCENT, t.press, 1);
    }
    // where you are, and the way on
    int near = (int)lroundf(s_pos);
    for (int i = 0; i < TIP_N; i++)
        g->fillCircle(CX - (TIP_N - 1) * 7 + i * 14, 404, i == near ? 5 : 3, i == near ? COLOR_TEXT : COLOR_TEXT_DIM);
    bool last = near == TIP_N - 1;
    g->fillRoundRect(CX - 60, 420, 120, 34, 17, last ? COLOR_ACCENT : (uint16_t)0x2945);
    ui_text_center(CX, 437, COLOR_TEXT, last ? "Done" : "Skip", 1);
    ui_text_center(CX, 40, COLOR_TEXT_DIM, "Tips", 2);
}

static void tips_touch(int x, int y, bool pressed) {
    if (pressed) {
        if (!s_drag) { s_drag = true; s_moved = false; s_x0 = x; s_pos0 = s_pos; return; }
        if (abs(x - s_x0) > 10) s_moved = true;
        float p = s_pos0 - (float)(x - s_x0) / LCD_WIDTH;
        if (p < 0) p *= 0.3f;
        if (p > TIP_N - 1) p = (TIP_N - 1) + (p - (TIP_N - 1)) * 0.3f;
        s_pos = p;
        return;
    }
    if (!s_drag) return;
    s_drag = false;
    if (s_moved) {
        touch_cancel_swipes();
        int to = (int)lroundf(s_pos + (s_pos - s_pos0) * 0.6f);
        if (to < 0) to = 0;
        if (to > TIP_N - 1) to = TIP_N - 1;
        if (to != s_page) { s_page = to; s_t0 = millis(); haptic(HAP_CLICK); }
        return;
    }
    if (y > 414 && abs(x - CX) < 70) { finish(); return; }   // Skip / Done
    // a tap on the right half: next; left: back
    int to = s_page + (x > CX ? 1 : -1);
    if (to >= 0 && to < TIP_N) { s_page = to; s_t0 = millis(); haptic(HAP_CLICK); }
    else if (to >= TIP_N) finish();
}

static void tips_tick() {
    if (s_drag) return;
    float d = s_page - s_pos;
    s_pos += fabsf(d) < 0.005f ? d : d * 0.25f;
}

static void tips_gesture(Gesture) {}

Screen tips_screen = {
    nullptr, GESTURE_MODE_FREE, UI_FRAME_MS_DEFAULT,
    tips_create, tips_draw, tips_touch, tips_tick, nullptr, tips_gesture,
    0,
    true,   // suppress_idle: the demos move
    false, false,
    true,   // no_drag_back: sideways is paging here
    true,   // hide_status
};
