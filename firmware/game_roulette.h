#pragma once
#include "ui.h"
extern Screen roulette_screen;   // Roulette, in the Casino (casino.h)
// What comes back (stakes included) when the ball lands on n: out[9] = Red,
// Black, Odd, Even, 1-18, 19-36, 1st/2nd/3rd 12; num[37] = straight bets.
#include <stdint.h>
int32_t roulette_win(const int32_t *out, const int32_t *num, int n);
