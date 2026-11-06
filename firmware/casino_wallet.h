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
