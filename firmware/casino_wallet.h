/*
 * casino_wallet.h
 * One credit for every game in the Casino (casino.h): Crystal Cavern,
 * Blackjack, Video Poker, Roulette and Hi-Lo Dice all bet from it and pay
 * into it, and the
 * Casino's map shows it. Play money only. Kept in the game save
 * ("casino_wallet"); the first time, it starts from what the two older
 * games had saved on their own (their balances added), else $1000.
 * Out of chips: the cashier tops it back up to CASINO_TOP_UP.
 */
#pragma once
#include <stdint.h>

#define CASINO_START   1000
#define CASINO_TOP_UP  500
#define CASINO_MIN_BET 5

int32_t casino_credit();
void    casino_set(int32_t credit);
// Below the smallest bet: back up to CASINO_TOP_UP. The new credit.
int32_t casino_top_up();

// ---- stats and the daily bonus (the Casino's "Stats" page) ----
enum CasinoGame : uint8_t { CG_SLOTS, CG_BLACKJACK, CG_POKER, CG_ROULETTE, CG_DICE, CG_COUNT };
struct CasinoGameStats { uint32_t rounds; int32_t net; int32_t best; };   // best: the biggest win in one round
const char *casino_game_name(CasinoGame g);
// After each round: what it made or lost overall (winnings minus the stake).
void casino_record(CasinoGame g, int32_t net);
CasinoGameStats casino_stats(CasinoGame g);
// Video Poker's best hand so far (PokerHand, game_videopoker.h; 255 = none yet).
void    casino_record_poker_hand(uint8_t hand);
uint8_t casino_best_poker_hand();
// Once a day (day = yyyymmdd) the cashier gives CASINO_DAILY_BONUS, whatever
// is in the wallet: the amount given, 0 if it was already taken today.
#define CASINO_DAILY_BONUS 100
int32_t casino_daily_bonus(uint32_t day);
bool    casino_daily_bonus_ready(uint32_t day);
void    casino_stats_reset();   // tests
