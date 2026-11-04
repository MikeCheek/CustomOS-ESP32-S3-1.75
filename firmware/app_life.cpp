// Life: habits, nudges, Breathe and Stretch - see app_life.h.
#include "app_life.h"
#include "store.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "haptics.h"
#include "dnd.h"
#include "fitness.h"
#include "phone_link.h"
#include "app_clock.h"
#include "app_workout.h"
#include "hal_ble.h"
#include "hal_rtc.h"
#include "hal_sleep.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

// ---- the plan (from the app; these defaults work without it) ---------------------------------
struct LifePlan {
    uint16_t day_from = 8 * 60, day_to = 22 * 60;            // nudges only in here
    uint8_t  w_on = 1, w_goal = 8; uint16_t w_every = 90;     // water: glasses, minutes
    uint8_t  s_on = 1, s_goal = 3; uint16_t s_every = 120, s_from = 9 * 60, s_to = 18 * 60;
    uint8_t  t_on = 0, t_days = 0x2A, t_goal = 30; uint16_t t_at = 18 * 60;   // Mon, Wed, Fri
    uint8_t  m_on = 1, m_min = 3; uint16_t m_at = 21 * 60;
    uint8_t  me_on = 1; uint16_t me_at[3] = {8 * 60, 13 * 60, 19 * 60 + 30};
    uint8_t  so_on = 0, so_days = 0x41; uint16_t so_at = 19 * 60; char so_who[24] = "";
    uint8_t  wd_on = 1; uint16_t wd_at = 22 * 60 + 30;
};
static LifePlan s_plan;
static char s_meal[3][72] = {"", "", ""};

// ---- today, and the last days ------------------------------------------------------------------
struct LifeDay {
    uint32_t date;
    uint8_t water, stretch, mind, train, meals, social, wind, score;
};
static LifeDay s_today = {};
// The days before today, newest first: 90 in the store (store.h), 7 in NVS
// when there's nowhere else.
static const int HIST_MAX = 90;
static LifeDay s_hist[HIST_MAX];
static int s_hist_n = 0;
static bool s_hist_dirty = false;
static int hist_cap() { return store_ok() ? HIST_MAX : 7; }
static uint16_t s_last_water = 0, s_last_stretch = 0;   // minute of day (or the last nudge)

static uint32_t today_date() {
    WatchTime t = rtc_now();
    return t.valid ? (uint32_t)t.year * 10000 + t.month * 100 + t.day : 0;
}
static int now_min() {
    WatchTime t = rtc_now();
    return t.valid ? t.hour * 60 + t.minute : -1;
}
static bool day_bit(uint8_t days) {
    WatchTime t = rtc_now();
    return t.valid && (days & (1 << t.weekday));
}

static void save_day() {
    Preferences p;
    if (!p.begin("life", false)) return;
    p.putBytes("today", &s_today, sizeof(s_today));
    if (store_ok()) {
        if (s_hist_dirty && store_write("life_hist", s_hist, sizeof(LifeDay) * s_hist_n)) {
            s_hist_dirty = false;
            if (p.isKey("hist")) { p.remove("hist"); p.remove("hn"); }   // moved: frees NVS
        }
    } else {
        int n = s_hist_n < 7 ? s_hist_n : 7;
        p.putUChar("hn", (uint8_t)n);
        p.putBytes("hist", s_hist, sizeof(LifeDay) * n);
        s_hist_dirty = false;
    }
    p.end();
}
static void save_plan() {
    Preferences p;
    if (!p.begin("life", false)) return;
    p.putBytes("plan", &s_plan, sizeof(s_plan));
    p.putBytes("meals", s_meal, sizeof(s_meal));
    p.end();
}

void life_init() {
    Preferences p;
    if (!p.begin("life", true)) return;
    if (p.getBytesLength("plan") == sizeof(s_plan)) p.getBytes("plan", &s_plan, sizeof(s_plan));
    if (p.getBytesLength("meals") == sizeof(s_meal)) p.getBytes("meals", s_meal, sizeof(s_meal));
    if (p.getBytesLength("today") == sizeof(s_today)) p.getBytes("today", &s_today, sizeof(s_today));
    int sz = store_size("life_hist");
    if (sz > 0 && sz % (int)sizeof(LifeDay) == 0) {
        int n = store_read("life_hist", s_hist, sizeof(s_hist));
        s_hist_n = n > 0 ? n / (int)sizeof(LifeDay) : 0;
    } else {
        // the history from before (NVS): moved to the store at the next save
        s_hist_n = p.getUChar("hn", 0);
        if (s_hist_n > 7 || (s_hist_n && p.getBytes("hist", s_hist, sizeof(LifeDay) * s_hist_n) != sizeof(LifeDay) * s_hist_n))
            s_hist_n = 0;
        s_hist_dirty = s_hist_n > 0 && store_ok();
    }
    p.end();
}

// ---- the score: the habits done out of those set up ----------------------------------------------
struct Habit { const char *name; uint16_t color; };
enum HabitId { H_WATER, H_STRETCH, H_MIND, H_TRAIN, H_MEALS, H_SOCIAL, H_WIND, H_COUNT };
static const Habit HABITS[H_COUNT] = {
    {"Water", 0x2D7F},   {"Stretch", 0xFD20}, {"Mindful", 0x5E3F}, {"Train", 0x9F45},
    {"Meals", 0xFE60},   {"Social", 0xF8AA},  {"Wind down", 0x7C1F},
};

static bool habit_on(int h) {
    switch (h) {
    case H_WATER: return s_plan.w_on;
    case H_STRETCH: return s_plan.s_on;
    case H_MIND: return s_plan.m_on;
    case H_TRAIN: return s_plan.t_on && day_bit(s_plan.t_days);
    case H_MEALS: return s_plan.me_on;
    case H_SOCIAL: return s_plan.so_on && day_bit(s_plan.so_days);
    case H_WIND: return s_plan.wd_on;
    }
    return false;
}
// 0..1 of today's goal for that habit
static float habit_frac(int h) {
    switch (h) {
    case H_WATER: return (float)s_today.water / (s_plan.w_goal ? s_plan.w_goal : 8);
    case H_STRETCH: return (float)s_today.stretch / (s_plan.s_goal ? s_plan.s_goal : 3);
    case H_MIND: return (float)s_today.mind / (s_plan.m_min ? s_plan.m_min : 3);
    case H_TRAIN: return (float)fitness_exercise_min() / (s_plan.t_goal ? s_plan.t_goal : 30);
    case H_MEALS: return (float)__builtin_popcount(s_today.meals) / 3;
    case H_SOCIAL: return s_today.social ? 1 : 0;
    case H_WIND: return s_today.wind ? 1 : 0;
    }
    return 0;
}

int life_score() {
    float sum = 0;
    int n = 0;
    for (int h = 0; h < H_COUNT; h++) {
        if (!habit_on(h)) continue;
        float f = habit_frac(h);
        sum += f > 1 ? 1 : f;
        n++;
    }
    return n ? (int)lroundf(sum * 100 / n) : 0;
}

static void roll_day() {
    uint32_t d = today_date();
    if (!d || d == s_today.date) return;
    if (s_today.date) {
        int cap = hist_cap();
        memmove(&s_hist[1], &s_hist[0], sizeof(LifeDay) * (cap - 1));
        s_hist[0] = s_today;
        if (s_hist_n < cap) s_hist_n++;
        s_hist_dirty = true;
    }
    s_today = {};
    s_today.date = d;
    s_last_water = s_last_stretch = 0;
    save_day();
}

// ---- telling the phone ----------------------------------------------------------------------------
static void day_json(JsonArray a, const LifeDay &d) {
    a.add(d.date); a.add(d.water); a.add(d.stretch); a.add(d.mind); a.add(d.train);
    a.add(__builtin_popcount(d.meals)); a.add(d.social); a.add(d.wind); a.add(d.score);
}

static void roll_day();

void life_send(bool history) {
    roll_day();   // the date is set even before the first loop pass
    s_today.score = (uint8_t)life_score();
    s_today.train = (uint8_t)(fitness_exercise_min() > 255 ? 255 : fitness_exercise_min());
    JsonDocument d;
    d["e"] = "life";
    d["d"] = s_today.date;
    d["w"] = s_today.water;
    d["st"] = s_today.stretch;
    d["mi"] = s_today.mind;
    d["tr"] = s_today.train;
    d["me"] = __builtin_popcount(s_today.meals);
    d["so"] = s_today.social;
    d["wd"] = s_today.wind;
    d["sc"] = s_today.score;
    if (history) {
        JsonArray h = d["h"].to<JsonArray>();
        for (int i = 0; i < s_hist_n && i < 7; i++) day_json(h.add<JsonArray>(), s_hist[i]);
    }
    phone_link_send(d);
    // the older days, 15 a message: {"e":"lh","h":[[...],...]}
    for (int o = 7; history && o < s_hist_n; o += 15) {
        JsonDocument x;
        x["e"] = "lh";
        JsonArray h = x["h"].to<JsonArray>();
        for (int i = o; i < s_hist_n && i < o + 15; i++) day_json(h.add<JsonArray>(), s_hist[i]);
        phone_link_send(x);
    }
}

static void logged() {
    roll_day();
    s_today.score = (uint8_t)life_score();
    save_day();
    life_send(false);
}

// ---- the plan from the app ---------------------------------------------------------------------------
static void u8(JsonVariant v, uint8_t &dst) { if (v.is<int>()) dst = (uint8_t)v.as<int>(); }
static void u16(JsonVariant v, uint16_t &dst) { if (v.is<int>()) dst = (uint16_t)v.as<int>(); }

void life_on_plan(JsonDocument &doc) {
    LifePlan &p = s_plan;
    u16(doc["df"], p.day_from); u16(doc["dt"], p.day_to);
    JsonVariant w = doc["w"];  u8(w["on"], p.w_on); u8(w["g"], p.w_goal); u16(w["ev"], p.w_every);
    JsonVariant s = doc["s"];  u8(s["on"], p.s_on); u8(s["g"], p.s_goal); u16(s["ev"], p.s_every); u16(s["f"], p.s_from); u16(s["t"], p.s_to);
    JsonVariant t = doc["tr"]; u8(t["on"], p.t_on); u8(t["dy"], p.t_days); u8(t["g"], p.t_goal); u16(t["at"], p.t_at);
    JsonVariant m = doc["m"];  u8(m["on"], p.m_on); u8(m["g"], p.m_min); u16(m["at"], p.m_at);
    JsonVariant me = doc["me"];
    u8(me["on"], p.me_on); u16(me["b"], p.me_at[0]); u16(me["l"], p.me_at[1]); u16(me["d"], p.me_at[2]);
    JsonVariant so = doc["so"];
    u8(so["on"], p.so_on); u8(so["dy"], p.so_days); u16(so["at"], p.so_at);
    if (so["who"].is<const char *>()) { strlcpy(p.so_who, so["who"], sizeof(p.so_who)); ble_fold_utf8(p.so_who); }
    JsonVariant wd = doc["wd"]; u8(wd["on"], p.wd_on); u16(wd["at"], p.wd_at);
    if (!p.w_every) p.w_every = 90;
    if (!p.s_every) p.s_every = 120;
    save_plan();
    life_send(true);
}

void life_on_meals(JsonDocument &doc) {
    static const char *const K[3] = {"b", "l", "d"};
    for (int i = 0; i < 3; i++) {
        if (!doc[K[i]].is<const char *>()) continue;
        strlcpy(s_meal[i], doc[K[i]], sizeof(s_meal[i]));
        ui_utf8_trim(s_meal[i]);
        ble_fold_utf8(s_meal[i]);
    }
    save_plan();
}

// =========================================================================
//  Nudges
// =========================================================================
extern Screen life_nudge_screen;
static int  s_nudge = -1;            // the habit the card is about
static int  s_meal_i = 0;            // which meal, for H_MEALS
static uint32_t s_snooze_until[H_COUNT] = {0};
static int  s_last_min = -1;
static uint32_t s_nudge_ms = 0;

static bool quiet_now() {
    return dnd_active() || workout_active() || clock_alarm_ringing() || ui_screen_on_top(&life_nudge_screen) ||
           ui_screen_on_top(&breathe_screen) || ui_screen_on_top(&stretch_screen);
}

static void nudge(int h, int meal = 0) {
    if (quiet_now()) return;
    if (s_snooze_until[h] && (int32_t)(millis() - s_snooze_until[h]) < 0) return;
    s_nudge = h;
    s_meal_i = meal;
    sleep_register_activity();
    haptic(HAP_REMINDER);
    s_nudge_ms = millis();
    ui_push(&life_nudge_screen);
}

void life_update() {
    static uint32_t last = 0;
    uint32_t ms = millis();
    if (ms - last < 5000) return;
    last = ms;
    uint32_t before = s_today.date;
    roll_day();
    // a new day: tell the phone, which sends that day's meal ideas
    if (before && s_today.date != before && phone_link_active()) life_send(false);
    uint32_t ex = fitness_exercise_min();
    s_today.train = (uint8_t)(ex > 255 ? 255 : ex);   // kept for the history
    int m = now_min();
    if (m < 0) return;
    // snoozed ones come back on their own
    for (int h = 0; h < H_COUNT; h++)
        if (s_snooze_until[h] && (int32_t)(ms - s_snooze_until[h]) >= 0 && s_nudge != h) {
            s_snooze_until[h] = 0;
            if (habit_on(h) && habit_frac(h) < 1) nudge(h, h == H_MEALS ? s_meal_i : 0);
            return;
        }
    if (m == s_last_min) return;
    s_last_min = m;
    const LifePlan &p = s_plan;
    bool day = m >= p.day_from && m < p.day_to;
    // timed ones
    if (p.t_on && day_bit(p.t_days) && m == p.t_at && habit_frac(H_TRAIN) < 1) { nudge(H_TRAIN); return; }
    if (p.m_on && m == p.m_at && habit_frac(H_MIND) < 1) { nudge(H_MIND); return; }
    if (p.so_on && day_bit(p.so_days) && m == p.so_at && !s_today.social) { nudge(H_SOCIAL); return; }
    if (p.wd_on && m == p.wd_at && !s_today.wind) { nudge(H_WIND); return; }
    if (p.me_on)
        for (int i = 0; i < 3; i++)
            if (m == p.me_at[i] && !(s_today.meals & (1 << i))) { nudge(H_MEALS, i); return; }
    // every so often
    if (!day) return;
    if (p.w_on && s_today.water < p.w_goal) {
        if (!s_last_water) s_last_water = m;   // counting from the first look today
        if (m - s_last_water >= p.w_every) { s_last_water = m; nudge(H_WATER); return; }
    }
    if (p.s_on && s_today.stretch < p.s_goal && m >= p.s_from && m < p.s_to) {
        if (!s_last_stretch) s_last_stretch = m;
        if (m - s_last_stretch >= p.s_every) { s_last_stretch = m; nudge(H_STRETCH); return; }
    }
}

bool life_next(char *out, int n) {
    int m = now_min();
    if (m < 0) return false;
    const LifePlan &p = s_plan;
    int best = 24 * 60, which = -1;
    auto consider = [&](int h, int at) {
        int in = at - m;
        if (in > 0 && in < best) { best = in; which = h; }
    };
    if (p.w_on && s_today.water < p.w_goal && s_last_water) consider(H_WATER, s_last_water + p.w_every);
    if (p.s_on && s_today.stretch < p.s_goal && s_last_stretch) consider(H_STRETCH, s_last_stretch + p.s_every);
    if (p.t_on && day_bit(p.t_days) && habit_frac(H_TRAIN) < 1) consider(H_TRAIN, p.t_at);
    if (p.m_on && habit_frac(H_MIND) < 1) consider(H_MIND, p.m_at);
    if (p.me_on) for (int i = 0; i < 3; i++) if (!(s_today.meals & (1 << i))) consider(H_MEALS, p.me_at[i]);
    if (p.so_on && day_bit(p.so_days) && !s_today.social) consider(H_SOCIAL, p.so_at);
    if (p.wd_on && !s_today.wind) consider(H_WIND, p.wd_at);
    if (which < 0) return false;
    if (best < 60) snprintf(out, n, "%s in %d min", HABITS[which].name, best);
    else snprintf(out, n, "%s at %d:%02d", HABITS[which].name, (m + best) / 60 % 24, (m + best) % 60);
    return true;
}

// ---- drawing pieces ------------------------------------------------------------------------------------
struct Btn {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x - 6 && px < x + w + 6 && py >= y - 6 && py < y + h + 6; }
};
static void pill(const Btn &b, uint16_t bg, uint16_t fg, const char *t) {
    ui_gfx()->fillRoundRect(b.x, b.y, b.w, b.h, b.h / 2, bg);
    ui_text_center(b.x + b.w / 2, b.y + b.h / 2, fg, t, 2);
}

// A small symbol per habit, drawn with shapes.
static void habit_icon(int h, int cx, int cy, int r) {
    Arduino_GFX *g = ui_gfx();
    uint16_t c = HABITS[h].color;
    g->fillCircle(cx, cy, r, ui_dim(c, 0.28f));
    int k = r * 6 / 10;
    switch (h) {
    case H_WATER:   // a drop
        g->fillCircle(cx, cy + k / 3, k * 6 / 10, c);
        g->fillTriangle(cx - k * 55 / 100, cy + k / 4, cx + k * 55 / 100, cy + k / 4, cx, cy - k, c);
        break;
    case H_STRETCH:   // a figure, arms up
        g->fillCircle(cx, cy - k * 7 / 10, k / 4 + 1, c);
        g->drawLine(cx, cy - k / 3, cx, cy + k / 3, c);
        g->drawLine(cx, cy - k / 3, cx - k * 7 / 10, cy - k, c);
        g->drawLine(cx, cy - k / 3, cx + k * 7 / 10, cy - k, c);
        g->drawLine(cx, cy + k / 3, cx - k / 2, cy + k, c);
        g->drawLine(cx, cy + k / 3, cx + k / 2, cy + k, c);
        break;
    case H_MIND:   // petals
        for (int i = 0; i < 6; i++) {
            float a = i * 1.0472f;
            g->drawCircle(cx + (int)(k / 2 * cosf(a)), cy + (int)(k / 2 * sinf(a)), k / 2, c);
        }
        break;
    case H_TRAIN:   // a dumbbell
        g->fillRect(cx - k, cy - 2, 2 * k, 4, c);
        g->fillRoundRect(cx - k, cy - k / 2, k / 3, k, 2, c);
        g->fillRoundRect(cx + k - k / 3, cy - k / 2, k / 3, k, 2, c);
        break;
    case H_MEALS:   // a plate
        g->drawCircle(cx, cy, k, c);
        g->fillCircle(cx, cy, k / 2, c);
        break;
    case H_SOCIAL:   // two heads
        g->fillCircle(cx - k / 2, cy - k / 4, k / 3, c);
        g->fillCircle(cx + k / 2, cy - k / 4, k / 3, c);
        g->fillRoundRect(cx - k, cy + k / 4, k, k / 2, k / 4, c);
        g->fillRoundRect(cx, cy + k / 4, k, k / 2, k / 4, c);
        break;
    case H_WIND:   // a moon
        g->fillCircle(cx, cy, k, c);
        g->fillCircle(cx + k / 2, cy - k / 3, k * 8 / 10, ui_dim(c, 0.28f));
        break;
    }
}

static void habit_value(int h, char *b, int n) {
    switch (h) {
    case H_WATER: snprintf(b, n, "%u/%u glasses", s_today.water, s_plan.w_goal); break;
    case H_STRETCH: snprintf(b, n, "%u/%u breaks", s_today.stretch, s_plan.s_goal); break;
    case H_MIND: snprintf(b, n, "%u/%u min", s_today.mind, s_plan.m_min); break;
    case H_TRAIN: snprintf(b, n, "%lu/%u min", (unsigned long)fitness_exercise_min(), s_plan.t_goal); break;
    case H_MEALS: snprintf(b, n, "%d/3 meals", __builtin_popcount(s_today.meals)); break;
    case H_SOCIAL: snprintf(b, n, "%s", s_today.social ? "Done" : (s_plan.so_who[0] ? s_plan.so_who : "Someone you like")); break;
    case H_WIND:
        if (s_today.wind) snprintf(b, n, "Done");
        else snprintf(b, n, "At %d:%02d", s_plan.wd_at / 60, s_plan.wd_at % 60);
        break;
    }
}

// =========================================================================
//  The Life app: the score and today's habits
// =========================================================================
static UiScroll s_scroll;
static int s_rows[H_COUNT], s_row_n = 0;
static const int ROW = 84, TOP = 250;
extern Screen life_meal_screen;

static void build_rows() {
    s_row_n = 0;
    for (int h = 0; h < H_COUNT; h++)
        if (habit_on(h)) s_rows[s_row_n++] = h;
}

static void life_create() {
    roll_day();
    build_rows();
    ui_scroll_reset(&s_scroll, TOP + s_row_n * ROW + 60 - LCD_HEIGHT, 0);
}

static void life_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    ui_scroll_tick(&s_scroll);
    g->fillScreen(COLOR_BG);
    int off = ui_scroll_offset(&s_scroll);
    // the score: one ring of segments, a habit each, coloured when done
    int ry = 166 - off;
    int sc = life_score();
    if (s_row_n) {
        float seg = 360.0f / s_row_n;
        for (int i = 0; i < s_row_n; i++) {
            int h = s_rows[i];
            float f = habit_frac(h);
            if (f > 1) f = 1;
            ui_arc(CX, ry, 86, 14, i * seg + 3, seg - 6, ui_dim(HABITS[h].color, 0.25f), false);
            if (f > 0.01f) ui_arc(CX, ry, 86, 14, i * seg + 3, (seg - 6) * f, HABITS[h].color, true);
        }
    }
    char b[24];
    snprintf(b, sizeof(b), "%d%%", sc);
    ui_text_center(CX, ry - 8, COLOR_TEXT, b, 5);
    ui_text_center(CX, ry + 30, COLOR_TEXT_DIM, sc >= 100 ? "A great day" : "today", 1);
    for (int i = 0; i < s_row_n; i++) {
        int h = s_rows[i];
        int y = TOP + i * ROW - off;
        if (y > LCD_HEIGHT || y + ROW < 0) continue;
        g->fillRoundRect(CX - 180, y + 6, 360, ROW - 12, 26, 0x18E3);
        habit_icon(h, CX - 140, y + ROW / 2, 24);
        ui_print(CX - 104, y + 18, 2, COLOR_TEXT, HABITS[h].name);
        habit_value(h, b, sizeof(b));
        ui_print(CX - 104, y + 46, 1, HABITS[h].color, b);
        // what a tap does
        const char *act = h == H_WATER ? "+1" : h == H_STRETCH ? "Start" : h == H_MIND ? "Breathe" : h == H_TRAIN ? "Go"
                        : h == H_MEALS ? "Ideas" : "Done";
        bool done = habit_frac(h) >= 1 && h != H_WATER;
        g->fillRoundRect(CX + 72, y + 22, 92, 40, 20, done ? COLOR_PANEL : HABITS[h].color);
        ui_text_center(CX + 118, y + 42, done ? COLOR_TEXT_DIM : COLOR_BG, done ? "Done" : act, 1);
    }
    if (!s_row_n) ui_text_center(CX, TOP + 30 - off, COLOR_TEXT_DIM, "Set up your habits in the app's Life screen", 1);
    if (off > 0) g->fillRect(0, 0, LCD_WIDTH, 60, COLOR_BG);
}

static void do_habit(int h) {
    switch (h) {
    case H_WATER: {
        if (s_today.water < 50) s_today.water++;
        s_last_water = (uint16_t)now_min();
        haptic(s_today.water == s_plan.w_goal ? HAP_SUCCESS : HAP_CLICK);
        char b[40];
        snprintf(b, sizeof(b), "Glass %u of %u", s_today.water, s_plan.w_goal);
        ui_show_toast(b, 1200);
        logged();
        break;
    }
    case H_STRETCH: ui_push(&stretch_screen); break;
    case H_MIND: ui_push(&breathe_screen); break;
    case H_TRAIN: ui_push(workout_active() ? &workout_live_screen : &workout_screen); break;
    case H_MEALS: ui_push(&life_meal_screen); break;
    case H_SOCIAL: s_today.social = 1; haptic(HAP_SUCCESS); logged(); break;
    case H_WIND:
        s_today.wind = 1;
        dnd_set(true, false);
        haptic(HAP_SUCCESS);
        ui_show_toast("Wind down: Do Not Disturb is on - lights low, screens off", 2500);
        logged();
        break;
    }
}

static void life_touch(int x, int y, bool pressed) {
    if (!ui_scroll_touch(&s_scroll, y, pressed)) return;
    int cy = y + ui_scroll_offset(&s_scroll);
    if (cy < TOP) return;
    int i = (cy - TOP) / ROW;
    if (i < 0 || i >= s_row_n) return;
    (void)x;
    do_habit(s_rows[i]);
}

static void life_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen life_screen = {
    "Life", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    life_create, life_draw, life_touch, nullptr, nullptr, life_gesture,
    500,
};

// ---- meals ---------------------------------------------------------------------------------------------
static void meal_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    static const char *const N[3] = {"Breakfast", "Lunch", "Dinner"};
    for (int i = 0; i < 3; i++) {
        int y = 84 + i * 112;
        bool done = s_today.meals & (1 << i);
        g->fillRoundRect(CX - 180, y, 360, 100, 26, 0x18E3);
        char t[24];
        snprintf(t, sizeof(t), "%s  %d:%02d", N[i], s_plan.me_at[i] / 60, s_plan.me_at[i] % 60);
        ui_print(CX - 156, y + 14, 1, HABITS[H_MEALS].color, t);
        char idea[72];
        strlcpy(idea, s_meal[i][0] ? s_meal[i] : "Ideas come from the app's Life screen", sizeof(idea));
        char a[72], b[72];
        strlcpy(a, idea, sizeof(a));
        b[0] = 0;
        if (ui_text_width(a, 2) > 250) {
            int cut = -1;
            for (int j = (int)strlen(a) - 1; j > 0; j--)
                if (a[j] == ' ') { a[j] = 0; if (ui_text_width(a, 2) <= 250) { cut = j; break; } a[j] = ' '; }
            strlcpy(a, idea, sizeof(a));
            if (cut > 0) { a[cut] = 0; strlcpy(b, idea + cut + 1, sizeof(b)); }
            ui_fit(a, 2, 250);
            ui_fit(b, 2, 250);
        }
        ui_print(CX - 156, y + 36, 2, s_meal[i][0] ? COLOR_TEXT : COLOR_TEXT_DIM, a);
        if (b[0]) ui_print(CX - 156, y + 62, 2, s_meal[i][0] ? COLOR_TEXT : COLOR_TEXT_DIM, b);
        g->fillCircle(CX + 146, y + 50, 18, done ? HABITS[H_MEALS].color : COLOR_PANEL);
        if (done) ui_text_center(CX + 146, y + 50, COLOR_BG, "v", 1);
    }
    ui_text_center(CX, 428, COLOR_TEXT_DIM, "Tap a meal when you've had it", 1);
}

static void meal_touch(int x, int y, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (!down) return;
    down = false;
    (void)x;
    int i = (y - 84) / 112;
    if (y < 84 || i < 0 || i > 2) return;
    s_today.meals ^= 1 << i;
    if (s_today.meals & (1 << i)) haptic(HAP_SUCCESS);
    logged();
}

Screen life_meal_screen = {
    "Meals", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    nullptr, meal_draw, meal_touch, nullptr, nullptr, nullptr,
    500,
};

// ---- the nudge card ------------------------------------------------------------------------------------
static void nudge_text(char *title, int tn, char *body, int bn, const char **act) {
    const LifePlan &p = s_plan;
    switch (s_nudge) {
    case H_WATER:
        snprintf(title, tn, "Time for water");
        snprintf(body, bn, "%u of %u glasses so far today", s_today.water, p.w_goal);
        *act = "Drank one";
        break;
    case H_STRETCH:
        snprintf(title, tn, "Stretch break");
        snprintf(body, bn, "3 minutes: neck, shoulders, back, wrists");
        *act = "Start";
        break;
    case H_MIND:
        snprintf(title, tn, "Take a breath");
        snprintf(body, bn, "A minute of slow breathing calms the day");
        *act = "Breathe";
        break;
    case H_TRAIN:
        snprintf(title, tn, "Time to train");
        snprintf(body, bn, "%lu of %u exercise minutes today", (unsigned long)fitness_exercise_min(), p.t_goal);
        *act = "Workout";
        break;
    case H_MEALS: {
        static const char *const N[3] = {"Breakfast", "Lunch", "Dinner"};
        snprintf(title, tn, "%s time", N[s_meal_i % 3]);
        snprintf(body, bn, "%s", s_meal[s_meal_i % 3][0] ? s_meal[s_meal_i % 3] : "Something fresh and balanced");
        *act = "Ate it";
        break;
    }
    case H_SOCIAL:
        snprintf(title, tn, "Check in");
        if (p.so_who[0]) snprintf(body, bn, "Call or text %s today", p.so_who);
        else snprintf(body, bn, "Call or text someone you like");
        *act = "Done";
        break;
    case H_WIND:
        snprintf(title, tn, "Wind down");
        snprintf(body, bn, "Lights low, screens off - sleep comes easier");
        *act = "Quiet now";
        break;
    default:
        title[0] = body[0] = 0;
        *act = "OK";
    }
}

static void nudge_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g || s_nudge < 0) return;
    g->fillScreen(COLOR_BG);
    char title[48], body[96];
    const char *act;
    nudge_text(title, sizeof(title), body, sizeof(body), &act);
    habit_icon(s_nudge, CX, 120, 46);
    ui_text_center(CX, 200, COLOR_TEXT, title, 3);
    char a[96];
    strlcpy(a, body, sizeof(a));
    ui_fit(a, 1, 340);
    ui_text_center(CX, 236, COLOR_TEXT_DIM, a, 1);
    pill({CX - 170, 280, 160, 64}, HABITS[s_nudge].color, COLOR_BG, act);
    pill({CX + 10, 280, 160, 64}, COLOR_PANEL, COLOR_TEXT, "Later");
}

static void nudge_touch(int x, int y, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (!down) return;
    down = false;
    int h = s_nudge;
    if (h < 0) return;
    if (Btn{CX - 170, 280, 160, 64}.hit(x, y)) {
        ui_pop_screen();
        if (h == H_MEALS) { s_today.meals |= 1 << s_meal_i; haptic(HAP_SUCCESS); logged(); }
        else do_habit(h);
        s_nudge = -1;
    } else if (Btn{CX + 10, 280, 160, 64}.hit(x, y)) {
        s_snooze_until[h] = millis() + 15 * 60000UL;
        if (!s_snooze_until[h]) s_snooze_until[h] = 1;
        s_nudge = -1;
        ui_pop_screen();
    }
}

static void nudge_tick() {
    if (millis() - s_nudge_ms > 60000) { s_nudge = -1; ui_pop_screen(); }   // unanswered: it goes, no fuss
}

static void nudge_destroy() {}

Screen life_nudge_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    nullptr, nudge_draw, nudge_touch, nudge_tick, nudge_destroy, nullptr,
    500,
};

// =========================================================================
//  Breathe: in 4 s, out 6 s, the circle growing and shrinking
// =========================================================================
static int s_br_len = 1;          // minutes
static uint32_t s_br_start = 0;   // 0: choosing the length
static int s_br_phase = -1;

static void br_create() { s_br_start = 0; s_br_phase = -1; }

static void br_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    const uint16_t C = HABITS[H_MIND].color;
    if (!s_br_start) {
        habit_icon(H_MIND, CX, 120, 46);
        ui_text_center(CX, 196, COLOR_TEXT, "Breathe", 3);
        ui_text_center(CX, 228, COLOR_TEXT_DIM, "Follow the circle: in as it grows, out as it shrinks", 1);
        static const int L[3] = {1, 3, 5};
        for (int i = 0; i < 3; i++) {
            int x = CX - 120 + i * 120;
            g->fillCircle(x, 300, 44, L[i] == s_br_len ? C : COLOR_PANEL);
            char b[8];
            snprintf(b, sizeof(b), "%d", L[i]);
            ui_text_center(x, 292, L[i] == s_br_len ? COLOR_BG : COLOR_TEXT, b, 3);
            ui_text_center(x, 322, L[i] == s_br_len ? COLOR_BG : COLOR_TEXT_DIM, "min", 1);
        }
        pill({CX - 80, 366, 160, 52}, C, COLOR_BG, "Start");
        return;
    }
    uint32_t el = millis() - s_br_start;
    uint32_t total = s_br_len * 60000UL;
    if (el >= total) {
        ui_text_center(CX, CY - 20, COLOR_TEXT, "Well done", 3);
        char b[40];
        snprintf(b, sizeof(b), "%u mindful minutes today", s_today.mind);
        ui_text_center(CX, CY + 20, COLOR_TEXT_DIM, b, 1);
        return;
    }
    uint32_t cyc = el % 10000;   // 4 s in, 6 s out
    bool in = cyc < 4000;
    float f = in ? cyc / 4000.0f : 1.0f - (cyc - 4000) / 6000.0f;
    float e = 0.5f - 0.5f * cosf(f * 3.14159f);   // smooth
    int r = 50 + (int)(e * 120);
    for (int i = 0; i < 6; i++) {   // petals, as Apple's Breathe
        float a = i * 1.0472f + e * 0.6f;
        int pr = r / 2 + 6;
        g->fillCircle(CX + (int)(r / 2 * cosf(a)), CY - 20 + (int)(r / 2 * sinf(a)), pr,
                      ui_dim(C, (i % 2 ? 0.30f : 0.45f) + 0.25f * e));   // two shades, so the petals show
    }
    ui_text_center(CX, CY + 160, COLOR_TEXT, in ? "Breathe in" : "Breathe out", 2);
    int ph = in ? 0 : 1;
    if (ph != s_br_phase) { s_br_phase = ph; haptic(HAP_CLICK); }
    ui_arc(CX, CY, 226, 4, 0, 360.0f * el / total, C, true);
}

static void br_tick() {
    if (!s_br_start) return;
    sleep_register_activity();   // the screen stays on through it
    if (millis() - s_br_start >= s_br_len * 60000UL && s_br_phase != 9) {
        s_br_phase = 9;
        s_today.mind += s_br_len;
        haptic(HAP_SUCCESS);
        logged();
    }
}

static void br_touch(int x, int y, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (!down) return;
    down = false;
    if (s_br_start) {
        if (s_br_phase == 9) ui_pop_screen();
        return;
    }
    static const int L[3] = {1, 3, 5};
    for (int i = 0; i < 3; i++) {
        int cx = CX - 120 + i * 120;
        if ((x - cx) * (x - cx) + (y - 300) * (y - 300) < 48 * 48) s_br_len = L[i];
    }
    if (Btn{CX - 80, 366, 160, 52}.hit(x, y)) { s_br_start = millis(); s_br_phase = -1; }
}

Screen breathe_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_SMOOTH,
    br_create, br_draw, br_touch, br_tick, nullptr, nullptr,
    0, true,
};

// =========================================================================
//  Stretch: six moves, 30 s each, a ring counting down
// =========================================================================
static const struct { const char *name, *how; } MOVES[] = {
    {"Neck rolls", "Slow circles, both ways"},
    {"Shoulder shrugs", "Up to the ears, then drop"},
    {"Chest opener", "Hands behind you, lift the chest"},
    {"Side bends", "One arm over, then the other"},
    {"Forward fold", "Soft knees, let the head hang"},
    {"Wrist stretch", "Arm out, pull the fingers back gently"},
};
static const int MOVE_N = sizeof(MOVES) / sizeof(MOVES[0]);
static uint32_t s_st_start = 0;
static int s_st_move = -1;

static void st_create() { s_st_start = millis(); s_st_move = -1; }

static void st_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    const uint16_t C = HABITS[H_STRETCH].color;
    uint32_t el = millis() - s_st_start;
    int mv = (int)(el / 30000);
    if (mv >= MOVE_N) {
        habit_icon(H_STRETCH, CX, 150, 50);
        ui_text_center(CX, 236, COLOR_TEXT, "All done", 3);
        ui_text_center(CX, 272, COLOR_TEXT_DIM, "Your body says thanks", 1);
        return;
    }
    float f = (el % 30000) / 30000.0f;
    ui_arc(CX, CY, 214, 14, 0, 360, ui_dim(C, 0.25f), false);
    ui_arc(CX, CY, 214, 14, 0, 360 * (1 - f), C, true);
    char b[16];
    snprintf(b, sizeof(b), "%d", 30 - (int)(el % 30000 / 1000));
    habit_icon(H_STRETCH, CX, 136, 34);
    ui_text_center(CX, 210, COLOR_TEXT, MOVES[mv].name, 3);
    char how[72];
    strlcpy(how, MOVES[mv].how, sizeof(how));
    ui_fit(how, 1, 320);
    ui_text_center(CX, 248, COLOR_TEXT_DIM, how, 1);
    ui_text_center(CX, 304, C, b, 5);
    snprintf(b, sizeof(b), "%d of %d", mv + 1, MOVE_N);
    ui_text_center(CX, 352, COLOR_TEXT_DIM, b, 1);
}

static void st_tick() {
    sleep_register_activity();
    int mv = (int)((millis() - s_st_start) / 30000);
    if (mv != s_st_move) {
        s_st_move = mv;
        if (mv >= MOVE_N) {
            if (s_today.stretch < 50) s_today.stretch++;
            s_last_stretch = (uint16_t)now_min();
            haptic(HAP_SUCCESS);
            logged();
        } else if (mv > 0) {
            haptic(HAP_NOTIFY);   // the next move
        }
    }
}

static void st_touch(int, int, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (down && s_st_move >= MOVE_N) ui_pop_screen();
    down = false;
}

Screen stretch_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    st_create, st_draw, st_touch, st_tick, nullptr, nullptr,
    0, true,
};
