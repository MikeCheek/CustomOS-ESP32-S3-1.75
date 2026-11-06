/*
 * usage_stats.h
 * How the watch is used, for the companion app's Diagnostics: the battery
 * over the last 24 hours (every 10 minutes, with charging), screen-on time
 * and wake-ups per hour (today and yesterday), time spent in each app,
 * and counters (wakes, Bluetooth drops, notifications, restarts), with
 * the lowest free memory and the SD card's space. Kept in NVS each hour
 * so a restart loses little.
 *
 * The app asks {"t":"usage"}; the watch answers in three parts (the link
 * takes ~700 bytes a message):
 *   {"e":"usage","p":0,"iv":10,"bat":"5252ff...","chg":"0001..."}   oldest first, 2 hex digits each, ff = no sample
 *   {"e":"usage","p":1,"on":[s x24],"wk":[n x24],"ony":[s x24]}      seconds on / wakes per hour
 *   {"e":"usage","p":2,"apps":[["Music",1234],...],"wake":n,"disc":n,"ntf":n,"boot":n,
 *    "crash":n,"heap":KB,"psram":KB,"sd":[MB total, MB used],"up":s,"rr":"..."}
 *   {"e":"usage","p":3,"off":s,"pm":1,"aw":{"wakeword":s,...}}      what kept it from sleeping deeply
 *     with the screen off today (unplugged): "build" (no power management in this
 *     firmware), "setting" (Deep sleep off), "wakeword", "aod", "gps", "wifi",
 *     "espnow", "audio", "usbmode" - seconds each; "off": seconds with the screen off;
 *     "ft":{"speaker":s,"mic":s,"gps":s,"wifi":s} - how long each was in use today
 *     (unplugged), for the app's battery estimate per feature
 */
#pragma once
#include <stdint.h>

void usage_init();
void usage_update();            // loop(): once a second does the bookkeeping
void usage_count_notification();
void usage_send();              // phone link "usage"
float usage_drain_per_hour();   // the last 24 h, unplugged, %/hour; < 0 without 2 h of it
