// The Casino - see casino.h.
#include "casino.h"
#include "casino_wallet.h"
#include "game_roulette.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "ui.h"
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
struct Station { const char *name, *sub; int x, y, r; Screen *screen; };
static Station STATIONS[3] = {
    {"Crystal Cavern", "slots", CX - 112, 196, 70, &cavern_screen},
    {"Blackjack", "cards", CX + 112, 196, 70, &blackjack_screen},
    {"Roulette", "wheel", CX, 322, 72, &roulette_screen},
};
static const int CASHIER_Y = 420;
static int s_press = -1;           // 0-2 a station, 3 the cashier
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

static void draw_wheel(Arduino_GFX *g, int x, int y, uint32_t now, bool lit) {
    int r = 46;
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
    draw_wheel(g, STATIONS[2].x, STATIONS[2].y, now, s_press == 2);
    for (int i = 0; i < 3; i++) {
        const Station &s = STATIONS[i];
        int ly = i == 2 ? s.y + 68 : s.y + 64;
        g->fillRoundRect(s.x - 64, ly - 12, 128, 24, 12, s_press == i ? C_GOLD : COLOR565(0x1a, 0x06, 0x12));
        ui_text_center(s.x, ly, s_press == i ? C_BLACK : COLOR_TEXT, s.name, 1);
    }
    // the cashier
    bool broke = casino_credit() < CASINO_MIN_BET;
    if (s_msg[0] && now - s_msg_ms < 2200) {
        ui_text_center(CX, CASHIER_Y, COLOR_WARN, s_msg, 1);
    } else {
        g->fillRoundRect(CX - 58, CASHIER_Y - 14, 116, 28, 14, s_press == 3 ? C_GOLD : broke ? C_RED : COLOR565(0x1a, 0x06, 0x12));
        ui_text_center(CX, CASHIER_Y, s_press == 3 ? C_BLACK : COLOR_TEXT, broke ? "Cashier: top up" : "Cashier", 1);
    }
}

static int hit(int x, int y) {
    for (int i = 0; i < 3; i++) {
        int dx = x - STATIONS[i].x, dy = y - STATIONS[i].y;
        if (dx * dx + dy * dy <= STATIONS[i].r * STATIONS[i].r) return i;
    }
    if (abs(x - CX) < 64 && abs(y - CASHIER_Y) < 18) return 3;
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
    if (p == 3) {
        int32_t before = casino_credit(), after = casino_top_up();
        if (after > before) { snprintf(s_msg, sizeof(s_msg), "Here's $%ld - good luck!", (long)(after - before)); game_sfx_levelup(); }
        else snprintf(s_msg, sizeof(s_msg), "Top-ups are for under $%d", CASINO_MIN_BET);
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
