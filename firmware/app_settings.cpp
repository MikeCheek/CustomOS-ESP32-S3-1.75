#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "app_settings_state.h"
#include "app_calibration.h"
#include "app_wifi_setup.h"
#include "app_usb.h"
#include "hal_usb.h"
#include "hal_display.h"
#include "hal_audio.h"
#include "hal_touch.h"
#include "hal_nvs.h"
#include "hal_wifi.h"
#include "hal_gps.h"
#include "hal_ble.h"
#include "hal_ntp.h"
#include "hal_espnow.h"
#include "icons.h"
#include "battery_mode.h"
#include "complications.h"
#include "hal_power.h"
#include <Arduino_GFX_Library.h>
#include <string.h>

// =========================================================================
//  Settings - restructured as a scrollable list of rows (Wear OS
//  ToggleChip pattern: label + one-line description, with a switch on
//  the right for simple on/off options), or a chevron for rows that
//  open a dedicated sub-screen for anything that doesn't fit a simple
//  on/off (sliders, battery mode presets). Previously this was a grid
//  of sliders/buttons scattered around the screen; this is a genuine
//  restructuring of how settings are organized, not just a visual
//  pass over the old layout.
// =========================================================================

// Battery modes live in battery_mode.h/.cpp (shared with the top panel).

// Blends a color toward black by `fade` (0 = no change, 1 = fully
// black) - RGB565 channel-wise interpolation, approximating Wear OS's
// ScalingLazyColumn dimming as rows move away from the screen's
// vertical center.
static uint16_t fade_color(uint16_t c, float fade) {
    if (fade <= 0.0f) return c;
    if (fade > 1.0f) fade = 1.0f;
    int r = (c >> 11) & 0x1F, gr = (c >> 5) & 0x3F, b = c & 0x1F;
    r = (int)(r * (1.0f - fade));
    gr = (int)(gr * (1.0f - fade));
    b = (int)(b * (1.0f - fade));
    return (uint16_t)((r << 11) | (gr << 5) | b);
}
static float row_fade(int center_y) {
    int dy = center_y - LCD_HEIGHT / 2;
    if (dy < 0) dy = -dy;
    float f = (float)dy / (float)(LCD_HEIGHT / 2) * 0.55f;
    if (f > 0.55f) f = 0.55f;
    return f;
}

// ---- Side switch (Wear OS Switch selection control) -----------------------
static void draw_switch(Arduino_GFX *g, int x, int y_center, bool on, float fade) {
    int track_w = 50, track_h = 26;
    int ty = y_center - track_h / 2;
    uint16_t track_c = fade_color(on ? COLOR_GOOD : COLOR_PANEL, fade);
    uint16_t border_c = fade_color(on ? COLOR_GOOD : COLOR_TEXT_DIM, fade);
    // Border via an inset double-fill (border-color fill at full size,
    // then a 2px-smaller track-color fill on top) rather than
    // drawRoundRect - this project's own notes elsewhere document
    // drawFastHLine/drawFastVLine as broken on this CO5300 driver, and
    // Adafruit_GFX-derived drawRoundRect's outline typically implements
    // its straight edges through exactly those calls. fillRect/
    // fillRoundRect are the confirmed-safe primitives used everywhere
    // else in this project, so those are what this sticks to.
    g->fillRoundRect(x, ty, track_w, track_h, track_h / 2, border_c);
    g->fillRoundRect(x + 2, ty + 2, track_w - 4, track_h - 4, (track_h - 4) / 2, track_c);
    int thumb_r = 10;
    int thumb_x = on ? x + track_w - thumb_r - 3 : x + thumb_r + 3;
    g->fillCircle(thumb_x, y_center, thumb_r, fade_color(COLOR_TEXT, fade));
}

// ---- Row list ---------------------------------------------------------

// Small named getter/setter pair per boolean setting rather than raw
// field pointers - lets rows with side effects (auto-rotate's
// calibration check, wifi/ble/gps needing enable()/disable() calls,
// not just a flipped bool) share the exact same row-list machinery as
// plain fields, instead of the list needing two different kinds of
// entries.
static bool get_touch_feedback() { return g_app_settings.touch_feedback; }
static void tap_touch_feedback() { g_app_settings.touch_feedback = !g_app_settings.touch_feedback; nvs_save_settings(g_app_settings); }

static bool get_dnd_bed() { return g_app_settings.dnd_bedtime; }
static void tap_dnd_bed() { g_app_settings.dnd_bedtime = !g_app_settings.dnd_bedtime; nvs_save_settings(g_app_settings); }
static bool get_wake_touch() { return g_app_settings.wake_on_touch; }
static void tap_wake_touch() { g_app_settings.wake_on_touch = !g_app_settings.wake_on_touch; nvs_save_settings(g_app_settings); }

static bool get_wake_motion() { return g_app_settings.wake_on_motion; }
static void tap_wake_motion() { g_app_settings.wake_on_motion = !g_app_settings.wake_on_motion; nvs_save_settings(g_app_settings); }

static bool get_haptic() { return g_app_settings.haptic_on_touch; }
static void tap_haptic() { g_app_settings.haptic_on_touch = !g_app_settings.haptic_on_touch; nvs_save_settings(g_app_settings); }

static bool get_aod() { return g_app_settings.aod_on; }
static void tap_aod() { g_app_settings.aod_on = !g_app_settings.aod_on; nvs_save_settings(g_app_settings); }
static bool get_awake_charge() { return g_app_settings.stay_awake_charging; }
static void tap_awake_charge() { g_app_settings.stay_awake_charging = !g_app_settings.stay_awake_charging; nvs_save_settings(g_app_settings); }

static bool get_sfx() { return g_app_settings.sfx_enabled; }
static void tap_sfx() { g_app_settings.sfx_enabled = !g_app_settings.sfx_enabled; nvs_save_settings(g_app_settings); }

static bool get_music() { return g_app_settings.music_enabled; }
static void tap_music() { g_app_settings.music_enabled = !g_app_settings.music_enabled; nvs_save_settings(g_app_settings); }

static bool get_wifi() { return g_app_settings.wifi_on; }
static void tap_wifi() {
    g_app_settings.wifi_on = !g_app_settings.wifi_on;
    if (g_app_settings.wifi_on) { wifi_enable(); ntp_sync(); } else wifi_disable();
    nvs_save_settings(g_app_settings);
}

static bool get_ble() { return ble_is_enabled(); }
static void tap_ble() {
    g_app_settings.ble_on = !g_app_settings.ble_on;
    if (g_app_settings.ble_on) {
        if (!ble_enable()) { g_app_settings.ble_on = false; ui_show_toast("Not enough memory for Bluetooth", 2500); }
    } else ble_disable();
    nvs_save_settings(g_app_settings);
}

static bool get_espnow() { return espnow_is_enabled(); }
static void tap_espnow() {
    g_app_settings.espnow_on = !g_app_settings.espnow_on;
    if (g_app_settings.espnow_on) espnow_enable(); else espnow_disable();
    nvs_save_settings(g_app_settings);
}

static bool get_gps() { return gps_is_enabled(); }
static void tap_gps() {
    g_app_settings.gps_on = !g_app_settings.gps_on;
    if (g_app_settings.gps_on) gps_enable(); else gps_disable();
    nvs_save_settings(g_app_settings);
}

static bool get_smooth_fonts() { return g_app_settings.smooth_fonts; }
static void tap_smooth_fonts() {
    g_app_settings.smooth_fonts = !g_app_settings.smooth_fonts;
    ui_fonts_set_smooth(g_app_settings.smooth_fonts);
    nvs_save_settings(g_app_settings);
}

static bool get_auto_rotate() { return g_app_settings.auto_rotate; }
static void tap_auto_rotate() {
    if (!g_app_settings.auto_rotate && !g_app_settings.tilt_calibrated) {
        ui_show_confirm("Calibrate tilt sensor?",
                        "Auto-rotate needs the tilt sensor calibrated first.",
                        "Calibrate", "Cancel",
                        [](bool confirmed) {
                            if (confirmed) ui_push(&calibration_screen);
                        });
        return;
    }
    g_app_settings.auto_rotate = !g_app_settings.auto_rotate;
    if (!g_app_settings.auto_rotate) {
        // Without this, disabling left s_rot_angle stuck at whatever
        // it last was - the render loop only checks "is the angle
        // non-zero", not whether auto-rotate is even still on.
        ui_reset_auto_rotate_state();
    }
    nvs_save_settings(g_app_settings);
}

// Sub-screens (defined further down this file) - forward-declared here
// so the row table can point to them.
extern Screen settings_brightness_screen;
extern Screen settings_volume_screen;
extern Screen settings_mic_screen;
extern Screen settings_comp_screen;
extern Screen settings_battery_screen;
extern Screen diagnostics_screen;
extern Screen fwup_screen;

enum RowKind { ROW_TOGGLE, ROW_SUBSCREEN };
struct SettingsRow {
    const char *label;
    const char *desc;
    RowKind kind;
    bool (*get_value)();   // ROW_TOGGLE only
    void (*on_tap)();      // ROW_TOGGLE only - performs the toggle + any side effects + save
    Screen *subscreen;     // ROW_SUBSCREEN only
    const uint8_t *icon;   // 32x32 1-bit bitmap from icons.h, or nullptr for a label-only row
    // Optional: false = the feature isn't there on this board (row greyed,
    // tap explains instead of toggling). nullptr = always available.
    bool (*available)() = nullptr;
    const char *unavailable_desc = nullptr;
};

static bool gps_row_available() { return gps_presence() != GPS_ABSENT; }

// Deliberately NOT const - read every visible row, every frame, while
// this screen is scrolling. See icons.h's matching note for the full
// reasoning: flash-mapped const data briefly becomes inaccessible
// whenever anything writes to flash (including the BLE stack's own
// internal NVS writes on its own task, independent of any code here),
// and this is the single most exposed read in the project to that
// window - a real, confirmed crash, not a theoretical one.
static SettingsRow S_ROWS[] = {
    // Ordered most- to least-used: display & sound first, then the
    // radios the phone/clock depend on, power, everyday behaviour, sound
    // details, and finally hardware extras and debug aids.
    { "Brightness", "Screen brightness level", ROW_SUBSCREEN, nullptr, nullptr, &settings_brightness_screen, ICON_BRIGHTNESS },
    { "Volume", "Speaker volume level", ROW_SUBSCREEN, nullptr, nullptr, &settings_volume_screen, ICON_VOLUME },
    { "Microphone", "Recording sensitivity", ROW_SUBSCREEN, nullptr, nullptr, &settings_mic_screen, ICON_SOUND },
    { "Watchface slots", "Steps, events, weather... on the face", ROW_SUBSCREEN, nullptr, nullptr, &settings_comp_screen, ICON_BRIGHTNESS },
    { "Bluetooth", "Radio for phone sync", ROW_TOGGLE, get_ble, tap_ble, nullptr, ICON_BLUETOOTH },
    { "WiFi", "Wireless radio for NTP sync", ROW_TOGGLE, get_wifi, tap_wifi, nullptr, ICON_WIFI },
    { "WiFi Setup", "Scan, pick a network, enter password", ROW_SUBSCREEN, nullptr, nullptr, &wifi_setup_screen, ICON_WIFI },
    { "Battery Mode", "", ROW_SUBSCREEN, nullptr, nullptr, &settings_battery_screen, ICON_BATTERY },
    // desc is filled in live with the selected mode (settings_draw)
    { "USB Mode", "", ROW_SUBSCREEN, nullptr, nullptr, &usb_mode_screen, ICON_POWER },
    { "Always-on display", "Dim clock while asleep (not below 20%)", ROW_TOGGLE, get_aod, tap_aod, nullptr, ICON_BRIGHTNESS },
    { "Wake on Motion", "Wake when you raise your wrist", ROW_TOGGLE, get_wake_motion, tap_wake_motion, nullptr, ICON_ROTATE },
    { "Wake on Touch", "Wake the display on tap", ROW_TOGGLE, get_wake_touch, tap_wake_touch, nullptr, ICON_TOUCH },
    { "Bedtime DND", "Silent at night (hours set in the app)", ROW_TOGGLE, get_dnd_bed, tap_dnd_bed, nullptr, ICON_SOUND },
    { "Auto-Rotate", "Rotate display with your wrist", ROW_TOGGLE, get_auto_rotate, tap_auto_rotate, nullptr, ICON_ROTATE },
    { "Smooth fonts", "Sharper text with accented letters", ROW_TOGGLE, get_smooth_fonts, tap_smooth_fonts, nullptr, nullptr },
    { "Sound Effects", "Play SFX in games and apps", ROW_TOGGLE, get_sfx, tap_sfx, nullptr, ICON_SOUND },
    { "Music", "Background music in games", ROW_TOGGLE, get_music, tap_music, nullptr, ICON_SOUND },
    { "Haptic on Touch", "Vibrate briefly on every tap", ROW_TOGGLE, get_haptic, tap_haptic, nullptr, ICON_VIBRATION },
    { "Awake on Charge", "Skip auto-sleep while charging", ROW_TOGGLE, get_awake_charge, tap_awake_charge, nullptr, ICON_POWER },
    { "GPS", "Location module (draws more power)", ROW_TOGGLE, get_gps, tap_gps, nullptr, ICON_GPS,
      gps_row_available, "No GPS module on this board" },
    { "Touch Feedback", "Show a dot under your finger", ROW_TOGGLE, get_touch_feedback, tap_touch_feedback, nullptr, ICON_TOUCH },
    { "Joystick Pairing", "ESP-NOW radio for a physical controller", ROW_TOGGLE, get_espnow, tap_espnow, nullptr, ICON_WIFI },
    { "Software update", "Check GitHub for new firmware", ROW_SUBSCREEN, nullptr, nullptr, &fwup_screen, nullptr },
    { "Diagnostics", "Version, memory, last crash report", ROW_SUBSCREEN, nullptr, nullptr, &diagnostics_screen, nullptr },
};
static const int S_ROW_COUNT = sizeof(S_ROWS) / sizeof(S_ROWS[0]);
static const int S_ROW_H = 78;
static const int S_ROW_GAP = 8;
static const int S_ROW_TOP = 80; // >=73.7 needed for a 340px-wide row to clear the round display's safe area at this height - computed from the display's actual radius, not guessed
static const int S_ROW_W = 340; // safe at the topmost row's y - see the round-safety check this was derived from
static const int S_ROW_PITCH = S_ROW_H + S_ROW_GAP;
// Bottom padding lets the last row scroll up clear of the round screen's
// bottom curve instead of stopping half-hidden in it (MUSE pads its
// settings list by 110px for the same reason).
static const int S_LIST_BOTTOM_PAD = 110;
static const int S_CONTENT_BOTTOM = S_ROW_TOP + S_ROW_COUNT * S_ROW_PITCH - S_ROW_GAP + S_LIST_BOTTOM_PAD;

// Kinetic scroller (ui.h): drag tracks the finger, flicks coast with
// friction, the ends rubber-band, and it settles on a whole row.
static UiScroll s_scroll;
static bool s_back_armed = false;

static void settings_create() {
    ui_scroll_reset(&s_scroll, S_CONTENT_BOTTOM - LCD_HEIGHT, S_ROW_PITCH);
    s_back_armed = false;
}

static void draw_settings_row(Arduino_GFX *g, int y, const SettingsRow &row) {
    int x = (LCD_WIDTH - S_ROW_W) / 2;
    float fade = row_fade(y + S_ROW_H / 2);
    bool unavailable = row.available && !row.available();
    if (unavailable) fade = fade + (1.0f - fade) * 0.55f;   // greyed out
    // Same inset-double-fill border technique as draw_switch() above -
    // border-color fill at full size, panel-color fill 2px smaller on
    // top, no drawRoundRect (see that function's comment for why).
    g->fillRoundRect(x, y, S_ROW_W, S_ROW_H, 18, fade_color(COLOR_TEXT_DIM, fade));
    g->fillRoundRect(x + 2, y + 2, S_ROW_W - 4, S_ROW_H - 4, 16, fade_color(COLOR_PANEL, fade));

    int pad = 18;
    int text_x = x + pad;
    // Icon rendering removed here entirely (not just skipped while
    // dragging) after a device crash traced to a watchdog timeout on
    // this exact screen, right after a previous fix that specifically
    // targeted a different crash type also on this screen.
    // drawBitmap() is the single newest, least-tested code path in
    // this project - nothing else here used it before this feature -
    // and rather than keep narrowing around an increasingly specific
    // guess with real device crashes at stake, this removes the most
    // concrete remaining suspect outright. See the chat for the full
    // reasoning and crash dumps this is responding to.
    g->setTextSize(2);
    g->setTextColor(fade_color(COLOR_TEXT, fade));
    ui_print(text_x, y + 14, 2, fade_color(COLOR_TEXT, fade), row.label);

    g->setTextSize(1);
    g->setTextColor(fade_color(COLOR_TEXT_DIM, fade));
    ui_print(text_x, y + 44, 1, fade_color(COLOR_TEXT_DIM, fade), unavailable && row.unavailable_desc ? row.unavailable_desc : row.desc);

    if (row.kind == ROW_TOGGLE) {
        bool on = row.get_value ? row.get_value() : false;
        draw_switch(g, x + S_ROW_W - pad - 50, y + S_ROW_H / 2, on, fade);
    } else {
        // Chevron for "opens a sub-screen" rows - one filled triangle
        // instead of 4 separate line calls for the same arrow shape.
        int cx = x + S_ROW_W - pad - 12, cy = y + S_ROW_H / 2;
        uint16_t c = fade_color(COLOR_TEXT_DIM, fade);
        g->fillTriangle(cx - 4, cy - 9, cx - 4, cy + 9, cx + 8, cy, c);
    }
}

static void settings_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    // Rows are drawn in full while scrolling - the old "simplified" rows
    // (label only while dragging) were a workaround for every frame
    // blocking on a full-screen flush, which the background panel
    // sender (hal_display) has removed.
    int off = -ui_scroll_offset(&s_scroll);
    for (int i = 0; i < S_ROW_COUNT; i++) {
        if (S_ROWS[i].subscreen == &settings_battery_screen) {
            S_ROWS[i].desc = battery_mode_adjusted() ? "Adjusted by hand" : battery_profile(battery_mode_current()).name;
        }
        if (S_ROWS[i].subscreen == &usb_mode_screen) {
            S_ROWS[i].desc = usb_mode_restart_pending() ? "Restart pending - tap to finish"
                                                        : usb_mode_name(usb_mode_saved());
        }
        int y = S_ROW_TOP + i * S_ROW_PITCH + off;
        if (y + S_ROW_H < 0 || y > LCD_HEIGHT) continue; // off-screen, skip drawing
        draw_settings_row(g, y, S_ROWS[i]);
    }
}

static void settings_touch(int x, int y, bool pressed) {
    // Back button (fixed screen position, never scrolled) - fires on
    // release, so it can't double up with a scroll that starts there.
    if (pressed && !s_scroll.dragging) s_back_armed = (x < 54 && y < 54);
    if (s_back_armed) {
        if (!pressed) { s_back_armed = false; ui_pop_screen(); }
        return;
    }

    if (!ui_scroll_touch(&s_scroll, y, pressed)) return;

    // A tap (released without scrolling, list wasn't coasting): which row?
    int cy = y + ui_scroll_offset(&s_scroll);
    int row_x = (LCD_WIDTH - S_ROW_W) / 2;
    for (int i = 0; i < S_ROW_COUNT; i++) {
        int ry = S_ROW_TOP + i * S_ROW_PITCH;
        if (cy >= ry && cy <= ry + S_ROW_H && x >= row_x && x <= row_x + S_ROW_W) {
            const SettingsRow &row = S_ROWS[i];
            if (row.available && !row.available()) { ui_show_toast(row.unavailable_desc ? row.unavailable_desc : "Not available", 1800); break; }
            if (row.kind == ROW_TOGGLE && row.on_tap) row.on_tap();
            else if (row.kind == ROW_SUBSCREEN && row.subscreen) ui_push(row.subscreen);
            break;
        }
    }
}

static void settings_tick() {
    ui_scroll_tick(&s_scroll);
}

static void settings_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen settings_screen = {
    "Settings", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH, // 60 fps target while scrolling/coasting...
    settings_create, settings_draw, settings_touch, settings_tick, nullptr, settings_gesture,
    250,                // ...4 fps once idle (nothing moves; only the toggles' state can change)
};

// =========================================================================
//  Brightness sub-screen
// =========================================================================

static const int SUB_SLIDER_LEFT = 80;
static const int SUB_SLIDER_RIGHT = LCD_WIDTH - 80;
static const int SUB_SLIDER_W = SUB_SLIDER_RIGHT - SUB_SLIDER_LEFT;
static const int SUB_SLIDER_Y = LCD_HEIGHT / 2 - 5;
static const int SUB_SLIDER_H = 10;
static bool s_bright_dragging = false;

static void brightness_create() { s_bright_dragging = false; }
static void brightness_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_draw_centered_text(SUB_SLIDER_Y - 40, COLOR_TEXT_DIM, "Brightness", 2);
    g->fillRoundRect(SUB_SLIDER_LEFT, SUB_SLIDER_Y, SUB_SLIDER_W, SUB_SLIDER_H, 5, COLOR_PANEL);
    int fill = (int)g_app_settings.brightness * SUB_SLIDER_W / 255;
    g->fillRoundRect(SUB_SLIDER_LEFT, SUB_SLIDER_Y, fill, SUB_SLIDER_H, 5, COLOR_ACCENT);
    g->fillCircle(SUB_SLIDER_LEFT + fill, SUB_SLIDER_Y + SUB_SLIDER_H / 2, 14, COLOR_ACCENT);
    g->fillCircle(SUB_SLIDER_LEFT + fill, SUB_SLIDER_Y + SUB_SLIDER_H / 2, 9, COLOR_BG);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)g_app_settings.brightness * 100 / 255);
    ui_draw_centered_text(SUB_SLIDER_Y + 30, COLOR_TEXT_DIM, buf, 2);
}
static void brightness_apply(int x) {
    int v = (x - SUB_SLIDER_LEFT) * 255 / SUB_SLIDER_W;
    if (v < 0) v = 0; if (v > 255) v = 255;
    g_app_settings.brightness = v;
    display_set_brightness(v);
}
static void brightness_touch(int x, int y, bool pressed) {
    if (!pressed) {
        if (s_bright_dragging) nvs_save_settings(g_app_settings);
        s_bright_dragging = false;
        return;
    }
    if (x < 54 && y < 54) { ui_pop_screen(); return; }
    if (s_bright_dragging || (y >= SUB_SLIDER_Y - 30 && y <= SUB_SLIDER_Y + SUB_SLIDER_H + 30)) {
        s_bright_dragging = true;
        brightness_apply(x);
    }
}
// Nothing to poll: brightness_touch() already gets every touch event while the
// finger is down. (This used to call touch_read() itself - a second
// reader of the touch controller, which could swallow the finger-up
// before the main loop saw it, leaving the slider stuck "dragging" and
// the setting unsaved.)
static void brightness_tick() {}
static void brightness_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}
Screen settings_brightness_screen = {
    "Brightness", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    brightness_create, brightness_draw, brightness_touch, brightness_tick, nullptr, brightness_gesture
};

// =========================================================================
//  Volume sub-screen
// =========================================================================

static bool s_vol_dragging = false;
static void volume_create() { s_vol_dragging = false; }
static void volume_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_draw_centered_text(SUB_SLIDER_Y - 40, COLOR_TEXT_DIM, "Volume", 2);
    g->fillRoundRect(SUB_SLIDER_LEFT, SUB_SLIDER_Y, SUB_SLIDER_W, SUB_SLIDER_H, 5, COLOR_PANEL);
    int fill = (int)g_app_settings.volume * SUB_SLIDER_W / 100;
    g->fillRoundRect(SUB_SLIDER_LEFT, SUB_SLIDER_Y, fill, SUB_SLIDER_H, 5, COLOR_ACCENT2);
    g->fillCircle(SUB_SLIDER_LEFT + fill, SUB_SLIDER_Y + SUB_SLIDER_H / 2, 14, COLOR_ACCENT2);
    g->fillCircle(SUB_SLIDER_LEFT + fill, SUB_SLIDER_Y + SUB_SLIDER_H / 2, 9, COLOR_BG);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)g_app_settings.volume);
    ui_draw_centered_text(SUB_SLIDER_Y + 30, COLOR_TEXT_DIM, buf, 2);
}
static void volume_apply(int x) {
    int v = (x - SUB_SLIDER_LEFT) * 100 / SUB_SLIDER_W;
    if (v < 0) v = 0; if (v > 100) v = 100;
    g_app_settings.volume = v;
    audio_set_volume(v);
}
static void volume_touch(int x, int y, bool pressed) {
    if (!pressed) {
        if (s_vol_dragging) nvs_save_settings(g_app_settings);
        s_vol_dragging = false;
        return;
    }
    if (x < 54 && y < 54) { ui_pop_screen(); return; }
    if (s_vol_dragging || (y >= SUB_SLIDER_Y - 30 && y <= SUB_SLIDER_Y + SUB_SLIDER_H + 30)) {
        s_vol_dragging = true;
        volume_apply(x);
    }
}
// Nothing to poll: volume_touch() already gets every touch event while the
// finger is down. (This used to call touch_read() itself - a second
// reader of the touch controller, which could swallow the finger-up
// before the main loop saw it, leaving the slider stuck "dragging" and
// the setting unsaved.)
static void volume_tick() {}
static void volume_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}
Screen settings_volume_screen = {
    "Volume", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    volume_create, volume_draw, volume_touch, volume_tick, nullptr, volume_gesture
};

// =========================================================================
//  Microphone sensitivity sub-screen
//
//  Five steps (Low .. Max) and a live level meter, so you can talk and see
//  how loud the mics pick you up before recording.
// =========================================================================

static const char *MIC_NAMES[5] = { "Low", "Normal", "High", "Very high", "Max" };
static const int MIC_SEG_Y = LCD_HEIGHT / 2 - 30;
static const int MIC_SEG_H = 56;
static const int MIC_SEG_GAP = 6;
static int s_mic_meter = 0;      // smoothed 0..100
static uint32_t s_mic_peak_ms = 0;
static int s_mic_peak = 0;

static int mic_seg_x(int i) {
    int w = (SUB_SLIDER_W + 40 - 4 * MIC_SEG_GAP) / 5;
    return SUB_SLIDER_LEFT - 20 + i * (w + MIC_SEG_GAP);
}
static int mic_seg_w() { return (SUB_SLIDER_W + 40 - 4 * MIC_SEG_GAP) / 5; }

static void mic_create() { s_mic_meter = 0; s_mic_peak = 0; }
static void mic_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_draw_centered_text(MIC_SEG_Y - 70, COLOR_TEXT_DIM, "Microphone", 2);
    int cur = g_app_settings.mic_sens;
    for (int i = 0; i < 5; i++) {
        bool on = i + 1 <= cur;
        int h = 18 + i * 9;   // rising bars, like a signal icon
        int x = mic_seg_x(i);
        g->fillRoundRect(x, MIC_SEG_Y + MIC_SEG_H - h, mic_seg_w(), h, 6, on ? COLOR_ACCENT : COLOR_PANEL);
    }
    ui_draw_centered_text(MIC_SEG_Y + MIC_SEG_H + 14, COLOR_TEXT, MIC_NAMES[cur - 1], 2);

    // live level
    int my = MIC_SEG_Y + MIC_SEG_H + 70;
    g->fillRoundRect(SUB_SLIDER_LEFT, my, SUB_SLIDER_W, 12, 6, COLOR_PANEL);
    int fill = s_mic_meter * SUB_SLIDER_W / 100;
    uint16_t c = s_mic_meter > 85 ? COLOR_BAD : s_mic_meter > 60 ? COLOR_WARN : COLOR_GOOD;
    if (fill > 0) g->fillRoundRect(SUB_SLIDER_LEFT, my, fill < 12 ? 12 : fill, 12, 6, c);
    int px = SUB_SLIDER_LEFT + s_mic_peak * SUB_SLIDER_W / 100;
    if (s_mic_peak > 0) g->fillRect(px - 1, my - 4, 3, 20, COLOR_TEXT);
    ui_draw_centered_text(my + 24, COLOR_TEXT_DIM,
                          audio_mic_ok() ? "Speak to test" : "Microphone not responding", 1);
}
static void mic_set(int level) {
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    if (level == g_app_settings.mic_sens) return;
    g_app_settings.mic_sens = (uint8_t)level;
    audio_set_mic_sensitivity((uint8_t)level);
    nvs_save_settings(g_app_settings);
}
static void mic_touch(int x, int y, bool pressed) {
    if (!pressed) return;
    if (x < 54 && y < 54) { ui_pop_screen(); return; }
    if (y < MIC_SEG_Y - 20 || y > MIC_SEG_Y + MIC_SEG_H + 20) return;
    for (int i = 0; i < 5; i++) {
        if (x >= mic_seg_x(i) - MIC_SEG_GAP / 2 && x < mic_seg_x(i) + mic_seg_w() + MIC_SEG_GAP / 2) { mic_set(i + 1); return; }
    }
}
static void mic_tick() {
    int lvl = audio_mic_level_percent();
    s_mic_meter = lvl > s_mic_meter ? lvl : (s_mic_meter * 3 + lvl) / 4;   // fast attack, slow release
    if (lvl >= s_mic_peak || millis() - s_mic_peak_ms > 1500) { s_mic_peak = lvl; s_mic_peak_ms = millis(); }
}
static void mic_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}
Screen settings_mic_screen = {
    "Microphone", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    mic_create, mic_draw, mic_touch, mic_tick, nullptr, mic_gesture
};

// =========================================================================
//  Watchface slots sub-screen: a live preview of the 3 complications, and
//  one button per slot - tap to cycle through what it shows.
// =========================================================================

static const int COMP_BTN_Y = 262, COMP_BTN_H = 50, COMP_BTN_GAP = 10;

static void comp_draw_screen() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    comp_draw_slots(g, 160, 38);
    for (int i = 0; i < COMP_SLOTS; i++) {
        int y = COMP_BTN_Y + i * (COMP_BTN_H + COMP_BTN_GAP);
        g->fillRoundRect(LCD_WIDTH / 2 - 130, y, 260, COMP_BTN_H, COMP_BTN_H / 2, COLOR_PANEL);
        char buf[40];
        snprintf(buf, sizeof(buf), "%s: %s", i == 0 ? "Left" : i == 1 ? "Middle" : "Right", comp_label(comp_slot(i)));
        ui_text_center(LCD_WIDTH / 2, y + COMP_BTN_H / 2, COLOR_TEXT, buf, 2);
    }
}
static void comp_touch(int x, int y, bool pressed) {
    static bool s_prev = false;
    bool tap = pressed && !s_prev;
    s_prev = pressed;
    if (!tap) return;
    if (x < 54 && y < 54) { ui_pop_screen(); return; }
    for (int i = 0; i < COMP_SLOTS; i++) {
        int by = COMP_BTN_Y + i * (COMP_BTN_H + COMP_BTN_GAP);
        if (y >= by && y < by + COMP_BTN_H && x > LCD_WIDTH / 2 - 140 && x < LCD_WIDTH / 2 + 140) {
            comp_set_slot(i, (CompId)((comp_slot(i) + 1) % COMP_COUNT));
            return;
        }
    }
}
static void comp_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}
Screen settings_comp_screen = {
    "Watchface slots", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, comp_draw_screen, comp_touch, nullptr, nullptr, comp_gesture
};

// =========================================================================
//  Battery mode sub-screen
//
//  Battery level up top (edge arc + big %), then one card per mode with
//  its glyph, what it does, and which one is active ("ADJUSTED" when the
//  settings have since been changed by hand - tapping it re-applies).
// =========================================================================

static const int BM_TOP = 150, BM_CARD_H = 84, BM_GAP = 10, BM_W = 340;
static const int BM_X = (LCD_WIDTH - BM_W) / 2;
static const int BM_CONTENT = BM_TOP + BATT_MODE_COUNT * (BM_CARD_H + BM_GAP) - BM_GAP + 110;
static UiScroll s_bm_scroll;
static bool     s_bm_back = false;
static uint32_t s_bm_cache_ms = 0;
static int      s_bm_pct = -1;
static bool     s_bm_chg = false;

static void battery_create() {
    ui_scroll_reset(&s_bm_scroll, BM_CONTENT - LCD_HEIGHT, 0);
    s_bm_back = false;
    s_bm_cache_ms = 0;
}

static void battery_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_scroll_tick(&s_bm_scroll);
    uint32_t now = millis();
    if (!s_bm_cache_ms || now - s_bm_cache_ms > 1000) {   // PMU reads are I2C
        s_bm_cache_ms = now;
        s_bm_pct = power_get_battery_percent();
        s_bm_chg = power_is_charging();
    }
    BatteryMode cur = battery_mode_current();
    const BatteryProfile &cp = battery_profile(cur);
    int off = -ui_scroll_offset(&s_bm_scroll);

    // Battery level: arc along the top edge + big percentage.
    uint16_t lc = s_bm_chg ? COLOR_GOOD : (s_bm_pct >= 0 && s_bm_pct <= 20 ? COLOR_BAD : cp.color);
    int pct = s_bm_pct < 0 ? 0 : s_bm_pct;
    ui_edge_ring(300, 120, ui_dim(lc, 0.22f), 8, 4);
    if (pct > 0) ui_edge_ring(300, 120 * pct / 100.0f, lc, 8, 4);
    char buf[24];
    if (s_bm_pct >= 0) snprintf(buf, sizeof(buf), "%d%%", s_bm_pct); else snprintf(buf, sizeof(buf), "--%%");
    ui_text_center(LCD_WIDTH / 2, 98 + off, COLOR_TEXT, buf, 4);
    snprintf(buf, sizeof(buf), "%s%s", s_bm_chg ? "Charging - " : "", cp.name);
    ui_text_center(LCD_WIDTH / 2, 130 + off, lc, buf, 2);

    for (int i = 0; i < BATT_MODE_COUNT; i++) {
        BatteryMode m = (BatteryMode)i;
        const BatteryProfile &p = battery_profile(m);
        int y = BM_TOP + i * (BM_CARD_H + BM_GAP) + off;
        if (y + BM_CARD_H < 0 || y > LCD_HEIGHT) continue;
        bool sel = m == cur;
        g->fillRoundRect(BM_X, y, BM_W, BM_CARD_H, 22, sel ? p.color : COLOR_PANEL);
        g->fillRoundRect(BM_X + 2, y + 2, BM_W - 4, BM_CARD_H - 4, 20, sel ? ui_dim(p.color, 0.22f) : COLOR_PANEL);
        int icx = BM_X + 44, icy = y + BM_CARD_H / 2;
        g->fillCircle(icx, icy, 28, sel ? p.color : COLOR_BG);
        uint16_t ink = sel ? ((p.color == COLOR_WARN || p.color == COLOR_GOOD) ? COLOR_BG : COLOR_TEXT) : p.color;
        battery_mode_draw_icon(g, m, icx, icy, ink, sel ? p.color : COLOR_BG);
        int tx = BM_X + 86;
        g->setTextSize(2);
        g->setTextColor(COLOR_TEXT);
        ui_print(tx, y + 13, 2, COLOR_TEXT, p.name);
        g->setTextSize(1);
        g->setTextColor(sel ? COLOR_TEXT : COLOR_TEXT_DIM);
        ui_print(tx, y + 39, 1, sel ? COLOR_TEXT : COLOR_TEXT_DIM, p.line1);
        ui_print(tx, y + 53, 1, sel ? COLOR_TEXT : COLOR_TEXT_DIM, p.line2);
        if (sel) {
            bool adj = battery_mode_adjusted();
            g->setTextColor(adj ? COLOR_WARN : p.color);
            ui_print(tx, y + 67, 1, adj ? COLOR_WARN : p.color, adj ? "ADJUSTED - tap to reset" : "ACTIVE");
        }
    }
}

static void battery_touch(int x, int y, bool pressed) {
    if (pressed && !s_bm_scroll.dragging) s_bm_back = (x < 54 && y < 54);
    if (s_bm_back) {
        if (!pressed) { s_bm_back = false; ui_pop_screen(); }
        return;
    }
    if (!ui_scroll_touch(&s_bm_scroll, y, pressed)) return;
    int cy = y + ui_scroll_offset(&s_bm_scroll);
    if (x < BM_X || x > BM_X + BM_W) return;
    for (int i = 0; i < BATT_MODE_COUNT; i++) {
        int ry = BM_TOP + i * (BM_CARD_H + BM_GAP);
        if (cy < ry || cy > ry + BM_CARD_H) continue;
        BatteryMode m = (BatteryMode)i;
        if (m == battery_mode_current() && !battery_mode_adjusted()) return;
        battery_mode_apply(m);
        char msg[32];
        snprintf(msg, sizeof(msg), "%s on", battery_profile(m).name);
        ui_show_toast(msg, 1500);
        return;
    }
}

static void battery_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen settings_battery_screen = {
    "Battery Mode", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    battery_create, battery_draw, battery_touch, nullptr, nullptr, battery_gesture,
    250,
};
