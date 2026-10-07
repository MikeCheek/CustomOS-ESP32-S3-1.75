/*
 * ui_keypad.cpp
 * See ui_keypad.h. Drawing sticks to this project's safe primitives on
 * the CO5300 (fillRoundRect / fillRect / fillTriangle / text) - no
 * drawRoundRect or fast H/V lines.
 */
#include "ui_keypad.h"
#include "config.h"
#include "board_pins.h"
#include "hal_vibrate.h"
#include "ui_font.h"
#include <Arduino_GFX_Library.h>
#include <ctype.h>
#include <string.h>

// ---- Layout (466px round panel) ---------------------------------------------
// Checked against the 233px radius: the grid's outer corners sit at most
// ~195px from the centre, and the bottom row is narrower because the
// circle is - its corners are ~221px out.
#define KP_KEY_W     96
#define KP_KEY_H     50
#define KP_GAP       6
#define KP_GRID_X    (LCD_WIDTH / 2 - (3 * KP_KEY_W + 2 * KP_GAP) / 2)   // 83
#define KP_GRID_Y    140
#define KP_BOTTOM_Y  (KP_GRID_Y + 4 * (KP_KEY_H + KP_GAP))               // 364
#define KP_BOTTOM_H  44
#define KP_FIELD_W   300
#define KP_FIELD_H   44

#define TAP_MS       1000   // second tap on the same key within this = next character
#define HOLD_MS      600    // holding a letter key this long types its digit
#define REPEAT_DELAY 450    // backspace auto-repeat
#define REPEAT_MS    90
#define REVEAL_MS    1500   // hidden password: how long the last typed char stays visible

#define COLOR_KEY          COLOR_PANEL
#define COLOR_KEY_PRESSED  ((uint16_t)0x4228)
#define COLOR_KEY_DIM      ((uint16_t)0x18C3)

enum { MODE_LETTERS, MODE_DIGITS, MODE_SYMBOLS, MODE_COUNT };
enum { KEY_SHIFT = 9, KEY_SPACE = 10, KEY_BACKSPACE = 11, KEY_MODE = 12, KEY_SHOW = 13, KEY_DONE = 14, KEY_COUNT = 15 };

// What each grid key types, in tap order (nullptr = shift/backspace).
static const char *const CHARS[MODE_COUNT][12] = {
    { ".-_@1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", nullptr, " 0", nullptr },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", nullptr },
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", " ", "0", nullptr },
};
static const char *const LABELS[MODE_COUNT][12] = {
    { ".-_@", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz", "", "space", "" },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "" },
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", "space", "0", "" },
};
static const char *const UPPER_LABELS[9] = { ".-_@", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ" };
static const char *const NEXT_MODE[MODE_COUNT] = { "123", "#+=", "abc" };

static char       *s_buf = nullptr;
static size_t      s_cap = 0;
static bool        s_password = false;
static bool        s_hidden = true;
static bool        s_auto_cap = false;
static const char *s_ok_label = "OK";
static int         s_mode = MODE_LETTERS;
static bool        s_upper = false;
static int         s_pending = -1;    // key whose character can still change
static int         s_step = 0;
static uint32_t    s_tap_ms = 0;
static uint32_t    s_typed_ms = 0;

static bool       s_touching = false; // a press is in progress (on a key or not)
static int         s_down = -1;       // key under the current press
static bool        s_on_key = false;  // finger still on s_down
static bool        s_held = false;    // this press already typed its digit
static uint32_t    s_press_ms = 0;
static uint32_t    s_next_repeat = 0;

// ---- Geometry (shared by draw and touch) ----------------------------------------
static bool key_rect(int k, int *x, int *y, int *w, int *h) {
    if (k >= 0 && k < 12) {
        *x = KP_GRID_X + (k % 3) * (KP_KEY_W + KP_GAP);
        *y = KP_GRID_Y + (k / 3) * (KP_KEY_H + KP_GAP);
        *w = KP_KEY_W;
        *h = KP_KEY_H;
        return true;
    }
    // Bottom row: kept in from the sides, where the circle cuts it off.
    int n = s_password ? 3 : 2;
    int bw = s_password ? 84 : 120;
    int gap = s_password ? KP_GAP : 12;
    int x0 = LCD_WIDTH / 2 - (n * bw + (n - 1) * gap) / 2;
    int slot;
    if (k == KEY_MODE) slot = 0;
    else if (k == KEY_SHOW && s_password) slot = 1;
    else if (k == KEY_DONE) slot = n - 1;
    else return false;
    *x = x0 + slot * (bw + gap);
    *y = KP_BOTTOM_Y;
    *w = bw;
    *h = KP_BOTTOM_H;
    return true;
}

static int key_at(int x, int y) {
    for (int k = 0; k < KEY_COUNT; k++) {
        int kx, ky, kw, kh;
        if (!key_rect(k, &kx, &ky, &kw, &kh)) continue;
        // A few px of slack between keys so a tap on a gap still lands.
        if (x >= kx - KP_GAP / 2 && x < kx + kw + KP_GAP / 2 &&
            y >= ky - KP_GAP / 2 && y < ky + kh + KP_GAP / 2) return k;
    }
    return -1;
}

// ---- Editing ---------------------------------------------------------------------
static size_t buf_len() { return s_buf ? strlen(s_buf) : 0; }

static bool add_char(char c) {
    size_t n = buf_len();
    if (!s_buf || n + 1 >= s_cap) return false;
    s_buf[n] = c;
    s_buf[n + 1] = '\0';
    s_typed_ms = millis();
    return true;
}

static void delete_char() {
    size_t n = buf_len();
    if (n > 0) s_buf[n - 1] = '\0';
}

static void commit() {
    s_pending = -1;
}

// Should the next letter be upper case? pending_in_buf: the character
// still being chosen (if any) is in the buffer and must be ignored - true
// when drawing labels, false inside type_key() (which has already
// removed it before asking).
static bool want_upper(bool pending_in_buf) {
    if (s_mode != MODE_LETTERS) return false;
    if (s_upper) return true;
    if (!s_auto_cap) return false;
    size_t n = buf_len();
    if (pending_in_buf && s_pending >= 0 && n > 0) n--;
    return n == 0 || s_buf[n - 1] == ' ';
}

static void type_key(int k) {
    const char *chars = CHARS[s_mode][k];
    if (!chars) return;
    int n = (int)strlen(chars);
    if (n == 1) {
        commit();
        add_char(chars[0]);
        return;
    }
    if (k == s_pending && millis() - s_tap_ms < TAP_MS) {
        s_step = (s_step + 1) % n;
        delete_char();
    } else {
        commit();
        s_step = 0;
    }
    char c = chars[s_step];
    if (want_upper(false)) c = (char)toupper((unsigned char)c);
    if (!add_char(c)) {
        commit(); // full: nothing to replace on the next tap
        return;
    }
    s_pending = k;
    s_tap_ms = millis();
}

static char held_digit(int k) {
    if (k < 0 || k >= 12 || s_mode != MODE_LETTERS || !CHARS[MODE_LETTERS][k]) return 0;
    const char *chars = CHARS[MODE_LETTERS][k];
    char last = chars[strlen(chars) - 1];
    return isdigit((unsigned char)last) ? last : 0;
}

// Returns KP_DONE for the OK key.
static KeypadEvent activate(int k) {
    switch (k) {
        case KEY_SHIFT:
            if (s_mode == MODE_LETTERS) { commit(); s_upper = !s_upper; return KP_CHANGED; }
            type_key(k);
            return KP_CHANGED;
        case KEY_MODE:
            commit();
            s_mode = (s_mode + 1) % MODE_COUNT;
            return KP_CHANGED;
        case KEY_SHOW:
            s_hidden = !s_hidden;
            return KP_CHANGED;
        case KEY_DONE:
            commit();
            return KP_DONE;
        default:
            if (k >= 0 && k < 12) { type_key(k); return KP_CHANGED; }
            return KP_NONE;
    }
}

// ---- Public API ---------------------------------------------------------------------
void keypad_begin(char *buf, size_t cap, bool password, const char *ok_label, bool auto_cap) {
    s_buf = buf;
    s_cap = cap;
    if (s_buf && s_cap) s_buf[s_cap - 1] = '\0';
    s_password = password;
    s_hidden = true;
    s_auto_cap = auto_cap;
    s_ok_label = ok_label ? ok_label : "OK";
    s_mode = MODE_LETTERS;
    s_upper = false;
    s_pending = -1;
    s_down = -1;
    s_held = false;
    s_touching = false;
}

void keypad_ignore_current_touch() {
    s_touching = true;
    s_down = -1;
    s_held = false;
}

void keypad_tick() {
    if (s_pending >= 0 && millis() - s_tap_ms >= TAP_MS) commit();
}

KeypadEvent keypad_touch(int x, int y, bool pressed) {
    uint32_t now = millis();
    if (pressed) {
        if (!s_touching) {
            // New press
            s_touching = true;
            int k = key_at(x, y);
            if (k < 0) return KP_NONE;
            s_down = k;
            s_on_key = true;
            s_held = false;
            s_press_ms = now;
            if (k == KEY_BACKSPACE) {
                commit();
                delete_char();
                s_next_repeat = now + REPEAT_DELAY;
                vibrate_buzz();
                return KP_CHANGED;
            }
            return KP_NONE;
        }
        if (s_down < 0) return KP_NONE;
        s_on_key = key_at(x, y) == s_down;
        if (s_down == KEY_BACKSPACE && s_on_key && now >= s_next_repeat) {
            delete_char();
            s_next_repeat = now + REPEAT_MS;
            return KP_CHANGED;
        }
        char digit = held_digit(s_down);
        if (digit && s_on_key && !s_held && now - s_press_ms >= HOLD_MS) {
            s_held = true;
            commit();
            add_char(digit);
            vibrate_buzz();
            return KP_CHANGED;
        }
        return KP_NONE;
    }

    // Release
    s_touching = false;
    int k = s_down;
    bool fire = k >= 0 && k != KEY_BACKSPACE && !s_held && s_on_key;
    s_down = -1;
    s_held = false;
    if (!fire) return KP_NONE;
    vibrate_buzz();
    return activate(k);
}

// ---- Drawing ------------------------------------------------------------------------
static void text_centered(Arduino_GFX *g, int cx, int cy, const char *t, int size, uint16_t color) {
    int w = ui_text_width(t, size), h = 8 * size;
    g->setTextSize(size);
    g->setTextColor(color);
    ui_print(cx - w / 2, cy - h / 2, size, color, t);
}

static void draw_shift_glyph(Arduino_GFX *g, int cx, int cy, uint16_t c) {
    g->fillTriangle(cx, cy - 11, cx - 11, cy + 1, cx + 11, cy + 1, c);
    g->fillRect(cx - 5, cy + 1, 10, 9, c);
}

static void draw_backspace_glyph(Arduino_GFX *g, int cx, int cy, uint16_t c) {
    g->fillTriangle(cx - 16, cy, cx - 6, cy - 10, cx - 6, cy + 10, c);
    g->fillRect(cx - 6, cy - 10, 22, 21, c);
    // the "x", as two thin triangle pairs in the key colour
    uint16_t k = COLOR_KEY;
    g->fillTriangle(cx - 1, cy - 6, cx + 2, cy - 6, cx + 11, cy + 6, k);
    g->fillTriangle(cx - 1, cy - 6, cx + 8, cy + 6, cx + 11, cy + 6, k);
    g->fillTriangle(cx + 8, cy - 6, cx + 11, cy - 6, cx + 2, cy + 6, k);
    g->fillTriangle(cx + 8, cy - 6, cx - 1, cy + 6, cx + 2, cy + 6, k);
}

void keypad_draw(Arduino_GFX *g) {
    if (!g) return;
    for (int k = 0; k < KEY_COUNT; k++) {
        int x, y, w, h;
        if (!key_rect(k, &x, &y, &w, &h)) continue;
        bool pressed = (k == s_down && s_on_key);
        bool checked = (k == s_pending) || (k == KEY_SHIFT && s_mode == MODE_LETTERS && s_upper);
        uint16_t fill = COLOR_KEY;
        if (k == KEY_MODE || k == KEY_SHOW) fill = COLOR_KEY_DIM;
        if (k == KEY_DONE) fill = COLOR_GOOD;
        if (checked) fill = COLOR_ACCENT;
        if (pressed) fill = (k == KEY_DONE) ? COLOR_ACCENT2 : COLOR_KEY_PRESSED;
        g->fillRoundRect(x, y, w, h, 12, fill);

        int cx = x + w / 2, cy = y + h / 2;
        if (k == KEY_SHIFT && s_mode == MODE_LETTERS) {
            draw_shift_glyph(g, cx, cy, COLOR_TEXT);
        } else if (k == KEY_BACKSPACE) {
            draw_backspace_glyph(g, cx, cy, COLOR_TEXT);
        } else if (k == KEY_MODE) {
            text_centered(g, cx, cy, NEXT_MODE[s_mode], 2, COLOR_TEXT);
        } else if (k == KEY_SHOW) {
            text_centered(g, cx, cy, s_hidden ? "Show" : "Hide", 2, COLOR_TEXT);
        } else if (k == KEY_DONE) {
            text_centered(g, cx, cy, s_ok_label, 2, COLOR_TEXT);
        } else {
            const char *label = (s_mode == MODE_LETTERS && k < 9 && want_upper(true))
                                    ? UPPER_LABELS[k] : LABELS[s_mode][k];
            text_centered(g, cx, cy + 2, label, 2, COLOR_TEXT);
            char d = held_digit(k);
            if (d && k != KEY_SPACE) {
                char ds[2] = { d, 0 };
                g->setTextSize(1);
                g->setTextColor(COLOR_TEXT_DIM);
                ui_print(x + w - 12, y + 5, 1, COLOR_TEXT_DIM, ds);
            }
        }
    }
}

void keypad_draw_field(Arduino_GFX *g, int y, const char *placeholder) {
    if (!g) return;
    int x = LCD_WIDTH / 2 - KP_FIELD_W / 2;
    g->fillRoundRect(x, y, KP_FIELD_W, KP_FIELD_H, 10, COLOR_TEXT_DIM);
    g->fillRoundRect(x + 2, y + 2, KP_FIELD_W - 4, KP_FIELD_H - 4, 8, COLOR_BG);

    size_t n = buf_len();
    if (n == 0) {
        if (placeholder) text_centered(g, LCD_WIDTH / 2, y + KP_FIELD_H / 2, placeholder, 1, COLOR_TEXT_DIM);
        // caret at the start
        if ((millis() / 500) % 2 == 0) g->fillRect(x + 14, y + 12, 2, 20, COLOR_ACCENT);
        return;
    }

    // What to show: masked for a hidden password, except the character
    // being typed right now.
    const size_t MAX_SHOWN = 22;
    char shown[MAX_SHOWN + 1];
    size_t start = n > MAX_SHOWN ? n - MAX_SHOWN : 0;
    size_t m = n - start;
    bool reveal_last = s_pending >= 0 || millis() - s_typed_ms < REVEAL_MS;
    for (size_t i = 0; i < m; i++) {
        char c = s_buf[start + i];
        if (s_password && s_hidden && !(reveal_last && start + i == n - 1)) c = '*';
        shown[i] = c;
    }
    shown[m] = '\0';

    int tx = x + 14, ty = y + KP_FIELD_H / 2 - 8;
    if (s_pending >= 0 && m > 0) {
        // Highlight the character still being chosen.
        char prefix[sizeof(shown)];
        memcpy(prefix, shown, m - 1);
        prefix[m - 1] = '\0';
        char last[2] = { shown[m - 1], '\0' };
        int px = tx + ui_text_width(prefix, 2);
        g->fillRoundRect(px - 2, ty - 3, ui_text_width(last, 2) + 4, 22, 4, COLOR_ACCENT);
    }
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    ui_print(tx, ty, 2, COLOR_TEXT, shown);
    if (s_pending < 0 && (millis() / 500) % 2 == 0) {
        g->fillRect(tx + ui_text_width(shown, 2) + 1, y + 12, 2, 20, COLOR_ACCENT);
    }
}
