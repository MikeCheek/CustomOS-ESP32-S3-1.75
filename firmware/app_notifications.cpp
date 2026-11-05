#include "app_notifications.h"
#include "store.h"
#include "dnd.h"
#include "config.h"
#include "board_pins.h"
#include "hal_ble.h"
#include "phone_link.h"
#include "app_settings_state.h"
#include "ui.h"
#include "ui_font.h"
#include "phone_images.h"
#include "haptics.h"
#include "usage_stats.h"
#include "app_calendar.h"
#include "hal_touch.h"
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

// ---- History (ring buffer, in PSRAM) ------------------------------------
// Allocated on first use, not by a global initialiser: those run before
// PSRAM is up in some core builds (PlatformIO), leaving it null for good.
static NotifHistoryItem *s_history_buf = nullptr;
static int s_history_count = 0;   // valid entries, caps at NOTIF_HISTORY_SIZE
static int s_history_write = 0;   // next slot to write
static uint32_t s_revision = 0;   // bumps on every change, screens re-read on change
static uint32_t s_saved_rev = 0, s_changed_ms = 0;

// In the store: [header][the ring]. Times are kept as "how long before the
// save" plus the clock at the save, so "5 min ago" is still right after a
// restart (millis() starts again from 0).
struct NotifFile { uint32_t magic, count, write, saved_epoch, saved_ms, size; };
static const uint32_t NOTIF_MAGIC = 0x3146544E;   // "NTF1"

static void history_load(NotifHistoryItem *buf) {
    int sz = store_size("notifications");
    if (sz != (int)(sizeof(NotifFile) + sizeof(NotifHistoryItem) * NOTIF_HISTORY_SIZE)) return;
    uint8_t *raw = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) return;
    if (store_read("notifications", raw, sz) == sz) {
        NotifFile h;
        memcpy(&h, raw, sizeof(h));
        if (h.magic == NOTIF_MAGIC && h.size == NOTIF_HISTORY_SIZE && h.count <= NOTIF_HISTORY_SIZE && h.write < NOTIF_HISTORY_SIZE) {
            memcpy(buf, raw + sizeof(h), sizeof(NotifHistoryItem) * NOTIF_HISTORY_SIZE);
            uint32_t now_epoch = calendar_now(), now = millis();
            uint32_t off_ms = now_epoch > h.saved_epoch && h.saved_epoch ? (now_epoch - h.saved_epoch) * 1000UL : 0;
            for (int i = 0; i < NOTIF_HISTORY_SIZE; i++) {
                NotifHistoryItem &it = buf[i];
                uint32_t age = h.saved_ms - it.received_ms + off_ms;   // how long ago, now
                it.received_ms = now - age;                          // (mod 2^32: ages still come out right)
                it.source[sizeof(it.source) - 1] = it.title[sizeof(it.title) - 1] = it.text[sizeof(it.text) - 1] = 0;
            }
            s_history_count = h.count;
            s_history_write = h.write;
        }
    }
    heap_caps_free(raw);
}

static void history_save(NotifHistoryItem *buf) {
    size_t sz = sizeof(NotifFile) + sizeof(NotifHistoryItem) * NOTIF_HISTORY_SIZE;
    uint8_t *raw = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw) return;
    NotifFile h = {NOTIF_MAGIC, (uint32_t)s_history_count, (uint32_t)s_history_write, calendar_now(), millis(),
                   NOTIF_HISTORY_SIZE};
    memcpy(raw, &h, sizeof(h));
    memcpy(raw + sizeof(h), buf, sizeof(NotifHistoryItem) * NOTIF_HISTORY_SIZE);
    store_write("notifications", raw, sz);
    heap_caps_free(raw);
}

static NotifHistoryItem *history() {
    if (!s_history_buf) {
        s_history_buf = (NotifHistoryItem *)heap_caps_calloc(NOTIF_HISTORY_SIZE, sizeof(NotifHistoryItem),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_history_buf) history_load(s_history_buf);   // what was there before a restart
    }
    return s_history_buf;
}
#define s_history (history())

static void copy_str(char *dst, size_t n, const char *src) {
    if (!src) src = "";
    strncpy(dst, src, n - 1);
    dst[n - 1] = 0;
    ui_utf8_trim(dst);
}

static int slot_of(int idx) {
    return (s_history_write - 1 - idx + NOTIF_HISTORY_SIZE * 2) % NOTIF_HISTORY_SIZE;
}

// ---- muted apps: still listed, but no pop-up or cue (for today, from the detail view) ----------
struct Mute { char app[32]; uint32_t until; };   // local epoch seconds
static const int MAX_MUTES = 12;
static Mute s_mutes[MAX_MUTES];
static int  s_mute_n = -1;   // -1: not loaded yet
static uint32_t s_unread = 0;

static void mutes_load() {
    if (s_mute_n >= 0) return;
    s_mute_n = 0;
    Preferences p;
    if (!p.begin("nmute", true)) return;
    int n = p.getUChar("n", 0);
    if (n <= MAX_MUTES && p.getBytes("l", s_mutes, sizeof(Mute) * n) == sizeof(Mute) * n) s_mute_n = n;
    p.end();
}
static void mutes_save() {
    Preferences p;
    if (!p.begin("nmute", false)) return;
    p.putUChar("n", (uint8_t)s_mute_n);
    p.putBytes("l", s_mutes, sizeof(Mute) * s_mute_n);
    p.end();
}
bool notifications_app_muted(const char *app) {
    mutes_load();
    uint32_t now = calendar_now();
    for (int i = 0; i < s_mute_n; i++)
        if (!strcmp(s_mutes[i].app, app)) return now < s_mutes[i].until;
    return false;
}
void notifications_mute_app(const char *app, bool on) {
    mutes_load();
    for (int i = 0; i < s_mute_n; i++) {
        if (strcmp(s_mutes[i].app, app)) continue;
        memmove(&s_mutes[i], &s_mutes[i + 1], sizeof(Mute) * (s_mute_n - i - 1));
        s_mute_n--;
        break;
    }
    if (on) {
        if (s_mute_n == MAX_MUTES) { memmove(s_mutes, s_mutes + 1, sizeof(Mute) * (MAX_MUTES - 1)); s_mute_n--; }
        Mute &m = s_mutes[s_mute_n++];
        strlcpy(m.app, app, sizeof(m.app));
        uint32_t now = calendar_now();
        m.until = now - now % 86400 + 86400;   // until midnight
    }
    mutes_save();
}
uint32_t notifications_unread() { return s_unread; }

static void toast_for(const NotifHistoryItem &it) {
    s_unread++;
    usage_count_notification();
    if (dnd_active() || notifications_app_muted(it.source)) return;
    haptic(HAP_NOTIFY);
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

// Rebuilds the ring without entry `idx` (0 = newest) - for the plain-text
// ones, which have no id to remove them by.
static void remove_index(int idx) {
    if (!s_history || idx < 0 || idx >= s_history_count) return;
    int n = s_history_count, kept = 0;
    NotifHistoryItem *tmp = (NotifHistoryItem *)heap_caps_malloc(n * sizeof(NotifHistoryItem), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) return;
    for (int i = n - 1; i >= 0; i--)
        if (i != idx) tmp[kept++] = s_history[slot_of(i)];
    for (int i = 0; i < kept; i++) s_history[i] = tmp[i];
    s_history_count = kept;
    s_history_write = kept % NOTIF_HISTORY_SIZE;
    s_revision++;
    heap_caps_free(tmp);
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

void notifications_clear_all() {
    if (!s_history) return;
    for (int i = 0; i < s_history_count; i++) {
        const NotifHistoryItem &it = s_history[slot_of(i)];
        if (it.id) phone_link_notif_dismiss(it.id);
    }
    s_history_count = 0;
    s_history_write = 0;
    s_revision++;
}

void notifications_update() {
    // saved 20 s after the last change (a burst of messages is one write)
    uint32_t ms = millis();
    if (s_revision != s_saved_rev) {
        if (!s_changed_ms) s_changed_ms = ms ? ms : 1;
        else if (ms - s_changed_ms > 20000 && s_history_buf) {
            history_save(s_history_buf);
            s_saved_rev = s_revision;
            s_changed_ms = 0;
        }
    }
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
// Smart replies for the message come right after "Dictate".
struct SmartReplies {
    uint32_t id;
    uint8_t n;
    char r[NOTIF_SMART_MAX][40];
};
static const int SMART_SLOTS = 6;
static SmartReplies *s_smart = nullptr;   // PSRAM, on first use
static int s_smart_next = 0;

void notifications_set_smart_replies(uint32_t id, const char *const *items, int n) {
    if (!id) return;
    if (!s_smart) s_smart = (SmartReplies *)heap_caps_calloc(SMART_SLOTS, sizeof(SmartReplies), MALLOC_CAP_SPIRAM);
    if (!s_smart) return;
    SmartReplies *slot = nullptr;
    for (int i = 0; i < SMART_SLOTS; i++)
        if (s_smart[i].id == id) slot = &s_smart[i];
    if (!slot) { slot = &s_smart[s_smart_next]; s_smart_next = (s_smart_next + 1) % SMART_SLOTS; }
    slot->id = id;
    slot->n = 0;
    for (int i = 0; i < n && slot->n < NOTIF_SMART_MAX; i++) {
        if (!items[i] || !items[i][0]) continue;
        char *d = slot->r[slot->n++];
        strncpy(d, items[i], 39);
        d[39] = 0;
        ble_fold_utf8(d);
        ui_utf8_trim(d);
    }
}

static const SmartReplies *smart_for(uint32_t id) {
    if (!s_smart || !id) return nullptr;
    for (int i = 0; i < SMART_SLOTS; i++)
        if (s_smart[i].id == id && s_smart[i].n) return &s_smart[i];
    return nullptr;
}

static NotifHistoryItem s_open;     // the one being viewed (a copy - the ring may move)

static int smart_count() { const SmartReplies *s = smart_for(s_open.id); return s ? s->n : 0; }
static int reply_count() { return quick_reply_count() + smart_count(); }
static bool reply_is_smart(int i) { return i >= 1 && i <= smart_count(); }
static const char *reply_at(int i) {
    int k = smart_count();
    if (i >= 1 && i <= k) return smart_for(s_open.id)->r[i - 1];
    return quick_reply(i > k ? i - k : i);
}
#define REPLY_COUNT reply_count()
#define REPLIES_AT(i) reply_at(i)

enum View { V_LIST, V_DETAIL, V_REPLY, V_DIGEST };

// The phone's LLM: a summary of the recent notifications ("Catch me up",
// {"e":"digest"} -> {"t":"digest"}) and one message translated
// ({"e":"tr"} -> {"t":"tr"}). PSRAM, on first use.
static const int DIGEST_LEN = 720, TR_LEN = 480;
static char *s_digest = nullptr, *s_tr = nullptr;
static bool s_digest_wait = false, s_tr_wait = false;
static uint32_t s_tr_id = 0, s_ask_ms = 0;
static bool alloc_ai() {
    if (!s_digest) s_digest = (char *)heap_caps_calloc(1, DIGEST_LEN, MALLOC_CAP_SPIRAM);
    if (!s_tr) s_tr = (char *)heap_caps_calloc(1, TR_LEN, MALLOC_CAP_SPIRAM);
    return s_digest && s_tr;
}
static void put_text(char *dst, int n, const char *src) {
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
    ble_fold_utf8(dst);
    ui_utf8_trim(dst);
}
void notifications_on_digest(const char *text, const char *err) {
    if (!alloc_ai()) return;
    put_text(s_digest, DIGEST_LEN, err && err[0] ? err : text);
    s_digest_wait = false;
}
void notifications_on_translation(uint32_t id, const char *text, const char *err) {
    if (!alloc_ai() || id != s_tr_id) return;
    put_text(s_tr, TR_LEN, err && err[0] ? err : text);
    s_tr_wait = false;
}
static View     s_view = V_LIST;
static UiScroll s_scroll;
static bool     s_prev = false, s_back = false;
static int      s_press = -1;
static uint32_t s_seen_rev = 0;

// The list starts lower when "Catch me up" sits above it, clear of the title.
static int list_top() {
    return LIST_TOP + (notifications_history_count() >= 2 && phone_link_active() ? 28 : 0);
}

// ---- the list's rows: one app's notifications stack up into one row
// (tap to fan them out), each row swipes left to clear, "Clear all" ends it.
enum RowKind : uint8_t { R_ITEM, R_STACK, R_HEADER, R_CLEAR };
struct Row { RowKind kind; int8_t idx; uint8_t n; int16_t y, h; };
static Row  s_rows[NOTIF_HISTORY_SIZE * 2 + 2];
static int  s_row_n = 0;
static char s_expanded[32] = "";   // the app fanned out
static int  s_swipe_row = -1;      // the row following the finger sideways
static float s_swipe_dx = 0;
static int  s_swipe_x0 = 0, s_swipe_y0 = 0;
static bool s_swiping = false;

static void build_rows() {
    s_row_n = 0;
    int count = notifications_history_count();
    bool used[NOTIF_HISTORY_SIZE] = {false};
    int y = list_top();
    auto push = [&](RowKind k, int idx, int n, int h) {
        s_rows[s_row_n++] = {k, (int8_t)idx, (uint8_t)n, (int16_t)y, (int16_t)h};
        y += h + GAP;
    };
    NotifHistoryItem a, b;
    for (int i = 0; i < count; i++) {
        if (used[i] || !notifications_get_history(i, a)) continue;
        int same[NOTIF_HISTORY_SIZE], n = 0;
        for (int j = i; j < count; j++) {
            if (used[j] || !notifications_get_history(j, b) || strcmp(a.source, b.source)) continue;
            same[n++] = j;
            used[j] = true;
        }
        if (n == 1) push(R_ITEM, i, 1, ROW_H);
        else if (!strcmp(s_expanded, a.source)) {
            push(R_HEADER, i, n, 40);
            for (int k = 0; k < n; k++) push(R_ITEM, same[k], 1, ROW_H);
        } else push(R_STACK, i, n, ROW_H + 10);
    }
    if (count > 0) push(R_CLEAR, -1, 0, 52);
}

static int list_content_h() {
    build_rows();
    int end = s_row_n ? s_rows[s_row_n - 1].y + s_rows[s_row_n - 1].h : list_top();
    return end + 90;
}

// Clears one row: that notification, or the whole app's stack.
static void dismiss_row(int r) {
    if (r < 0 || r >= s_row_n) return;
    NotifHistoryItem it;
    if (!notifications_get_history(s_rows[r].idx, it)) return;
    haptic(HAP_CLICK);
    if (s_rows[r].kind == R_ITEM) {
        if (it.id) { phone_link_notif_dismiss(it.id); notifications_remove(it.id); }
        else remove_index(s_rows[r].idx);
        return;
    }
    char app[32];
    strlcpy(app, it.source, sizeof(app));
    for (int i = notifications_history_count() - 1; i >= 0; i--) {
        if (!notifications_get_history(i, it) || strcmp(it.source, app)) continue;
        if (it.id) { phone_link_notif_dismiss(it.id); notifications_remove(it.id); }
        else remove_index(i);   // from the oldest up, so the indexes ahead stay right
    }
}

static void enter(View v) {
    s_view = v;
    s_press = -1;
    if (v == V_LIST) ui_scroll_reset(&s_scroll, list_content_h() - LCD_HEIGHT, 0);
    else if (v == V_REPLY) ui_scroll_reset(&s_scroll, LIST_TOP + REPLY_COUNT * 66 + 110 - LCD_HEIGHT, 0);
    else if (v == V_DIGEST) ui_scroll_reset(&s_scroll, 260, 0);
    else ui_scroll_reset(&s_scroll, 0, 0);
}

static void notifications_create() {
    s_prev = s_back = false;
    s_expanded[0] = 0;
    s_swipe_row = -1;
    s_swipe_dx = 0;
    s_seen_rev = s_revision;
    enter(V_LIST);
}

static void notifications_tick() {
    ui_scroll_tick(&s_scroll);
    s_unread = 0;   // looked at
    if (!s_swiping && s_swipe_dx < 0) { s_swipe_dx *= 0.6f; if (s_swipe_dx > -1) { s_swipe_dx = 0; s_swipe_row = -1; } }
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
    if (count >= 2 && phone_link_active()) {
        // "Catch me up": the phone's LLM sums them up
        if (list_top() - 48 + off > 0) {
            g->fillRoundRect(CX - 110, list_top() - 50 + off, 220, 32, 16, s_press == 200 ? COLOR_ACCENT2 : ui_dim(COLOR_ACCENT2, 0.35f));
            snprintf(cnt, sizeof(cnt), "Catch me up (%d)", count);
            ui_text_center(CX, list_top() - 34 + off, COLOR_TEXT, cnt, 1);
        }
    } else if (LIST_TOP - 22 + off > 0) {
        ui_text_center(CX, LIST_TOP - 16 + off, COLOR_TEXT_DIM, cnt, 1);
    }

    build_rows();
    char a[48], t[96], x[96], when[8];
    for (int r = 0; r < s_row_n; r++) {
        const Row &row = s_rows[r];
        int y = row.y + off;
        if (y + row.h < 0) continue;
        if (y > LCD_HEIGHT) break;
        if (row.kind == R_CLEAR) {
            g->fillRoundRect(CX - 90, y, 180, 46, 23, s_press == 300 ? ui_dim(COLOR_BAD, 0.6f) : COLOR_PANEL);
            ui_text_center(CX, y + 23, COLOR_BAD, "Clear all", 2);
            continue;
        }
        NotifHistoryItem it;
        if (!notifications_get_history(row.idx, it)) continue;
        if (row.kind == R_HEADER) {
            fit(a, sizeof(a), it.source, 2, 200);
            ui_print(ROW_X + 12, y + 10, 2, COLOR_TEXT, a);
            g->fillRoundRect(ROW_X + ROW_W - 120, y + 4, 116, 32, 16, s_press == 400 + r ? COLOR_ACCENT : COLOR_PANEL);
            ui_text_center(ROW_X + ROW_W - 62, y + 20, COLOR_TEXT, "Show less", 1);
            continue;
        }
        int dx = r == s_swipe_row ? (int)s_swipe_dx : 0;
        if (dx < 0) {
            // behind the row: what letting go does
            g->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 22, dx < -110 ? COLOR_BAD : ui_dim(COLOR_BAD, 0.45f));
            ui_text_center(ROW_X + ROW_W - 50, y + ROW_H / 2, COLOR_TEXT, row.kind == R_STACK ? "Clear all" : "Clear", 1);
        }
        int rx = ROW_X + dx;
        if (row.kind == R_STACK) {
            // the others, peeking out under the newest
            g->fillRoundRect(rx + 16, y + ROW_H - 10, ROW_W - 32, 20, 12, ui_dim(COLOR_PANEL, 0.7f));
        }
        bool pressed = s_press == r;
        g->fillRoundRect(rx, y, ROW_W, ROW_H, 22, pressed ? ui_dim(COLOR_TEXT, 0.22f) : COLOR_PANEL);
        // app badge: the app's icon, or its initial until that arrives
        const uint16_t *icon = phone_icon(it.icon);
        if (icon) {
            phone_image_draw_round(g, icon, PHONE_ICON_SIZE, PHONE_ICON_SIZE, rx + 30, y + 26, 15);
        } else {
            g->fillCircle(rx + 30, y + 26, 13, COLOR_ACCENT);
            char ini[5];
            ui_first_char(it.source, ini);
            ui_text_center(rx + 30, y + 26, COLOR_TEXT, ini, 2);
        }
        ago(it.received_ms, when, sizeof(when));
        int ww = ui_text_width(when, 1);
        char src[48];
        if (row.kind == R_STACK) snprintf(src, sizeof(src), "%s  +%d", it.source, row.n - 1);
        else strlcpy(src, it.source, sizeof(src));
        fit(a, sizeof(a), src, 1, ROW_W - 52 - 30 - ww);
        ui_print(rx + 52, y + 16, 1, notifications_app_muted(it.source) ? COLOR_TEXT_DIM : COLOR_ACCENT, a);
        ui_print(rx + ROW_W - 22 - ww, y + 16, 1, COLOR_TEXT_DIM, when);
        fit(t, sizeof(t), it.title[0] ? it.title : it.text, 2, ROW_W - 52 - 24);
        ui_print(rx + 52, y + 36, 2, COLOR_TEXT, t);
        if (it.title[0]) {
            fit(x, sizeof(x), it.text, 2, ROW_W - 52 - 30);
            ui_print(rx + 52, y + 64, 2, COLOR_TEXT_DIM, x);
        }
    }
}

// Detail: app, title, wrapped text, then action pills.
static int s_btn_y = 0;
static bool show_translate(bool translated) {
    return s_open.id && !translated && phone_link_active() && strlen(s_open.text) > 3;
}
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
    bool translated = s_tr && s_tr_id == it.id && it.id && (s_tr_wait || s_tr[0]);
    if (translated) {
        y = draw_wrapped(g, s_tr_wait ? "Translating..." : s_tr, y, 330, 2, COLOR_ACCENT2, 7);
    } else {
        y = draw_wrapped(g, it.text, y, 330, 2, ui_dim(COLOR_TEXT, 0.8f), 7);
    }
    s_btn_y = y + 14 < 330 ? 330 : y + 14;
    if (s_btn_y > LCD_HEIGHT - 70) s_btn_y = LCD_HEIGHT - 70;
    // "Translate" (the phone's LLM) and "Mute today", above the actions, when there's room
    if (y + 8 < s_btn_y - 40) {
        bool tr = show_translate(translated);
        int mx = tr ? CX + 6 : CX - 70;
        if (tr) {
            g->fillRoundRect(CX - 146, s_btn_y - 44, 140, 32, 16, s_press == 102 ? COLOR_ACCENT2 : COLOR_PANEL);
            ui_text_center(CX - 76, s_btn_y - 28, COLOR_ACCENT2, "Translate", 1);
        }
        bool muted = notifications_app_muted(it.source);
        g->fillRoundRect(mx, s_btn_y - 44, 140, 32, 16, s_press == 103 ? COLOR_WARN : COLOR_PANEL);
        ui_text_center(mx + 70, s_btn_y - 28, COLOR_WARN, muted ? "Unmute app" : "Mute today", 1);
    }
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

static void draw_digest(Arduino_GFX *g) {
    int off = -ui_scroll_offset(&s_scroll);
    ui_text_center(CX, 84 + off, COLOR_ACCENT2, "Catch me up", 2);
    const char *t = s_digest_wait ? "Summarizing on your phone..." : (s_digest && s_digest[0] ? s_digest : "");
    if (s_digest_wait && millis() - s_ask_ms > 90000) t = "No answer from the phone";
    draw_wrapped(g, t, 120 + off, 330, 2, COLOR_TEXT, 14);
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
        if (reply_is_smart(i)) {
            // written for this message: outlined in the second accent
            g->fillRoundRect(CX - 150, y, 300, 56, 28, pressed ? COLOR_ACCENT2 : ui_dim(COLOR_ACCENT2, 0.55f));
            g->fillRoundRect(CX - 148, y + 2, 296, 52, 26, pressed ? COLOR_ACCENT2 : COLOR_PANEL);
            ui_text_center(CX, y + 28, COLOR_TEXT, REPLIES_AT(i), 1);
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
    else if (s_view == V_DIGEST) draw_digest(g);
    else draw_reply(g);
}

static void back() {
    if (s_view == V_REPLY) enter(V_DETAIL);
    else if (s_view == V_DETAIL || s_view == V_DIGEST) enter(V_LIST);
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
            if (y >= s_btn_y - 48 && y <= s_btn_y - 8) {
                bool translated = s_tr && s_tr_id == s_open.id && s_open.id && (s_tr_wait || s_tr[0]);
                bool tr = show_translate(translated);
                int mx = tr ? CX + 6 : CX - 70;
                if (tr && x >= CX - 150 && x <= CX - 2) s_press = 102;
                if (x >= mx - 4 && x <= mx + 144) s_press = 103;
            }
        }
        if (!pressed && s_press >= 0) {
            int p = s_press;
            s_press = -1;
            if (p == 103) {
                bool m = !notifications_app_muted(s_open.source);
                notifications_mute_app(s_open.source, m);
                char b[64];
                snprintf(b, sizeof(b), m ? "%s muted until tomorrow" : "%s unmuted", s_open.source);
                ui_show_toast(b, 1800);
            } else if (p == 102) {
                if (alloc_ai() && phone_link_notif_translate(s_open.id)) {
                    s_tr_id = s_open.id;
                    s_tr[0] = 0;
                    s_tr_wait = true;
                }
            } else if (p == 100) enter(V_REPLY);
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
        if (s_view == V_DIGEST) {
            // just scrolling
        } else if (s_view == V_LIST) {
            s_swipe_row = -1;
            s_swipe_dx = 0;
            s_swiping = false;
            s_swipe_x0 = x;
            s_swipe_y0 = y;
            for (int r = 0; r < s_row_n; r++) {
                const Row &row = s_rows[r];
                if (cy < row.y || cy >= row.y + row.h) continue;
                if (row.kind == R_CLEAR) { if (abs(x - CX) < 95) s_press = 300; }
                else if (row.kind == R_HEADER) { if (x > ROW_X + ROW_W - 130) s_press = 400 + r; }
                else if (x >= ROW_X && x <= ROW_X + ROW_W) { s_press = r; s_swipe_row = r; }
                break;
            }
            if (notifications_history_count() >= 2 && phone_link_active() && abs(x - CX) <= 115 &&
                cy >= list_top() - 54 && cy <= list_top() - 14)
                s_press = 200;
        } else if (abs(x - CX) <= 150 && cy >= LIST_TOP) {
            int i = (cy - LIST_TOP) / 66;
            if (i < REPLY_COUNT && (cy - LIST_TOP) % 66 < 56) s_press = i;
        }
    }
    // a row dragged sideways: it follows the finger (left only), the list stays put
    if (s_view == V_LIST && s_swipe_row >= 0) {
        if (pressed && !s_swiping && abs(x - s_swipe_x0) > 14 && abs(x - s_swipe_x0) > abs(y - s_swipe_y0) * 3 / 2) {
            s_swiping = true;
            s_press = -1;
            s_scroll.moved = true;
        }
        if (s_swiping) {
            if (pressed) { s_swipe_dx = x - s_swipe_x0 < 0 ? x - s_swipe_x0 : 0; return; }
            touch_cancel_swipes();
            s_swiping = false;
            ui_scroll_touch(&s_scroll, s_swipe_y0, false);
            if (s_swipe_dx < -110) {
                dismiss_row(s_swipe_row);
                s_swipe_row = -1;
                s_swipe_dx = 0;
                ui_scroll_set_max(&s_scroll, list_content_h() - LCD_HEIGHT);
            }
            return;   // springs back in the tick otherwise
        }
    }
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    if (s_scroll.moved) s_press = -1;
    if (!pressed) {
        int p = s_press;
        s_press = -1;
        if (!tap || p < 0) return;
        if (s_view == V_LIST && p == 200) {
            if (alloc_ai() && phone_link_notif_digest()) {
                s_digest[0] = 0;
                s_digest_wait = true;
                s_ask_ms = millis();
                enter(V_DIGEST);
            }
        } else if (s_view == V_DIGEST) {
            // nothing to tap
        } else if (s_view == V_LIST) {
            if (p == 300) {
                notifications_clear_all();
                haptic(HAP_SUCCESS);
                enter(V_LIST);
            } else if (p >= 400) {
                s_expanded[0] = 0;
                enter(V_LIST);
            } else if (p < s_row_n && s_rows[p].kind == R_STACK) {
                NotifHistoryItem it;
                if (notifications_get_history(s_rows[p].idx, it)) strlcpy(s_expanded, it.source, sizeof(s_expanded));
                enter(V_LIST);
            } else if (p < s_row_n && notifications_get_history(s_rows[p].idx, s_open)) {
                enter(V_DETAIL);
            }
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
