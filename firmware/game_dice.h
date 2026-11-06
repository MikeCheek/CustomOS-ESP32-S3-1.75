#pragma once
#include "ui.h"
#include <stdint.h>
extern Screen dice_screen;   // Hi-Lo Dice, in the Casino (casino.h)

// What comes back (stakes included) when two dice make [total]: Under 7
// and Over 7 pay 1:1, Seven pays 4:1.
int32_t dice_win(int32_t under, int32_t seven, int32_t over, int total);
