#pragma once
#include "ui.h"
extern Screen lock_anim_screen;

// Configure the transition before pushing lock_anim_screen:
//   waking = true  -> unlock: a CRT line flashes on, then the screen
//                      underneath swings up from the distance into place.
//   waking = false -> lock: what's on screen tilts away into the distance
//                      and switches off like an old CRT, to a line, to a dot.
//   sleep_after    -> lock only: calls sleep_force_sleep() itself once the
//                      animation finishes, so the display doesn't go dark
//                      before the lock has been seen.
void lock_anim_set_mode(bool waking, bool sleep_after);

// Power-on instead: a swarm of particles streams in, forms a spinning
// sphere, folds into a ring around the name, then warps outward as the
// watch face flies in. A tap skips it.
void lock_anim_set_boot();
