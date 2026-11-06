// The Casino - see casino.h.
#include "casino.h"
#include "casino_wallet.h"
#include "game_roulette.h"
#include "game_videopoker.h"
#include "game_dice.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "hal_rtc.h"
#include "ui.h"
#include "ui_font.h"
#include <stdlib.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

extern Screen cavern_screen;
extern Screen blackjack_screen;

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const uint16_t C_CARPET = COLOR565(0x3a, 0x0c, 0x22), C_CARPET2 = COLOR565(0x4a, 0x14, 0x2e),
                      C_GOLD = COLOR565(0xe8, 0xc0, 0x50), C_FELT = COLOR565(0x0e, 0x6a, 0x3a),
                      C_WOOD = COLOR565(0x70, 0x40, 0x1e), C_RED = COLOR565(0xc8, 0x20, 0x2c),
                      C_BLACK = COLOR565(0x20, 0x20, 0x24);

// The stations on the floor: where they are and how far a tap reaches.
// Two rows: the slot machine, the blackjack table and the poker machine,
// then the roulette wheel and the dice table.
struct Station { const char *name, *sub; int x, y, r; Screen *screen; };
static const int N_ST = 5, CASHIER = N_ST;
static Station STATIONS[N_ST] = {
    {"Cavern slots", "slots", CX - 124, 182, 58, &cavern_screen},
    {"Blackjack", "cards", CX, 182, 58, &blackjack_screen},
    {"Video poker", "poker", CX + 124, 182, 58, &videopoker_screen},
    {"Roulette", "wheel", CX - 84, 316, 62, &roulette_screen},
    {"Hi-Lo dice", "dice", CX + 84, 316, 62, &dice_screen},
};
static const int CASHIER_Y = 432, STATS = N_ST + 1;
static int s_press = -1;           // a station, CASHIER or STATS
extern Screen casino_stats_screen;

static uint32_t today() {
    WatchTime t = rtc_now();
    return t.valid ? (uint32_t)t.year * 10000 + t.month * 100 + t.day : 0;
}
static bool s_prev = false;
static uint32_t s_msg_ms = 0;
static char s_msg[40] = "";

static void casino_create() { s_press = -1; s_prev = false; s_msg[0] = 0; }

// ---- drawings of the games -------------------------------------------------------------
static void draw_slot(Arduino_GFX *g, int x, int y, uint32_t now, bool lit) {
    // a cabinet: a marquee on top, a screen with three crystals, a lever
    g->fillRoundRect(x - 40, y - 52, 80, 100, 10, lit ? COLOR565(0x6a, 0x4a, 0xb0) : COLOR565(0x4a, 0x2e, 0x86));
    g->fillRoundRect(x - 34, y - 46, 68, 18, 6, C_GOLD);
    for (int k = 0; k < 5; k++)   // bulbs chasing along the marquee
        g->fillCircle(x - 26 + k * 13, y - 37, 3, ((now / 150 + k) % 2) ? COLOR_TEXT : COLOR565(0xb0, 0x80, 0x20));
    g->fillRoundRect(x - 32, y - 22, 64, 34, 6, COLOR565(0x12, 0x10, 0x20));
    static const uint16_t CR[3] = {COLOR565(0x50, 0x90, 0xff), COLOR565(0xff, 0xd8, 0x30), COLOR565(0xb0, 0x60, 0xf0)};
    for (int k = 0; k < 3; k++) {
        int cx = x - 20 + k * 20, cy = y - 5;
        uint16_t c = CR[(k + now / 900) % 3];
        g->fillTriangle(cx - 7, cy, cx + 7, cy, cx, cy - 10, c);
        g->fillTriangle(cx - 7, cy, cx + 7, cy, cx, cy + 10, ui_dim(c, 0.7f));
    }
    g->fillRect(x - 30, y + 18, 60, 8, COLOR565(0x2a, 0x1a, 0x50));
    g->fillRect(x + 42, y - 30, 4, 34, COLOR565(0xb0, 0xb0, 0xb8));   // the lever
    g->fillCircle(x + 44, y - 32, 7, C_RED);
}

static void draw_table(Arduino_GFX *g, int x, int y, uint32_t now, bool lit) {
    // a half-moon table, green felt, the dealer's cards and a few chips
    g->fillCircle(x, y - 14, 56, C_WOOD);
    g->fillCircle(x, y - 14, 50, lit ? ui_dim(C_FELT, 0.75f) : C_FELT);
    g->fillRect(x - 60, y - 70, 120, 50, C_CARPET);   // only the near half shows
    g->fillRect(x - 56, y - 22, 112, 6, C_WOOD);
    for (int k = 0; k < 2; k++) {
        int cx = x - 18 + k * 22, cy = y + 4;
        g->fillRoundRect(cx - 10, cy - 14, 20, 28, 3, COLOR565(0xf4, 0xf0, 0xe6));
        if (k == 0) { g->fillCircle(cx, cy - 2, 4, C_RED); g->fillTriangle(cx - 4, cy, cx + 4, cy, cx, cy + 7, C_RED); }
        else ui_text_center(cx, cy, C_BLACK, "A", 2);
    }
    static const uint16_t CH[3] = {C_RED, COLOR565(0x30, 0x80, 0xe0), COLOR565(0xe0, 0xe0, 0xe0)};
    for (int k = 0; k < 3; k++) {
        int cx = x + 30 - k * 4, cy = y + 22 - k * 5;
        g->fillCircle(cx, cy, 8, CH[k]);
        g->fillCircle(cx, cy, 4, ui_dim(CH[k], 0.7f));
    }
    (void)now;
}

static void draw_poker(Arduino_GFX *g, int x, int y, uint32_t now, bool lit) {
    // an upright machine: a blue cabinet, five cards on the screen (one
    // flips now and then), the buttons below
    g->fillRoundRect(x - 40, y - 52, 80, 100, 10, lit ? COLOR565(0x1a, 0x40, 0x80) : COLOR565(0x12, 0x30, 0x66));
    g->fillRoundRect(x - 34, y - 46, 68, 14, 5, C_GOLD);
    ui_text_center(x, y - 39, C_BLACK, "POKER", 1);
    g->fillRoundRect(x - 34, y - 28, 68, 40, 6, COLOR565(0x0a, 0x1a, 0x3a));
    int flip = (now / 700) % 5;
    for (int k = 0; k < 5; k++) {
        int cx = x - 26 + k * 13;
        g->fillRoundRect(cx - 5, y - 20, 11, 24, 2, k == flip ? C_RED : COLOR565(0xf4, 0xf0, 0xe6));
        if (k != flip) g->fillCircle(cx, y - 8, 2, k % 2 ? C_RED : C_BLACK);
    }
    for (int k = 0; k < 5; k++) g->fillRoundRect(x - 32 + k * 13, y + 20, 10, 8, 2, k == flip ? C_GOLD : COLOR565(0xb0, 0xb0, 0xb8));
}

static void draw_dice(Arduino_GFX *g, int x, int y, uint32_t now, bool lit) {
    // a round felt table with a wooden rim and two dice that turn over
    g->fillCircle(x, y, 50, C_WOOD);
    g->fillCircle(x, y, 44, lit ? ui_dim(C_FELT, 0.75f) : C_FELT);
    static const uint16_t PIPS[7] = {0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16d};
    for (int d = 0; d < 2; d++) {
        int v = (int)((now / 600 + d * 3) % 6) + 1;
        int dx = x - 16 + d * 32, dy = y - 4 + d * 8;
        g->fillRoundRect(dx - 13, dy - 13, 26, 26, 5, COLOR565(0xf4, 0xf0, 0xe6));
        for (int k = 0; k < 9; k++)
            if (PIPS[v] & (1 << (8 - k))) g->fillCircle(dx + (k % 3 - 1) * 7, dy + (k / 3 - 1) * 7, 2, v == 1 ? C_RED : C_BLACK);
    }
}

static void draw_wheel(Arduino_GFX *g, int x, int y, uint32_t now, bool lit) {
    int r = 44;
    g->fillCircle(x, y, r + 8, lit ? ui_dim(C_WOOD, 0.75f) : C_WOOD);
    const float step = 2 * 3.14159265f / 18;
    float ang = now / 1500.0f;
    for (int i = 0; i < 18; i++) {   // fewer pockets than the real wheel: a drawing of one
        float a0 = ang + i * step, a1 = a0 + step;
        uint16_t c = i == 0 ? COLOR565(0x10, 0x90, 0x48) : (i % 2 ? C_RED : C_BLACK);
        g->fillTriangle(x, y, x + (int)(cosf(a0) * r), y + (int)(sinf(a0) * r), x + (int)(cosf(a1) * r),
                        y + (int)(sinf(a1) * r), c);
    }
    g->fillCircle(x, y, r * 55 / 100, COLOR565(0x8a, 0x5a, 0x2a));
    g->fillCircle(x, y, 6, C_GOLD);
    float b = -now / 700.0f;   // the ball, the other way round
    g->fillCircle(x + (int)(cosf(b) * (r + 2)), y + (int)(sinf(b) * (r + 2)), 4, COLOR_TEXT);
}

static void casino_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    uint32_t now = millis();
    g->fillScreen(C_CARPET);
    // the carpet's pattern: a grid of small diamonds
    for (int yy = 30; yy < LCD_HEIGHT; yy += 36)
        for (int xx = ((yy / 36) % 2) * 18 + 10; xx < LCD_WIDTH; xx += 36)
            g->fillTriangle(xx - 5, yy, xx + 5, yy, xx, yy - 7, C_CARPET2), g->fillTriangle(xx - 5, yy, xx + 5, yy, xx, yy + 7, C_CARPET2);
    // marquee lights round the edge, chasing
    for (int k = 0; k < 40; k++) {
        float a = k * (360.0f / 40);
        int x, y;
        ui_polar(a, 222, &x, &y);
        bool on = ((now / 120) + k) % 3 == 0;
        g->fillCircle(x, y, 4, on ? COLOR565(0xff, 0xe8, 0x90) : COLOR565(0x80, 0x60, 0x20));
    }
    ui_text_center(CX, 58, C_GOLD, "CASINO", 3);
    char b[40];
    snprintf(b, sizeof(b), "Wallet $%ld", (long)casino_credit());
    ui_text_center(CX, 96, COLOR_TEXT, b, 2);

    draw_slot(g, STATIONS[0].x, STATIONS[0].y, now, s_press == 0);
    draw_table(g, STATIONS[1].x, STATIONS[1].y, now, s_press == 1);
    draw_poker(g, STATIONS[2].x, STATIONS[2].y, now, s_press == 2);
    draw_wheel(g, STATIONS[3].x, STATIONS[3].y, now, s_press == 3);
    draw_dice(g, STATIONS[4].x, STATIONS[4].y, now, s_press == 4);
    for (int i = 0; i < N_ST; i++) {
        const Station &s = STATIONS[i];
        int ly = s.y + (i < 3 ? 62 : 64);
        g->fillRoundRect(s.x - 56, ly - 11, 112, 22, 11, s_press == i ? C_GOLD : COLOR565(0x1a, 0x06, 0x12));
        ui_text_center(s.x, ly, s_press == i ? C_BLACK : COLOR_TEXT, s.name, 1);
    }
    // the cashier
    bool broke = casino_credit() < CASINO_MIN_BET;
    bool bonus = casino_daily_bonus_ready(today());
    if (s_msg[0] && now - s_msg_ms < 2200) {
        ui_text_center(CX, CASHIER_Y, COLOR_WARN, s_msg, 1);
    } else {
        // the cashier (a top-up when broke, else the day's bonus) and the stats
        uint16_t cb = s_press == CASHIER ? C_GOLD : broke ? C_RED
                      : bonus ? (((now / 500) % 2) ? COLOR565(0x8a, 0x6a, 0x10) : COLOR565(0x6a, 0x4a, 0x08))
                      : COLOR565(0x1a, 0x06, 0x12);
        g->fillRoundRect(CX - 110, CASHIER_Y - 13, 104, 26, 13, cb);
        ui_text_center(CX - 58, CASHIER_Y, s_press == CASHIER ? C_BLACK : COLOR_TEXT,
                       broke ? "Top up" : bonus ? "Daily bonus" : "Cashier", 1);
        g->fillRoundRect(CX + 6, CASHIER_Y - 13, 104, 26, 13, s_press == STATS ? C_GOLD : COLOR565(0x1a, 0x06, 0x12));
        ui_text_center(CX + 58, CASHIER_Y, s_press == STATS ? C_BLACK : COLOR_TEXT, "Stats", 1);
    }
}

static int hit(int x, int y) {
    for (int i = 0; i < N_ST; i++) {
        int dx = x - STATIONS[i].x, dy = y - STATIONS[i].y;
        if (dx * dx + dy * dy <= STATIONS[i].r * STATIONS[i].r) return i;
    }
    if (abs(y - CASHIER_Y) < 16 && x >= CX - 110 && x < CX - 6) return CASHIER;
    if (abs(y - CASHIER_Y) < 16 && x >= CX + 6 && x < CX + 110) return STATS;
    return -1;
}

static void casino_touch(int x, int y, bool pressed) {
    bool down = pressed && !s_prev, up = !pressed && s_prev;
    s_prev = pressed;
    if (down) { s_press = hit(x, y); return; }
    if (!up) return;
    int p = s_press;
    s_press = -1;
    if (p < 0 || hit(x, y) != p) return;
    if (p == STATS) { game_sfx_score(); ui_push(&casino_stats_screen); return; }
    if (p == CASHIER) {
        int32_t before = casino_credit(), after = casino_top_up();
        int32_t bonus = after > before ? 0 : casino_daily_bonus(today());
        if (after > before) { snprintf(s_msg, sizeof(s_msg), "Here's $%ld - good luck!", (long)(after - before)); game_sfx_levelup(); }
        else if (bonus > 0) { snprintf(s_msg, sizeof(s_msg), "Today's bonus: $%ld!", (long)bonus); game_sfx_levelup(); }
        else snprintf(s_msg, sizeof(s_msg), "Bonus taken - back tomorrow");
        s_msg_ms = millis();
        return;
    }
    game_sfx_score();
    ui_push(STATIONS[p].screen);
}

Screen casino_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    casino_create, casino_draw, casino_touch, nullptr, nullptr, nullptr,
    0, false, false, false, false,
    true,   // hide_status - the marquee lights run round the edge
};

// ---- the stats page ----------------------------------------------------------------------
// Per game: rounds played, what it made or lost overall, the biggest win in
// one round; Video Poker's best hand. Swipe sideways or tap: back.
static void stats_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillScreen(C_CARPET);
    ui_text_center(CX, 62, C_GOLD, "CASINO STATS", 2);
    char b[48];
    int32_t total = 0;
    uint32_t rounds = 0;
    for (int i = 0; i < CG_COUNT; i++) { CasinoGameStats s = casino_stats((CasinoGame)i); total += s.net; rounds += s.rounds; }
    snprintf(b, sizeof(b), "%lu round%s  -  %s$%ld overall", (unsigned long)rounds, rounds == 1 ? "" : "s",
             total < 0 ? "-" : "+", (long)labs(total));
    ui_text_center(CX, 94, total < 0 ? COLOR_BAD : COLOR_GOOD, b, 1);
    int y = 132;
    for (int i = 0; i < CG_COUNT; i++, y += 52) {
        CasinoGameStats s = casino_stats((CasinoGame)i);
        g->fillRoundRect(CX - 180, y - 20, 360, 44, 14, COLOR565(0x1a, 0x06, 0x12));
        ui_print(CX - 164, y - 8, 1, COLOR_TEXT, casino_game_name((CasinoGame)i));
        if (!s.rounds) { ui_print(CX - 164, y + 8, 1, COLOR_TEXT_DIM, "not played yet"); continue; }
        snprintf(b, sizeof(b), "%lu round%s  -  best win $%ld", (unsigned long)s.rounds, s.rounds == 1 ? "" : "s",
                 (long)(s.best > 0 ? s.best : 0));
        ui_print(CX - 164, y + 8, 1, COLOR_TEXT_DIM, b);
        snprintf(b, sizeof(b), "%s$%ld", s.net < 0 ? "-" : "+", (long)labs(s.net));
        ui_text_center(CX + 128, y, s.net < 0 ? COLOR_BAD : COLOR_GOOD, b, 2);
    }
    uint8_t h = casino_best_poker_hand();
    snprintf(b, sizeof(b), "Best poker hand: %s", h < PH_NOTHING ? poker_name((PokerHand)h) : "none yet");
    ui_text_center(CX, y + 4, C_GOLD, b, 1);
}

static void stats_touch(int, int, bool pressed) {
    static bool down = false;
    if (pressed) { down = true; return; }
    if (down) { down = false; ui_pop_screen(); }
}
static void stats_gesture(Gesture g) { if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen(); }

Screen casino_stats_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, stats_draw, stats_touch, nullptr, nullptr, stats_gesture,
    0, false, false, false, false,
    true,   // hide_status
};
