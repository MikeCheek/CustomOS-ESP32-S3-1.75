#pragma once
#include <Arduino_GFX_Library.h>
#include "ui.h"

// A small companion creature that walks and slides around the
// watchface's rim, pausing now and then to "play" (a little bounce/
// tail-wag idle animation). Call once per rendered frame from each
// watchface's own draw function, after its own content, so the pet
// draws on top - see app_watchface.cpp, app_watchface_minimal.cpp,
// and app_custom_watchface.cpp for the three call sites (this project
// has three separate watchfaces, each with its own draw function -
// no single shared render path to hook into instead).
void pet_step_and_draw(Arduino_GFX *g);

// Draws the currently-selected pet species as a static icon at the
// given position, in its idle pose - for other apps/games that want
// to use the player's chosen pet as a sprite (see game_flappy.cpp).
void pet_draw_icon(Arduino_GFX *g, int cx, int cy);

// Draggable: call from each watchface's own on_touch, BEFORE its own
// touch handling, so grabbing the pet takes priority over whatever
// else that watchface does with touch (e.g. the Eyes watchface's
// gaze-follow). Returns true if the touch was on/dragging the pet
// (the caller should treat the touch as consumed and return early);
// false means the touch had nothing to do with the pet and the
// caller's own logic should run as normal.
bool pet_handle_touch(int x, int y, bool pressed);

// ---- Species selection (see the "Pet" app / pet_screen below) ----
#define PET_SPECIES_COUNT 4
int pet_get_species();
void pet_set_species(int index);
const char *pet_species_name(int index);

// The "Pet" app - a small gallery to preview and pick which companion
// species walks the watchface rim.
extern Screen pet_screen;
