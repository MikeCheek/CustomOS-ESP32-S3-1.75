/*
 * app_notifications.h
 * Notification history + the Notifications screen.
 *
 * Notifications arrive two ways: the phone link (phone_link.cpp, with an
 * id so they can be dismissed or replied to from the watch) and the
 * legacy plain-text characteristic (hal_ble.h's ble_get_notification).
 * Both land in one ring-buffer history, and each new one pops a toast
 * (unless Do Not Disturb is on).
 */

#pragma once
#include "ui.h"
#include "hal_ble.h"

// Call every loop() iteration - drains the legacy BLE queue into history.
void notifications_update();

#define NOTIF_HISTORY_SIZE 50   // kept across restarts in the store (store.h)

struct NotifHistoryItem {
    uint32_t id;                  // phone-side id, 0 = legacy (no actions)
    char     source[32];          // app name
    char     title[48];
    char     text[BLE_NOTIF_MAX_LEN * 2];
    bool     can_reply;
    uint32_t received_ms;
    uint32_t icon;                // app icon (phone_images.h), 0 = none
};

// From the phone link. Same id again updates that entry in place.
void notifications_add(uint32_t id, const char *app, const char *title, const char *text, bool can_reply,
                       uint32_t icon = 0);
// The phone dismissed it (or it was handled there).
void notifications_remove(uint32_t id);

// Smart replies the phone suggests for one message ({"t":"sr"}): shown
// first in its reply list. The last few messages keep theirs.
#define NOTIF_SMART_MAX 3
void notifications_set_smart_replies(uint32_t id, const char *const *items, int n);

// The phone's LLM, for the Notifications screen: a summary of the recent
// ones ({"t":"digest"}) and one message translated ({"t":"tr"}).
void notifications_on_digest(const char *text, const char *err);
void notifications_on_translation(uint32_t id, const char *text, const char *err);

int notifications_history_count(); // 0..NOTIF_HISTORY_SIZE
// New since the Notifications screen was last opened (the red dot on the faces).
uint32_t notifications_unread();
// Dismissed on the watch and the phone, all of them.
void notifications_clear_all();
// Muted apps (until midnight): listed, but no pop-up or sound.
bool notifications_app_muted(const char *app);
void notifications_mute_app(const char *app, bool on);
// idx: 0 = most recent. Returns false if idx is out of range.
bool notifications_get_history(int idx, NotifHistoryItem &out);
