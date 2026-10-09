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

#define NOTIF_HISTORY_SIZE 24

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

int notifications_history_count(); // 0..NOTIF_HISTORY_SIZE
// idx: 0 = most recent. Returns false if idx is out of range.
bool notifications_get_history(int idx, NotifHistoryItem &out);
