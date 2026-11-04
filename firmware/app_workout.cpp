// Workout app - see app_workout.h.
#include "haptics.h"
#include "app_workout.h"
#include "store.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "fitness.h"
#include "phone_link.h"
#include "app_calendar.h"
#include "hal_audio.h"
#include "hal_imu.h"
#include "hal_sleep.h"
#if FEATURE_GPS
#include "hal_gps.h"
#endif
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

struct WoType { const char *name; uint8_t met_x10; bool outdoor; float stride_m; uint16_t color; };
static const WoType TYPES[] = {
    {"Outdoor Walk", 38, true, 0.75f, 0x9F45}, {"Outdoor Run", 95, true, 1.05f, 0x9F45},
    {"Indoor Walk", 35, false, 0.70f, 0x9F45}, {"Indoor Run", 90, false, 1.00f, 0x9F45},
    {"Cycling", 75, true, 0, 0x9F45},          {"Hiking", 60, true, 0.70f, 0x9F45},
    {"Strength", 50, false, 0, 0xF8AA},        {"HIIT", 80, false, 0, 0xF8AA},
    {"Other", 45, false, 0, 0x1F5D},
};
static const int TYPE_N = sizeof(TYPES) / sizeof(TYPES[0]);

// ---- the running workout -----------------------------------------------------------------------
static bool     s_on = false, s_paused = false;
static int      s_type = 0;
static uint32_t s_start_epoch = 0;     // local epoch seconds
static uint32_t s_active_ms = 0;       // counted before the current stretch
static uint32_t s_stretch_ms = 0;      // millis() the current (unpaused) stretch began
static uint32_t s_steps = 0, s_last_steps = 0;
static float    s_gps_m = 0;
static bool     s_gps_used = false, s_gps_was_on = false;
static double   s_last_lat = 0, s_last_lon = 0;
static bool     s_have_last = false;
static uint32_t s_last_sec_ms = 0;
static int      s_still_windows = 0, s_brisk_windows = 0, s_brisk_kind = ACT_WALK;
static uint32_t s_last_prompt_ms = 0;
static bool     s_prompted_end = false;

bool workout_active() { return s_on; }
bool workout_paused() { return s_on && s_paused; }
uint32_t workout_elapsed_s() { return (s_active_ms + (s_on && !s_paused ? millis() - s_stretch_ms : 0)) / 1000; }
const char *workout_type_name() { return TYPES[s_type].name; }

float workout_km() {
    if (s_gps_used && s_gps_m > 0) return s_gps_m / 1000.0f;
    return s_steps * TYPES[s_type].stride_m * fitness_stride_scale() / 1000.0f;
}

static float workout_kcal() {
    float per_min = (TYPES[s_type].met_x10 / 10.0f - 1.0f) * 3.5f * fitness_kg() / 200.0f;
    return per_min * workout_elapsed_s() / 60.0f;
}

static bool has_distance() { return TYPES[s_type].stride_m > 0 || (s_gps_used && s_gps_m > 0); }

// ---- saved ones ------------------------------------------------------------------------------------
struct WoRecord {
    uint32_t start, dur_s, dist_m, steps;
    uint16_t kcal;
    uint8_t  type, flags;   // bit 0: sent, bit 1: GPS
};
// oldest first: 100 in the store (store.h), the last 20 in NVS when there's nowhere else
static const int MAX_REC = 100, NVS_REC = 20;
static WoRecord s_rec[MAX_REC];
static int s_rec_n = 0;

static void save_records() {
    if (store_ok() && store_write("workouts", s_rec, sizeof(WoRecord) * s_rec_n)) {
        Preferences p;
        if (p.begin("wo", false)) {
            if (p.isKey("r")) { p.remove("r"); p.remove("n"); }   // moved: frees NVS
            p.end();
        }
        return;
    }
    Preferences p;
    if (!p.begin("wo", false)) return;
    int skip = s_rec_n > NVS_REC ? s_rec_n - NVS_REC : 0;
    p.putUChar("n", (uint8_t)(s_rec_n - skip));
    p.putBytes("r", s_rec + skip, sizeof(WoRecord) * (s_rec_n - skip));
    p.end();
}

void workout_init() {
    int sz = store_size("workouts");
    if (sz >= 0 && sz % (int)sizeof(WoRecord) == 0) {
        int n = store_read("workouts", s_rec, sizeof(s_rec));
        s_rec_n = n > 0 ? n / (int)sizeof(WoRecord) : 0;
        return;
    }
    Preferences p;
    if (!p.begin("wo", true)) return;
    s_rec_n = p.getUChar("n", 0);
    if (s_rec_n > NVS_REC || (s_rec_n && p.getBytes("r", s_rec, sizeof(WoRecord) * s_rec_n) != sizeof(WoRecord) * s_rec_n))
        s_rec_n = 0;
    p.end();
    if (s_rec_n && store_ok()) save_records();   // the ones from before move to the store
}

static bool send_record(const WoRecord &r) {
    JsonDocument d;
    d["e"] = "wo";
    d["ty"] = TYPES[r.type < TYPE_N ? r.type : TYPE_N - 1].name;
    d["st"] = r.start;
    d["du"] = r.dur_s;
    d["m"] = r.dist_m;
    d["kc"] = r.kcal;
    d["stp"] = r.steps;
    d["gps"] = (r.flags & 2) ? 1 : 0;
    return phone_link_send(d);
}

void workout_send_pending() {
    for (int i = 0; i < s_rec_n; i++)
        if (!(s_rec[i].flags & 1)) send_record(s_rec[i]);
}

void workout_on_ack(uint32_t start) {
    for (int i = 0; i < s_rec_n; i++)
        if (s_rec[i].start == start && !(s_rec[i].flags & 1)) { s_rec[i].flags |= 1; save_records(); }
}

// ---- start, pause, end -----------------------------------------------------------------------------
static void gps_on(bool on) {
#if FEATURE_GPS
    if (on) {
        s_gps_used = TYPES[s_type].outdoor && gps_available();
        if (!s_gps_used) return;
        s_gps_was_on = gps_is_enabled();
        if (!s_gps_was_on) gps_enable();
    } else if (s_gps_used && !s_gps_was_on) {
        gps_disable();
    }
#else
    (void)on;
    s_gps_used = false;
#endif
}

// back_s: started that long ago (a detected workout counts from its start)
static void start(int type, uint32_t back_s, uint32_t back_steps) {
    s_type = type;
    s_on = true;
    s_paused = false;
    s_start_epoch = calendar_now() - back_s;
    s_active_ms = back_s * 1000;
    s_stretch_ms = millis();
    s_steps = back_steps;
    s_last_steps = imu_get_step_count();
    s_gps_m = 0;
    s_have_last = false;
    s_still_windows = 0;
    s_prompted_end = false;
    gps_on(true);
    fitness_workout(true, TYPES[type].met_x10);
    haptic(HAP_START);
}

static void set_paused(bool p) {
    if (!s_on || p == s_paused) return;
    if (p) s_active_ms += millis() - s_stretch_ms;
    else { s_stretch_ms = millis(); s_last_steps = imu_get_step_count(); s_have_last = false; }
    s_paused = p;
    fitness_workout(!p, TYPES[s_type].met_x10);
    haptic(p ? HAP_STOP : HAP_START);
}

static WoRecord s_summary;
extern Screen workout_summary_screen;

static void finish() {
    if (!s_on) return;
    set_paused(true);
    WoRecord r = {s_start_epoch, workout_elapsed_s(), (uint32_t)(workout_km() * 1000), s_steps, (uint16_t)workout_kcal(),
                  (uint8_t)s_type, (uint8_t)(s_gps_used && s_gps_m > 0 ? 2 : 0)};
    if (!has_distance()) r.dist_m = 0;
    s_on = false;
    s_paused = false;
    gps_on(false);
    fitness_workout(false, 0);
    s_summary = r;
    if (r.dur_s >= 60) {   // under a minute: not worth keeping
        if (s_rec_n == MAX_REC) { memmove(s_rec, s_rec + 1, sizeof(WoRecord) * (MAX_REC - 1)); s_rec_n--; }
        send_record(r);   // marked sent when the phone answers ("woack")
        s_rec[s_rec_n++] = r;
        save_records();
    }
}

static double haversine_m(double la1, double lo1, double la2, double lo2) {
    const double R = 6371000.0, D = 3.14159265358979 / 180.0;
    double dla = (la2 - la1) * D, dlo = (lo2 - lo1) * D;
    double a = sin(dla / 2) * sin(dla / 2) + cos(la1 * D) * cos(la2 * D) * sin(dlo / 2) * sin(dlo / 2);
    return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

extern Screen workout_detect_screen;
static int s_detect_type = 0;
static uint32_t s_detect_back_s = 0, s_detect_steps = 0;
static bool s_detect_end = false;   // the prompt is "Still working out?"

void workout_update() {
    if (!s_on || s_paused) return;
    uint32_t ms = millis();
    if (ms - s_last_sec_ms < 1000) return;
    s_last_sec_ms = ms;
    uint32_t st = imu_get_step_count();
    if (st >= s_last_steps) s_steps += st - s_last_steps;
    s_last_steps = st;
#if FEATURE_GPS
    if (s_gps_used) {
        GpsFix f = gps_get_fix();
        if (f.has_fix && f.hdop > 0 && f.hdop < 4.0f) {
            if (s_have_last) {
                double d = haversine_m(s_last_lat, s_last_lon, f.latitude, f.longitude);
                if (d >= 2.0 && d < 60.0) {   // standing-still jitter and jumps left out
                    s_gps_m += (float)d;
                    s_last_lat = f.latitude;
                    s_last_lon = f.longitude;
                } else if (d >= 60.0) {
                    s_last_lat = f.latitude;   // a jump: start again from here
                    s_last_lon = f.longitude;
                }
            } else {
                s_last_lat = f.latitude;
                s_last_lon = f.longitude;
                s_have_last = true;
            }
        }
    }
#endif
}

void workout_feed(Activity a, int cadence) {
    uint32_t now = millis();
    if (s_on) {
        if (s_paused) return;
        s_still_windows = a == ACT_STILL ? s_still_windows + 1 : 0;
        if (s_still_windows >= 180 && !s_prompted_end) {   // 15 minutes
            s_prompted_end = true;
            s_detect_end = true;
            sleep_register_activity();
            if (!ui_screen_on_top(&workout_detect_screen)) ui_push(&workout_detect_screen);
        }
        return;
    }
    bool brisk = a == ACT_RUN || (a == ACT_WALK && cadence >= 100);
    if (!brisk) { s_brisk_windows = 0; return; }
    if (s_brisk_windows == 0) s_brisk_kind = a;
    if (a == ACT_RUN) s_brisk_kind = ACT_RUN;
    s_brisk_windows++;
    if (s_brisk_windows < 120) return;   // 10 minutes
    if (s_last_prompt_ms && now - s_last_prompt_ms < 60 * 60000UL) return;
    s_last_prompt_ms = now ? now : 1;
    s_brisk_windows = 0;
    s_detect_end = false;
    s_detect_type = s_brisk_kind == ACT_RUN ? 1 : 0;
    s_detect_back_s = 600;
    s_detect_steps = (uint32_t)(cadence * 10);
    sleep_register_activity();
    haptic(HAP_REMINDER);
    if (!ui_screen_on_top(&workout_detect_screen)) ui_push(&workout_detect_screen);
}

// ---- drawing helpers --------------------------------------------------------------------------------
struct Btn {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x - 6 && px < x + w + 6 && py >= y - 6 && py < y + h + 6; }
};
static void pill(const Btn &b, uint16_t bg, uint16_t fg, const char *t) {
    ui_gfx()->fillRoundRect(b.x, b.y, b.w, b.h, b.h / 2, bg);
    ui_text_center(b.x + b.w / 2, b.y + b.h / 2, fg, t, 2);
}
static void fmt_time(char *b, int n, uint32_t s) {
    if (s >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}
static void fmt_pace(char *b, int n, uint32_t secs, float km) {
    if (km < 0.05f) { snprintf(b, n, "--'--\""); return; }
    uint32_t p = (uint32_t)(secs / km);
    if (p >= 3600) snprintf(b, n, "--'--\"");
    else snprintf(b, n, "%lu'%02lu\"", (unsigned long)(p / 60), (unsigned long)(p % 60));
}
// A small runner, the app's symbol.
static void runner(Arduino_GFX *g, int cx, int cy, int r, uint16_t c) {
    g->fillCircle(cx + r / 5, cy - r * 7 / 10, r / 5, c);
    g->drawLine(cx + r / 10, cy - r / 2, cx - r / 10, cy + r / 6, c);
    g->drawLine(cx + r / 10 + 1, cy - r / 2, cx - r / 10 + 1, cy + r / 6, c);
    g->drawLine(cx - r / 10, cy + r / 6, cx - r / 2, cy + r * 7 / 10, c);
    g->drawLine(cx - r / 10, cy + r / 6, cx + r / 3, cy + r / 2, c);
    g->drawLine(cx + r / 3, cy + r / 2, cx + r / 4, cy + r * 8 / 10, c);
    g->drawLine(cx, cy - r / 3, cx + r / 2, cy - r / 8, c);
    g->drawLine(cx, cy - r / 3, cx - r / 2, cy - r / 5, c);
}

// ---- choosing a type ------------------------------------------------------------------------------------
static UiScroll s_scroll;
static const int ROW = 92, TOP = 92;
static uint32_t s_countdown_ms = 0;
static int s_countdown_type = -1;

static void list_create() {
    ui_scroll_reset(&s_scroll, TYPE_N * ROW - (LCD_HEIGHT - TOP - 40), 0);
    s_countdown_type = -1;
}

static void list_tick() {
    if (s_on && s_countdown_type < 0) { ui_pop_screen(); ui_push_instant(&workout_live_screen); return; }
    if (s_countdown_type >= 0 && millis() - s_countdown_ms >= 3000) {
        start(s_countdown_type, 0, 0);
        s_countdown_type = -1;
        ui_pop_screen();
        ui_push_instant(&workout_live_screen);
    }
}

static void list_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    if (s_countdown_type >= 0) {
        uint32_t el = millis() - s_countdown_ms;
        int n = 3 - (int)(el / 1000);
        float f = (el % 1000) / 1000.0f;
        ui_arc(CX, CY, 180, 16, 0, 360, ui_dim(COLOR_GOOD, 0.25f), false);
        ui_arc(CX, CY, 180, 16, 0, 360 * (1 - f), COLOR_GOOD, true);
        char b[4];
        snprintf(b, sizeof(b), "%d", n < 1 ? 1 : n);
        ui_text_center(CX, CY - 10, COLOR_TEXT, b, 6);
        ui_text_center(CX, CY + 60, COLOR_TEXT_DIM, TYPES[s_countdown_type].name, 2);
        return;
    }
    ui_scroll_tick(&s_scroll);
    int off = ui_scroll_offset(&s_scroll);
    for (int i = 0; i < TYPE_N; i++) {
        int y = TOP + i * ROW - off;
        if (y > LCD_HEIGHT || y + ROW < 0) continue;
        g->fillRoundRect(CX - 180, y + 6, 360, ROW - 12, 26, 0x18E3);
        g->fillCircle(CX - 136, y + ROW / 2, 26, ui_dim(TYPES[i].color, 0.3f));
        runner(g, CX - 136, y + ROW / 2, 22, TYPES[i].color);
        ui_print(CX - 96, y + ROW / 2 - 18, 2, COLOR_TEXT, TYPES[i].name);
        bool gps = false;
#if FEATURE_GPS
        gps = TYPES[i].outdoor && gps_available();
#endif
        ui_print(CX - 96, y + ROW / 2 + 8, 1, COLOR_TEXT_DIM, gps ? "GPS distance" : TYPES[i].stride_m > 0 ? "Distance from steps" : "Time and calories");
    }
    g->fillRect(0, 0, LCD_WIDTH, 84, COLOR_BG);
}

static void list_touch(int, int y, bool pressed) {
    if (s_countdown_type >= 0) return;
    if (!ui_scroll_touch(&s_scroll, y, pressed) || y < TOP) return;
    int i = (y - TOP + ui_scroll_offset(&s_scroll)) / ROW;
    if (i < 0 || i >= TYPE_N) return;
    s_countdown_type = i;
    s_countdown_ms = millis();
    audio_play_sfx(988, 60);
}

static void list_gesture(Gesture g) {
    if (s_countdown_type >= 0) { s_countdown_type = -1; return; }
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen workout_screen = {
    "Workout", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    list_create, list_draw, list_touch, list_tick, nullptr, list_gesture,
    250,
};

// ---- live ---------------------------------------------------------------------------------------------
static bool s_controls = false;

static void live_create() { s_controls = false; }

static void live_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    if (!s_on) { ui_text_center(CX, CY, COLOR_TEXT_DIM, "No workout", 2); return; }
    const WoType &t = TYPES[s_type];
    uint32_t el = workout_elapsed_s();
    if (s_controls) {
        ui_text_center(CX, 110, COLOR_TEXT_DIM, t.name, 2);
        pill({CX - 170, 160, 160, 120}, s_paused ? COLOR_GOOD : COLOR_WARN, COLOR_BG, s_paused ? "Resume" : "Pause");
        pill({CX + 10, 160, 160, 120}, COLOR_BAD, COLOR_TEXT, "End");
        ui_text_center(CX, 330, COLOR_TEXT_DIM, "Tap outside to go back", 1);
        return;
    }
    runner(g, CX - 140, 104, 16, t.color);
    ui_print(CX - 116, 94, 1, t.color, t.name);
    char b[24];
    fmt_time(b, sizeof(b), el);
    ui_print(CX - 150, 140, 5, s_paused ? COLOR_TEXT_DIM : COLOR_WARN, b);
    if (s_paused) ui_print(CX - 150, 188, 1, COLOR_WARN, "PAUSED");
    int y = 216;
    snprintf(b, sizeof(b), "%d", (int)workout_kcal());
    ui_print(CX - 150, y, 4, COLOR_TEXT, b);
    ui_print(CX - 150 + ui_text_width(b, 4) + 8, y + 12, 1, FIT_MOVE_COLOR, "ACTIVE KCAL");
    y += 52;
    if (has_distance()) {
        float km = workout_km();
        snprintf(b, sizeof(b), "%.2f", km);
        ui_print(CX - 150, y, 4, COLOR_TEXT, b);
        ui_print(CX - 150 + ui_text_width(b, 4) + 8, y + 12, 1, COLOR_ACCENT2, s_gps_used && s_gps_m > 0 ? "KM (GPS)" : "KM");
        y += 52;
        fmt_pace(b, sizeof(b), el, km);
        ui_print(CX - 150, y, 4, COLOR_TEXT, b);
        ui_print(CX - 150 + ui_text_width(b, 4) + 8, y + 12, 1, COLOR_ACCENT3, "AVG PACE /KM");
    } else {
        snprintf(b, sizeof(b), "%lu", (unsigned long)s_steps);
        ui_print(CX - 150, y, 4, COLOR_TEXT, b);
        ui_print(CX - 150 + ui_text_width(b, 4) + 8, y + 12, 1, COLOR_ACCENT2, "STEPS");
    }
    // the Exercise ring, at the bottom
    float ex = (float)fitness_exercise_min() / fitness_exercise_goal();
    ui_arc(CX, 400, 22, 8, 0, 360, ui_dim(FIT_EXERCISE_COLOR, 0.25f), false);
    if (ex > 0.01f) ui_arc(CX, 400, 22, 8, 0, 360 * (ex > 1 ? 1 : ex), FIT_EXERCISE_COLOR, true);
}

static void live_touch(int x, int y, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (!down) return;
    down = false;
    if (!s_on) return;
    if (!s_controls) { s_controls = true; return; }
    if (Btn{CX - 170, 160, 160, 120}.hit(x, y)) { set_paused(!s_paused); s_controls = false; }
    else if (Btn{CX + 10, 160, 160, 120}.hit(x, y)) {
        finish();
        ui_pop_screen();
        ui_push(&workout_summary_screen);
    } else s_controls = false;
}

static void live_gesture(Gesture g) {
    if (s_controls) { s_controls = false; return; }
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();   // it keeps going
}

Screen workout_live_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    live_create, live_draw, live_touch, nullptr, nullptr, live_gesture,
    1000,
    true,   // suppress_idle: the screen stays on while it runs
};

// ---- the summary ------------------------------------------------------------------------------------------
static void sum_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    const WoRecord &r = s_summary;
    const WoType &t = TYPES[r.type < TYPE_N ? r.type : 0];
    runner(g, CX, 96, 22, t.color);
    ui_text_center(CX, 140, COLOR_TEXT, t.name, 3);
    char b[32], c[24];
    fmt_time(c, sizeof(c), r.dur_s);
    struct { const char *label; uint16_t col; } rows[4] = {{"Total time", COLOR_WARN}, {"Active kcal", FIT_MOVE_COLOR},
                                                          {"Distance", COLOR_ACCENT2}, {"Avg pace", COLOR_ACCENT3}};
    int n = r.dist_m ? 4 : 2;
    for (int i = 0; i < n; i++) {
        int y = 186 + i * 46;
        if (i == 0) strlcpy(b, c, sizeof(b));
        else if (i == 1) snprintf(b, sizeof(b), "%u", (unsigned)r.kcal);
        else if (i == 2) snprintf(b, sizeof(b), "%.2f km", r.dist_m / 1000.0f);
        else fmt_pace(b, sizeof(b), r.dur_s, r.dist_m / 1000.0f);
        ui_print(CX - 150, y, 2, COLOR_TEXT_DIM, rows[i].label);
        ui_print(CX + 150 - ui_text_width(b, 3), y - 4, 3, rows[i].col, b);
    }
    if (r.dur_s < 60) ui_text_center(CX, 380, COLOR_TEXT_DIM, "Under a minute: not saved", 1);
    pill({CX - 80, 384, 160, 52}, COLOR_PANEL, COLOR_TEXT, "Done");
}

static void sum_touch(int x, int y, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (down && Btn{CX - 80, 384, 160, 52}.hit(x, y)) ui_pop_screen();
    down = false;
}

Screen workout_summary_screen = {
    "Summary", GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    nullptr, sum_draw, sum_touch, nullptr, nullptr, nullptr,
    500,
};

// ---- "Record a workout?" / "Still working out?" -------------------------------------------------------------
static void det_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(COLOR_BG);
    g->fillCircle(CX, 120, 46, ui_dim(FIT_EXERCISE_COLOR, 0.3f));
    runner(g, CX, 120, 34, FIT_EXERCISE_COLOR);
    if (s_detect_end) {
        ui_text_center(CX, 200, COLOR_TEXT, "Still working out?", 3);
        ui_text_center(CX, 236, COLOR_TEXT_DIM, "You haven't moved in 15 minutes", 1);
        pill({CX - 170, 274, 160, 64}, COLOR_BAD, COLOR_TEXT, "End");
        pill({CX + 10, 274, 160, 64}, COLOR_PANEL, COLOR_TEXT, "Keep going");
    } else {
        char b[48];
        snprintf(b, sizeof(b), "Looks like an %s", TYPES[s_detect_type].name);
        ui_text_center(CX, 200, COLOR_TEXT, b, 2);
        ui_text_center(CX, 232, COLOR_TEXT_DIM, "Record it? It counts from 10 minutes ago", 1);
        pill({CX - 170, 274, 160, 64}, COLOR_GOOD, COLOR_BG, "Record");
        pill({CX + 10, 274, 160, 64}, COLOR_PANEL, COLOR_TEXT, "Not now");
    }
}

static void det_touch(int x, int y, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (!down) return;
    down = false;
    bool left = Btn{CX - 170, 274, 160, 64}.hit(x, y), right = Btn{CX + 10, 274, 160, 64}.hit(x, y);
    if (!left && !right) return;
    ui_pop_screen();
    if (s_detect_end) {
        if (left) { finish(); ui_push(&workout_summary_screen); }
        else s_still_windows = 0;
    } else if (left && !s_on) {
        start(s_detect_type, s_detect_back_s, s_detect_steps);
        ui_push(&workout_live_screen);
    }
}

Screen workout_detect_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_DEFAULT,
    nullptr, det_draw, det_touch, nullptr, nullptr, nullptr,
    500,
};
