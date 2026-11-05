// Reminders - see app_reminders.h.
#include "haptics.h"
#include "app_reminders.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "complications.h"
#include "phone_link.h"
#include "hal_ble.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2;
static const int MAX_ITEMS = 24;
struct Item { char k[40]; char x[80]; };
static Item s_items[MAX_ITEMS];
static int  s_n = 0;
static Item s_stage[MAX_ITEMS];   // pages arriving
static int  s_stage_n = 0;
// Ticked here but not yet gone from the phone's list (it was away): sent
// again whenever a list comes, and kept out of it until the phone agrees.
static char s_pending[8][40];
static int  s_pending_n = 0;
static int      s_ticked = -1;    // ticked a moment ago: shown done, then removed
static uint32_t s_ticked_ms = 0;

static void save() {
    Preferences p;
    if (!p.begin("todos", false)) return;
    p.putUChar("n", (uint8_t)s_n);
    p.putBytes("l", s_items, sizeof(Item) * s_n);
    p.end();
    comp_set_todos(s_n, s_n ? s_items[0].x : "");
}

void reminders_init() {
    Preferences p;
    if (!p.begin("todos", true)) return;
    s_n = p.getUChar("n", 0);
    if (s_n > MAX_ITEMS || (s_n && p.getBytes("l", s_items, sizeof(Item) * s_n) != sizeof(Item) * s_n)) s_n = 0;
    p.end();
}

int reminders_count() { return s_n; }
const char *reminders_first() { return s_n ? s_items[0].x : ""; }

void reminders_on_link(JsonDocument &doc) {
    if ((doc["r"] | 0) != 0) s_stage_n = 0;
    for (JsonVariant v : doc["l"].as<JsonArray>()) {
        if (s_stage_n >= MAX_ITEMS) break;
        Item &it = s_stage[s_stage_n++];
        strlcpy(it.k, v["k"] | "", sizeof(it.k));
        strlcpy(it.x, v["x"] | "", sizeof(it.x));
        ui_utf8_trim(it.x);
        ble_fold_utf8(it.x);
    }
    if ((doc["done"] | 0) != 0) {
        for (int p = 0; p < s_pending_n; p++) {
            bool still = false;
            for (int i = 0; i < s_stage_n; i++) {
                if (strcmp(s_stage[i].k, s_pending[p])) continue;
                still = true;
                memmove(&s_stage[i], &s_stage[i + 1], sizeof(Item) * (s_stage_n - i - 1));
                s_stage_n--;
                break;
            }
            if (still) {
                JsonDocument d;
                d["e"] = "todo"; d["k"] = s_pending[p]; d["d"] = 1;
                phone_link_send(d);
            } else {
                memmove(s_pending[p], s_pending[p + 1], sizeof(s_pending[0]) * (s_pending_n - p - 1));
                s_pending_n--;
                p--;
            }
        }
        memcpy(s_items, s_stage, sizeof(Item) * s_stage_n);
        s_n = s_stage_n;
        s_ticked = -1;
        save();
    }
}

static void remove_at(int i) {
    if (i < 0 || i >= s_n) return;
    memmove(&s_items[i], &s_items[i + 1], sizeof(Item) * (s_n - i - 1));
    s_n--;
    save();
}

// ---- the app ----------------------------------------------------------------------------------
static UiScroll s_scroll;
static const int ROW = 84, TOP = 92;

static void rem_create() { ui_scroll_reset(&s_scroll, s_n * ROW - (LCD_HEIGHT - TOP - 60), 0); }

static void rem_tick() {
    if (s_ticked >= 0 && millis() - s_ticked_ms > 700) {
        remove_at(s_ticked);
        s_ticked = -1;
        ui_scroll_set_max(&s_scroll, s_n * ROW - (LCD_HEIGHT - TOP - 60));
    }
}

static void rem_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_scroll_tick(&s_scroll);
    g->fillScreen(COLOR_BG);
    if (s_n == 0) {
        g->fillCircle(CX, 190, 44, ui_dim(COLOR_GOOD, 0.3f));
        g->drawLine(CX - 18, 192, CX - 4, 206, COLOR_GOOD);
        g->drawLine(CX - 4, 206, CX + 22, 176, COLOR_GOOD);
        ui_text_center(CX, 270, COLOR_TEXT, "All done", 2);
        ui_text_center(CX, 304, COLOR_TEXT_DIM, "To-dos from your voice memos show up here", 1);
        g->fillRect(0, 0, LCD_WIDTH, 84, COLOR_BG);
        return;
    }
    int off = ui_scroll_offset(&s_scroll);
    for (int i = 0; i < s_n; i++) {
        int y = TOP + i * ROW - off;
        if (y > LCD_HEIGHT || y + ROW < 0) continue;
        bool done = i == s_ticked;
        g->fillRoundRect(CX - 180, y + 6, 360, ROW - 12, 24, 0x18E3);
        // the circle to tick
        g->drawCircle(CX - 146, y + ROW / 2, 16, done ? COLOR_GOOD : COLOR_ACCENT3);
        g->drawCircle(CX - 146, y + ROW / 2, 15, done ? COLOR_GOOD : COLOR_ACCENT3);
        if (done) {
            g->fillCircle(CX - 146, y + ROW / 2, 14, COLOR_GOOD);
            g->drawLine(CX - 154, y + ROW / 2, CX - 148, y + ROW / 2 + 6, COLOR_BG);
            g->drawLine(CX - 148, y + ROW / 2 + 6, CX - 137, y + ROW / 2 - 6, COLOR_BG);
        }
        // two lines of text at most
        char a[80], b[80];
        strlcpy(a, s_items[i].x, sizeof(a));
        b[0] = 0;
        if (ui_text_width(a, 2) > 280) {
            int len = strlen(a), cut = len;
            for (int j = len - 1; j > 0; j--) {
                if (a[j] != ' ') continue;
                a[j] = 0;
                if (ui_text_width(a, 2) <= 280) { cut = j; break; }
                a[j] = ' ';
            }
            strlcpy(a, s_items[i].x, sizeof(a));
            if (cut < len) { a[cut] = 0; strlcpy(b, s_items[i].x + cut + 1, sizeof(b)); }
            else ui_fit(a, 2, 280);
            ui_fit(b, 2, 280);
        }
        uint16_t c = done ? COLOR_TEXT_DIM : COLOR_TEXT;
        if (b[0]) {
            ui_print(CX - 116, y + 18, 2, c, a);
            ui_print(CX - 116, y + 44, 2, c, b);
        } else {
            ui_print(CX - 116, y + ROW / 2 - 10, 2, c, a);
        }
    }
    g->fillRect(0, 0, LCD_WIDTH, 84, COLOR_BG);
}

static void rem_touch(int x, int y, bool pressed) {
    if (!ui_scroll_touch(&s_scroll, y, pressed)) return;
    if (y < TOP || s_ticked >= 0) return;
    int i = (y - TOP + ui_scroll_offset(&s_scroll)) / ROW;
    if (i < 0 || i >= s_n || x > CX + 190) return;
    JsonDocument d;
    d["e"] = "todo";
    d["k"] = s_items[i].k;
    d["d"] = 1;
    phone_link_send(d);   // the phone ticks it off too
    if (s_pending_n < 8) strlcpy(s_pending[s_pending_n++], s_items[i].k, sizeof(s_pending[0]));
    s_ticked = i;
    s_ticked_ms = millis();
    haptic(HAP_SUCCESS);
}

static void rem_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen reminders_screen = {
    "Reminders", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    rem_create, rem_draw, rem_touch, rem_tick, nullptr, rem_gesture,
    250,
};
