/*
 * aod.cpp
 * Always-on display - see aod.h.
 */
#include "aod.h"
#include "config.h"
#include "board_pins.h"
#include "app_settings_state.h"
#include "app_notifications.h"
#include "hal_display.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "phone_images.h"
#include "ui.h"
#include "ui_font.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#define AOD_BRIGHTNESS   10      // panel 0..255; the clock is the only thing lit
#define AOD_MIN_BATTERY  20

static bool s_on = false;
static int s_last_minute = -1;
static uint32_t s_since_ms = 0;      // notifications after this show up
static uint32_t s_last_check_ms = 0;
static uint32_t s_newest_ms = 0;     // newest notification seen
static uint32_t s_drawn_ms = 0;

static bool allowed() {
    if (!g_app_settings.aod_on) return false;
    if (power_is_charging()) return true;
    int pct = power_get_battery_percent();
    return pct < 0 || pct >= AOD_MIN_BATTERY;
}

bool aod_active() { return s_on; }

bool aod_sending() { return s_on && millis() - s_drawn_ms < 300; }

static void draw(Arduino_GFX *g) {
    WatchTime t = rtc_now();
    // Burn-in: walk the whole layout round a 5x5 grid of offsets, one
    // step per minute.
    int step = t.minute % 25;
    int ox = (step % 5 - 2) * 4, oy = (step / 5 - 2) * 4;
    int cx = LCD_WIDTH / 2 + ox, cy = LCD_HEIGHT / 2 + oy;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
    ui_print(cx - ui_text_width(buf, 6) / 2, cy - 40, 6, COLOR_TEXT, buf);

    static const char *DAYS[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    snprintf(buf, sizeof(buf), "%s %d", DAYS[(t.weekday % 7 + 7) % 7], t.day);
    ui_print(cx - ui_text_width(buf, 2) / 2, cy + 22, 2, COLOR_TEXT_DIM, buf);

    // What arrived while asleep: the newest app's icon, or a dot.
    NotifHistoryItem it;
    if (notifications_get_history(0, it) && (int32_t)(it.received_ms - s_since_ms) > 0) {
        const uint16_t *icon = phone_icon(it.icon);
        if (icon) phone_image_draw_round(g, icon, PHONE_ICON_SIZE, PHONE_ICON_SIZE, cx, cy + 74, 12, 20);
        else g->fillCircle(cx, cy + 74, 5, COLOR_ACCENT);
    }

    int pct = power_get_battery_percent();
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", pct);
        ui_print(cx - ui_text_width(buf, 1) / 2, cy + 104, 1, power_is_charging() ? COLOR_GOOD : COLOR_TEXT_DIM, buf);
    }
}

static void redraw() {
    WatchTime t = rtc_now();
    s_last_minute = t.minute;
    ui_render_offscreen(draw);
    s_drawn_ms = millis();
}

void aod_sleep_display() {
    if (!allowed()) {
        s_on = false;
        display_sleep();
        return;
    }
    s_on = true;
    s_since_ms = millis();
    s_last_check_ms = millis();
    NotifHistoryItem it;
    s_newest_ms = notifications_get_history(0, it) ? it.received_ms : 0;
    display_set_brightness(AOD_BRIGHTNESS);
    redraw();
    DEBUG_PRINTF("[aod] on\n");
}

void aod_wake() {
    if (!s_on) return;
    s_on = false;
    display_set_brightness(g_app_settings.brightness);
}

void aod_update() {
    if (!s_on) return;
    uint32_t now = millis();
    if (now - s_last_check_ms < 1000) return;
    s_last_check_ms = now;
    // A notification arrived: show it now, not at the next minute.
    NotifHistoryItem it;
    uint32_t newest = notifications_get_history(0, it) ? it.received_ms : 0;
    bool fresh = newest != s_newest_ms;
    s_newest_ms = newest;
    if (rtc_now().minute == s_last_minute && !fresh) return;
    if (!allowed()) {
        // Setting turned off from the phone, or the battery ran low.
        s_on = false;
        display_sleep();
        DEBUG_PRINTF("[aod] off (battery or setting)\n");
        return;
    }
    redraw();
}
