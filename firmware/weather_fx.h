/*
 * weather_fx.h
 * Animated weather behind the main watchface, from the phone's last
 * weather push: rain, drizzle and showers, snow, a storm with lightning,
 * fog banks, drifting clouds, sun rays (heat shimmer when it's hot) and
 * stars at night. Kept dim so the face stays readable.
 *
 * Settings > Display > Weather effects turns it off.
 */
#pragma once

struct Arduino_GFX;

// Draws this frame's weather over the cleared screen; call first in the
// face's draw. Nothing when off or there's no weather yet.
void weather_fx_draw(Arduino_GFX *g);
