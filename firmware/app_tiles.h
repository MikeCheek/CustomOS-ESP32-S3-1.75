/*
 * app_tiles.h
 * Tiles, the Wear OS / Galaxy way: full-screen pages to flick through from
 * the face (swipe in from the right, by default) - Activity, Weather, Up
 * next, Music (with its buttons), Timer (one-tap 1/3/5/10 minutes), Life,
 * World clock, Alarm, Reminders, Battery. Which ones and in what order are
 * set in the app's Faces tab (home_gestures.h keeps them). A tap opens the
 * tile's app; pulling right past the first goes back to the face.
 */
#pragma once
#include "ui.h"

extern Screen tiles_screen;
void tiles_set_page(int i);   // the next time it opens (the simulator's shots)
