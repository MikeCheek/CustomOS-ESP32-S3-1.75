/*
 * ui_panel.cpp
 * The swipe-down control panel, laid out for the round screen:
 *
 *                 ___battery arc___
 *          sun  /       87%        \  speaker
 *              /       19:31        \
 *   brightness|      Sun 4 Oct       |volume
 *   edge arc  |   Phone connected    |edge arc
 *   (drag)    |  (o) (o) (o) (o)     |(drag)
 *             |  (o) (o) (o) (o)     |
 *              \ (o) (o) (o) (o)    /
 *               \    (o)  (o)      /
 *                 ------ handle ----
 *
 * Tiles act on release (so a press can turn into a long-press or a swipe
 * without also toggling). Tiles with a ring around them while held have a
 * long-press action (open the matching settings page). Brightness and
 * volume are the two edge arcs - drag along them; the value is saved when
 * the finger lifts. Closes with a swipe up, a tap on the handle, or BOOT.
 */
#include "ui.h"
#include "dnd.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "apps.h"
#include "app_wifi_setup.h"
#include "app_usb.h"
#include "app_poweroff.h"
#include "app_torch.h"
#include "anim_lock.h"
#include "hal_display.h"
#include "hal_audio.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_ble.h"
#include "hal_wifi.h"
#include "hal_ntp.h"
#include "hal_gps.h"
#include "hal_nvs.h"
#include "hal_touch.h"
#include "hal_usb.h"
#include "battery_mode.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

extern Screen settings_battery_screen;   // app_settings.cpp
extern Screen settings_volume_screen;

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int TILE_R = 33;

// ---- edge arcs (clock degrees: 0 = 12 o'clock, clockwise) -----------------
static const float BAT_START = 324, BAT_SWEEP = 72;     // top
static const float BRI_START = 222, BRI_SWEEP = 90;     // left, fills upward
static const float VOL_START = 138, VOL_SWEEP = -90;    // right, fills upward
static const int   ARC_THICK = 10, ARC_INSET = 5;
static const int   ARC_TOUCH_R = 196;                   // touches further out than this grab an arc

// ---- open/close animation ---------------------------------------------------
enum PanelState : uint8_t { P_CLOSED, P_OPENING, P_OPEN, P_CLOSING };
static PanelState s_state = P_CLOSED;
static uint32_t   s_anim_t0 = 0;
static const uint32_t OPEN_MS = 260, CLOSE_MS = 190;
static void (*s_after_close)() = nullptr;   // runs once the panel is out of the way

// ---- touch ------------------------------------------------------------------
static bool     s_prev = false;
static int      s_press_tile = -1;
static bool     s_press_valid = false;
static bool     s_long_fired = false;
static uint32_t s_press_t0 = 0;
static int      s_px0 = 0, s_py0 = 0;
static uint8_t  s_drag_arc = 0;             // 0 none, 1 brightness, 2 volume
static bool     s_handle_press = false;

// ---- status caches (each of these is an I2C read) ---------------------------
static uint32_t  s_cache_ms = 0;
static int       s_bat = -1;
static bool      s_charging = false;
static WatchTime s_time{};

static char     s_caption[40] = "";
static uint32_t s_caption_until = 0;

static void caption(const char *fmt, const char *arg = nullptr, int num = 0) {
    if (arg) snprintf(s_caption, sizeof(s_caption), fmt, arg);
    else     snprintf(s_caption, sizeof(s_caption), fmt, num);
    s_caption_until = millis() + 1600;
}

// =============================================================================
//  Actions shared with Settings (same side effects as its rows)
// =============================================================================

static void set_wifi(bool on) {
    g_app_settings.wifi_on = on;
    if (on) { wifi_enable(); ntp_sync(); } else wifi_disable();
    nvs_save_settings(g_app_settings);
}
static void set_ble(bool on) {
    g_app_settings.ble_on = on;
    if (on) {
        if (!ble_enable()) { g_app_settings.ble_on = false; ui_show_toast("Not enough memory for Bluetooth", 2500); }
    } else ble_disable();
    nvs_save_settings(g_app_settings);
}
static void set_gps(bool on) {
    g_app_settings.gps_on = on;
    if (on) gps_enable(); else gps_disable();
    nvs_save_settings(g_app_settings);
}

// =============================================================================
//  Tiles
// =============================================================================

enum TileId : uint8_t {
    T_WIFI, T_BT, T_GPS, T_DND,
    T_SFX, T_MUSIC, T_ROTATE, T_AWAKE,
    T_BATT, T_TORCH, T_USB, T_LOCK,
    T_SETTINGS, T_POWER,
    T_COUNT
};
struct Tile { int x, y; };
static const int ROW1 = 188, ROW2 = 262, ROW3 = 336, ROW4 = 406;
static const Tile TILES[T_COUNT] = {
    {CX - 120, ROW1}, {CX - 40, ROW1}, {CX + 40, ROW1}, {CX + 120, ROW1},
    {CX - 120, ROW2}, {CX - 40, ROW2}, {CX + 40, ROW2}, {CX + 120, ROW2},
    {CX - 120, ROW3}, {CX - 40, ROW3}, {CX + 40, ROW3}, {CX + 120, ROW3},
    {CX - 44, ROW4},  {CX + 44, ROW4},
};

static bool tile_on(int t) {
    switch (t) {
    case T_WIFI:   return g_app_settings.wifi_on;
    case T_BT:     return ble_is_enabled();
    case T_GPS:    return gps_is_enabled();
    case T_DND:    return dnd_active();
    case T_SFX:    return g_app_settings.sfx_enabled;
    case T_MUSIC:  return g_app_settings.music_enabled;
    case T_ROTATE: return g_app_settings.auto_rotate;
    case T_AWAKE:  return ui_keep_awake();
    case T_BATT:   return true;     // always shows the active mode, in its colour
    case T_USB:    return usb_cable_present() && usb_mode_active() != USB_MODE_CHARGE_ONLY;
    default:       return false;
    }
}
static uint16_t tile_hue(int t) {
    if (t == T_BATT) return battery_profile(battery_mode_current()).color;
    switch (t) {
    case T_WIFI: case T_BT: case T_USB: return COLOR_ACCENT2;
    case T_GPS:                         return COLOR_GOOD;
    case T_AWAKE:                       return COLOR_WARN;
    case T_TORCH:                       return COLOR_TEXT;
    default:                            return COLOR_ACCENT;
    }
}
static bool tile_is_action(int t) {
    return t == T_TORCH || t == T_LOCK || t == T_SETTINGS || t == T_POWER || t == T_USB;
}
static Screen *tile_long_target(int t) {
    switch (t) {
    case T_WIFI:  return &wifi_setup_screen;
    case T_BT:    return &ble_status_screen;
    case T_SFX: case T_MUSIC: return &settings_volume_screen;
    case T_BATT: return &settings_battery_screen;
    default:      return nullptr;
    }
}

// Deferred navigation: actions that open a screen close the panel first and
// push once it's gone, so the new screen slides in over the real one.
static Screen *s_pending_push = nullptr;
static void push_pending() { if (s_pending_push) { Screen *s = s_pending_push; s_pending_push = nullptr; ui_push(s); } }
static void lock_now() { lock_anim_set_mode(false, true); ui_push(&lock_anim_screen); }
static void close_then_push(Screen *s) { s_pending_push = s; s_after_close = push_pending; ui_top_panel_close(); }

static void tile_tap(int t) {
    bool on;
    switch (t) {
    case T_WIFI:  on = !g_app_settings.wifi_on; set_wifi(on); caption(on ? "Wi-Fi on" : "Wi-Fi off"); break;
    case T_BT:    on = !ble_is_enabled(); set_ble(on); caption(on ? "Bluetooth on" : "Bluetooth off"); break;
    case T_GPS:
        if (!gps_available()) { caption("No GPS module fitted"); break; }
        on = !gps_is_enabled(); set_gps(on); caption(on ? "GPS on" : "GPS off");
        break;
    case T_DND:
        if (dnd_bedtime_now() && !dnd_manual()) {
            caption("Bedtime - change it in Settings");
            break;
        }
        dnd_set(!dnd_manual(), false);   // the phone follows, if DND sync is on in the app
        caption(dnd_manual() ? "Notifications silenced" : "Notifications on");
        break;
    case T_SFX:
        g_app_settings.sfx_enabled = !g_app_settings.sfx_enabled;
        nvs_save_settings(g_app_settings);
        caption(g_app_settings.sfx_enabled ? "Sound effects on" : "Sound effects off");
        break;
    case T_MUSIC:
        g_app_settings.music_enabled = !g_app_settings.music_enabled;
        nvs_save_settings(g_app_settings);
        caption(g_app_settings.music_enabled ? "Game music on" : "Game music off");
        break;
    case T_ROTATE:
        if (!g_app_settings.auto_rotate && !g_app_settings.tilt_calibrated) {
            close_then_push(nullptr);
            s_after_close = [] {
                ui_show_confirm("Calibrate tilt?", "Needed for auto-rotate", "Calibrate", "Cancel",
                                [](bool ok) { if (ok) ui_push(&calibration_screen); });
            };
            return;
        }
        g_app_settings.auto_rotate = !g_app_settings.auto_rotate;
        if (!g_app_settings.auto_rotate) ui_reset_auto_rotate_state();
        nvs_save_settings(g_app_settings);
        caption(g_app_settings.auto_rotate ? "Auto-rotate on" : "Auto-rotate off");
        break;
    case T_AWAKE:
        ui_set_keep_awake(!ui_keep_awake());
        caption(ui_keep_awake() ? "Screen stays on" : "Screen sleeps normally");
        break;
    case T_BATT: {
        // Cycles Performance -> Balanced -> Saver -> Ultra; long-press opens
        // the Battery Mode page with all four explained.
        BatteryMode m = battery_mode_next(battery_mode_current());
        battery_mode_apply(m);
        caption("%s", battery_profile(m).name);
        break;
    }
    case T_TORCH:    close_then_push(&torch_screen); break;
    case T_USB:      close_then_push(&usb_mode_screen); break;
    case T_SETTINGS: close_then_push(&settings_screen); break;
    case T_POWER:    close_then_push(&poweroff_screen); break;
    case T_LOCK:     s_after_close = lock_now; ui_top_panel_close(); break;
    }
}

// =============================================================================
//  Drawing
// =============================================================================

// Thick line as two triangles + round ends (drawLine is 1px; the fast
// H/V line paths are unreliable on this driver - see ui_draw_list_row()).
static void tline(Arduino_GFX *g, float x0, float y0, float x1, float y1, float w, uint16_t c) {
    float dx = x1 - x0, dy = y1 - y0, L = sqrtf(dx * dx + dy * dy);
    if (L < 0.5f) { g->fillCircle((int)x0, (int)y0, (int)(w / 2), c); return; }
    float nx = -dy / L * w * 0.5f, ny = dx / L * w * 0.5f;
    g->fillTriangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    g->fillTriangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
    int r = (int)(w / 2);
    if (r > 0) { g->fillCircle((int)x0, (int)y0, r, c); g->fillCircle((int)x1, (int)y1, r, c); }
}

static void icon_sun(Arduino_GFX *g, int cx, int cy, int r, uint16_t c) {
    g->fillCircle(cx, cy, r, c);
    for (int i = 0; i < 8; i++) {
        float a = i * 0.7854f;
        tline(g, cx + sinf(a) * (r + 3), cy - cosf(a) * (r + 3), cx + sinf(a) * (r + 6), cy - cosf(a) * (r + 6), 2, c);
    }
}
static void icon_speaker(Arduino_GFX *g, int cx, int cy, uint16_t c, uint16_t bg, bool waves, bool crossed) {
    g->fillRect(cx - 13, cy - 5, 7, 10, c);
    g->fillTriangle(cx - 8, cy - 5, cx + 1, cy - 12, cx + 1, cy + 12, c);
    g->fillTriangle(cx - 8, cy - 5, cx + 1, cy + 12, cx - 8, cy + 5, c);
    if (waves) {
        ui_arc(cx + 1, cy, 10, 3, 45, 90, c);
        ui_arc(cx + 1, cy, 17, 3, 45, 90, c);
    }
    if (crossed) { tline(g, cx + 6, cy - 6, cx + 15, cy + 6, 3, c); tline(g, cx + 15, cy - 6, cx + 6, cy + 6, 3, c); }
    (void)bg;
}

static void draw_icon(Arduino_GFX *g, int t, int cx, int cy, uint16_t c, uint16_t bg, bool on) {
    switch (t) {
    case T_WIFI:
        ui_arc(cx, cy + 10, 24, 4, 315, 90, c);
        ui_arc(cx, cy + 10, 16, 4, 315, 90, c);
        g->fillCircle(cx, cy + 8, 4, c);
        if (!on) tline(g, cx - 14, cy - 12, cx + 14, cy + 14, 3, c);
        break;
    case T_BT:
        tline(g, cx - 1, cy - 15, cx - 1, cy + 15, 3, c);
        tline(g, cx - 1, cy - 15, cx + 8, cy - 7, 3, c);
        tline(g, cx + 8, cy - 7, cx - 9, cy + 7, 3, c);
        tline(g, cx - 1, cy + 15, cx + 8, cy + 7, 3, c);
        tline(g, cx + 8, cy + 7, cx - 9, cy - 7, 3, c);
        break;
    case T_GPS:
        g->fillCircle(cx, cy - 5, 11, c);
        g->fillTriangle(cx - 10, cy - 1, cx + 10, cy - 1, cx, cy + 15, c);
        g->fillCircle(cx, cy - 5, 4, bg);
        break;
    case T_DND:
        g->fillCircle(cx, cy, 13, c);
        g->fillCircle(cx + 8, cy - 6, 11, bg);
        break;
    case T_SFX:
        icon_speaker(g, cx - 2, cy, c, bg, on, !on);
        break;
    case T_MUSIC:
        g->fillCircle(cx - 8, cy + 9, 5, c);
        g->fillCircle(cx + 8, cy + 6, 5, c);
        g->fillRect(cx - 5, cy - 11, 3, 20, c);
        g->fillRect(cx + 11, cy - 14, 3, 20, c);
        tline(g, cx - 4, cy - 11, cx + 12, cy - 14, 5, c);
        if (!on) tline(g, cx - 14, cy - 14, cx + 14, cy + 14, 3, c);
        break;
    case T_ROTATE:
        // phone outline + two curved arrows around it
        g->fillRoundRect(cx - 6, cy - 10, 12, 20, 3, c);
        g->fillRoundRect(cx - 3, cy - 7, 6, 14, 1, bg);
        ui_arc(cx, cy, 17, 3, 25, 60, c, false);
        ui_arc(cx, cy, 17, 3, 205, 60, c, false);
        g->fillTriangle(cx + 17, cy - 4, cx + 11, cy - 4, cx + 15, cy + 3, c);   // end of the right arrow (85 deg)
        g->fillTriangle(cx - 17, cy + 4, cx - 11, cy + 4, cx - 15, cy - 3, c);   // end of the left arrow (265 deg)
        break;
    case T_AWAKE:
        g->fillRoundRect(cx - 12, cy - 3, 19, 15, 4, c);
        ui_arc(cx + 7, cy + 4, 7, 3, 20, 140, c, false);
        tline(g, cx - 7, cy - 14, cx - 7, cy - 8, 2, c);
        tline(g, cx - 1, cy - 15, cx - 1, cy - 8, 2, c);
        tline(g, cx + 5, cy - 14, cx + 5, cy - 8, 2, c);
        break;
    case T_BATT:
        battery_mode_draw_icon(g, battery_mode_current(), cx, cy, c, bg);
        break;
    case T_TORCH:
        g->fillTriangle(cx - 11, cy - 12, cx + 11, cy - 12, cx + 5, cy - 2, c);
        g->fillTriangle(cx - 11, cy - 12, cx + 5, cy - 2, cx - 5, cy - 2, c);
        g->fillRect(cx - 5, cy - 2, 10, 17, c);
        g->fillRect(cx - 1, cy + 2, 3, 5, bg);
        break;
    case T_USB:
        tline(g, cx, cy + 12, cx, cy - 9, 3, c);
        g->fillTriangle(cx - 5, cy - 9, cx + 5, cy - 9, cx, cy - 17, c);
        tline(g, cx, cy + 5, cx - 8, cy - 1, 3, c);
        tline(g, cx - 8, cy - 1, cx - 8, cy - 4, 3, c);
        g->fillCircle(cx - 8, cy - 6, 3, c);
        tline(g, cx, cy + 1, cx + 8, cy - 4, 3, c);
        g->fillRect(cx + 5, cy - 11, 7, 7, c);
        g->fillCircle(cx, cy + 14, 4, c);
        break;
    case T_LOCK:
        ui_arc(cx, cy - 3, 9, 3, 270, 180, c, false);
        g->fillRect(cx - 9, cy - 3, 3, 5, c);
        g->fillRect(cx + 6, cy - 3, 3, 5, c);
        g->fillRoundRect(cx - 12, cy + 1, 24, 15, 3, c);
        g->fillCircle(cx, cy + 7, 2, bg);
        break;
    case T_SETTINGS:
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7854f;
            g->fillCircle(cx + (int)roundf(sinf(a) * 12), cy - (int)roundf(cosf(a) * 12), 4, c);
        }
        g->fillCircle(cx, cy, 11, c);
        g->fillCircle(cx, cy, 5, bg);
        break;
    case T_POWER:
        ui_arc(cx, cy + 1, 14, 3, 35, 290, c);
        tline(g, cx, cy - 15, cx, cy - 2, 3, c);
        break;
    }
}

static void draw_tile(Arduino_GFX *g, int t, int oy, uint32_t now) {
    int x = TILES[t].x, y = TILES[t].y + oy;
    bool on = tile_on(t);
    bool pressed = s_press_valid && s_press_tile == t;
    if (t == T_GPS && !gps_available()) {
        // No module on this board: hollow, dim tile with a slash.
        g->fillCircle(x, y, TILE_R, ui_dim(COLOR_PANEL, 0.6f));
        g->fillCircle(x, y, TILE_R - 2, COLOR_BG);
        uint16_t c = ui_dim(COLOR_TEXT_DIM, 0.6f);
        draw_icon(g, t, x, y, c, COLOR_BG, false);
        tline(g, x - 16, y - 16, x + 16, y + 16, 3, c);
        return;
    }
    uint16_t hue = tile_hue(t);
    uint16_t fill, ink;
    if (tile_is_action(t) && !(t == T_USB && on)) {
        fill = pressed ? ui_dim(COLOR_TEXT, 0.30f) : COLOR_PANEL;
        ink = t == T_POWER ? COLOR_BAD : COLOR_TEXT;
    } else if (on) {
        fill = pressed ? ui_dim(hue, 0.75f) : hue;
        ink = (hue == COLOR_WARN || hue == COLOR_TEXT || hue == COLOR_GOOD) ? COLOR_BG : COLOR_TEXT;
    } else {
        fill = pressed ? ui_dim(COLOR_TEXT, 0.30f) : COLOR_PANEL;
        ink = COLOR_TEXT_DIM;
    }
    g->fillCircle(x, y, pressed ? TILE_R - 2 : TILE_R, fill);
    draw_icon(g, t, x, y, ink, fill, on);

    // Long-press progress ring for tiles that have a long action.
    if (pressed && !s_long_fired && tile_long_target(t)) {
        uint32_t held = now - s_press_t0;
        if (held > 150) {
            float f = (held - 150) / 400.0f;
            if (f > 1) f = 1;
            ui_arc(x, y, TILE_R + 5, 3, 0, 360 * f, COLOR_TEXT);
        }
    }
    // "Connected" badge on radios.
    bool linked = (t == T_WIFI && wifi_is_connected()) || (t == T_BT && ble_is_connected());
    if (linked) {
        g->fillCircle(x + 23, y - 23, 7, COLOR_BG);
        g->fillCircle(x + 23, y - 23, 5, COLOR_GOOD);
    }
}

static float arc_frac(uint8_t which) {
    return which == 1 ? (g_app_settings.brightness - 8) / 247.0f : audio_get_volume() / 100.0f;
}

static void draw_value_arc(uint8_t which, int oy, uint16_t color) {
    float start = which == 1 ? BRI_START : VOL_START;
    float sweep = which == 1 ? BRI_SWEEP : VOL_SWEEP;
    int r = ui_screen_radius() - ARC_INSET;
    float f = arc_frac(which);
    if (f < 0) f = 0; if (f > 1) f = 1;
    bool active = s_drag_arc == which;
    ui_arc(CX, CY + oy, r, ARC_THICK, start, sweep, ui_dim(color, 0.22f));
    if (f > 0.01f) ui_arc(CX, CY + oy, r, ARC_THICK, start, sweep * f, color);
    int kx, ky;
    float a = (start + sweep * f) * 0.017453f;
    kx = CX + (int)roundf(sinf(a) * (r - ARC_THICK / 2));
    ky = CY + oy - (int)roundf(cosf(a) * (r - ARC_THICK / 2));
    Arduino_GFX *g = ui_gfx();
    g->fillCircle(kx, ky, active ? 12 : 9, COLOR_TEXT);
    g->fillCircle(kx, ky, active ? 6 : 4, color);
}

static void refresh_cache(uint32_t now) {
    if (s_cache_ms && now - s_cache_ms < 1000) return;
    s_cache_ms = now;
    s_bat = power_get_battery_percent();
    s_charging = power_is_charging();
    s_time = rtc_now();
}

static float ease_out(float t) { float u = 1 - t; return 1 - u * u * u; }
static float ease_in(float t)  { return t * t * t; }

// Vertical offset of the panel: -LCD_HEIGHT (hidden) .. 0 (fully down).
static int panel_offset(uint32_t now) {
    float t;
    switch (s_state) {
    case P_OPENING:
        t = (now - s_anim_t0) / (float)OPEN_MS;
        if (t >= 1) { s_state = P_OPEN; return 0; }
        return (int)(-(1 - ease_out(t)) * LCD_HEIGHT);
    case P_CLOSING:
        t = (now - s_anim_t0) / (float)CLOSE_MS;
        if (t >= 1) {
            s_state = P_CLOSED;
            void (*fn)() = s_after_close;
            s_after_close = nullptr;
            if (fn) fn();
            return -LCD_HEIGHT;
        }
        return (int)(-ease_in(t) * LCD_HEIGHT);
    case P_OPEN: return 0;
    default:     return -LCD_HEIGHT;
    }
}

void ui_top_panel_draw() {
    Arduino_GFX *g = ui_gfx();
    if (s_state == P_CLOSED || !g) return;
    uint32_t now = millis();
    int oy = panel_offset(now);
    if (s_state == P_CLOSED) return;
    refresh_cache(now);

    // Long press fires while still held.
    if (s_press_valid && !s_long_fired && s_press_tile >= 0 && tile_long_target(s_press_tile) &&
        now - s_press_t0 >= 550) {
        s_long_fired = true;
        close_then_push(tile_long_target(s_press_tile));
    }

    g->fillRect(0, oy, LCD_WIDTH, LCD_HEIGHT, COLOR_BG);
    if (oy < 0) g->fillRect(0, oy + LCD_HEIGHT - 3, LCD_WIDTH, 3, ui_dim(COLOR_ACCENT, 0.6f));

    // ---- battery arc + % ----
    int r = ui_screen_radius() - ARC_INSET;
    uint16_t bc = s_charging ? COLOR_GOOD : (s_bat >= 0 && s_bat <= 20 ? COLOR_BAD : COLOR_TEXT);
    int bat = s_bat < 0 ? 0 : s_bat;
    ui_arc(CX, CY + oy, r, 6, BAT_START, BAT_SWEEP, ui_dim(bc, 0.22f));
    if (bat > 0) ui_arc(CX, CY + oy, r, 6, BAT_START, BAT_SWEEP * bat / 100.0f, bc);
    char buf[32];
    if (s_bat >= 0) snprintf(buf, sizeof(buf), s_charging ? "%d%% +" : "%d%%", s_bat);
    else snprintf(buf, sizeof(buf), "--%%");
    ui_text_center(CX, 40 + oy, bc, buf, 2);

    // ---- clock ----
    if (s_time.valid) {
        static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        snprintf(buf, sizeof(buf), "%02d:%02d", s_time.hour, s_time.minute);
        ui_text_center(CX, 80 + oy, COLOR_TEXT, buf, 4);
        int wd = s_time.weekday >= 0 && s_time.weekday < 7 ? s_time.weekday : 0;
        int mo = s_time.month >= 1 && s_time.month <= 12 ? s_time.month - 1 : 0;
        snprintf(buf, sizeof(buf), "%s %d %s", DAYS[wd], s_time.day, MONTHS[mo]);
        ui_text_center(CX, 114 + oy, COLOR_TEXT_DIM, buf, 2);
    }

    // ---- caption: last action / dragged value / connection summary ----
    const char *cap = nullptr;
    uint16_t cap_c = COLOR_TEXT;
    if (s_drag_arc == 1) {
        snprintf(buf, sizeof(buf), "Brightness %d%%", (int)roundf(arc_frac(1) * 100));
        cap = buf;
    } else if (s_drag_arc == 2) {
        snprintf(buf, sizeof(buf), "Volume %d%%", audio_get_volume());
        cap = buf;
    } else if (now < s_caption_until) {
        cap = s_caption;
    } else {
        cap_c = COLOR_TEXT_DIM;
        if (dnd_active())                cap = dnd_manual() ? "Do not disturb" : "Bedtime - do not disturb";
        else if (ble_is_connected())     cap = "Phone connected";
        else if (wifi_is_connected())    cap = "Wi-Fi connected";
        else if (usb_cable_present())    { snprintf(buf, sizeof(buf), "USB: %s", usb_mode_name(usb_mode_active())); cap = buf; }
    }
    if (cap) ui_text_center(CX, 142 + oy, cap_c, cap, 2);

    // ---- edge arcs ----
    draw_value_arc(1, oy, COLOR_WARN);
    draw_value_arc(2, oy, COLOR_ACCENT2);
    {
        int sx, sy, vx, vy;
        float a1 = 318 * 0.017453f, a2 = 42 * 0.017453f;
        sx = CX + (int)(sinf(a1) * 198); sy = CY + oy - (int)(cosf(a1) * 198);
        vx = CX + (int)(sinf(a2) * 198); vy = CY + oy - (int)(cosf(a2) * 198);
        icon_sun(g, sx, sy, 5, s_drag_arc == 1 ? COLOR_WARN : COLOR_TEXT_DIM);
        icon_speaker(g, vx + 2, vy, s_drag_arc == 2 ? COLOR_ACCENT2 : COLOR_TEXT_DIM, COLOR_BG, true, false);
    }

    // ---- tiles ----
    for (int t = 0; t < T_COUNT; t++) draw_tile(g, t, oy, now);

    // ---- handle ----
    g->fillRoundRect(CX - 22, 446 + oy, 44, 6, 3, s_handle_press ? COLOR_TEXT : COLOR_TEXT_DIM);
}

// =============================================================================
//  Touch
// =============================================================================

static bool in_arc_zone(int x, int y, uint8_t which) {
    if (ui_radius_of(x, y) < ARC_TOUCH_R) return false;
    float a = ui_angle_of(x, y);
    return which == 1 ? ui_angle_in_arc(a, BRI_START - 8, BRI_SWEEP + 16)
                      : ui_angle_in_arc(a, VOL_START + 8, VOL_SWEEP - 16);
}

static void arc_set_from(int x, int y) {
    float a = ui_angle_of(x, y);
    float f;
    if (s_drag_arc == 1) {
        f = (a - BRI_START) / BRI_SWEEP;
        if (a < 90) f = 1;                   // dragged past the top onto the right half
        if (f < 0) f = 0; if (f > 1) f = 1;
        uint8_t b = (uint8_t)(8 + f * 247 + 0.5f);
        if (b != g_app_settings.brightness) { g_app_settings.brightness = b; display_set_brightness(b); }
    } else {
        f = (VOL_START - a) / -VOL_SWEEP;
        if (a > 270) f = 1;                  // past the top onto the left half
        if (f < 0) f = 0; if (f > 1) f = 1;
        uint8_t v = (uint8_t)(f * 100 + 0.5f);
        if (v != audio_get_volume()) { audio_set_volume(v); g_app_settings.volume = v; }
    }
}

void ui_top_panel_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_prev;
    s_prev = pressed;
    if (s_state != P_OPEN && s_state != P_OPENING) { s_press_valid = false; s_drag_arc = 0; return; }

    if (edge) {
        s_px0 = x; s_py0 = y;
        s_press_t0 = millis();
        s_long_fired = false;
        s_press_valid = false;
        s_press_tile = -1;
        s_handle_press = false;
        if (in_arc_zone(x, y, 1))      { s_drag_arc = 1; arc_set_from(x, y); touch_cancel_swipes(); return; }
        if (in_arc_zone(x, y, 2))      { s_drag_arc = 2; arc_set_from(x, y); touch_cancel_swipes(); return; }
        for (int t = 0; t < T_COUNT; t++) {
            int dx = x - TILES[t].x, dy = y - TILES[t].y;
            if (dx * dx + dy * dy <= (TILE_R + 6) * (TILE_R + 6)) { s_press_tile = t; s_press_valid = true; return; }
        }
        if (y > 430 && abs(x - CX) < 70) s_handle_press = true;
        return;
    }

    if (pressed) {
        if (s_drag_arc) { arc_set_from(x, y); touch_cancel_swipes(); return; }
        // Moved too far: it's a swipe, not a tap.
        if (s_press_valid && (abs(x - s_px0) > 22 || abs(y - s_py0) > 22)) s_press_valid = false;
        return;
    }

    // release
    if (s_drag_arc) {
        s_drag_arc = 0;
        nvs_save_settings(g_app_settings);   // once per drag, not per frame
        touch_cancel_swipes();
        return;
    }
    if (s_handle_press) { s_handle_press = false; ui_top_panel_close(); return; }
    if (s_press_valid && !s_long_fired) {
        int t = s_press_tile;
        s_press_valid = false;
        tile_tap(t);
    }
    s_press_valid = false;
}

// =============================================================================
//  State
// =============================================================================

void ui_top_panel_open() {
    if (s_state == P_OPEN || s_state == P_OPENING) return;
    uint32_t now = millis();
    if (s_state == P_CLOSING) {
        // Reverse from wherever it is.
        float t = (now - s_anim_t0) / (float)CLOSE_MS; if (t > 1) t = 1;
        float shown = 1 - ease_in(t);   // fraction visible
        // find opening t with ease_out(t) = shown -> approximate by inverse
        float u = 1 - cbrtf(1 - shown);
        s_anim_t0 = now - (uint32_t)(u * OPEN_MS);
        s_after_close = nullptr;
    } else {
        s_anim_t0 = now;
    }
    s_state = P_OPENING;
    s_prev = false;
    s_press_valid = false;
    s_drag_arc = 0;
    s_cache_ms = 0;
    s_caption_until = 0;
}

void ui_top_panel_close() {
    if (s_state == P_CLOSED || s_state == P_CLOSING) return;
    uint32_t now = millis();
    if (s_state == P_OPENING) {
        float t = (now - s_anim_t0) / (float)OPEN_MS; if (t > 1) t = 1;
        float shown = ease_out(t);
        float u = cbrtf(1 - shown);
        s_anim_t0 = now - (uint32_t)(u * CLOSE_MS);
    } else {
        s_anim_t0 = now;
    }
    if (s_drag_arc) { s_drag_arc = 0; nvs_save_settings(g_app_settings); }
    s_press_valid = false;
    s_state = P_CLOSING;
}

void ui_top_panel_toggle() {
    if (s_state == P_OPEN || s_state == P_OPENING) ui_top_panel_close();
    else ui_top_panel_open();
}

// "Open" for input routing: everything but fully closed. A closing panel
// still swallows touches so a tap can't land on the screen it's uncovering.
bool ui_top_panel_is_open() { return s_state != P_CLOSED; }
bool ui_top_panel_animating() { return s_state == P_OPENING || s_state == P_CLOSING; }
bool ui_top_panel_covers_screen() { return s_state == P_OPEN; }
bool ui_top_panel_busy() { return s_drag_arc != 0 || s_press_valid; }
