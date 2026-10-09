#include "app_notifications.h"
#include "dnd.h"
#include "config.h"
#include "board_pins.h"
#include "hal_ble.h"
#include "phone_link.h"
#include "app_settings_state.h"
#include "ui.h"
#include "ui_font.h"
#include "phone_images.h"
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

// ---- History (ring buffer, in PSRAM) ------------------------------------
// Allocated on first use, not by a global initialiser: those run before
// PSRAM is up in some core builds (PlatformIO), leaving it null for good.
static NotifHistoryItem *s_history_buf = nullptr;
static NotifHistoryItem *history() {
    if (!s_history_buf)
        s_history_buf = (NotifHistoryItem *)heap_caps_calloc(NOTIF_HISTORY_SIZE, sizeof(NotifHistoryItem),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_history_buf;
}
#define s_history (history())
static int s_history_count = 0;   // valid entries, caps at NOTIF_HISTORY_SIZE
static int s_history_write = 0;   // next slot to write
static uint32_t s_revision = 0;   // bumps on every change, screens re-read on change

static void copy_str(char *dst, size_t n, const char *src) {
    if (!src) src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
    ui_utf8_trim(dst);
}

static int slot_of(int idx) {
    return (s_history_write - 1 - idx + NOTIF_HISTORY_SIZE * 2) % NOTIF_HISTORY_SIZE;
}

static void toast_for(const NotifHistoryItem &it) {
    if (dnd_active()) return;
    char t[96];
    if (it.title[0]) snprintf(t, sizeof(t), "%s: %s", it.title, it.text);
    else snprintf(t, sizeof(t), "%s", it.text);
    if (it.icon) phone_icon(it.icon);   // ask now, so it's likely there by the next frame
    ui_show_toast_icon(t, 3500, it.icon);
}

static NotifHistoryItem &next_slot() {
    NotifHistoryItem &item = s_history[s_history_write];
    s_history_write = (s_history_write + 1) % NOTIF_HISTORY_SIZE;
    if (s_history_count < NOTIF_HISTORY_SIZE) s_history_count++;
    return item;
}

void notifications_add(uint32_t id, const char *app, const char *title, const char *text, bool can_reply,
                       uint32_t icon) {
    if (!s_history) return;
    // An update of a notification we already have (chat with new message)
    // replaces it in place instead of filling the list with copies.
    if (id) {
        for (int i = 0; i < s_history_count; i++) {
            NotifHistoryItem &it = s_history[slot_of(i)];
            if (it.id == id) {
                copy_str(it.title, sizeof(it.title), title);
                copy_str(it.text, sizeof(it.text), text);
                ble_fold_utf8(it.title);
                ble_fold_utf8(it.text);
                it.can_reply = can_reply;
                it.received_ms = millis();
                if (icon) it.icon = icon;
                s_revision++;
                toast_for(it);
                return;
            }
        }
    }
    NotifHistoryItem &it = next_slot();
    it.id = id;
    copy_str(it.source, sizeof(it.source), app);
    copy_str(it.title, sizeof(it.title), title);
    copy_str(it.text, sizeof(it.text), text);
    ble_fold_utf8(it.source);
    ble_fold_utf8(it.title);
    ble_fold_utf8(it.text);
    it.can_reply = can_reply;
    it.received_ms = millis();
    it.icon = icon;
    s_revision++;
    toast_for(it);
}

void notifications_remove(uint32_t id) {
    if (!s_history || !id) return;
    // Rebuild the ring without that entry (oldest first, keeps order).
    // Through a temporary copy: compacting in place would overwrite slots
    // that haven't been read yet.
    int n = s_history_count;
    NotifHistoryItem *tmp = (NotifHistoryItem *)heap_caps_malloc(n * sizeof(NotifHistoryItem),
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) return;
    int kept = 0;
    for (int i = n - 1; i >= 0; i--) {
        const NotifHistoryItem &it = s_history[slot_of(i)];
        if (it.id != id) tmp[kept++] = it;
    }
    if (kept != n) {
        for (int i = 0; i < kept; i++) s_history[i] = tmp[i];
        s_history_count = kept;
        s_history_write = kept % NOTIF_HISTORY_SIZE;
        s_revision++;
    }
    heap_caps_free(tmp);
}

void notifications_update() {
    BleNotification n;
    // Legacy path: plain text, no id, no actions.
    while (ble_get_notification(n)) {
        if (!s_history) continue;
        NotifHistoryItem &it = next_slot();
        it.id = 0;
        copy_str(it.source, sizeof(it.source), n.source);
        it.title[0] = 0;
        copy_str(it.text, sizeof(it.text), n.text);
        it.can_reply = false;
        it.received_ms = millis();
        it.icon = 0;
        s_revision++;
        toast_for(it);
    }
}

int notifications_history_count() {
    return s_history ? s_history_count : 0;
}

bool notifications_get_history(int idx, NotifHistoryItem &out) {
    if (!s_history || idx < 0 || idx >= s_history_count) return false;
    out = s_history[slot_of(idx)];
    return true;
}

// =========================================================================
//  Notifications screen: list -> detail (dismiss / reply) -> reply picker
// =========================================================================

static const int CX = LCD_WIDTH / 2;
static const int ROW_W = 360, ROW_X = (LCD_WIDTH - ROW_W) / 2, ROW_H = 96, GAP = 8;
static const int LIST_TOP = 92;

void dictate_open(uint32_t notif_id, const char *to);

// Row 0 of the reply picker is "Dictate" (voice reply, app_dictate.cpp);
// then the quick replies - edited in the app, built-in presets otherwise
// (dnd.cpp).
#define REPLY_COUNT quick_reply_count()
#define REPLIES_AT(i) quick_reply(i)

enum View { V_LIST, V_DETAIL, V_REPLY };
static View     s_view = V_LIST;
static UiScroll s_scroll;
static bool     s_prev = false, s_back = false;
static int      s_press = -1;
static NotifHistoryItem s_open;     // the one being viewed (a copy - the ring may move)
static uint32_t s_seen_rev = 0;

static int list_content_h() {
    return LIST_TOP + notifications_history_count() * (ROW_H + GAP) + 120;
}

static void enter(View v) {
    s_view = v;
    s_press = -1;
    if (v == V_LIST) ui_scroll_reset(&s_scroll, list_content_h() - LCD_HEIGHT, 0);
    else if (v == V_REPLY) ui_scroll_reset(&s_scroll, LIST_TOP + REPLY_COUNT * 66 + 110 - LCD_HEIGHT, 0);
    else ui_scroll_reset(&s_scroll, 0, 0);
}

static void notifications_create() {
    s_prev = s_back = false;
    s_seen_rev = s_revision;
    enter(V_LIST);
}

static void notifications_tick() {
    ui_scroll_tick(&s_scroll);
    if (s_seen_rev != s_revision) {
        s_seen_rev = s_revision;
        if (s_view == V_LIST) ui_scroll_set_max(&s_scroll, list_content_h() - LCD_HEIGHT);
    }
}

// One line of src into dst, cut with ".." to fit max_px at `size`.
static void fit(char *dst, size_t n, const char *src, int size, int max_px) {
    copy_str(dst, n, src);
    for (char *p = dst; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    ui_fit(dst, size, max_px);
}

static void ago(uint32_t ms, char *out, size_t n) {
    uint32_t s = (millis() - ms) / 1000;
    if (s < 60) snprintf(out, n, "now");
    else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(out, n, "%luh", (unsigned long)(s / 3600));
    else snprintf(out, n, "%lud", (unsigned long)(s / 86400));
}

// Word-wraps text (pixel widths, UTF-8 safe), centred lines from y.
// Returns the y after the last line.
static int draw_wrapped(Arduino_GFX *g, const char *text, int y, int width_px, int size, uint16_t color, int max_lines) {
    (void)g;
    int lh = 8 * size + 8;
    const char *p = text;
    char line[120];
    int lines = 0;
    while (*p && lines < max_lines) {
        while (*p == ' ' || *p == '\n') p++;
        if (!*p) break;
        // grow the line word by word while it fits
        int take = 0, best = 0;
        while (p[take] && p[take] != '\n' && take < (int)sizeof(line) - 1) {
            int end = take;
            while (p[end] && p[end] != ' ' && p[end] != '\n' && end < (int)sizeof(line) - 1) end++;
            memcpy(line, p, end);
            line[end] = 0;
            if (ui_text_width(line, size) > width_px) {
                if (best == 0) { // the first word alone is too wide: break it mid-word
                    int k = 0, fitk = 0;
                    while (k < end) {
                        int nk = k + 1;
                        while (nk < end && ((uint8_t)p[nk] & 0xC0) == 0x80) nk++;
                        memcpy(line, p, nk);
                        line[nk] = 0;
                        if (ui_text_width(line, size) > width_px) break;
                        fitk = nk;
                        k = nk;
                    }
                    best = fitk > 0 ? fitk : 1;
                }
                break;
            }
            best = end;
            take = end;
            while (p[take] == ' ') take++;
        }
        if (best == 0) best = take > 0 ? take : 1;
        memcpy(line, p, best);
        line[best] = 0;
        ui_utf8_trim(line);
        int used = (int)strlen(line);
        if (lines == max_lines - 1 && p[used] && p[used] != '\n') {
            // last allowed line and more text follows: end with ".."
            char more[124];
            snprintf(more, sizeof(more), "%s ..........", line);
            ui_fit(more, size, width_px);
            ui_text_center(CX, y + 4 * size, color, more, size);
        } else {
            ui_text_center(CX, y + 4 * size, color, line, size);
        }
        y += lh;
        p += used > 0 ? used : 1;
        lines++;
    }
    return y;
}

static void draw_list(Arduino_GFX *g) {
    int count = notifications_history_count();
    if (count == 0) {
        // bell
        int cx = CX, cy = LCD_HEIGHT / 2 - 46;
        g->fillCircle(cx, cy, 26, COLOR_PANEL);
        g->fillRoundRect(cx - 12, cy - 12, 24, 22, 10, COLOR_TEXT_DIM);
        g->fillRect(cx - 15, cy + 6, 30, 5, COLOR_TEXT_DIM);
        g->fillCircle(cx, cy + 15, 4, COLOR_TEXT_DIM);
        ui_text_center(CX, LCD_HEIGHT / 2 + 4, COLOR_TEXT, "All caught up", 2);
        ui_text_center(CX, LCD_HEIGHT / 2 + 32, COLOR_TEXT_DIM,
                       phone_link_active() ? "Phone notifications show up here" : "Connect the companion app", 1);
        return;
    }
    int off = -ui_scroll_offset(&s_scroll);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d notification%s", count, count == 1 ? "" : "s");
    if (LIST_TOP - 22 + off > 0) ui_text_center(CX, LIST_TOP - 16 + off, COLOR_TEXT_DIM, cnt, 1);

    char a[48], t[96], x[96], when[8];
    for (int i = 0; i < count; i++) {
        int y = LIST_TOP + i * (ROW_H + GAP) + off;
        if (y + ROW_H < 0) continue;
        if (y > LCD_HEIGHT) break;
        NotifHistoryItem it;
        if (!notifications_get_history(i, it)) break;
        bool pressed = s_press == i;
        g->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 22, pressed ? ui_dim(COLOR_TEXT, 0.22f) : COLOR_PANEL);
        // app badge: the app's icon, or its initial until that arrives
        const uint16_t *icon = phone_icon(it.icon);
        if (icon) {
            phone_image_draw_round(g, icon, PHONE_ICON_SIZE, PHONE_ICON_SIZE, ROW_X + 30, y + 26, 15);
        } else {
            g->fillCircle(ROW_X + 30, y + 26, 13, COLOR_ACCENT);
            char ini[5];
            ui_first_char(it.source, ini);
            ui_text_center(ROW_X + 30, y + 26, COLOR_TEXT, ini, 2);
        }
        ago(it.received_ms, when, sizeof(when));
        int ww = ui_text_width(when, 1);
        fit(a, sizeof(a), it.source, 1, ROW_W - 52 - 30 - ww);
        ui_print(ROW_X + 52, y + 16, 1, COLOR_ACCENT, a);
        ui_print(ROW_X + ROW_W - 22 - ww, y + 16, 1, COLOR_TEXT_DIM, when);
        fit(t, sizeof(t), it.title[0] ? it.title : it.text, 2, ROW_W - 52 - 24);
        ui_print(ROW_X + 52, y + 36, 2, COLOR_TEXT, t);
        if (it.title[0]) {
            fit(x, sizeof(x), it.text, 2, ROW_W - 52 - 30);
            ui_print(ROW_X + 52, y + 64, 2, COLOR_TEXT_DIM, x);
        }
    }
}

// Detail: app, title, wrapped text, then action pills.
static int s_btn_y = 0;
static void draw_detail(Arduino_GFX *g) {
    const NotifHistoryItem &it = s_open;
    char a[48];
    fit(a, sizeof(a), it.source, 2, 240);
    const uint16_t *icon = phone_icon(it.icon);
    if (icon) phone_image_draw_round(g, icon, PHONE_ICON_SIZE, PHONE_ICON_SIZE, CX, 44, 18);
    ui_text_center(CX, 78, COLOR_ACCENT, a, 2);
    int y = 104;
    if (it.title[0]) y = draw_wrapped(g, it.title, y, 300, 2, COLOR_TEXT, 2);
    y += 4;
    y = draw_wrapped(g, it.text, y, 330, 2, ui_dim(COLOR_TEXT, 0.8f), 7);
    s_btn_y = y + 14 < 330 ? 330 : y + 14;
    if (s_btn_y > LCD_HEIGHT - 70) s_btn_y = LCD_HEIGHT - 70;
    if (it.id) {
        bool two = it.can_reply;
        int w = two ? 140 : 180, gap = 16;
        int x0 = two ? CX - w - gap / 2 : CX - w / 2;
        if (two) {
            g->fillRoundRect(x0, s_btn_y, w, 50, 25, s_press == 100 ? COLOR_TEXT : COLOR_ACCENT);
            ui_text_center(x0 + w / 2, s_btn_y + 25, s_press == 100 ? COLOR_ACCENT : COLOR_TEXT, "Reply", 2);
            x0 += w + gap;
        }
        g->fillRoundRect(x0, s_btn_y, w, 50, 25, s_press == 101 ? COLOR_TEXT : COLOR_PANEL);
        ui_text_center(x0 + w / 2, s_btn_y + 25, s_press == 101 ? COLOR_BG : COLOR_TEXT, "Dismiss", 2);
    } else {
        ui_text_center(CX, s_btn_y + 25, COLOR_TEXT_DIM, "Update the companion app for actions", 1);
    }
}

static void draw_reply(Arduino_GFX *g) {
    int off = -ui_scroll_offset(&s_scroll);
    ui_text_center(CX, LIST_TOP - 20 + off, COLOR_TEXT_DIM, "Quick reply", 1);
    for (int i = 0; i < REPLY_COUNT; i++) {
        int y = LIST_TOP + i * 66 + off;
        if (y + 56 < 0 || y > LCD_HEIGHT) continue;
        bool pressed = s_press == i;
        if (i == 0) {
            // voice: accent pill with a small microphone
            g->fillRoundRect(CX - 150, y, 300, 56, 28, pressed ? ui_dim(COLOR_ACCENT, 0.6f) : COLOR_ACCENT);
            int tw = ui_text_width(REPLIES_AT(0), 2);
            int mx = CX - tw / 2 - 16;
            g->fillRoundRect(mx - 6, y + 14, 12, 20, 6, COLOR_TEXT);
            g->fillRect(mx - 1, y + 34, 3, 6, COLOR_TEXT);
            g->fillRect(mx - 7, y + 40, 15, 3, COLOR_TEXT);
            ui_text_center(CX + 12, y + 28, COLOR_TEXT, REPLIES_AT(0), 2);
            continue;
        }
        g->fillRoundRect(CX - 150, y, 300, 56, 28, pressed ? COLOR_ACCENT : COLOR_PANEL);
        ui_text_center(CX, y + 28, COLOR_TEXT, REPLIES_AT(i), 2);
    }
}

static void notifications_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_view == V_LIST) draw_list(g);
    else if (s_view == V_DETAIL) draw_detail(g);
    else draw_reply(g);
}

static void back() {
    if (s_view == V_REPLY) enter(V_DETAIL);
    else if (s_view == V_DETAIL) enter(V_LIST);
    else ui_pop_screen();
}

static void notifications_touch(int x, int y, bool pressed) {
    if (pressed && !s_scroll.dragging) s_back = (x < 54 && y < 54);
    if (s_back) { if (!pressed) { s_back = false; back(); } return; }
    bool edge = pressed && !s_prev;
    s_prev = pressed;

    if (s_view == V_DETAIL) {
        if (edge) {
            s_press = -1;
            if (s_open.id && y >= s_btn_y && y <= s_btn_y + 50) {
                bool two = s_open.can_reply;
                int w = two ? 140 : 180, gap = 16;
                int x0 = two ? CX - w - gap / 2 : CX - w / 2;
                if (two && x >= x0 && x <= x0 + w) s_press = 100;
                int xd = two ? x0 + w + gap : x0;
                if (x >= xd && x <= xd + w) s_press = 101;
            }
        }
        if (!pressed && s_press >= 0) {
            int p = s_press;
            s_press = -1;
            if (p == 100) enter(V_REPLY);
            else if (p == 101) {
                phone_link_notif_dismiss(s_open.id);
                notifications_remove(s_open.id);
                enter(V_LIST);
            }
        }
        return;
    }

    int cy = y + ui_scroll_offset(&s_scroll);
    if (edge) {
        s_press = -1;
        if (s_view == V_LIST) {
            if (x >= ROW_X && x <= ROW_X + ROW_W && cy >= LIST_TOP) {
                int i = (cy - LIST_TOP) / (ROW_H + GAP);
                if (i < notifications_history_count() && (cy - LIST_TOP) % (ROW_H + GAP) < ROW_H) s_press = i;
            }
        } else if (abs(x - CX) <= 150 && cy >= LIST_TOP) {
            int i = (cy - LIST_TOP) / 66;
            if (i < REPLY_COUNT && (cy - LIST_TOP) % 66 < 56) s_press = i;
        }
    }
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    if (s_scroll.moved) s_press = -1;
    if (!pressed) {
        int p = s_press;
        s_press = -1;
        if (!tap || p < 0) return;
        if (s_view == V_LIST) {
            if (notifications_get_history(p, s_open)) enter(V_DETAIL);
        } else if (p == 0) {
            enter(V_DETAIL);   // back here after the voice reply screen
            dictate_open(s_open.id, s_open.title[0] ? s_open.title : s_open.source);
        } else {
            ui_show_toast(phone_link_notif_reply(s_open.id, REPLIES_AT(p)) ? "Reply sent" : "Phone not reachable", 1500);
            enter(V_LIST);
        }
    }
}

static void notifications_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT) back();
}

Screen notifications_screen = {
    "Notifications", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    notifications_create, notifications_draw, notifications_touch, notifications_tick, nullptr, notifications_gesture,
    250, false, false, false, true, // no edge-drag back: swipe right steps back inside the screen
};
