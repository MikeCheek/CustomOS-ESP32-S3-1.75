/*
 * game_dice.cpp - Hi-Lo Dice, in the Casino (casino.h)
 *
 * Two dice. Put chips on Under 7 (2-6, pays 1:1), Seven (pays 4:1) or
 * Over 7 (8-12, pays 1:1) - each tap adds the chip (5 / 25 / 100, tap
 * the chip to change it), on one or more of them - then Roll: the dice
 * tumble across the felt and stop. From the Casino's shared wallet
 * (casino_wallet.h); play money only. Tap after the result to roll again
 * with the same bets. Swipe sideways: back to the Casino.
 */
#include "game_dice.h"
#include "casino_wallet.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

int32_t dice_win(int32_t under, int32_t seven, int32_t over, int total) {
    if (total < 7) return under * 2;
    if (total == 7) return seven * 5;
    return over * 2;
}

static const int CX = LCD_WIDTH / 2;
static const uint16_t C_FELT = COLOR565(0x0c, 0x50, 0x2c), C_RIM = COLOR565(0x6a, 0x3c, 0x1c),
                      C_GOLD = COLOR565(0xe8, 0xc0, 0x50), C_DIE = COLOR565(0xf4, 0xf0, 0xe6),
                      C_PIP = COLOR565(0x18, 0x18, 0x20), C_RED = COLOR565(0xc8, 0x20, 0x2c);

enum DState { D_BET, D_ROLL, D_RESULT };
static DState s_state;
static int32_t s_bets[3], s_last[3];   // under, seven, over
static const int CHIPS[3] = {5, 25, 100};
static int s_chip = 0;
static int s_d1 = 3, s_d2 = 4;         // the dice showing
static int s_f1 = 3, s_f2 = 4;         // where they'll stop
static uint32_t s_roll_ms = 0, s_last_tick = 0;
static int32_t s_won = 0, s_staked = 0;
static bool s_prev = false;
static int s_press = -1;               // 0-2 the bets, 10 chip, 11 clear, 12 roll
static const char *s_msg = "";
static uint32_t s_msg_ms = 0;
static uint8_t s_hist[8];
static int s_hist_n = 0;
static const uint32_t ROLL_MS = 1400;

struct Rect { int x, y, w, h; bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };
static Rect bet_rect(int i) { return {CX - 186 + i * 126, 262, 120, 72}; }
static const Rect R_CHIP = {CX - 150, 346, 140, 44}, R_CLEAR = {CX + 10, 346, 140, 44}, R_ROLL = {CX - 80, 400, 160, 44};
static const char *const BET_NAME[3] = {"Under 7", "Seven", "Over 7"};
static const char *const BET_PAYS[3] = {"pays 1:1", "pays 4:1", "pays 1:1"};

static void say(const char *m) { s_msg = m; s_msg_ms = millis(); }
static int32_t staked() { return s_bets[0] + s_bets[1] + s_bets[2]; }

static void dice_create() {
    s_state = D_BET;
    memset(s_bets, 0, sizeof(s_bets));
    s_prev = false;
    s_press = -1;
    s_msg = "";
}

static void roll() {
    int32_t want = staked();
    if (want <= 0) { say("Put a chip on a bet first"); return; }
    int32_t credit = casino_credit();
    if (want > credit) { say("Not enough in the wallet"); return; }
    casino_set(credit - want);
    s_staked = want;
    memcpy(s_last, s_bets, sizeof(s_last));
    s_f1 = random(6) + 1;
    s_f2 = random(6) + 1;
    s_roll_ms = s_last_tick = millis();
    s_state = D_ROLL;
    game_sfx_shoot();
}

static void settle() {
    int total = s_f1 + s_f2;
    s_d1 = s_f1;
    s_d2 = s_f2;
    s_won = dice_win(s_bets[0], s_bets[1], s_bets[2], total);
    if (s_won > 0) casino_set(casino_credit() + s_won);
    casino_record(CG_DICE, s_won - s_staked);
    if (s_hist_n < 8) s_hist_n++;
    memmove(s_hist + 1, s_hist, sizeof(s_hist) - 1);
    s_hist[0] = (uint8_t)total;
    s_state = D_RESULT;
    if (total == 7 && s_bets[1] > 0) game_sfx_levelup();
    else if (s_won > 0) game_sfx_score();
    else game_sfx_hit();
}

static void dice_tick() {
    if (s_state != D_ROLL) return;
    uint32_t now = millis(), t = now - s_roll_ms;
    if (t >= ROLL_MS) { settle(); return; }
    // faces flick fast, then slower as the dice come to rest
    uint32_t every = 50 + t / 8;
    if (now - s_last_tick >= every) {
        s_last_tick = now;
        s_d1 = random(6) + 1;
        s_d2 = random(6) + 1;
        game_sfx_simon_note(random(4));
    }
}

// ---- drawing ---------------------------------------------------------------------------
static void die(Arduino_GFX *g, int cx, int cy, int s, int v, float tilt) {
    // a slightly turned square while it tumbles: two triangles for the body
    float c = cosf(tilt), sn = sinf(tilt);
    int hx[4], hy[4];
    const int px[4] = {-1, 1, 1, -1}, py[4] = {-1, -1, 1, 1};
    for (int k = 0; k < 4; k++) {
        hx[k] = cx + (int)((px[k] * c - py[k] * sn) * s / 2);
        hy[k] = cy + (int)((px[k] * sn + py[k] * c) * s / 2);
    }
    if (tilt == 0) g->fillRoundRect(cx - s / 2, cy - s / 2, s, s, s / 6, C_DIE);
    else {
        g->fillTriangle(hx[0], hy[0], hx[1], hy[1], hx[2], hy[2], C_DIE);
        g->fillTriangle(hx[0], hy[0], hx[2], hy[2], hx[3], hy[3], C_DIE);
    }
    // the pips, on a 3 x 3 grid
    static const uint16_t PIPS[7] = {0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16d};
    int q = s / 4, pr = s / 11 + 1;
    for (int k = 0; k < 9; k++) {
        if (!(PIPS[v] & (1 << (8 - k)))) continue;
        int gx = (k % 3 - 1) * q, gy = (k / 3 - 1) * q;
        int x = cx + (int)(gx * c - gy * sn), y = cy + (int)(gx * sn + gy * c);
        g->fillCircle(x, y, pr, v == 1 ? C_RED : C_PIP);
    }
}

static void chip_mark(Arduino_GFX *g, int x, int y, int32_t amount) {
    if (amount <= 0) return;
    char b[12];
    snprintf(b, sizeof(b), amount >= 1000 ? "%ldk" : "%ld", (long)(amount >= 1000 ? amount / 1000 : amount));
    g->fillCircle(x, y, 15, C_GOLD);
    g->fillCircle(x, y, 11, COLOR565(0xb0, 0x80, 0x20));
    ui_text_center(x, y, COLOR_TEXT, b, 1);
}

static void button(Arduino_GFX *g, const Rect &r, uint16_t bg, const char *label, bool lit, int size = 2) {
    g->fillRoundRect(r.x, r.y, r.w, r.h, 14, lit ? ui_dim(bg, 0.6f) : bg);
    ui_text_center(r.x + r.w / 2, r.y + r.h / 2, COLOR_TEXT, label, size);
}

static void dice_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    uint32_t now = millis();
    g->fillScreen(C_RIM);
    g->fillCircle(CX, LCD_HEIGHT / 2, 220, C_FELT);

    ui_text_center(CX, 48, C_GOLD, "HI-LO DICE", 2);
    char b[48];
    snprintf(b, sizeof(b), "$%ld", (long)casino_credit());
    ui_text_center(CX, 78, COLOR_TEXT, b, 2);

    // the dice: bouncing across while they roll, side by side at rest
    int y = 158, x1 = CX - 52, x2 = CX + 52;
    float t1 = 0, t2 = 0;
    if (s_state == D_ROLL) {
        float t = (now - s_roll_ms) / (float)ROLL_MS;
        float bounce = fabsf(sinf(t * 9.0f)) * (1 - t) * 34;
        y -= (int)bounce;
        x1 += (int)((1 - t) * -40 * sinf(t * 5));
        x2 += (int)((1 - t) * 40 * sinf(t * 6));
        t1 = (1 - t) * 7.0f * t;
        t2 = -(1 - t) * 6.0f * t;
    }
    die(g, x1, y, 76, s_d1, t1);
    die(g, x2, y, 76, s_d2, t2);

    int total = s_d1 + s_d2;
    if (s_state == D_RESULT) {
        snprintf(b, sizeof(b), "%d", total);
        ui_text_center(CX, 222, total == 7 ? C_GOLD : COLOR_TEXT, b, 3);
    } else if (s_state == D_BET) {
        ui_text_center(CX, 222, COLOR_TEXT_DIM, "Under, seven or over?", 1);
    }

    for (int i = 0; i < 3; i++) {
        Rect r = bet_rect(i);
        bool win = s_state == D_RESULT && s_last[i] > 0 &&
                   ((i == 0 && total < 7) || (i == 1 && total == 7) || (i == 2 && total > 7));
        uint16_t bg = win ? COLOR565(0x10, 0x80, 0x48) : (i == 1 ? COLOR565(0x7a, 0x5a, 0x10) : COLOR565(0x14, 0x3a, 0x6a));
        g->fillRoundRect(r.x, r.y, r.w, r.h, 14, s_press == i ? ui_dim(bg, 0.6f) : bg);
        ui_text_center(r.x + r.w / 2, r.y + 24, COLOR_TEXT, BET_NAME[i], 2);
        ui_text_center(r.x + r.w / 2, r.y + 52, ui_dim(COLOR_TEXT, 0.8f), BET_PAYS[i], 1);
        chip_mark(g, r.x + r.w - 10, r.y + 6, s_state == D_RESULT ? s_last[i] : s_bets[i]);
    }

    if (s_state == D_RESULT) {
        if (s_won > 0) snprintf(b, sizeof(b), "Won $%ld", (long)s_won);
        else snprintf(b, sizeof(b), "Lost $%ld", (long)s_staked);
        ui_text_center(CX, 368, s_won > 0 ? COLOR_GOOD : COLOR_BAD, b, 2);
        ui_text_center(CX, 402, COLOR_TEXT_DIM, "Tap to roll again", 1);
    } else if (s_state == D_BET) {
        snprintf(b, sizeof(b), "Chip $%d", CHIPS[s_chip]);
        button(g, R_CHIP, COLOR565(0x7a, 0x5a, 0x10), b, s_press == 10);
        button(g, R_CLEAR, COLOR_PANEL, "Clear", s_press == 11);
        button(g, R_ROLL, COLOR565(0xb0, 0x1c, 0x28), "Roll", s_press == 12);
    }
    // the last totals, along the bottom
    if (s_hist_n && s_state != D_BET) {
        int n = s_hist_n > 6 ? 6 : s_hist_n;
        for (int k = 0; k < n; k++) {
            snprintf(b, sizeof(b), "%d", s_hist[k]);
            ui_text_center(CX - (n - 1) * 15 + k * 30, 432, s_hist[k] == 7 ? C_GOLD : COLOR_TEXT_DIM, b, 1);
        }
    }
    if (s_msg[0] && now - s_msg_ms < 2200) ui_text_center(CX, 240, COLOR_WARN, s_msg, 1);
}

static int hit(int x, int y) {
    if (s_state != D_BET) return -1;
    for (int i = 0; i < 3; i++) if (bet_rect(i).hit(x, y)) return i;
    if (R_CHIP.hit(x, y)) return 10;
    if (R_CLEAR.hit(x, y)) return 11;
    if (R_ROLL.hit(x, y)) return 12;
    return -1;
}

static void dice_touch(int x, int y, bool pressed) {
    bool down = pressed && !s_prev, up = !pressed && s_prev;
    s_prev = pressed;
    if (s_state == D_ROLL) return;
    if (s_state == D_RESULT) {
        if (up && millis() - s_roll_ms > ROLL_MS + 500) {
            // the same bets again, if the wallet covers them
            if (s_last[0] + s_last[1] + s_last[2] <= casino_credit()) memcpy(s_bets, s_last, sizeof(s_bets));
            else memset(s_bets, 0, sizeof(s_bets));
            s_state = D_BET;
        }
        return;
    }
    if (down) { s_press = hit(x, y); return; }
    if (!up) return;
    int p = s_press;
    s_press = -1;
    if (p < 0 || hit(x, y) != p) return;
    if (p < 3) {
        if (staked() + CHIPS[s_chip] > casino_credit()) { say(casino_credit() < CASINO_MIN_BET ? "Out of chips - the cashier tops up" : "That's the whole wallet"); return; }
        s_bets[p] += CHIPS[s_chip];
        game_sfx_score();
    } else if (p == 10) s_chip = (s_chip + 1) % 3;
    else if (p == 11) memset(s_bets, 0, sizeof(s_bets));
    else if (p == 12) roll();
}

static void dice_gesture(Gesture gst) {
    if (s_state == D_ROLL) return;
    if (gst == GESTURE_SWIPE_LEFT || gst == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen dice_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    dice_create, dice_draw, dice_touch, dice_tick, nullptr, dice_gesture,
    0, true, false, false,
    true,   // no_drag_back - swipes are for leaving only
    true,   // hide_status - the wallet sits at the top
};
