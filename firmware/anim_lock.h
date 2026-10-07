#pragma once
#include "ui.h"
extern Screen lock_anim_screen;

// Configure the transition before pushing lock_anim_screen:
//   waking = true  -> particles burst outward from a point into a
//                      spinning shape (sphere/torus/cube, randomly
//                      picked each time), then it pops itself.
//   waking = false -> particles contract from a spinning shape down
//                      to a point.
//   sleep_after    -> if true, calls sleep_force_sleep() itself once
//                      the contract animation finishes, right before
//                      popping - lets the visual "lock" finish before
//                      the display actually goes to sleep, instead of
//                      sleeping first and hiding the animation.
void lock_anim_set_mode(bool waking, bool sleep_after);
