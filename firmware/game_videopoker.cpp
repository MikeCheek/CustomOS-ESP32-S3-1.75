/*
 * game_videopoker.cpp - Video Poker, Jacks or Better, in the Casino (casino.h)
 *
 * Pick the bet (the chip: 5 / 25 / 100) and Deal: five cards. Tap the ones
 * to keep (HELD), then Draw: the others are replaced, once. The hand pays
 * from the Casino's shared wallet (casino_wallet.h), "for 1" (the stake
 * included) - the usual 9/6 table:
 *   Royal flush 250  Straight flush 50  Four of a kind 25  Full house 9
 *   Flush 6  Straight 4  Three of a kind 3  Two pair 2  Jacks or better 1
 * Tap after the result to deal again with the same bet. Swipe sideways:
 * back to the Casino. Play money only.
 */
#include "game_videopoker.h"
#include "casino_wallet.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>
#include <string.h>

static const int CX = LCD_WIDTH / 2;
static const uint16_t C_FELT = COLOR565(0x0a, 0x2a, 0x5a), C_FELT2 = COLOR565(0x12, 0x38, 0x70),
                      C_GOLD = COLOR565(0xe8, 0xc0, 0x50), C_RED = COLOR565(0xe0, 0x20, 0x30),
                      C_CARD = COLOR565(0xf4, 0xf0, 0xe6), C_INK = COLOR565(0x18, 0x18, 0x20),
                      C_BTN = COLOR565(0x1c, 0x52, 0x9c);

// ---- the hand ------------------------------------------------------------------------
PokerHand poker_hand(const uint8_t rank[5], const uint8_t suit[5]) {
    int cnt[14] = {};
    for (int i = 0; i < 5; i++) cnt[rank[i]]++;
    bool flush = true;
    for (int i = 1; i < 5; i++) if (suit[i] != suit[0]) flush = false;
    // five different ranks in a row; the ace plays low (A-2-3-4-5) or high (10-J-Q-K-A)
    bool straight = false, royal = false;
    int distinct = 0;
    for (int r = 1; r <= 13; r++) if (cnt[r]) distinct++;
    if (distinct == 5) {
        int lo = 14, hi = 0;
        for (int r = 1; r <= 13; r++) if (cnt[r]) { if (r < lo) lo = r; if (r > hi) hi = r; }
        if (hi - lo == 4) straight = true;
        if (cnt[1] && cnt[10] && cnt[11] && cnt[12] && cnt[13]) straight = royal = true;
    }
    if (straight && flush) return royal ? PH_ROYAL : PH_STRAIGHT_FLUSH;
    int pairs = 0, threes = 0, fours = 0, high_pair = 0;
    for (int r = 1; r <= 13; r++) {
        if (cnt[r] == 4) fours++;
        else if (cnt[r] == 3) threes++;
        else if (cnt[r] == 2) { pairs++; if (r == 1 || r >= 11) high_pair++; }
    }
    if (fours) return PH_FOUR;
    if (threes && pairs) return PH_FULL_HOUSE;
    if (flush) return PH_FLUSH;
    if (straight) return PH_STRAIGHT;
    if (threes) return PH_THREE;
    if (pairs == 2) return PH_TWO_PAIR;
    if (high_pair) return PH_JACKS;
    return PH_NOTHING;
}

int poker_pays(PokerHand h) {
    static const int P[] = {250, 50, 25, 9, 6, 4, 3, 2, 1, 0};
    return h <= PH_NOTHING ? P[h] : 0;
}

const char *poker_name(PokerHand h) {
    static const char *const N[] = {"Royal flush", "Straight flush", "Four of a kind", "Full house", "Flush",
                                    "Straight", "Three of a kind", "Two pair", "Jacks or better", "No win"};
    return h <= PH_NOTHING ? N[h] : "";
}

// ---- the game --------------------------------------------------------------------------
enum VPState { VP_BET, VP_HOLD, VP_RESULT };
static VPState s_state;
static uint8_t s_deck[52], s_pos;
static uint8_t s_rank[5], s_suit[5];
static bool s_held[5];
static const int CHIPS[3] = {5, 25, 100};
static int s_chip = 0;
static int32_t s_bet = 0, s_won = 0;
static PokerHand s_hand = PH_NOTHING;
static uint32_t s_anim_ms = 0;       // when the cards were (re)dealt, for the flip-in
static bool s_prev = false;
static int s_press = -1;             // 0-4 a card, 10 chip, 11 deal / draw
static const char *s_msg = "";
static uint32_t s_msg_ms = 0;

struct Rect { int x, y, w, h; bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };
static const int CW = 68, CH = 98, GAP = 8, CARD_Y = 160;
static Rect card_rect(int i) { return {CX - (5 * CW + 4 * GAP) / 2 + i * (CW + GAP), CARD_Y, CW, CH}; }
static const Rect R_CHIP = {CX - 150, 316, 140, 50}, R_GO = {CX + 10, 316, 140, 50};

static void shuffle() {
    for (int i = 0; i < 52; i++) s_deck[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) { int j = random(i + 1); uint8_t t = s_deck[i]; s_deck[i] = s_deck[j]; s_deck[j] = t; }
    s_pos = 0;
}
static void draw_one(int i) {
    uint8_t c = s_deck[s_pos++];
    s_rank[i] = c % 13 + 1;
    s_suit[i] = c / 13;
}

static void vp_create() {
    s_state = VP_BET;
    s_prev = false;
    s_press = -1;
    s_msg = "";
    memset(s_held, 0, sizeof(s_held));
    for (int i = 0; i < 5; i++) { s_rank[i] = 0; s_suit[i] = 0; }
}

static void say(const char *m) { s_msg = m; s_msg_ms = millis(); }

static void deal() {
    int32_t credit = casino_credit();
    int32_t bet = CHIPS[s_chip];
    if (credit < bet) {
        say(credit < CASINO_MIN_BET ? "Out of chips - the cashier tops up" : "Not enough for that bet");
        return;
    }
    casino_set(credit - bet);
    s_bet = bet;
    s_won = 0;
    shuffle();
    for (int i = 0; i < 5; i++) { draw_one(i); s_held[i] = false; }
    s_hand = poker_hand(s_rank, s_suit);   // shown as a hint: what's already there
    s_state = VP_HOLD;
    s_anim_ms = millis();
    game_sfx_shoot();
}

static void draw_cards() {
    for (int i = 0; i < 5; i++) if (!s_held[i]) draw_one(i);
    s_hand = poker_hand(s_rank, s_suit);
    s_won = s_bet * poker_pays(s_hand);
    if (s_won > 0) casino_set(casino_credit() + s_won);
    casino_record(CG_POKER, s_won - s_bet);
    if (s_hand < PH_NOTHING) casino_record_poker_hand(s_hand);
    s_state = VP_RESULT;
    s_anim_ms = millis();
    if (s_hand <= PH_FOUR) game_sfx_levelup();
    else if (s_won > 0) game_sfx_score();
    else game_sfx_hit();
}

// ---- drawing ---------------------------------------------------------------------------
static void suit_pip(Arduino_GFX *g, int cx, int cy, int s, int suit, uint16_t col) {
    int q = s / 2 > 0 ? s / 2 : 1;
    switch (suit) {
    case 1:   // hearts
        g->fillCircle(cx - q, cy - q / 2, q, col);
        g->fillCircle(cx + q, cy - q / 2, q, col);
        g->fillTriangle(cx - s, cy - q / 3, cx + s, cy - q / 3, cx, cy + s, col);
        break;
    case 2:   // diamonds
        g->fillTriangle(cx, cy - s, cx - (s * 3) / 4, cy, cx + (s * 3) / 4, cy, col);
        g->fillTriangle(cx, cy + s, cx - (s * 3) / 4, cy, cx + (s * 3) / 4, cy, col);
        break;
    case 0:   // spades
        g->fillCircle(cx - q, cy + q / 3, q, col);
        g->fillCircle(cx + q, cy + q / 3, q, col);
        g->fillTriangle(cx - s, cy + q / 4, cx + s, cy + q / 4, cx, cy - s, col);
        g->fillTriangle(cx, cy + q / 2, cx - q, cy + s, cx + q, cy + s, col);
        break;
    default:  // clubs
        g->fillCircle(cx, cy - q, q, col);
        g->fillCircle(cx - q, cy + q / 3, q, col);
        g->fillCircle(cx + q, cy + q / 3, q, col);
        g->fillTriangle(cx, cy, cx - q, cy + s, cx + q, cy + s, col);
        break;
    }
}

static const char *rank_str(int r) {
    static const char *const R[] = {"?", "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
    return r >= 0 && r <= 13 ? R[r] : "?";
}

static void draw_card(Arduino_GFX *g, int i, uint32_t now) {
    Rect r = card_rect(i);
    bool lift = s_held[i] && s_state == VP_HOLD;
    int y = r.y - (lift ? 10 : 0);
    // a back, until it's dealt; the new cards flip in one after another
    bool shown = s_rank[i] != 0;
    if (shown && s_state != VP_BET && !s_held[i]) {
        uint32_t t = now - s_anim_ms;
        shown = t > (uint32_t)(i * 90);
    }
    if (!shown) {
        g->fillRoundRect(r.x, y, r.w, r.h, 7, C_CARD);
        g->fillRoundRect(r.x + 3, y + 3, r.w - 6, r.h - 6, 6, C_RED);
        g->fillRoundRect(r.x + 9, y + 9, r.w - 18, r.h - 18, 4, ui_dim(C_RED, 0.6f));
        return;
    }
    bool win = s_state == VP_RESULT && s_won > 0;
    g->fillRoundRect(r.x, y, r.w, r.h, 7, s_held[i] ? C_GOLD : C_CARD);
    g->fillRoundRect(r.x + 3, y + 3, r.w - 6, r.h - 6, 6, C_CARD);
    uint16_t col = (s_suit[i] == 1 || s_suit[i] == 2) ? C_RED : C_INK;
    ui_text_center(r.x + r.w / 2, y + 22, col, rank_str(s_rank[i]), 3);
    suit_pip(g, r.x + r.w / 2, y + 66, 13, s_suit[i], col);
    if (s_held[i]) ui_text_center(r.x + r.w / 2, y + r.h + 16, win ? COLOR_GOOD : C_GOLD, "HELD", 1);
}

static void button(Arduino_GFX *g, const Rect &r, uint16_t bg, const char *label, bool lit) {
    g->fillRoundRect(r.x, r.y, r.w, r.h, 14, lit ? ui_dim(bg, 0.6f) : bg);
    ui_text_center(r.x + r.w / 2, r.y + r.h / 2, COLOR_TEXT, label, 2);
}

static void vp_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    uint32_t now = millis();
    g->fillScreen(COLOR_BG);
    g->fillCircle(CX, LCD_HEIGHT / 2, 226, C_FELT);
    g->fillCircle(CX, LCD_HEIGHT / 2, 200, C_FELT2);
    g->fillCircle(CX, LCD_HEIGHT / 2, 196, C_FELT);

    ui_text_center(CX, 52, C_GOLD, "VIDEO POKER", 2);
    char b[48];
    snprintf(b, sizeof(b), "$%ld", (long)casino_credit());
    ui_text_center(CX, 84, COLOR_TEXT, b, 2);

    // what the hand is (or will be), above the cards
    const char *head = "";
    uint16_t hc = COLOR_TEXT_DIM;
    if (s_state == VP_BET) head = "Jacks or better";
    else if (s_state == VP_HOLD) { head = s_hand < PH_NOTHING ? poker_name(s_hand) : "Tap the cards to keep"; hc = s_hand < PH_NOTHING ? C_GOLD : COLOR_TEXT_DIM; }
    else { head = poker_name(s_hand); hc = s_won > 0 ? COLOR_GOOD : COLOR_TEXT_DIM; }
    ui_text_center(CX, 126, hc, head, 2);

    for (int i = 0; i < 5; i++) draw_card(g, i, now);

    if (s_state == VP_RESULT) {
        if (s_won > 0) snprintf(b, sizeof(b), "Won $%ld", (long)s_won);
        else snprintf(b, sizeof(b), "Lost $%ld", (long)s_bet);
        ui_text_center(CX, 296, s_won > 0 ? COLOR_GOOD : COLOR_BAD, b, 2);
    }

    snprintf(b, sizeof(b), "Bet $%d", CHIPS[s_chip]);
    button(g, R_CHIP, s_state == VP_HOLD ? COLOR_PANEL : COLOR565(0x7a, 0x5a, 0x10), b, s_press == 10);
    button(g, R_GO, s_state == VP_HOLD ? COLOR565(0x10, 0x80, 0x48) : C_BTN, s_state == VP_HOLD ? "Draw" : "Deal", s_press == 11);

    if (s_msg[0] && now - s_msg_ms < 2200) ui_text_center(CX, 392, COLOR_WARN, s_msg, 1);
    else if (s_state == VP_HOLD) ui_text_center(CX, 392, COLOR_TEXT_DIM, "Keep some, then Draw", 1);
    else ui_text_center(CX, 392, COLOR_TEXT_DIM, "Full house 9  Flush 6  Straight 4", 1);
    ui_text_center(CX, 412, COLOR_TEXT_DIM, "Royal 250  Four of a kind 25", 1);
}

static int hit(int x, int y) {
    if (s_state == VP_HOLD)
        for (int i = 0; i < 5; i++) {
            Rect r = card_rect(i);
            r.y -= 12; r.h += 24;
            if (r.hit(x, y)) return i;
        }
    if (R_CHIP.hit(x, y)) return 10;
    if (R_GO.hit(x, y)) return 11;
    return -1;
}

static void vp_touch(int x, int y, bool pressed) {
    bool down = pressed && !s_prev, up = !pressed && s_prev;
    s_prev = pressed;
    if (down) { s_press = hit(x, y); return; }
    if (!up) return;
    int p = s_press;
    s_press = -1;
    if (p < 0 || hit(x, y) != p) return;
    if (p < 5) { s_held[p] = !s_held[p]; game_sfx_simon_note(p % 4); return; }
    if (p == 10) {
        if (s_state != VP_HOLD) s_chip = (s_chip + 1) % 3;
        return;
    }
    if (s_state == VP_HOLD) draw_cards();
    else deal();
}

static void vp_gesture(Gesture gst) {
    if (gst == GESTURE_SWIPE_LEFT || gst == GESTURE_SWIPE_RIGHT) {
        if (s_state == VP_HOLD) { say("Draw first - the bet is on the table"); return; }
        ui_pop_screen();
    }
}

Screen videopoker_screen = {
    "", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    vp_create, vp_draw, vp_touch, nullptr, nullptr, vp_gesture,
    0, true, false, false,
    true,   // no_drag_back - the hand is finished first
    true,   // hide_status - the wallet sits at the top
};
