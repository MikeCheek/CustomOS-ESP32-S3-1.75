// The morning briefing - see app_briefing.h.
#include "app_briefing.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "app_settings_state.h"
#include "app_face_nav.h"
#include "app_calendar.h"
#include "app_life.h"
#include "app_sleep.h"
#include "app_weather.h"
#include "complications.h"
#include "clock_fmt.h"
#include "phone_link.h"
#include "hal_power.h"
#include "hal_rtc.h"
#include "hal_sleep.h"
#include "hal_touch.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2;
static int s_shown_day = -1;   // yyyymmdd it was shown (kept in NVS: once a morning, even across a restart)

static uint32_t today(const WatchTime &t) { return (uint32_t)t.year * 10000 + t.month * 100 + t.day; }

void briefing_update() {
    static uint32_t last = 0;
    uint32_t ms = millis();
    if (ms - last < 1000) return;
    last = ms;
    if (!g_app_settings.morning_brief || sleep_is_asleep()) return;
    WatchTime t = rtc_now();
    if (!t.valid || t.year < 2024 || t.hour < 5 || t.hour >= 11) return;
    if (s_shown_day < 0) {
        Preferences p;
        s_shown_day = p.begin("brief", true) ? (int)p.getUInt("day", 0) : 0;
        p.end();
    }
    if ((uint32_t)s_shown_day == today(t)) return;
    if (!face_nav_is_face(ui_active_screen()) || ui_top_panel_is_open()) return;   // not over an app
    if (sleep_ms_since_activity() > 5000) return;   // only right after a wake-up or a touch
    s_shown_day = (int)today(t);
    Preferences p;
    if (p.begin("brief", false)) { p.putUInt("day", s_shown_day); p.end(); }
    ui_push(&briefing_screen);
}

// one line: a coloured dot, a label, the value
static void line(int y, uint16_t c, const char *label, const char *value) {
    Arduino_GFX *g = ui_gfx();
    g->fillCircle(CX - 150, y, 6, c);
    ui_print(CX - 134, y - 6, 1, COLOR_TEXT_DIM, label);
    char v[64];
    strlcpy(v, value, sizeof(v));
    while (ui_text_width(v, 2) > 220 && strlen(v) > 4) {   // fit, with "..."
        size_t n = strlen(v);
        v[n - 4] = '.'; v[n - 3] = '.'; v[n - 2] = '.'; v[n - 1] = 0;
    }
    ui_print(CX - 70, y - 13, 2, COLOR_TEXT, v);
}

static void brief_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    WatchTime t = rtc_now();
    char b[64];
    if (g_app_settings.user_name[0]) snprintf(b, sizeof(b), "Good morning, %s", g_app_settings.user_name);
    else snprintf(b, sizeof(b), "Good morning");
    ui_text_center(CX, 70, COLOR_TEXT, b, 2);
    static const char *const DAYS[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MON[12] = {"January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December"};
    if (t.valid) {
        snprintf(b, sizeof(b), "%s %d %s", DAYS[t.weekday % 7], t.day, MON[(t.month + 11) % 12]);
        ui_text_center(CX, 100, COLOR_TEXT_DIM, b, 1);
    }

    int y = 150;
    const int STEP = 52;
    int temp;
    char cond[32];
    if (comp_weather_now(temp, cond, sizeof(cond))) {
        snprintf(b, sizeof(b), "%d\xC2\xB0 %s", temp, cond);
        line(y, COLOR_ACCENT2, "Weather", b);
        y += STEP;
    }
    int sl = sleeptrack_last_night_min();
    if (sl > 0) {
        snprintf(b, sizeof(b), "%dh %02dm asleep", sl / 60, sl % 60);
        line(y, COLOR_ACCENT, "Sleep", b);
        y += STEP;
    }
    CalEvent ev;
    if (calendar_next(ev)) {
        char when[32], v[64];
        calendar_format_when(ev, when, sizeof(when));
        snprintf(v, sizeof(v), "%s %s", when, ev.title);
        line(y, ev.color ? ev.color : COLOR_ACCENT2, "Up next", v);
        y += STEP;
    }
    char nx[48];
    if (life_next(nx, sizeof(nx))) {
        line(y, 0x2CCA, "Life", nx);
        y += STEP;
    }
    int p = power_get_battery_percent(), pp = phone_link_battery();
    if (p >= 0) {
        if (pp >= 0) snprintf(b, sizeof(b), "%d%%  -  phone %d%%", p, pp);
        else snprintf(b, sizeof(b), "%d%%", p);
        line(y, p <= 20 ? COLOR_BAD : COLOR_GOOD, "Battery", b);
        y += STEP;
    }
    ui_text_center(CX, LCD_HEIGHT - 48, COLOR_TEXT_DIM, "Tap to start the day", 1);
}

// The same card as one paragraph, for the phone to say on the watch's
// speaker (it only does when "Read the morning briefing aloud" is on there).
static void brief_create() {
    if (!phone_link_active()) return;
    char s[400], b[96];
    int n = snprintf(s, sizeof(s), g_app_settings.user_name[0] ? "Good morning, %s." : "Good morning.",
                     g_app_settings.user_name);
    int temp;
    char cond[32];
    if (comp_weather_now(temp, cond, sizeof(cond)) && n < (int)sizeof(s))
        n += snprintf(s + n, sizeof(s) - n, " It's %d degrees and %s.", temp, cond);
    int sl = sleeptrack_last_night_min();
    if (sl > 0 && n < (int)sizeof(s))
        n += snprintf(s + n, sizeof(s) - n, " You slept %d hours %d minutes.", sl / 60, sl % 60);
    CalEvent ev;
    if (calendar_next(ev) && n < (int)sizeof(s)) {
        calendar_format_when(ev, b, sizeof(b));
        n += snprintf(s + n, sizeof(s) - n, " Up next: %s, %s.", ev.title, b);
    }
    char nx[48];
    if (life_next(nx, sizeof(nx)) && n < (int)sizeof(s)) n += snprintf(s + n, sizeof(s) - n, " Then: %s.", nx);
    phone_link_say("briefing", 0, s);
}

static void brief_touch(int, int, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (!down) return;
    down = false;
    touch_cancel_swipes();
    ui_pop_screen();
}

static void brief_gesture(Gesture) { ui_pop_screen(); }

Screen briefing_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    brief_create, brief_draw, brief_touch, nullptr, nullptr, brief_gesture,
    1000,
};
