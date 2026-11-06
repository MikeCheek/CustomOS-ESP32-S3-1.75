#pragma once
#include "ui.h"
#include <stdint.h>
extern Screen videopoker_screen;   // Video Poker (Jacks or Better), in the Casino (casino.h)

// The hand five cards make (rank 1-13, 1 = ace; suit 0-3), best first.
enum PokerHand : uint8_t {
    PH_ROYAL, PH_STRAIGHT_FLUSH, PH_FOUR, PH_FULL_HOUSE, PH_FLUSH, PH_STRAIGHT, PH_THREE, PH_TWO_PAIR, PH_JACKS,
    PH_NOTHING
};
PokerHand   poker_hand(const uint8_t rank[5], const uint8_t suit[5]);
int         poker_pays(PokerHand h);        // for 1 (the stake included): 250, 50, 25, 9, 6, 4, 3, 2, 1, 0
const char *poker_name(PokerHand h);        // "Full house"...
