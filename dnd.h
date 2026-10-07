/*
 * dnd.h - Do Not Disturb (fw 3.1) and the quick-reply list.
 *
 * DND is on when it's switched on (tile, or synced from the phone) or during
 * the bedtime window in Settings. While on: no notification popups, no
 * reminder sounds, no wake for notifications.
 * Phone link: phone -> watch {"t":"dnd","on":0|1}; watch -> phone
 * {"e":"dnd","on":0|1} when the tile is used (the phone applies it if the
 * user allowed DND sync). Quick replies: {"t":"qr","l":["...", ...]}.
 */
#pragma once
#include <stdint.h>

bool dnd_active();                     // manual or bedtime
bool dnd_manual();                     // the switch itself
bool dnd_bedtime_now();                // inside the bedtime window (and it's enabled)
void dnd_set(bool on, bool from_phone);

// Quick replies: "Dictate" is always first, then the list from the phone
// (or the built-in presets if the phone never sent one).
int         quick_reply_count();
const char *quick_reply(int i);
void        quick_replies_set(const char *const *items, int n);   // saves to NVS
void        quick_replies_load();
