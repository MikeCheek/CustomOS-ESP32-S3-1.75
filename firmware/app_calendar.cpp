#include "app_calendar.h"
#include "dnd.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "hal_rtc.h"
#include "hal_sd.h"
#include "hal_ble.h"
#include "hal_sleep.h"
#include "hal_audio.h"
#include "app_settings_state.h"
#include "phone_link.h"
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// =========================================================================
//  Agenda storage
// =========================================================================

#define CAL_FILE   "/calendar.bin"
#define CAL_MAGIC  0x324C4143u   // "CAL2"

static CalEvent *s_ev = nullptr;        // live agenda (PSRAM)
static CalEvent *s_stage = nullptr;     // being received from the phone
static int  s_count = 0, s_stage_count = 0;
static int  s_lead_min = 10;            // reminder lead, from the app; 0 = off
static int  s_stage_lead = 10;
static uint32_t s_rev = 1;              // bumps when the agenda changes
static bool s_loaded = false;
static bool s_staging = false;          // an "r" started a set that hasn't been committed

static bool alloc() {
    if (!s_ev) s_ev = (CalEvent *)heap_caps_calloc(CAL_MAX_EVENTS, sizeof(CalEvent), MALLOC_CAP_SPIRAM);
    if (!s_stage) s_stage = (CalEvent *)heap_caps_calloc(CAL_MAX_EVENTS, sizeof(CalEvent), MALLOC_CAP_SPIRAM);
    return s_ev && s_stage;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

uint32_t calendar_now() {
    WatchTime t = rtc_now();
    if (t.year < 2024) return 0;
    return (uint32_t)days_from_civil(t.year, t.month, t.day) * 86400u + t.hour * 3600u + t.minute * 60u + t.second;
}

static uint16_t rgb_to_565(uint32_t rgb) {
    if (!rgb) return COLOR_ACCENT;
    return COLOR565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static void sort_events(CalEvent *ev, int n) {
    qsort(ev, n, sizeof(CalEvent), [](const void *a, const void *b) {
        const CalEvent *x = (const CalEvent *)a, *y = (const CalEvent *)b;
        if (x->all_day != y->all_day && x->start / 86400 == y->start / 86400) return x->all_day ? -1 : 1;
        return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
    });
}

static void save_sd() {
    if (!sd_is_mounted() || !s_ev) return;
    SD.remove(CAL_FILE);
    File f = SD.open(CAL_FILE, FILE_WRITE);
    if (!f) return;
    uint32_t hdr[4] = { CAL_MAGIC, (uint32_t)sizeof(CalEvent), (uint32_t)s_count, (uint32_t)s_lead_min };
    f.write((const uint8_t *)hdr, sizeof(hdr));
    f.write((const uint8_t *)s_ev, sizeof(CalEvent) * s_count);
    f.close();
}

static void load_sd() {
    s_loaded = true;
    if (!sd_is_mounted() || !alloc()) return;
    File f = SD.open(CAL_FILE, FILE_READ);
    if (!f) return;
    uint32_t hdr[4];
    if (f.read((uint8_t *)hdr, sizeof(hdr)) == sizeof(hdr) && hdr[0] == CAL_MAGIC &&
        hdr[1] == sizeof(CalEvent) && hdr[2] <= CAL_MAX_EVENTS) {
        int n = (int)hdr[2];
        if (f.read((uint8_t *)s_ev, sizeof(CalEvent) * n) == (int)(sizeof(CalEvent) * n)) {
            for (int i = 0; i < n; i++) {   // never trust what's on the card
                CalEvent &e = s_ev[i];
                e.title[sizeof(e.title) - 1] = 0;
                e.location[sizeof(e.location) - 1] = 0;
                e.all_day = e.all_day != 0;
                if (e.end < e.start) e.end = e.start;
            }
            s_count = n;
            s_lead_min = hdr[3] <= 1440 ? (int)hdr[3] : 10;
            s_rev++;
        }
    }
    f.close();
    DEBUG_PRINTF("[cal] %d events loaded from SD\n", s_count);
}

static void copy_txt(char *dst, size_t n, const char *src) {
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
    ble_fold_utf8(dst);
    ui_utf8_trim(dst);
}

void calendar_on_link(JsonDocument &doc) {
    if (!alloc()) return;
    if (doc["r"] | 0) {
        s_stage_count = 0;
        s_stage_lead = doc["rm"] | 10;
        s_staging = true;
    }
    if (!s_staging) return;   // a stray batch (the watch restarted mid-transfer)
    JsonArray arr = doc["ev"].as<JsonArray>();
    for (JsonObject o : arr) {
        if (s_stage_count >= CAL_MAX_EVENTS) break;
        CalEvent &e = s_stage[s_stage_count++];
        memset(&e, 0, sizeof(e));
        e.id = o["id"] | 0u;
        e.start = o["s"] | 0u;
        e.end = o["e"] | 0u;
        if (e.end < e.start) e.end = e.start;
        e.all_day = (o["ad"] | 0) != 0;
        e.color = rgb_to_565(o["c"] | 0u);
        copy_txt(e.title, sizeof(e.title), o["ti"] | "");
        copy_txt(e.location, sizeof(e.location), o["lo"] | "");
        if (!e.title[0]) snprintf(e.title, sizeof(e.title), "(No title)");
    }
    if (doc["done"] | 0) {
        s_staging = false;
        // A lost batch would leave holes: keep the old agenda instead.
        int expect = doc["n"] | -1;
        if (expect >= 0 && expect != s_stage_count && !(expect > CAL_MAX_EVENTS && s_stage_count == CAL_MAX_EVENTS)) {
            DEBUG_PRINTF("[cal] incomplete agenda (%d of %d) ignored\n", s_stage_count, expect);
            return;
        }
        memcpy(s_ev, s_stage, sizeof(CalEvent) * s_stage_count);
        s_count = s_stage_count;
        s_lead_min = s_stage_lead;
        sort_events(s_ev, s_count);
        s_rev++;
        s_loaded = true;
        save_sd();
        DEBUG_PRINTF("[cal] agenda: %d events, reminders %d min before\n", s_count, s_lead_min);
    }
}

int calendar_count() { return s_count; }

bool calendar_get(int i, CalEvent &out) {
    if (!s_ev || i < 0 || i >= s_count) return false;
    out = s_ev[i];
    return true;
}

bool calendar_next(CalEvent &out) {
    uint32_t now = calendar_now();
    if (!s_ev || !now) return false;
    for (int i = 0; i < s_count; i++)
        if (!s_ev[i].all_day && s_ev[i].end > now) { out = s_ev[i]; return true; }
    for (int i = 0; i < s_count; i++)
        if (s_ev[i].all_day && s_ev[i].end > now) { out = s_ev[i]; return true; }
    return false;
}

static const char *const WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

// "Today" / "Tomorrow" / "Wed 9" for the day containing local epoch t.
static void day_label(uint32_t t, char *out, int n) {
    uint32_t now = calendar_now();
    int32_t d = (int32_t)(t / 86400), today = (int32_t)(now / 86400);
    if (d <= today) snprintf(out, n, "Today");
    else if (d == today + 1) snprintf(out, n, "Tomorrow");
    else {
        // civil date back from the day number, for the day of month
        int32_t z = d + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
        uint32_t doe = (uint32_t)(z - era * 146097);
        uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        uint32_t mp = (5 * doy + 2) / 153;
        uint32_t day = doy - (153 * mp + 2) / 5 + 1;
        snprintf(out, n, "%s %lu", WD[(d + 4) % 7], (unsigned long)day);
    }
}

static void hhmm(uint32_t t, char *out, int n) {
    snprintf(out, n, "%02lu:%02lu", (unsigned long)(t / 3600 % 24), (unsigned long)(t / 60 % 60));
}

void calendar_format_when(const CalEvent &e, char *out, int n) {
    char day[16], a[8];
    uint32_t now = calendar_now();
    day_label(e.start < now ? now : e.start, day, sizeof(day));
    if (e.all_day) { snprintf(out, n, "%s, all day", day); return; }
    hhmm(e.start, a, sizeof(a));
    if (e.start <= now && now < e.end) {
        char b[8];
        hhmm(e.end, b, sizeof(b));
        snprintf(out, n, "Now, until %s", b);
    } else if (!strcmp(day, "Today")) {
        snprintf(out, n, "%s", a);
    } else {
        snprintf(out, n, "%s %s", day, a);
    }
}

// =========================================================================
//  Reminders
// =========================================================================

static uint32_t s_reminded[16];
static int s_reminded_n = 0;
static CalEvent s_remind;               // shown by the reminder screen
static uint32_t s_remind_ms = 0;
static uint32_t s_last_check_ms = 0;

static uint32_t key_of(const CalEvent &e) { return e.id * 2654435761u ^ e.start; }

static bool was_reminded(uint32_t k) {
    for (int i = 0; i < s_reminded_n; i++) if (s_reminded[i] == k) return true;
    return false;
}

void calendar_update() {
    uint32_t ms = millis();
    if (ms - s_last_check_ms < 5000) return;
    s_last_check_ms = ms;
    if (!s_loaded) load_sd();
    if (!s_ev || s_count == 0 || s_lead_min <= 0) return;
    uint32_t now = calendar_now();
    if (!now) return;
    for (int i = 0; i < s_count; i++) {
        const CalEvent &e = s_ev[i];
        if (e.all_day) continue;
        // Due: within the lead time, and not more than 2 minutes late.
        if (now + (uint32_t)s_lead_min * 60 < e.start || now > e.start + 120) continue;
        uint32_t k = key_of(e);
        if (was_reminded(k)) continue;
        // Never on top of a ringing call: the next check after the call
        // shows it (it stays due for 2 minutes past the start).
        if (phone_link_call().state != CALL_NONE) break;
        if (s_reminded_n < 16) {
            s_reminded[s_reminded_n++] = k;
        } else {   // forget the oldest
            memmove(s_reminded, s_reminded + 1, sizeof(uint32_t) * 15);
            s_reminded[15] = k;
        }
        if (dnd_active()) continue;
        s_remind = e;
        s_remind_ms = ms;
        sleep_register_activity();
        if (!ui_screen_in_stack(&calendar_reminder_screen)) ui_push(&calendar_reminder_screen);
#if FEATURE_AUDIO
        if (g_app_settings.sfx_enabled) { audio_play_sfx(1046, 90); audio_play_sfx(1318, 140); }
#endif
        DEBUG_PRINTF("[cal] reminder: %s\n", e.title);
        break;   // one at a time
    }
}

// =========================================================================
//  Agenda screen
// =========================================================================

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int ROW_W = 350, ROW_X = (LCD_WIDTH - ROW_W) / 2, ROW_H = 86, HDR_H = 40, GAP = 8;
static const int TOP = 92;

struct Item { int16_t y; int8_t is_header; int8_t idx; char label[16]; };
#define MAX_ITEMS (CAL_MAX_EVENTS * 2)
static Item *s_items = nullptr;   // PSRAM
static int s_item_count = 0, s_content_h = 0;
static uint32_t s_built_rev = 0;
static UiScroll s_scroll;
static bool s_back = false, s_prev = false;
static int s_press = -1, s_open = -1;

static void build() {
    s_built_rev = s_rev;
    s_item_count = 0;
    if (!s_items) s_items = (Item *)heap_caps_calloc(MAX_ITEMS, sizeof(Item), MALLOC_CAP_SPIRAM);
    if (!s_items || !s_ev) { s_content_h = LCD_HEIGHT; return; }
    int y = TOP;
    char last[16] = "";
    uint32_t now = calendar_now();
    for (int i = 0; i < s_count && s_item_count < MAX_ITEMS - 1; i++) {
        const CalEvent &e = s_ev[i];
        if (now && e.end <= now) continue;   // over
        char day[16];
        day_label(e.start < now ? now : e.start, day, sizeof(day));
        if (strcmp(day, last)) {
            Item &h = s_items[s_item_count++];
            h.y = y; h.is_header = 1; h.idx = -1;
            snprintf(h.label, sizeof(h.label), "%s", day);
            snprintf(last, sizeof(last), "%s", day);
            y += HDR_H;
        }
        Item &it = s_items[s_item_count++];
        it.y = y; it.is_header = 0; it.idx = i; it.label[0] = 0;
        y += ROW_H + GAP;
    }
    s_content_h = y + 110;
}

static void cal_create() {
    s_back = s_prev = false;
    s_press = s_open = -1;
    if (!s_loaded) load_sd();
    build();
    ui_scroll_reset(&s_scroll, s_content_h - LCD_HEIGHT, 0);
}

static void cal_tick() {
    ui_scroll_tick(&s_scroll);
    if (s_built_rev != s_rev) {
        s_open = -1;      // indexes changed under us
        s_press = -1;
        build();
        ui_scroll_set_max(&s_scroll, s_content_h - LCD_HEIGHT);
    }
}

static void draw_detail(Arduino_GFX *g, const CalEvent &e) {
    g->fillCircle(CX, 96, 12, e.color);
    char w[48];
    calendar_format_when(e, w, sizeof(w));
    if (!e.all_day && !(e.start <= calendar_now() && calendar_now() < e.end)) {
        char b[8];
        hhmm(e.end, b, sizeof(b));
        strncat(w, " - ", sizeof(w) - strlen(w) - 1);
        strncat(w, b, sizeof(w) - strlen(w) - 1);
    }
    ui_text_center(CX, 132, COLOR_ACCENT, w, 2);
    // title, up to 3 lines
    const char *p = e.title;
    int y = 182;
    for (int line = 0; line < 3 && *p; line++) {
        char buf[48];
        int take = (int)strlen(p);
        if (take > 47) take = 47;
        memcpy(buf, p, take); buf[take] = 0;
        while (take > 1 && ui_text_width(buf, 3) > 330) {
            int sp = take - 1;
            while (sp > 0 && buf[sp] != ' ') sp--;
            take = sp > 0 ? sp : take - 1;
            buf[take] = 0;
        }
        ui_utf8_trim(buf);
        take = (int)strlen(buf);
        if (take == 0) break;
        ui_text_center(CX, y, COLOR_TEXT, buf, 3);
        y += 40;
        p += take;
        while (*p == ' ') p++;
    }
    if (e.location[0]) {
        char loc[48];
        snprintf(loc, sizeof(loc), "%s", e.location);
        ui_fit(loc, 2, 320);
        ui_text_center(CX, y + 16, COLOR_TEXT_DIM, loc, 2);
    }
    ui_text_center(CX, LCD_HEIGHT - 70, COLOR_TEXT_DIM, "tap to close", 1);
}

static void cal_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_open >= 0 && s_open < s_count) { draw_detail(g, s_ev[s_open]); return; }

    if (s_item_count == 0) {
        // calendar glyph
        g->fillRoundRect(CX - 30, CY - 86, 60, 56, 10, COLOR_PANEL);
        g->fillRoundRect(CX - 30, CY - 86, 60, 18, 8, COLOR_BAD);
        g->fillRect(CX - 30, CY - 76, 60, 8, COLOR_BAD);
        ui_text_center(CX, CY - 46, COLOR_TEXT_DIM, "31", 2);
        ui_text_center(CX, CY + 4, COLOR_TEXT, s_count ? "Nothing else today" : "No upcoming events", 2);
        ui_text_center(CX, CY + 34, COLOR_TEXT_DIM,
                       ble_is_connected() ? "Your phone's calendar shows up here" : "Connect the companion app", 1);
        return;
    }

    int off = -ui_scroll_offset(&s_scroll);
    uint32_t now = calendar_now();
    for (int k = 0; k < s_item_count; k++) {
        const Item &it = s_items[k];
        int y = it.y + off;
        if (y > LCD_HEIGHT) break;
        if (it.is_header) {
            if (y + HDR_H < 0) continue;
            ui_print(ROW_X + 8, y + 12, 1, COLOR_TEXT_DIM, it.label);
            continue;
        }
        if (y + ROW_H < 0) continue;
        const CalEvent &e = s_ev[it.idx];
        bool live = !e.all_day && e.start <= now && now < e.end;
        g->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 20, s_press == k ? ui_dim(COLOR_TEXT, 0.22f) : COLOR_PANEL);
        g->fillRoundRect(ROW_X + 12, y + 14, 6, ROW_H - 28, 3, e.color);
        char t[48], when[24];
        if (e.all_day) snprintf(when, sizeof(when), "All day");
        else {
            char a[8], b[8];
            hhmm(e.start, a, sizeof(a));
            hhmm(e.end, b, sizeof(b));
            snprintf(when, sizeof(when), live ? "Now - %s" : "%s - %s", live ? b : a, b);
        }
        ui_print(ROW_X + 32, y + 12, 1, live ? COLOR_GOOD : COLOR_ACCENT, when);
        snprintf(t, sizeof(t), "%s", e.title);
        ui_fit(t, 2, ROW_W - 50);
        ui_print(ROW_X + 32, y + 34, 2, COLOR_TEXT, t);
        if (e.location[0]) {
            snprintf(t, sizeof(t), "%s", e.location);
            ui_fit(t, 1, ROW_W - 50);
            ui_print(ROW_X + 32, y + 62, 1, COLOR_TEXT_DIM, t);
        }
    }
}

static void cal_touch(int x, int y, bool pressed) {
    if (pressed && !s_scroll.dragging) s_back = (x < 54 && y < 54);
    if (s_back) { if (!pressed) { s_back = false; if (s_open >= 0) s_open = -1; else ui_pop_screen(); } return; }
    bool edge = pressed && !s_prev;
    s_prev = pressed;
    if (s_open >= 0) { if (edge) s_open = -1; return; }
    int cy = y + ui_scroll_offset(&s_scroll);
    if (edge) {
        s_press = -1;
        for (int k = 0; k < s_item_count; k++)
            if (!s_items[k].is_header && cy >= s_items[k].y && cy < s_items[k].y + ROW_H &&
                x >= ROW_X && x <= ROW_X + ROW_W) s_press = k;
    }
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    if (s_scroll.moved) s_press = -1;
    if (!pressed) {
        int p = s_press;
        s_press = -1;
        if (tap && p >= 0) s_open = s_items[p].idx;
    }
}

static void cal_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT) { if (s_open >= 0) s_open = -1; else ui_pop_screen(); }
}

Screen calendar_screen = {
    "Calendar", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    cal_create, cal_draw, cal_touch, cal_tick, nullptr, cal_gesture,
    300, false, false, false, true,
};

// =========================================================================
//  Reminder
// =========================================================================


static void rem_tick() {
    if (millis() - s_remind_ms > 90000) ui_pop_screen();   // closes itself after a while
}

static void rem_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    const CalEvent &e = s_remind;
    uint32_t now = calendar_now();
    float pulse = 0.5f + 0.5f * sinf(millis() / 300.0f);
    g->fillCircle(CX, 112, 34 + (int)(4 * pulse), ui_dim(e.color, 0.35f));
    g->fillCircle(CX, 112, 30, e.color);
    // little bell
    g->fillRoundRect(CX - 11, 98, 22, 22, 9, COLOR_TEXT);
    g->fillRect(CX - 14, 116, 28, 4, COLOR_TEXT);
    g->fillCircle(CX, 124, 4, COLOR_TEXT);

    char when[32];
    if (e.start > now) {
        uint32_t m = (e.start - now + 59) / 60;
        snprintf(when, sizeof(when), m <= 1 ? "In 1 minute" : "In %lu minutes", (unsigned long)m);
    } else {
        snprintf(when, sizeof(when), "Starting now");
    }
    ui_text_center(CX, 176, COLOR_ACCENT, when, 2);
    char t[48];
    snprintf(t, sizeof(t), "%s", e.title);
    ui_fit(t, 3, 340);
    ui_text_center(CX, 222, COLOR_TEXT, t, 3);
    char a[8], b[8], range[24];
    hhmm(e.start, a, sizeof(a));
    hhmm(e.end, b, sizeof(b));
    snprintf(range, sizeof(range), "%s - %s", a, b);
    ui_text_center(CX, 264, COLOR_TEXT_DIM, range, 2);
    if (e.location[0]) {
        snprintf(t, sizeof(t), "%s", e.location);
        ui_fit(t, 1, 300);
        ui_text_center(CX, 298, COLOR_TEXT_DIM, t, 1);
    }
    g->fillRoundRect(CX - 70, CY + 112, 140, 48, 24, COLOR_PANEL);
    ui_text_center(CX, CY + 136, COLOR_TEXT, "OK", 2);
}

static void rem_touch(int x, int y, bool pressed) {
    (void)x; (void)y;
    static bool down = false;
    if (pressed) { down = true; return; }
    if (down) { down = false; ui_pop_screen(); }
}

Screen calendar_reminder_screen = {
    "Reminder", GESTURE_MODE_NONE,
    UI_FRAME_MS_DEFAULT,
    nullptr, rem_draw, rem_touch, rem_tick, nullptr, nullptr,
    0, false, false, false, true,
};
