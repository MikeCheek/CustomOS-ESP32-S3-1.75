/*
 * game_roulette.cpp - Roulette, in the Casino (casino.h)
 *
 * A single-zero wheel (37 pockets, in the usual wheel order), bets from
 * the Casino's shared wallet (casino_wallet.h). Play money only.
 *
 * Betting: pick a chip (5 / 25 / 100, the chip at the top cycles) and
 * tap the bets to put it on them - each tap adds a chip:
 *   Red, Black, Odd, Even, 1-18, 19-36 ............ pay 1:1
 *   1st 12, 2nd 12, 3rd 12 ........................ pay 2:1
 *   Numbers (a table of 0-36, any of them) ........ pay 35:1
 * Zero loses every outside bet. Clear takes the chips back; Spin.
 *
 * The spin: the wheel turns one way and slows, the ball runs the other
 * way round the rim, drops in toward the pockets and settles where the
 * result (drawn when Spin is pressed) is - ticking as it passes the
 * frets. Then the number, what each bet won, and the last numbers.
 * Tap to bet again: the same bets stay, if you can afford them.
 * Swipe sideways: back to the Casino.
 */
#include "game_roulette.h"
#include "casino_wallet.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "hal_audio.h"
#include "app_settings_state.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const uint8_t WHEEL[37] = {0, 32, 15, 19, 4, 21, 2, 25, 17, 34, 6, 27, 13, 36, 11, 30, 8, 23, 10,
                                  5, 24, 16, 33, 1, 20, 14, 31, 9, 22, 18, 29, 7, 28, 12, 35, 3, 26};
static bool is_red(int n) {
    static const uint8_t R[18] = {1, 3, 5, 7, 9, 12, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 36};
    for (uint8_t r : R) if (r == n) return true;
    return false;
}
static const uint16_t C_RED = COLOR565(0xc8, 0x20, 0x2c), C_BLACK = COLOR565(0x20, 0x20, 0x24),
                      C_GREEN = COLOR565(0x10, 0x90, 0x48), C_WOOD = COLOR565(0x6a, 0x3c, 0x1c),
                      C_GOLD = COLOR565(0xe8, 0xc0, 0x50), C_FELT = COLOR565(0x0c, 0x50, 0x2c);
static uint16_t num_color(int n) { return n == 0 ? C_GREEN : is_red(n) ? C_RED : C_BLACK; }

// ---- bets -------------------------------------------------------------------------
enum Outside { B_RED, B_BLACK, B_ODD, B_EVEN, B_LOW, B_HIGH, B_D1, B_D2, B_D3, B_OUTSIDE };
static const char *const OUT_NAME[B_OUTSIDE] = {"Red", "Black", "Odd", "Even", "1-18", "19-36", "1st 12", "2nd 12", "3rd 12"};
static int32_t s_out[B_OUTSIDE];
static int32_t s_num[37];
static int32_t s_last_out[B_OUTSIDE], s_last_num[37];   // the previous round's, for "same bets"
static const int CHIPS[3] = {5, 25, 100};
static int s_chip = 0;

static bool out_wins(int b, int n) {
    if (n == 0) return false;
    switch (b) {
    case B_RED: return is_red(n);
    case B_BLACK: return !is_red(n);
    case B_ODD: return n % 2 == 1;
    case B_EVEN: return n % 2 == 0;
    case B_LOW: return n <= 18;
    case B_HIGH: return n >= 19;
    case B_D1: return n <= 12;
    case B_D2: return n >= 13 && n <= 24;
    default: return n >= 25;
    }
}
static int out_pays(int b) { return b >= B_D1 ? 2 : 1; }

static int32_t total_bet() {
    int32_t t = 0;
    for (int32_t v : s_out) t += v;
    for (int32_t v : s_num) t += v;
    return t;
}

// ---- state ------------------------------------------------------------------------
enum RState { R_BET, R_NUMBERS, R_SPIN, R_RESULT };
static RState s_state;
static int32_t s_credit;
static int s_result = 0;
static int32_t s_won = 0, s_staked = 0;
static uint8_t s_hist[10];
static int s_hist_n = 0;
static uint32_t s_spin_t0 = 0, s_msg_ms = 0;
static const char *s_msg = "";
static float s_w0 = 0, s_b_end = 0;        // the wheel's start angle, the ball's resting angle
static int s_last_fret = -1;
static bool s_prev_pressed = false;
static int s_press = -1;                    // a button lit while held
static const uint32_t SPIN_MS = 5200;

static void say(const char *m) { s_msg = m; s_msg_ms = millis(); }

// ---- layout: the betting page --------------------------------------------------------
// buttons: 0-8 the outside bets, 9 numbers, 10 clear, 11 spin, 12 the chip
struct Rect { int x, y, w, h; bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };
static Rect bet_rect(int i) {
    if (i < 6) {   // two rows of three: Red Black Odd / Even 1-18 19-36
        int r = i / 3, c = i % 3;
        return {CX - 165 + c * 112, 200 + r * 54, 106, 46};
    }
    if (i < 9) return {CX - 165 + (i - 6) * 112, 308, 106, 40};   // the dozens
    if (i == 9) return {CX - 110, 352, 220, 38};                   // numbers
    if (i == 10) return {CX - 104, 396, 98, 38};                   // clear (inside the circle's edge)
    if (i == 11) return {CX + 6, 396, 98, 38};                     // spin
    return {CX - 34, 84, 68, 68};                                  // the chip
}

// the numbers table: 0 on top, 1-36 in 6 x 6
static Rect num_rect(int n) {
    const int S = 46, X0 = CX - 3 * S, Y0 = 108;
    if (n == 0) return {CX - 70, 58, 140, 42};
    int i = n - 1;
    return {X0 + (i % 6) * S, Y0 + (i / 6) * S, S - 4, S - 4};
}
static const Rect NUM_DONE = {CX - 70, 392, 140, 44};

static void chip_mark(Arduino_GFX *g, int x, int y, int32_t amount) {
    if (amount <= 0) return;
    char b[12];
    snprintf(b, sizeof(b), amount >= 1000 ? "%ldk" : "%ld", (long)(amount >= 1000 ? amount / 1000 : amount));
    g->fillCircle(x, y, 14, C_GOLD);
    g->fillCircle(x, y, 10, COLOR565(0xb0, 0x80, 0x20));
    ui_text_center(x, y, COLOR_TEXT, b, 1);
}

static void draw_button(Arduino_GFX *g, const Rect &r, uint16_t bg, const char *label, bool lit, int size = 2) {
    g->fillRoundRect(r.x, r.y, r.w, r.h, 12, lit ? ui_dim(bg, 0.6f) : bg);
    ui_text_center(r.x + r.w / 2, r.y + r.h / 2, COLOR_TEXT, label, size);
}

// ---- the wheel ------------------------------------------------------------------------
static void draw_wheel(Arduino_GFX *g, int cx, int cy, int r, float ang, bool numbers) {
    g->fillCircle(cx, cy, r + 8, C_WOOD);
    g->fillCircle(cx, cy, r + 3, COLOR565(0x3a, 0x22, 0x10));
    const float step = 2 * 3.14159265f / 37;
    int ri = r * 62 / 100;
    for (int i = 0; i < 37; i++) {
        float a0 = ang + i * step, a1 = a0 + step;
        uint16_t c = num_color(WHEEL[i]);
        int x0 = cx + (int)(cosf(a0) * r), y0 = cy + (int)(sinf(a0) * r);
        int x1 = cx + (int)(cosf(a1) * r), y1 = cy + (int)(sinf(a1) * r);
        int x2 = cx + (int)(cosf(a0) * ri), y2 = cy + (int)(sinf(a0) * ri);
        int x3 = cx + (int)(cosf(a1) * ri), y3 = cy + (int)(sinf(a1) * ri);
        g->fillTriangle(x0, y0, x1, y1, x2, y2, c);
        g->fillTriangle(x1, y1, x3, y3, x2, y2, c);
        if (numbers && r >= 120) {
            float am = a0 + step / 2;
            char b[4];
            snprintf(b, sizeof(b), "%d", WHEEL[i]);
            ui_text_center(cx + (int)(cosf(am) * (r - 13)), cy + (int)(sinf(am) * (r - 13)), COLOR_TEXT, b, 1);
        }
    }
    g->fillCircle(cx, cy, ri, COLOR565(0x8a, 0x5a, 0x2a));     // the cone
    g->fillCircle(cx, cy, ri * 70 / 100, COLOR565(0x5a, 0x38, 0x18));
    // the turret: a little cross, turning with the wheel
    for (int k = 0; k < 4; k++) {
        float a = ang + k * 1.5708f;
        g->fillCircle(cx + (int)(cosf(a) * ri * 0.4f), cy + (int)(sinf(a) * ri * 0.4f), r / 22 + 2, C_GOLD);
    }
    g->fillCircle(cx, cy, r / 10 + 2, C_GOLD);
}

static int pocket_of(int n) {
    for (int i = 0; i < 37; i++) if (WHEEL[i] == n) return i;
    return 0;
}

// ---- the game ----------------------------------------------------------------------
static void start_bet() {
    s_state = R_BET;
    s_credit = casino_credit();
    if (s_credit < CASINO_MIN_BET) { s_credit = casino_top_up(); say("The cashier topped you up"); }
}

static void roulette_create() {
    memset(s_out, 0, sizeof(s_out));
    memset(s_num, 0, sizeof(s_num));
    memset(s_last_out, 0, sizeof(s_last_out));
    memset(s_last_num, 0, sizeof(s_last_num));
    s_prev_pressed = false;
    s_press = -1;
    start_bet();
}

static void add_bet(int32_t *slot) {
    int v = CHIPS[s_chip];
    if (total_bet() + v > s_credit) { say("Not enough credit"); game_sfx_hit(); return; }
    *slot += v;
    game_sfx_score();
}

static void spin() {
    int32_t t = total_bet();
    if (t <= 0) { say("Put a chip on a bet first"); return; }
    if (t > s_credit) { say("Not enough credit"); return; }
    s_staked = t;
    s_credit -= t;
    casino_set(s_credit);
    memcpy(s_last_out, s_out, sizeof(s_out));
    memcpy(s_last_num, s_num, sizeof(s_num));
    s_result = WHEEL[random(37)];
    s_w0 = random(628) / 100.0f;
    // where the ball rests: the result's pocket, where the wheel will be at the end
    const float step = 2 * 3.14159265f / 37;
    float w_end = s_w0 + 7.5f;   // the wheel turns ~1.2 times while it slows (see wheel_angle)
    s_b_end = w_end + pocket_of(s_result) * step + step / 2;
    s_spin_t0 = millis();
    s_last_fret = -1;
    s_state = R_SPIN;
}

static float spin_t() {
    float t = (millis() - s_spin_t0) / (float)SPIN_MS;
    return t > 1 ? 1 : t;
}
static float wheel_angle(float t) { return s_w0 + 7.5f * (2 * t - t * t); }   // slowing to a stop
static float ball_angle(float t) {
    float k = 1 - t;
    return s_b_end - 38.0f * k * k * k;    // the other way round, fast, then settling
}

// What comes back when the ball lands on n: each winning bet's stake and its pay.
int32_t roulette_win(const int32_t *out, const int32_t *num, int n) {
    int32_t w = 0;
    for (int b = 0; b < B_OUTSIDE; b++)
        if (out[b] && out_wins(b, n)) w += out[b] * (out_pays(b) + 1);
    if (n >= 0 && n <= 36 && num[n]) w += num[n] * 36;
    return w;
}

static void settle() {
    s_won = roulette_win(s_out, s_num, s_result);
    s_credit += s_won;
    casino_set(s_credit);
    casino_record(CG_ROULETTE, s_won - s_staked);
    memmove(s_hist + 1, s_hist, sizeof(s_hist) - 1);
    s_hist[0] = (uint8_t)s_result;
    if (s_hist_n < 10) s_hist_n++;
    if (s_won > s_staked) game_sfx_levelup();
    else if (s_won > 0) game_sfx_score();
    else game_sfx_gameover();
    s_state = R_RESULT;
}

static void roulette_tick() {
    if (s_state != R_SPIN) return;
    float t = spin_t();
    // a tick each time the ball passes a fret (fewer as it slows)
    const float step = 2 * 3.14159265f / 37;
    float rel = ball_angle(t) - wheel_angle(t);
    int fret = (int)floorf(rel / step);
    if (fret != s_last_fret) {
        if (s_last_fret != -1 && g_app_settings.sfx_enabled && t > 0.35f) audio_play_sfx(2600, 6);
        s_last_fret = fret;
    }
    if (t >= 1) settle();
}

static void draw_hist(Arduino_GFX *g, int y) {
    int n = s_hist_n;
    if (!n) return;
    int x0 = CX - (n - 1) * 15;
    for (int i = 0; i < n; i++) {
        char b[4];
        snprintf(b, sizeof(b), "%d", s_hist[i]);
        g->fillCircle(x0 + i * 30, y, 13, num_color(s_hist[i]));
        if (i == 0) g->drawCircle(x0, y, 14, C_GOLD);
        ui_text_center(x0 + i * 30, y, COLOR_TEXT, b, 1);
    }
}

static void roulette_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(C_FELT);
    char b[48];
    uint32_t now = millis();

    if (s_state == R_SPIN || s_state == R_RESULT) {
        float t = s_state == R_SPIN ? spin_t() : 1;
        float wa = wheel_angle(t);
        draw_wheel(g, CX, CY - 10, 170, wa, true);
        // the ball: on the rim, then dropping to the pockets, a little bounce at the end
        float ba = ball_angle(t);
        float rr = t < 0.62f ? 178 : t < 0.86f ? 178 - (t - 0.62f) / 0.24f * 44 : 134;
        if (t > 0.8f && t < 0.95f) rr += sinf((t - 0.8f) * 60) * 6 * (0.95f - t) / 0.15f;
        int bx = CX + (int)(cosf(ba) * rr), by = CY - 10 + (int)(sinf(ba) * rr);
        g->fillCircle(bx, by, 7, COLOR565(0xf4, 0xf4, 0xf0));
        g->fillCircle(bx - 2, by - 2, 2, COLOR_TEXT);
        if (s_state == R_RESULT) {
            g->fillCircle(CX, CY - 10, 50, num_color(s_result));
            g->drawCircle(CX, CY - 10, 51, C_GOLD);
            snprintf(b, sizeof(b), "%d", s_result);
            ui_text_center(CX, CY - 12, COLOR_TEXT, b, 4);
            int32_t net = s_won - s_staked;
            if (net > 0) snprintf(b, sizeof(b), "You win $%ld", (long)s_won);
            else if (s_won > 0) snprintf(b, sizeof(b), "Back $%ld", (long)s_won);
            else snprintf(b, sizeof(b), "No win - $%ld lost", (long)s_staked);
            g->fillRoundRect(CX - 130, 388, 260, 40, 18, COLOR_BG);
            ui_text_center(CX, 408, net > 0 ? C_GOLD : COLOR_TEXT, b, 2);
            snprintf(b, sizeof(b), "Wallet $%ld - tap to bet again", (long)s_credit);
            ui_text_center(CX, 444, COLOR_TEXT_DIM, b, 1);
        }
        return;
    }

    if (s_state == R_NUMBERS) {
        ui_text_center(CX, 38, COLOR_TEXT_DIM, "Numbers pay 35:1 - tap to add a chip", 1);
        for (int n = 0; n <= 36; n++) {
            Rect r = num_rect(n);
            g->fillRoundRect(r.x, r.y, r.w, r.h, 8, num_color(n));
            snprintf(b, sizeof(b), "%d", n);
            ui_text_center(r.x + r.w / 2, r.y + r.h / 2, COLOR_TEXT, b, 2);
            if (s_num[n]) chip_mark(g, r.x + r.w - 8, r.y + 8, s_num[n]);
        }
        draw_button(g, NUM_DONE, COLOR_ACCENT, "Done", s_press == 100);
        return;
    }

    // betting: the wheel small at the top, the chip, the bets
    draw_wheel(g, CX, -80, 150, now / 4000.0f, false);   // the bottom of a wheel, turning slowly
    snprintf(b, sizeof(b), "Wallet $%ld", (long)s_credit);
    ui_text_center(CX, 166, C_GOLD, b, 2);
    int32_t tb = total_bet();
    Rect cr = bet_rect(12);
    static const uint16_t CHIPC[3] = {COLOR565(0xe0, 0xe0, 0xe0), COLOR565(0x30, 0x80, 0xe0), COLOR565(0x20, 0x20, 0x20)};
    g->fillCircle(CX, cr.y + 34, 30, CHIPC[s_chip]);
    for (int k = 0; k < 8; k++) {
        float a = k * 0.785f;
        g->fillCircle(CX + (int)(cosf(a) * 25), cr.y + 34 + (int)(sinf(a) * 25), 4, k % 2 ? COLOR_TEXT : C_RED);
    }
    g->fillCircle(CX, cr.y + 34, 18, CHIPC[s_chip]);
    snprintf(b, sizeof(b), "%d", CHIPS[s_chip]);
    ui_text_center(CX, cr.y + 34, s_chip == 0 ? COLOR_BG : COLOR_TEXT, b, 2);
    ui_text_center(CX - 92, cr.y + 34, COLOR_TEXT_DIM, "chip", 1);
    snprintf(b, sizeof(b), "bet $%ld", (long)tb);
    ui_text_center(CX + 92, cr.y + 34, tb ? COLOR_TEXT : COLOR_TEXT_DIM, b, 1);

    for (int i = 0; i < B_OUTSIDE; i++) {
        Rect r = bet_rect(i);
        uint16_t bg = i == B_RED ? C_RED : i == B_BLACK ? C_BLACK : COLOR565(0x14, 0x66, 0x3a);
        draw_button(g, r, bg, OUT_NAME[i], s_press == i, i >= B_D1 ? 1 : 2);
        if (s_out[i]) chip_mark(g, r.x + r.w - 10, r.y + 10, s_out[i]);
    }
    int32_t nums = 0;
    for (int32_t v : s_num) nums += v;
    snprintf(b, sizeof(b), nums ? "Numbers  ($%ld on)" : "Numbers 0-36", (long)nums);
    draw_button(g, bet_rect(9), COLOR565(0x14, 0x66, 0x3a), b, s_press == 9, 1);
    draw_button(g, bet_rect(10), COLOR_PANEL, "Clear", s_press == 10);
    draw_button(g, bet_rect(11), tb ? C_GOLD : COLOR_PANEL, "Spin", s_press == 11);
    if (s_msg[0] && now - s_msg_ms < 1800) {
        g->fillRoundRect(CX - 140, 150, 280, 32, 16, COLOR_BG);
        ui_text_center(CX, 166, COLOR_WARN, s_msg, 1);
    } else if (s_hist_n) {
        draw_hist(g, 452 - 16);
    }
}

static void roulette_touch(int x, int y, bool pressed) {
    bool down = pressed && !s_prev_pressed, up = !pressed && s_prev_pressed;
    s_prev_pressed = pressed;
    if (s_state == R_SPIN) return;
    if (s_state == R_RESULT) {
        if (up && millis() - s_spin_t0 > SPIN_MS + 600) {
            // the same bets again, if there's the credit for them
            s_credit = casino_credit();
            int32_t want = 0;
            for (int32_t v : s_last_out) want += v;
            for (int32_t v : s_last_num) want += v;
            if (want <= s_credit) { memcpy(s_out, s_last_out, sizeof(s_out)); memcpy(s_num, s_last_num, sizeof(s_num)); }
            else { memset(s_out, 0, sizeof(s_out)); memset(s_num, 0, sizeof(s_num)); }
            start_bet();
        }
        return;
    }
    if (s_state == R_NUMBERS) {
        if (down) s_press = NUM_DONE.hit(x, y) ? 100 : -1;
        if (!up) return;
        bool done = s_press == 100 && NUM_DONE.hit(x, y);
        s_press = -1;
        if (done) { s_state = R_BET; return; }
        for (int n = 0; n <= 36; n++) if (num_rect(n).hit(x, y)) { add_bet(&s_num[n]); return; }
        return;
    }
    // betting
    if (down) {
        s_press = -1;
        for (int i = 0; i <= 12; i++) if (bet_rect(i).hit(x, y)) s_press = i;
        return;
    }
    if (!up) return;
    int p = s_press;
    s_press = -1;
    if (p < 0 || !bet_rect(p).hit(x, y)) return;
    if (p < B_OUTSIDE) add_bet(&s_out[p]);
    else if (p == 9) s_state = R_NUMBERS;
    else if (p == 10) { memset(s_out, 0, sizeof(s_out)); memset(s_num, 0, sizeof(s_num)); }
    else if (p == 11) spin();
    else if (p == 12) s_chip = (s_chip + 1) % 3;
}

static void roulette_gesture(Gesture gst) {
    if (s_state == R_SPIN) return;
    if (gst == GESTURE_SWIPE_LEFT || gst == GESTURE_SWIPE_RIGHT) {
        if (s_state == R_NUMBERS) s_state = R_BET;
        else ui_pop_screen();
    }
}

Screen roulette_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    roulette_create, roulette_draw, roulette_touch, roulette_tick, nullptr, roulette_gesture,
    0, true, false, false,
    true,   // no_drag_back - a swipe on the numbers table goes back to the bets
    true,   // hide_status - the wallet sits at the top
};
