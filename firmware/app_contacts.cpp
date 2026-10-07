#include "config.h"
#include "ui.h"
#include "ui_font.h"
#include "board_pins.h"
#include "hal_ble.h"
#include "hal_sd.h"
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// =========================================================================
//  Contacts screen
//
//  The companion app sends a JSON array [{"name","phone","email"}, ...]
//  over BLE. It's parsed into a small table here, kept in PSRAM and saved
//  to /contacts.json so the list survives a reboot without the phone.
// =========================================================================

#define CONTACTS_FILE "/contacts.json"
#define CONTACTS_MAX  300

struct Contact { char name[40]; char phone[28]; char email[48]; };

static Contact *s_list = nullptr;
static int      s_count = 0;
static bool     s_loaded = false;

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int ROW_W = 350, ROW_X = (LCD_WIDTH - ROW_W) / 2, ROW_H = 62, GAP = 6;
static const int LIST_TOP = 96;

static UiScroll s_scroll;
static bool s_back = false, s_prev = false;
static int  s_press = -1;
static int  s_open = -1;      // contact shown in the detail card

// ---- JSON -----------------------------------------------------------------

// Reads a JSON string starting at the opening quote into out (UTF-8
// folded to ASCII for the built-in font). Returns the char after the
// closing quote.
static const char *read_str(const char *p, const char *end, char *out, int n) {
    int o = 0;
    p++; // opening quote
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            char e = *p++;
            if (e == 'n' || e == 't' || e == 'r') c = ' ';
            else if (e == 'u') { // \uXXXX - keep ASCII, anything else -> '?'
                unsigned v = 0;
                for (int k = 0; k < 4 && p < end; k++, p++) v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                c = v < 128 ? (char)v : '?';
            } else c = e;
        }
        if (o < n - 1) out[o++] = c;
    }
    out[o] = 0;
    ble_fold_utf8(out);
    return p < end ? p + 1 : p;
}

static int parse_contacts(const char *js, int len) {
    if (!s_list) s_list = (Contact *)heap_caps_calloc(CONTACTS_MAX, sizeof(Contact), MALLOC_CAP_SPIRAM);
    if (!s_list) return 0;
    const char *p = js, *end = js + len;
    int n = 0;
    Contact cur;
    bool in_obj = false;
    while (p < end && n < CONTACTS_MAX) {
        if (*p == '{') { memset(&cur, 0, sizeof(cur)); in_obj = true; p++; continue; }
        if (*p == '}' && in_obj) {
            in_obj = false;
            if (cur.name[0] || cur.phone[0]) s_list[n++] = cur;
            p++; continue;
        }
        if (*p == '"' && in_obj) {
            char key[16];
            p = read_str(p, end, key, sizeof(key));
            while (p < end && (*p == ' ' || *p == ':')) p++;
            if (p < end && *p == '"') {
                char *dst = nullptr; int dn = 0;
                if (!strcmp(key, "name"))  { dst = cur.name;  dn = sizeof(cur.name); }
                else if (!strcmp(key, "phone")) { dst = cur.phone; dn = sizeof(cur.phone); }
                else if (!strcmp(key, "email")) { dst = cur.email; dn = sizeof(cur.email); }
                char skip[8];
                p = dst ? read_str(p, end, dst, dn) : read_str(p, end, skip, sizeof(skip));
            }
            continue;
        }
        p++;
    }
    // A-Z by name
    qsort(s_list, n, sizeof(Contact), [](const void *a, const void *b) {
        return strcasecmp(((const Contact *)a)->name, ((const Contact *)b)->name);
    });
    return n;
}

static void load_from_sd() {
    s_loaded = true;
    if (!sd_is_mounted()) return;
    File f = SD.open(CONTACTS_FILE, FILE_READ);
    if (!f) return;
    int len = f.size();
    if (len > 0 && len < 64 * 1024) {
        char *buf = (char *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (buf) {
            int got = f.read((uint8_t *)buf, len);
            s_count = parse_contacts(buf, got);
            heap_caps_free(buf);
        }
    }
    f.close();
    DEBUG_PRINTF("[contacts] %d loaded from SD\n", s_count);
}

// New list from the phone? Parse, persist, consume.
static void poll_ble() {
    const char *data = nullptr;
    int dlen = 0;
    if (!ble_get_contacts(&data, &dlen) || dlen <= 0) return;
    s_count = parse_contacts(data, dlen);
    if (sd_is_mounted()) {
        SD.remove(CONTACTS_FILE);
        File f = SD.open(CONTACTS_FILE, FILE_WRITE);
        if (f) { f.write((const uint8_t *)data, dlen); f.close(); }
    }
    ble_consume_contacts();
    s_loaded = true;
    s_open = -1;
    ui_scroll_set_max(&s_scroll, LIST_TOP + s_count * (ROW_H + GAP) + 110 - LCD_HEIGHT);
    DEBUG_PRINTF("[contacts] %d received from phone\n", s_count);
}

// ---- UI -------------------------------------------------------------------

static int content_h() { return LIST_TOP + s_count * (ROW_H + GAP) - GAP + 110; }

static void fit_px(char *s, int size, int max_px) {
    ui_utf8_trim(s);
    ui_fit(s, size, max_px);
}

static uint16_t avatar_color(const char *name) {
    static const uint16_t pal[] = {0x7B1F, 0x2E7F, 0x07F3, 0xFD20, 0xF8B2, 0x4E1F, 0x9FE0, 0xFB2C};
    unsigned h = 0;
    for (const char *c = name; *c; c++) h = h * 31 + (unsigned char)*c;
    return pal[h % 8];
}

static void contacts_create() {
    s_back = s_prev = false;
    s_press = s_open = -1;
    if (!s_loaded) load_from_sd();
    poll_ble();
    ui_scroll_reset(&s_scroll, content_h() - LCD_HEIGHT, ROW_H + GAP);
}

static void contacts_tick() {
    ui_scroll_tick(&s_scroll);
    poll_ble();
}

static void draw_detail(Arduino_GFX *g, const Contact &c) {
    g->fillCircle(CX, CY - 70, 44, avatar_color(c.name));
    char ini[5];
    ui_first_char(c.name, ini);
    ui_text_center(CX, CY - 70, COLOR_TEXT, ini, 5);
    char t[40];
    strncpy(t, c.name, sizeof(t)); t[sizeof(t) - 1] = 0; fit_px(t, 2, 320);
    ui_text_center(CX, CY + 2, COLOR_TEXT, t, 2);
    if (c.phone[0]) ui_text_center(CX, CY + 40, COLOR_ACCENT, c.phone, 2);
    if (c.email[0]) { strncpy(t, c.email, sizeof(t)); t[sizeof(t) - 1] = 0; fit_px(t, 1, 300); ui_text_center(CX, CY + 72, COLOR_TEXT_DIM, t, 1); }
    ui_text_center(CX, CY + 130, COLOR_TEXT_DIM, "tap to close", 1);
}

static void contacts_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    if (s_count == 0) {
        g->fillCircle(CX, CY - 52, 28, COLOR_PANEL);
        g->fillCircle(CX, CY - 60, 9, COLOR_TEXT_DIM);
        g->fillRoundRect(CX - 15, CY - 47, 30, 16, 8, COLOR_TEXT_DIM);
        ui_text_center(CX, CY + 4, COLOR_TEXT, "No contacts", 2);
        ui_text_center(CX, CY + 32, COLOR_TEXT_DIM, "Send them from the companion app", 1);
        return;
    }
    if (s_open >= 0 && s_open < s_count) { draw_detail(g, s_list[s_open]); return; }

    int off = -ui_scroll_offset(&s_scroll);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d contact%s", s_count, s_count == 1 ? "" : "s");
    if (LIST_TOP - 22 + off > 0) ui_text_center(CX, LIST_TOP - 18 + off, COLOR_TEXT_DIM, cnt, 1);

    char t[40], ph[30];
    for (int i = 0; i < s_count; i++) {
        int y = LIST_TOP + i * (ROW_H + GAP) + off;
        if (y + ROW_H < 0) continue;
        if (y > LCD_HEIGHT) break;
        const Contact &c = s_list[i];
        bool pressed = s_press == i;
        g->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 18, pressed ? ui_dim(COLOR_TEXT, 0.25f) : COLOR_PANEL);
        int ix = ROW_X + 32, iy = y + ROW_H / 2;
        g->fillCircle(ix, iy, 20, avatar_color(c.name));
        char ini[5];
        ui_first_char(c.name, ini);
        ui_text_center(ix, iy, COLOR_TEXT, ini, 2);
        strncpy(t, c.name[0] ? c.name : c.phone, sizeof(t)); t[sizeof(t) - 1] = 0; fit_px(t, 2, ROW_W - 64 - 20);
        strncpy(ph, c.phone, sizeof(ph)); ph[sizeof(ph) - 1] = 0; fit_px(ph, 1, ROW_W - 64 - 20);
        ui_print(ROW_X + 64, c.phone[0] ? y + 13 : y + 23, 2, COLOR_TEXT, t);
        if (c.phone[0]) ui_print(ROW_X + 64, y + 38, 1, COLOR_TEXT_DIM, ph);
    }
}

static void contacts_touch(int x, int y, bool pressed) {
    if (pressed && !s_scroll.dragging) s_back = (x < 54 && y < 54);
    if (s_back) { if (!pressed) { s_back = false; ui_pop_screen(); } return; }
    bool edge = pressed && !s_prev;
    s_prev = pressed;
    if (s_open >= 0) { if (edge) s_open = -1; return; }
    if (s_count == 0) return;
    int cy = y + ui_scroll_offset(&s_scroll);
    if (edge) {
        s_press = -1;
        if (x >= ROW_X && x <= ROW_X + ROW_W && cy >= LIST_TOP) {
            int i = (cy - LIST_TOP) / (ROW_H + GAP);
            if (i < s_count && (cy - LIST_TOP) % (ROW_H + GAP) < ROW_H) s_press = i;
        }
    }
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    if (s_scroll.moved) s_press = -1;
    if (!pressed) {
        int p = s_press;
        s_press = -1;
        if (tap && p >= 0) s_open = p;
    }
}

static void contacts_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) {
        if (s_open >= 0) s_open = -1; else ui_pop_screen();
    }
}

Screen contacts_screen = {
    "Contacts", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    contacts_create, contacts_draw, contacts_touch, contacts_tick, nullptr, contacts_gesture,
    250,
};
