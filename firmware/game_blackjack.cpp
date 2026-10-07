/*
 * game_blackjack.cpp
 * Full multi-seat blackjack with persistent virtual money, a live
 * betting countdown, two CPU seats at the same table, and the two
 * most common side bets (Perfect Pairs, 21+3), modeled after the
 * standard conventions used across mainstream online casino
 * blackjack products (bet timer, chip tray, side-bet spots, multiple
 * seats at one table) - adapted to fit a 466x466 round display rather
 * than a phone-sized table, since the two don't fit the same layout.
 *
 * Table rules: dealer stands on any 17+ (soft or hard). Blackjack
 * pays 3:2. A 4-deck shoe, reshuffled fresh every round, replaces the
 * single-deck version - Perfect Pairs' top tier and 21+3's suited-
 * trips tier are both impossible with only one of each card, which a
 * single 52-card deck can never produce; real money-side-bet tables
 * always use multiple decks for exactly this reason.
 *
 * Side bets resolve immediately after the initial deal, independent
 * of how the main hand plays out, matching real-table behavior - not
 * bundled into the end-of-round result.
 *
 * Turn order: CPUs act first (auto-played, paced so it's visible),
 * then the player, then the dealer - the same seat order real tables
 * use, dealer last.
 *
 * Every bet - main and both side bets - resets to zero at the start
 * of each new betting phase; nothing carries over between rounds.
 *
 * Drawing note: only fillRoundRect/fillRect/fillCircle/fillTriangle
 * are used for shapes - no drawRoundRect (documented elsewhere in
 * this project as unreliable on this display's CO5300 driver) and no
 * drawBitmap (implicated, though not fully confirmed, in a real
 * device crash investigation on a different screen this session).
 * Borders are an inset double-fill, the same pattern used everywhere
 * else in this project. Every row position below was verified against
 * this display's actual safe-area radius before being chosen.
 */
#include "game_blackjack.h"
#include "config.h"
#include "board_pins.h"
#include "hal_controller.h"
#include "hal_save.h"
#include "game_audio.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <math.h>

enum BJState { BJ_BETTING, BJ_CPU_TURN, BJ_PLAYER_TURN, BJ_DEALER_TURN, BJ_RESULT, BJ_GAMEOVER };

struct Card { uint8_t rank; uint8_t suit; }; // rank 1-13 (1=Ace,11=J,12=Q,13=K), suit 0-3 (S,H,D,C)

#define NUM_CPUS 2
#define DECK_COUNT 4 // multi-deck shoe - see file header for why
#define DECK_SIZE (52 * DECK_COUNT)
#define MAX_HAND 12 // generous upper bound - real hands rarely exceed 6-8 cards before busting
#define START_MONEY 500
#define BET_TIME_S 12

struct Seat {
    Card hand[MAX_HAND];
    int count;
    int32_t bet;
    const char *result_text;
    uint16_t result_color;
};

static BJState s_state;
static Card s_deck[DECK_SIZE];
static int s_deck_pos;

static Seat s_player;
static Seat s_cpu[NUM_CPUS];
static Card s_dealer_hand[MAX_HAND];
static int s_dealer_count;

static int32_t s_money;
static int32_t s_side_pp;    // Perfect Pairs wager
static int32_t s_side_213;   // 21+3 wager
static const char *s_side_pp_result;
static const char *s_side_213_result;

static uint32_t s_bet_deadline_ms;
static int s_cpu_turn_idx;
static uint32_t s_cpu_step_ms;
static uint32_t s_dealer_step_ms;

static bool s_player_doubled;
static bool s_player_surrendered;
static bool s_insurance_pending;    // dealer shows an Ace and the player hasn't answered the insurance offer yet
static int32_t s_insurance_bet;
static const char *s_insurance_result; // nullptr until settled
static int32_t s_round_start_money;    // for the result screen's +/- delta

// ---- Persistence --------------------------------------------------------
#define BJ_SAVE_MAGIC 0x424a4b32u // "BJK2" - bumped from the single-seat save format
struct BJSave { uint32_t magic; int32_t money; };

static void load_money() {
    BJSave s;
    if (game_load_blob("blackjack_money", &s, sizeof(s)) && s.magic == BJ_SAVE_MAGIC) {
        s_money = s.money;
    } else {
        s_money = START_MONEY;
    }
}
static void save_money() {
    BJSave s;
    s.magic = BJ_SAVE_MAGIC;
    s.money = s_money;
    game_save_blob("blackjack_money", &s, sizeof(s));
}

// ---- Deck / dealing -------------------------------------------------------
static void shuffle_deck() {
    int idx = 0;
    for (int d = 0; d < DECK_COUNT; d++)
        for (int suit = 0; suit < 4; suit++)
            for (int rank = 1; rank <= 13; rank++)
                s_deck[idx++] = Card{(uint8_t)rank, (uint8_t)suit};
    for (int i = DECK_SIZE - 1; i > 0; i--) {
        int j = random(i + 1);
        Card tmp = s_deck[i]; s_deck[i] = s_deck[j]; s_deck[j] = tmp;
    }
    s_deck_pos = 0;
}
static Card deal_card() {
    if (s_deck_pos >= DECK_SIZE) shuffle_deck(); // safety net - a fresh shuffle every round should never actually exhaust the shoe
    return s_deck[s_deck_pos++];
}

static int hand_value(Card *hand, int count) {
    int total = 0, aces = 0;
    for (int i = 0; i < count; i++) {
        int r = hand[i].rank;
        if (r == 1) { total += 11; aces++; }
        else if (r >= 10) total += 10;
        else total += r;
    }
    while (total > 21 && aces > 0) { total -= 10; aces--; }
    return total;
}

static int suit_color(int suit) { return (suit == 1 || suit == 2) ? 1 : 0; } // 1=red (H,D), 0=black (S,C)

// ---- Side bets --------------------------------------------------------
// 3-card straight check, handling both the low-ace (A,2,3) and
// high-ace (Q,K,A) straights - everything else is a plain run of
// three sequential ranks once sorted.
static bool is_straight3(int r0, int r1, int r2) {
    int r[3] = { r0, r1, r2 };
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (r[j] < r[i]) { int t = r[i]; r[i] = r[j]; r[j] = t; }
    if (r[0] + 1 == r[1] && r[1] + 1 == r[2]) return true;
    if (r[0] == 1 && r[1] == 12 && r[2] == 13) return true; // Q,K,A
    return false;
}

static void evaluate_side_bets() {
    s_side_pp_result = nullptr;
    s_side_213_result = nullptr;

    if (s_side_pp > 0) {
        Card a = s_player.hand[0], b = s_player.hand[1];
        int mult = 0;
        const char *label = "No pair";
        if (a.rank == b.rank) {
            if (a.suit == b.suit) { mult = 25; label = "Perfect Pair 25:1"; }
            else if (suit_color(a.suit) == suit_color(b.suit)) { mult = 12; label = "Colored Pair 12:1"; }
            else { mult = 5; label = "Mixed Pair 5:1"; }
        }
        if (mult > 0) { s_money += s_side_pp * mult; s_side_pp_result = label; }
        else { s_money -= s_side_pp; s_side_pp_result = label; }
    }

    if (s_side_213 > 0) {
        Card a = s_player.hand[0], b = s_player.hand[1], c = s_dealer_hand[0]; // dealer's up-card only
        bool flush = (a.suit == b.suit && b.suit == c.suit);
        bool trips = (a.rank == b.rank && b.rank == c.rank);
        bool straight = is_straight3(a.rank, b.rank, c.rank);
        int mult = 0;
        const char *label = "No hand";
        if (trips && flush) { mult = 100; label = "Suited Trips 100:1"; }
        else if (straight && flush) { mult = 40; label = "Straight Flush 40:1"; }
        else if (trips) { mult = 30; label = "Three of a Kind 30:1"; }
        else if (straight) { mult = 10; label = "Straight 10:1"; }
        else if (flush) { mult = 5; label = "Flush 5:1"; }
        if (mult > 0) { s_money += s_side_213 * mult; s_side_213_result = label; }
        else { s_money -= s_side_213; s_side_213_result = label; }
    }
    if (s_side_pp > 0 || s_side_213 > 0) save_money();
}

// ---- Round flow -------------------------------------------------------
static void start_betting() {
    s_player.bet = 0;
    s_player.count = 0;
    s_side_pp = 0;
    s_side_213 = 0;
    s_side_pp_result = nullptr;
    s_side_213_result = nullptr;
    for (int i = 0; i < NUM_CPUS; i++) { s_cpu[i].count = 0; s_cpu[i].result_text = nullptr; }
    s_player.result_text = nullptr;
    s_bet_deadline_ms = millis() + BET_TIME_S * 1000;
    s_state = BJ_BETTING;
}

static void deal_initial() {
    s_round_start_money = s_money;
    shuffle_deck();
    s_dealer_count = 0;
    s_player.count = 0;
    for (int i = 0; i < NUM_CPUS; i++) {
        s_cpu[i].count = 0;
        s_cpu[i].bet = 10 * (1 + random(10)); // flavor only - not drawn from real money
        s_cpu[i].result_text = nullptr;
    }

    // Two passes, one card at a time to each seat then the dealer -
    // matches how a real table actually deals, not that it changes
    // anything about the odds here.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < NUM_CPUS; i++) s_cpu[i].hand[s_cpu[i].count++] = deal_card();
        s_player.hand[s_player.count++] = deal_card();
        s_dealer_hand[s_dealer_count++] = deal_card();
    }
    game_sfx_hit();
    evaluate_side_bets();

    s_player.result_text = nullptr;
    s_player_doubled = false;
    s_player_surrendered = false;
    s_insurance_pending = (s_dealer_hand[0].rank == 1); // dealer's up-card is an Ace
    s_insurance_bet = 0;
    s_insurance_result = nullptr;

    s_cpu_turn_idx = 0;
    s_cpu_step_ms = millis();
    s_state = BJ_CPU_TURN;
}

static void resolve_seat(Seat &seat, int dealer_value, bool dealer_busted) {
    int v = hand_value(seat.hand, seat.count);
    bool seat_blackjack = (seat.count == 2 && v == 21);
    bool dealer_blackjack = (s_dealer_count == 2 && dealer_value == 21);
    if (v > 21) {
        seat.result_text = "Bust";
        seat.result_color = COLOR_BAD;
        s_money -= seat.bet;
    } else if (seat_blackjack && !dealer_blackjack) {
        seat.result_text = "Blackjack!";
        seat.result_color = COLOR_GOOD;
        s_money += (seat.bet * 3) / 2;
        game_sfx_levelup();
    } else if (dealer_busted) {
        seat.result_text = "Win";
        seat.result_color = COLOR_GOOD;
        s_money += seat.bet;
    } else if (v > dealer_value) {
        seat.result_text = "Win";
        seat.result_color = COLOR_GOOD;
        s_money += seat.bet;
    } else if (v < dealer_value) {
        seat.result_text = "Lose";
        seat.result_color = COLOR_BAD;
        s_money -= seat.bet;
    } else {
        seat.result_text = "Push";
        seat.result_color = COLOR_TEXT_DIM;
    }
}

// CPU seats use play money only (never touch s_money) - they're here
// for table atmosphere, not a second real wallet to track.
static void resolve_cpu_seat(Seat &seat, int dealer_value, bool dealer_busted) {
    int v = hand_value(seat.hand, seat.count);
    if (v > 21) { seat.result_text = "Bust"; seat.result_color = COLOR_BAD; }
    else if (dealer_busted || v > dealer_value) { seat.result_text = "Win"; seat.result_color = COLOR_GOOD; }
    else if (v < dealer_value) { seat.result_text = "Lose"; seat.result_color = COLOR_BAD; }
    else { seat.result_text = "Push"; seat.result_color = COLOR_TEXT_DIM; }
}

// ---- Player actions: insurance / double / surrender ------------------

static void accept_insurance() {
    s_insurance_bet = s_player.bet / 2;
    s_insurance_pending = false;
    bool dealer_bj = (hand_value(s_dealer_hand, 2) == 21); // hole card is already dealt, just hidden from display - peeking it doesn't need a new card
    if (dealer_bj) {
        s_money += s_insurance_bet * 2; // 2:1 payout - net profit is 2x the side wager since it was never deducted upfront (same "no upfront deduction" pattern as the main bet)
        s_insurance_result = "Won";
        s_state = BJ_DEALER_TURN; // dealer already has blackjack - the existing dealer-turn tick resolves this on its very next step
        s_dealer_step_ms = millis();
    } else {
        s_money -= s_insurance_bet;
        s_insurance_result = "Lost";
    }
    save_money();
}
static void decline_insurance() {
    s_insurance_pending = false;
    s_insurance_result = nullptr;
}

static void double_down() {
    s_player.bet *= 2;
    s_player_doubled = true;
    if (s_player.count < MAX_HAND) s_player.hand[s_player.count++] = deal_card();
    game_sfx_hit();
    s_state = BJ_DEALER_TURN;
    s_dealer_step_ms = millis();
}

static void surrender() {
    s_player_surrendered = true;
    s_money -= s_player.bet / 2;
    s_player.result_text = "Surrendered";
    s_player.result_color = COLOR_WARN;
    save_money();
    // The dealer still plays out normally so the CPU seats resolve
    // correctly against a completed dealer hand - only the player's
    // own result is already locked in and skipped in resolve_result().
    s_state = BJ_DEALER_TURN;
    s_dealer_step_ms = millis();
}

static void resolve_result() {
    int dv = hand_value(s_dealer_hand, s_dealer_count);
    bool dealer_busted = dv > 21;
    if (!s_player_surrendered) resolve_seat(s_player, dv, dealer_busted); // a surrendered result is already locked in and shouldn't be overwritten by how the dealer's hand happens to finish
    for (int i = 0; i < NUM_CPUS; i++) resolve_cpu_seat(s_cpu[i], dv, dealer_busted);
    if (strcmp(s_player.result_text, "Win") == 0 || strcmp(s_player.result_text, "Blackjack!") == 0) game_sfx_score();
    else if (strcmp(s_player.result_text, "Bust") == 0 || strcmp(s_player.result_text, "Lose") == 0) game_sfx_gameover();
    else if (strcmp(s_player.result_text, "Surrendered") == 0) game_sfx_hit();
    save_money();
    s_state = BJ_RESULT;
}

// ---- Drawing --------------------------------------------------------------
//
// Round-table layout. The screen edge is the score board: the top half of
// the ring fills with the dealer's total, the bottom half with yours (to
// 21 - green, gold at exactly 21, red when bust). During betting the whole
// ring is the countdown. Buttons are round and sit along the lower curve
// where the circle has room; the big numbers (totals, bank, bet) are the
// biggest things on screen.

struct RBtn { int16_t cx, cy, r; };

// Betting
static const RBtn BET_SPOT  = { LCD_WIDTH / 2, 196, 46 };
static const RBtn PP_SPOT   = { 106, 196, 32 };
static const RBtn T213_SPOT = { LCD_WIDTH - 106, 196, 32 };
static const RBtn CHIPS[4]  = { { 113, 292, 30 }, { 193, 292, 30 }, { 273, 292, 30 }, { 353, 292, 30 } };
static const int32_t CHIP_VALUES[4] = { 10, 25, 50, 100 };
static const RBtn DEAL_BTN  = { LCD_WIDTH / 2, 384, 40 };
static const RBtn CLEAR_BTN = { 126, 364, 26 };
// Playing
static const RBtn HIT_BTN   = { 153, 376, 40 };
static const RBtn STAND_BTN = { LCD_WIDTH - 153, 376, 40 };
static const RBtn DBL_BTN   = { 74, 300, 28 };
static const RBtn SURR_BTN  = { LCD_WIDTH - 74, 300, 28 };

#define CARD_W 38
#define CARD_H 54
#define BIG_W 46
#define BIG_H 64
#define MINI_W 22
#define MINI_H 30

static const uint16_t COLOR_CARD_RED = (uint16_t)COLOR565(0xE0, 0x20, 0x30);
static const uint16_t COLOR_GOLD     = (uint16_t)COLOR565(0xFF, 0xC8, 0x30);

static bool rbtn_hit(const RBtn &b, int x, int y, int slack = 8) {
    int dx = x - b.cx, dy = y - b.cy, r = b.r + slack;
    return dx * dx + dy * dy <= r * r;
}

static void draw_rbtn(Arduino_GFX *g, const RBtn &b, uint16_t color, const char *label, int text_sz, bool enabled) {
    uint16_t c = enabled ? color : COLOR_PANEL;
    if (enabled) g->fillCircle(b.cx, b.cy, b.r + 4, ui_dim(color, 0.3f)); // soft halo
    g->fillCircle(b.cx, b.cy, b.r, c);
    ui_text_center(b.cx, b.cy, enabled ? COLOR_TEXT : COLOR_TEXT_DIM, label, text_sz);
}

static const char *rank_str(int r) {
    static char buf[3];
    if (r == 1) return "A";
    if (r == 11) return "J";
    if (r == 12) return "Q";
    if (r == 13) return "K";
    snprintf(buf, sizeof(buf), "%d", r);
    return buf;
}

// Suit pips drawn from primitives (s = rough half-size in px).
static void draw_suit(Arduino_GFX *g, int cx, int cy, int s, int suit, uint16_t col) {
    if (s < 2) s = 2;
    int q = s / 2 > 0 ? s / 2 : 1;
    switch (suit) {
        case 1: // hearts
            g->fillCircle(cx - q, cy - q / 2, q, col);
            g->fillCircle(cx + q, cy - q / 2, q, col);
            g->fillTriangle(cx - s, cy - q / 3, cx + s, cy - q / 3, cx, cy + s, col);
            break;
        case 2: // diamonds
            g->fillTriangle(cx, cy - s, cx - (s * 3) / 4, cy, cx + (s * 3) / 4, cy, col);
            g->fillTriangle(cx, cy + s, cx - (s * 3) / 4, cy, cx + (s * 3) / 4, cy, col);
            break;
        case 0: // spades
            g->fillCircle(cx - q, cy + q / 3, q, col);
            g->fillCircle(cx + q, cy + q / 3, q, col);
            g->fillTriangle(cx - s, cy + q / 4, cx + s, cy + q / 4, cx, cy - s, col);
            g->fillTriangle(cx, cy + q / 2, cx - q, cy + s, cx + q, cy + s, col);
            break;
        default: // clubs
            g->fillCircle(cx, cy - q, q, col);
            g->fillCircle(cx - q, cy + q / 3, q, col);
            g->fillCircle(cx + q, cy + q / 3, q, col);
            g->fillTriangle(cx, cy, cx - q, cy + s, cx + q, cy + s, col);
            break;
    }
}

// White card face, big rank top-left-ish, suit pip below - readable at a glance.
static void draw_card_sized(Arduino_GFX *g, int x, int y, Card c, bool face_down, int w, int h, int text_sz) {
    if (face_down) {
        g->fillRoundRect(x, y, w, h, 5, COLOR_TEXT);
        g->fillRoundRect(x + 2, y + 2, w - 4, h - 4, 4, COLOR_ACCENT);
        g->fillRoundRect(x + 6, y + 6, w - 12, h - 12, 3, ui_dim(COLOR_ACCENT, 0.55f));
        return;
    }
    g->fillRoundRect(x, y, w, h, 5, COLOR_TEXT);
    uint16_t col = (suit_color(c.suit) == 1) ? COLOR_CARD_RED : COLOR_BG;
    const char *rs = rank_str(c.rank);
    g->setTextSize(text_sz);
    g->setTextColor(col);
    int rw = (int)strlen(rs) * 6 * text_sz;
    g->setCursor(x + (w - rw) / 2 + (text_sz > 1 ? 1 : 0), y + 4);
    g->print(rs);
    int pip = h / 5;
    draw_suit(g, x + w / 2, y + h - pip - 5, pip, c.suit, col);
}

static void draw_hand(Arduino_GFX *g, Card *hand, int count, int y, bool hide_last, int w, int h, int text_sz) {
    if (count <= 0) return;
    // Overlap cards once they no longer fit side by side.
    int step = w + 5;
    int max_w = 300;
    if (count * w + (count - 1) * 5 > max_w) step = (max_w - w) / (count - 1);
    int total_w = (count - 1) * step + w;
    int x = LCD_WIDTH / 2 - total_w / 2;
    for (int i = 0; i < count; i++) {
        bool face_down = hide_last && (i == count - 1);
        draw_card_sized(g, x, y, hand[i], face_down, w, h, text_sz);
        x += step;
    }
}

static uint16_t total_color(int v) {
    if (v > 21) return COLOR_BAD;
    if (v == 21) return COLOR_GOLD;
    if (v >= 17) return COLOR_GOOD;
    return COLOR_ACCENT2;
}

// Half-ring score meter: top = dealer (clockwise from 9 o'clock over the
// top), bottom = player (counter-clockwise from 9 o'clock under the bottom).
static void draw_score_arc(bool top, int value, uint16_t override_color) {
    float start = top ? 272.0f : 268.0f;
    float full = top ? 176.0f : -176.0f;
    ui_edge_ring(start, full, ui_dim(COLOR_PANEL, 1.0f), 8, 4);
    if (value <= 0) return;
    int v = value > 21 ? 21 : value;
    uint16_t c = override_color ? override_color : total_color(value);
    ui_edge_ring(start, full * v / 21.0f, c, 8, 4);
}

static void draw_badge(Arduino_GFX *g, int cx, int cy, int r, const char *txt, uint16_t color, int text_sz) {
    g->fillCircle(cx, cy, r + 3, ui_dim(color, 0.35f));
    g->fillCircle(cx, cy, r, COLOR_BG);
    ui_arc(cx, cy, r, 3, 0, 360, color, false);
    ui_text_center(cx, cy, color, txt, text_sz);
}

static void draw_chip(Arduino_GFX *g, const RBtn &b, uint16_t color, const char *label, bool enabled) {
    uint16_t c = enabled ? color : COLOR_PANEL;
    g->fillCircle(b.cx, b.cy, b.r, c);
    // edge notches - the casino-chip look
    for (int k = 0; k < 6; k++)
        ui_arc(b.cx, b.cy, b.r, 6, k * 60.0f + 15.0f, 14.0f, enabled ? COLOR_TEXT : COLOR_TEXT_DIM, false);
    g->fillCircle(b.cx, b.cy, b.r - 9, ui_dim(c, 0.7f));
    ui_text_center(b.cx, b.cy, enabled ? COLOR_TEXT : COLOR_TEXT_DIM, label, 2);
}

static void draw_betting(Arduino_GFX *g) {
    int32_t remain_ms = (int32_t)(s_bet_deadline_ms - millis());
    if (remain_ms < 0) remain_ms = 0;
    int remain_s = (remain_ms + 999) / 1000;
    uint16_t tcol = remain_s <= 3 ? COLOR_BAD : (remain_s <= 6 ? COLOR_WARN : COLOR_GOOD);

    // Countdown ring around the whole edge
    float frac = (float)remain_ms / (float)(BET_TIME_S * 1000);
    ui_edge_ring(0, 360, ui_dim(COLOR_PANEL, 1.0f), 8, 4, false);
    if (frac > 0.0f) ui_edge_ring(0, 360.0f * frac, tcol, 8, 4);

    char buf[24];
    snprintf(buf, sizeof(buf), "%d", remain_s);
    ui_text_center(LCD_WIDTH / 2, 52, tcol, buf, 3);
    ui_text_center(LCD_WIDTH / 2, 80, COLOR_TEXT_DIM, "PLACE YOUR BETS", 1);
    snprintf(buf, sizeof(buf), "$%ld", (long)s_money);
    ui_text_center(LCD_WIDTH / 2, 118, COLOR_TEXT, buf, 3);

    // Betting spots: main bet in the middle, side bets either side
    uint16_t spot_col = s_player.bet > 0 ? COLOR_GOLD : COLOR_TEXT_DIM;
    g->fillCircle(BET_SPOT.cx, BET_SPOT.cy, BET_SPOT.r, ui_dim(COLOR_GOLD, s_player.bet > 0 ? 0.18f : 0.08f));
    ui_arc(BET_SPOT.cx, BET_SPOT.cy, BET_SPOT.r, 3, 0, 360, spot_col, false);
    ui_text_center(BET_SPOT.cx, BET_SPOT.cy - 20, COLOR_TEXT_DIM, "BET", 1);
    snprintf(buf, sizeof(buf), "$%ld", (long)s_player.bet);
    ui_text_center(BET_SPOT.cx, BET_SPOT.cy + 6, s_player.bet > 0 ? COLOR_TEXT : COLOR_TEXT_DIM, buf, s_player.bet >= 1000 ? 2 : 3);

    const RBtn *sides[2] = { &PP_SPOT, &T213_SPOT };
    const char *side_names[2] = { "PP", "21+3" };
    int32_t side_vals[2] = { s_side_pp, s_side_213 };
    for (int i = 0; i < 2; i++) {
        const RBtn &sp = *sides[i];
        bool on = side_vals[i] > 0;
        g->fillCircle(sp.cx, sp.cy, sp.r, ui_dim(COLOR_ACCENT2, on ? 0.25f : 0.08f));
        ui_arc(sp.cx, sp.cy, sp.r, 2, 0, 360, on ? COLOR_ACCENT2 : COLOR_TEXT_DIM, false);
        ui_text_center(sp.cx, sp.cy - 9, COLOR_TEXT_DIM, side_names[i], 1);
        snprintf(buf, sizeof(buf), on ? "$%ld" : "+5", (long)side_vals[i]);
        ui_text_center(sp.cx, sp.cy + 8, on ? COLOR_TEXT : COLOR_TEXT_DIM, buf, on ? 2 : 1);
    }

    // Chips
    static const uint16_t CHIP_COLORS[4] = { COLOR_ACCENT2, COLOR_GOOD, (uint16_t)COLOR565(0xFF, 0x7A, 0x1A), COLOR_ACCENT };
    for (int i = 0; i < 4; i++) {
        char lbl[6];
        snprintf(lbl, sizeof(lbl), "%ld", (long)CHIP_VALUES[i]);
        draw_chip(g, CHIPS[i], CHIP_COLORS[i], lbl, s_player.bet + s_side_pp + s_side_213 < s_money);
    }

    bool any_bet = s_player.bet > 0 || s_side_pp > 0 || s_side_213 > 0;
    draw_rbtn(g, CLEAR_BTN, COLOR_PANEL, "CLR", 1, any_bet);
    draw_rbtn(g, DEAL_BTN, COLOR_GOOD, "DEAL", 2, s_player.bet > 0);
}

// CPU seats: compact, on the left/right flanks - table atmosphere, not focus.
static void draw_cpu_seats(Arduino_GFX *g) {
    const int cxs[NUM_CPUS] = { 76, LCD_WIDTH - 76 };
    for (int i = 0; i < NUM_CPUS && i < 2; i++) {
        int cx = cxs[i];
        int v = hand_value(s_cpu[i].hand, s_cpu[i].count);
        char nbuf[16];
        snprintf(nbuf, sizeof(nbuf), "CPU%d", i + 1);
        ui_text_center(cx, 144, COLOR_TEXT_DIM, nbuf, 1);
        uint16_t col = s_cpu[i].result_text ? s_cpu[i].result_color : (v > 21 ? COLOR_BAD : COLOR_TEXT);
        if (s_cpu[i].result_text) snprintf(nbuf, sizeof(nbuf), "%s", s_cpu[i].result_text);
        else snprintf(nbuf, sizeof(nbuf), "%d", v);
        ui_text_center(cx, 160, col, nbuf, s_cpu[i].result_text ? 1 : 2);
        int n = s_cpu[i].count;
        if (n <= 0) continue;
        // side by side while they fit in ~80px, then overlap
        int step = MINI_W + 3;
        if (n > 1 && MINI_W + (n - 1) * step > 80) step = (80 - MINI_W) / (n - 1);
        int total_w = MINI_W + (n - 1) * step;
        int x = cx - total_w / 2;
        for (int k = 0; k < n; k++) {
            draw_card_sized(g, x, 174, s_cpu[i].hand[k], false, MINI_W, MINI_H, 1);
            x += step;
        }
    }
}

static void blackjack_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    char buf[32];

    if (s_state == BJ_GAMEOVER) {
        ui_edge_ring(0, 360, ui_dim(COLOR_BAD, 0.5f), 8, 4, false);
        ui_text_center(LCD_WIDTH / 2, 170, COLOR_BAD, "OUT OF CHIPS", 3);
        ui_text_center(LCD_WIDTH / 2, 220, COLOR_TEXT_DIM, "Tap to restart with", 1);
        snprintf(buf, sizeof(buf), "$%d", START_MONEY);
        ui_text_center(LCD_WIDTH / 2, 262, COLOR_GOLD, buf, 4);
        return;
    }

    if (s_state == BJ_BETTING) {
        draw_betting(g);
        return;
    }

    bool hide_hole = (s_state == BJ_CPU_TURN || s_state == BJ_PLAYER_TURN);
    int dv = hide_hole ? hand_value(s_dealer_hand, 1) : hand_value(s_dealer_hand, s_dealer_count);
    int pv = hand_value(s_player.hand, s_player.count);

    // ---- Edge score meters (pulse in the result colour at the end) ----
    uint16_t result_col = 0;
    if (s_state == BJ_RESULT) {
        float pulse = 0.6f + 0.4f * sinf((float)millis() * 0.008f);
        result_col = ui_dim(s_player.result_color, pulse);
    }
    draw_score_arc(true, dv, result_col);
    draw_score_arc(false, pv, result_col);

    // ---- Dealer ----------------------------------------------------------
    if (hide_hole) snprintf(buf, sizeof(buf), "%d+", dv);
    else snprintf(buf, sizeof(buf), "%d", dv);
    draw_badge(g, LCD_WIDTH / 2, 50, 24, buf, hide_hole ? COLOR_TEXT_DIM : total_color(dv), dv >= 10 && hide_hole ? 2 : 3);
    ui_text_center(LCD_WIDTH / 2 - 52, 50, COLOR_TEXT_DIM, "DEALER", 1);
    draw_hand(g, s_dealer_hand, s_dealer_count, 82, hide_hole, CARD_W, CARD_H, 2);

    draw_cpu_seats(g);

    // ---- Centre line: bank & bet, or the result -----------------------
    if (s_state == BJ_RESULT) {
        const char *rt = s_player.result_text ? s_player.result_text : "";
        ui_text_center(LCD_WIDTH / 2, 160, s_player.result_color, rt, strlen(rt) > 8 ? 2 : 3);
        int32_t delta = s_money - s_round_start_money;
        if (delta > 0) snprintf(buf, sizeof(buf), "+$%ld", (long)delta);
        else if (delta < 0) snprintf(buf, sizeof(buf), "-$%ld", (long)-delta);
        else snprintf(buf, sizeof(buf), "$0");
        ui_text_center(LCD_WIDTH / 2, 188, delta > 0 ? COLOR_GOOD : (delta < 0 ? COLOR_BAD : COLOR_TEXT_DIM), buf, 2);
    } else {
        snprintf(buf, sizeof(buf), "$%ld", (long)s_money);
        ui_text_center(LCD_WIDTH / 2, 160, COLOR_TEXT, buf, 2);
        snprintf(buf, sizeof(buf), s_player_doubled ? "BET $%ld x2" : "BET $%ld", (long)s_player.bet);
        ui_text_center(LCD_WIDTH / 2, 182, COLOR_GOLD, buf, 1);
    }

    // ---- Player -------------------------------------------------------
    int bw = s_player.count > 5 ? CARD_W : BIG_W, bh = s_player.count > 5 ? CARD_H : BIG_H;
    draw_hand(g, s_player.hand, s_player.count, 206, false, bw, bh, s_player.count > 5 ? 2 : 3);
    snprintf(buf, sizeof(buf), "%d", pv);
    draw_badge(g, LCD_WIDTH / 2, 308, 26, buf, total_color(pv), 3);

    // ---- Actions / status ---------------------------------------------
    if (s_state == BJ_CPU_TURN || s_state == BJ_DEALER_TURN) {
        int dots = (millis() / 300) % 4;
        snprintf(buf, sizeof(buf), "%s%.*s", s_state == BJ_CPU_TURN ? "PLAYERS ACTING" : "DEALER'S TURN", dots, "...");
        ui_text_center(LCD_WIDTH / 2, 372, COLOR_TEXT_DIM, buf, 2);
    } else if (s_state == BJ_PLAYER_TURN && s_insurance_pending) {
        snprintf(buf, sizeof(buf), "INSURANCE? $%ld", (long)(s_player.bet / 2));
        ui_text_center(LCD_WIDTH / 2, 348, COLOR_WARN, buf, 1);
        draw_rbtn(g, HIT_BTN, COLOR_GOOD, "YES", 2, true);
        draw_rbtn(g, STAND_BTN, COLOR_BAD, "NO", 2, true);
    } else if (s_state == BJ_PLAYER_TURN) {
        draw_rbtn(g, HIT_BTN, COLOR_ACCENT2, "HIT", 2, true);
        draw_rbtn(g, STAND_BTN, COLOR_ACCENT, "STAND", 2, true);
        bool first_decision = (s_player.count == 2 && !s_player_doubled);
        if (first_decision) {
            bool can_double = s_player.bet * 2 <= s_money;
            draw_rbtn(g, DBL_BTN, COLOR_GOLD, "x2", 2, can_double);
            draw_rbtn(g, SURR_BTN, COLOR_WARN, "SURR", 1, true);
            ui_text_center(DBL_BTN.cx, DBL_BTN.cy + 40, COLOR_TEXT_DIM, "DOUBLE", 1);
            ui_text_center(SURR_BTN.cx - 4, SURR_BTN.cy + 40, COLOR_TEXT_DIM, "SURRENDER", 1);
        }
    } else if (s_state == BJ_RESULT) {
        int line_y = 350;
        if (s_insurance_result) {
            snprintf(buf, sizeof(buf), "Insurance: %s", s_insurance_result);
            ui_text_center(LCD_WIDTH / 2, line_y, strcmp(s_insurance_result, "Won") == 0 ? COLOR_GOOD : COLOR_BAD, buf, 1);
            line_y += 14;
        }
        if (s_side_pp_result) {
            bool won = !(s_side_pp > 0 && strncmp(s_side_pp_result, "Perfect", 7) != 0 &&
                         strncmp(s_side_pp_result, "Colored", 7) != 0 && strncmp(s_side_pp_result, "Mixed", 5) != 0);
            snprintf(buf, sizeof(buf), "PP: %s", s_side_pp_result);
            ui_text_center(LCD_WIDTH / 2, line_y, won ? COLOR_GOOD : COLOR_BAD, buf, 1);
            line_y += 14;
        }
        if (s_side_213_result) {
            snprintf(buf, sizeof(buf), "21+3: %s", s_side_213_result);
            ui_text_center(LCD_WIDTH / 2, line_y, strcmp(s_side_213_result, "No hand") == 0 ? COLOR_BAD : COLOR_GOOD, buf, 1);
            line_y += 14;
        }
        ui_text_center(LCD_WIDTH / 2, line_y + 22, COLOR_TEXT, "TAP TO PLAY AGAIN", 1);
    }
}

// ---- Touch / tick -----------------------------------------------------

static bool s_prev_pressed; // rising-edge only - every action below is a one-shot tap, none are drags, so this must not fire once per frame while held (see the project-wide touch-handling fix elsewhere this session for why that matters)

static void player_hit() {
    if (s_player.count < MAX_HAND) s_player.hand[s_player.count++] = deal_card();
    game_sfx_hit();
    if (hand_value(s_player.hand, s_player.count) >= 21) {
        s_state = BJ_DEALER_TURN;
        s_dealer_step_ms = millis();
    }
}

static void player_stand() {
    s_state = BJ_DEALER_TURN;
    s_dealer_step_ms = millis();
}

static void blackjack_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;

    if (s_state == BJ_GAMEOVER) {
        s_money = START_MONEY;
        save_money();
        start_betting();
        return;
    }

    if (s_state == BJ_BETTING) {
        for (int i = 0; i < 4; i++) {
            if (rbtn_hit(CHIPS[i], x, y, 4)) {
                int32_t new_bet = s_player.bet + CHIP_VALUES[i];
                s_player.bet = (new_bet + s_side_pp + s_side_213 <= s_money) ? new_bet : s_money - s_side_pp - s_side_213;
                if (s_player.bet < 0) s_player.bet = 0;
                game_sfx_hit();
                return;
            }
        }
        if (rbtn_hit(PP_SPOT, x, y)) {
            int32_t nb = s_side_pp + 5;
            if (nb + s_player.bet + s_side_213 <= s_money) s_side_pp = nb;
            return;
        }
        if (rbtn_hit(T213_SPOT, x, y)) {
            int32_t nb = s_side_213 + 5;
            if (nb + s_player.bet + s_side_pp <= s_money) s_side_213 = nb;
            return;
        }
        if (rbtn_hit(CLEAR_BTN, x, y)) {
            s_player.bet = 0; s_side_pp = 0; s_side_213 = 0;
            return;
        }
        if (rbtn_hit(DEAL_BTN, x, y) || rbtn_hit(BET_SPOT, x, y, 0)) {
            if (s_player.bet > 0) deal_initial();
            return;
        }
        return;
    }

    if (s_state == BJ_PLAYER_TURN && s_insurance_pending) {
        if (rbtn_hit(HIT_BTN, x, y)) { accept_insurance(); return; }
        if (rbtn_hit(STAND_BTN, x, y)) { decline_insurance(); return; }
        return;
    }

    if (s_state == BJ_PLAYER_TURN) {
        if (rbtn_hit(HIT_BTN, x, y)) { player_hit(); return; }
        if (rbtn_hit(STAND_BTN, x, y)) { player_stand(); return; }
        bool first_decision = (s_player.count == 2 && !s_player_doubled);
        if (first_decision && rbtn_hit(DBL_BTN, x, y)) {
            if (s_player.bet * 2 <= s_money) double_down();
            return;
        }
        if (first_decision && rbtn_hit(SURR_BTN, x, y)) {
            surrender();
            return;
        }
        return;
    }

    if (s_state == BJ_RESULT) {
        start_betting();
        return;
    }
}

static void blackjack_tick() {
    if (controller_connected() && s_state == BJ_PLAYER_TURN) {
        static bool s_ctrl_a_prev = false, s_ctrl_b_prev = false, s_ctrl_x_prev = false, s_ctrl_y_prev = false;
        bool a = controller_button(CTRL_BTN_A), b = controller_button(CTRL_BTN_B);
        bool x = controller_button(CTRL_BTN_X), y = controller_button(CTRL_BTN_Y);
        if (s_insurance_pending) {
            if (a && !s_ctrl_a_prev) accept_insurance();
            if (b && !s_ctrl_b_prev) decline_insurance();
        } else {
            if (a && !s_ctrl_a_prev) {
                if (s_player.count < MAX_HAND) s_player.hand[s_player.count++] = deal_card();
                game_sfx_hit();
                if (hand_value(s_player.hand, s_player.count) >= 21) {
                    s_state = BJ_DEALER_TURN;
                    s_dealer_step_ms = millis();
                }
            }
            if (b && !s_ctrl_b_prev) {
                s_state = BJ_DEALER_TURN;
                s_dealer_step_ms = millis();
            }
            bool first_decision = (s_player.count == 2 && !s_player_doubled);
            if (first_decision && x && !s_ctrl_x_prev && s_player.bet * 2 <= s_money) double_down();
            if (first_decision && y && !s_ctrl_y_prev) surrender();
        }
        s_ctrl_a_prev = a; s_ctrl_b_prev = b; s_ctrl_x_prev = x; s_ctrl_y_prev = y;
    }

    if (s_state == BJ_BETTING) {
        if (millis() >= s_bet_deadline_ms) {
            if (s_player.bet > 0) deal_initial();
            else s_bet_deadline_ms = millis() + BET_TIME_S * 1000; // no bet placed in time - give a fresh timer rather than force a wager
        }
        return;
    }

    if (s_state == BJ_CPU_TURN) {
        if (millis() - s_cpu_step_ms < 500) return;
        s_cpu_step_ms = millis();
        if (s_cpu_turn_idx >= NUM_CPUS) {
            bool player_blackjack = (s_player.count == 2 && hand_value(s_player.hand, s_player.count) == 21);
            if (player_blackjack) {
                s_state = BJ_DEALER_TURN;
                s_dealer_step_ms = millis();
            } else {
                s_state = BJ_PLAYER_TURN;
            }
            return;
        }
        Seat &cs = s_cpu[s_cpu_turn_idx];
        int v = hand_value(cs.hand, cs.count);
        if (v < 17 && cs.count < MAX_HAND) {
            cs.hand[cs.count++] = deal_card();
            game_sfx_hit();
        } else {
            s_cpu_turn_idx++;
        }
        return;
    }

    if (s_state != BJ_DEALER_TURN) return;
    if (millis() - s_dealer_step_ms < 700) return;
    s_dealer_step_ms = millis();

    int pv = hand_value(s_player.hand, s_player.count);
    if (pv > 21) { resolve_result(); return; } // already busted - dealer doesn't need to draw at all
    int dv = hand_value(s_dealer_hand, s_dealer_count);
    if (dv < 17) {
        if (s_dealer_count < MAX_HAND) s_dealer_hand[s_dealer_count++] = deal_card();
        game_sfx_hit();
    } else {
        resolve_result();
    }
}

static void blackjack_create() {
    load_money();
    start_betting();
    if (s_money <= 0) s_state = BJ_GAMEOVER;
    s_prev_pressed = false; // don't let a stale true from a previous session swallow this session's first tap
}

static void blackjack_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen blackjack_screen = {
    "", GESTURE_MODE_EDGE,   // no header - the table uses the whole round screen
    UI_FRAME_MS_DEFAULT,
    blackjack_create, blackjack_draw, blackjack_touch, blackjack_tick, nullptr, blackjack_gesture,
    0, false, false, false, false,
    true, // hide_status - the edge is the score/countdown ring
};
