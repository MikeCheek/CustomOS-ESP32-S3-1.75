/*
 * complications.h - small round data widgets for the watchfaces (fw 3.2).
 *
 * The default and minimal watchfaces have 3 slots (Settings > Watchface
 * slots, or the app). Custom watchfaces can place any of them with a widget
 * {"type":"complication","dataSrc":"next_event", ...} (radius = size).
 * Phone link: {"t":"comp","s":["steps","next_event","weather"]} sets the
 * slots; {"t":"todo","n":3,"top":"Call Marco"} gives the open to-do count
 * from the memos.
 */
#pragma once
#include <stdint.h>

class Arduino_GFX;

enum CompId : uint8_t {
    COMP_NONE = 0,
    COMP_STEPS,
    COMP_BATTERY,
    COMP_PHONE_BAT,
    COMP_NEXT_EVENT,
    COMP_WEATHER,
    COMP_NOTIFS,
    COMP_NAV,
    COMP_TODOS,
    COMP_SECONDS,
    COMP_COUNT
};

#define COMP_SLOTS 3

const char *comp_key(CompId id);          // "steps", "next_event"...
const char *comp_label(CompId id);        // "Steps", "Next event"...
CompId      comp_from_key(const char *k);

// Draws complication [id] centred at (cx, cy) with radius r.
void comp_draw(Arduino_GFX *g, CompId id, int cx, int cy, int r, uint16_t accent);
// The three watchface slots, left to right.
void comp_draw_slots(Arduino_GFX *g, int y, int r);
CompId comp_slot(int i);
void   comp_set_slot(int i, CompId id);   // saves to NVS
void   comp_load();

// Phone link (phone_link.cpp)
void comp_set_todos(int count, const char *top);
