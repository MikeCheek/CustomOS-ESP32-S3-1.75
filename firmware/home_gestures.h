/*
 * home_gestures.h
 * What each gesture on the watch face does - all of them can be changed,
 * on the watch (Settings > Home gestures) or in the app (Faces tab).
 *
 *   tap               Apps                 (Standard style)
 *   double-tap        Play / pause
 *   touch and hold    Faces (the switcher)
 *   swipe up          Smart Stack
 *   pull down         Control Center
 *   swipe from right  Tiles
 *   swipe from left   Notifications
 *   BOOT              Control Center
 *
 * Styles set them all at once: Standard, Apple-like, Wear OS-like,
 * Galaxy-like and Classic (this watch before); changing one makes it Custom.
 *
 * The Tiles (app_tiles.h) - which, in what order - are kept here too.
 *
 *   phone -> watch  {"t":"home"}                                    ask
 *                   {"t":"home","a":{"tap":"apps",...},"tiles":["activity",...]}   set (any part)
 *                   {"t":"home","style":"apple"}                    a style
 *   watch -> phone  {"e":"home","a":{"tap":"apps",...},"style":"standard"|"custom",
 *                    "tiles":["activity",...]}
 * The watch sends it on "hi" and whenever it changes there.
 */
#pragma once
#include <stdint.h>
#include <ArduinoJson.h>

enum HomeAction : uint8_t {
    HA_NONE, HA_APPS, HA_NOTIFS, HA_STACK, HA_CONTROL, HA_TILES, HA_FACES, HA_NEXT_FACE, HA_PREV_FACE,
    HA_ASSISTANT, HA_MUSIC, HA_PLAYPAUSE, HA_TIMER, HA_STOPWATCH, HA_WORKOUT, HA_TORCH, HA_RECORDER,
    HA_LIFE, HA_ACTIVITY, HA_WEATHER, HA_CALENDAR, HA_SETTINGS, HA_FACE_OWN,
    HA_WALLET,   // new ones go last: the settings keep the numbers
    HA_COUNT
};

enum HomeSlot : uint8_t { HS_TAP, HS_DOUBLE, HS_HOLD, HS_UP, HS_DOWN, HS_LEFT, HS_RIGHT, HS_BOOT, HS_COUNT };

enum TileKind : uint8_t {
    TL_ACTIVITY, TL_WEATHER, TL_NEXT, TL_MUSIC, TL_TIMER, TL_LIFE, TL_WORLD, TL_ALARM, TL_REMINDERS, TL_BATTERY,
    TL_COUNT
};
#define TILES_MAX TL_COUNT
#define TILE_END 0xFF

const char *home_action_name(HomeAction a);   // "Apps"
const char *home_action_key(HomeAction a);    // "apps" (the link's)
const char *home_slot_name(HomeSlot s);       // "Swipe up"
const char *home_slot_key(HomeSlot s);        // "up"
bool home_slot_allows(HomeSlot s, HomeAction a);   // "Face's own" only for a tap
HomeAction home_action(HomeSlot s);
void home_set_action(HomeSlot s, HomeAction a);    // saves
void home_cycle_action(HomeSlot s);                // the next allowed one (Settings rows)
void home_run_slot(HomeSlot s);
void home_run(HomeAction a);

int  home_style_count();
const char *home_style_name(int i);           // "Standard", ... ; i == home_style_count(): "Custom"
int  home_style();                            // the one matching now, or home_style_count() (Custom)
void home_apply_style(int i);                 // saves
void home_defaults(uint8_t *acts);            // Standard, HS_COUNT bytes (NVS defaults)

const char *tile_name(TileKind k);
const char *tile_key(TileKind k);
int  tiles_list(uint8_t *out);                // the shown tiles in order; returns how many
void tiles_defaults(uint8_t *tiles);          // TILES_MAX bytes

void home_on_link(JsonDocument &doc);         // phone link "home"
void home_send();
void home_update();                           // loop(): sends again after a change on the watch
