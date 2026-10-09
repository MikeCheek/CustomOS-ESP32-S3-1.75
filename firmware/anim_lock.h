#pragma once
#include "ui.h"
extern Screen lock_anim_screen;

// Configure the transition before pushing lock_anim_screen:
//   waking = true  -> unlock: the screen underneath opens out of the centre
//                      in a quick circle (~0.2 s).
//   waking = false -> lock: what's on screen closes into a circle and a dot.
//   sleep_after    -> lock only: calls sleep_force_sleep() itself once the
//                      animation finishes, so the display doesn't go dark
//                      before the lock has been seen.
void lock_anim_set_mode(bool waking, bool sleep_after);

// Power-on instead: a swarm of particles streams in, forms a spinning
// sphere, folds into a ring around the name, then warps outward as the
// watch face flies in. A tap skips it.
void lock_anim_set_boot();
