/*
 * phone_link.h
 * The watch side of the companion app's phone link: phone battery, what
 * the phone is playing (with controls), incoming calls (answer/decline),
 * notifications with dismiss/quick reply, find-my-phone, and the watch's
 * own steps/battery reported back to the phone.
 *
 * Transport: BLE characteristic 19B10009 (see hal_ble.h ble_link_*), one
 * JSON object per message. Full vocabulary in COMPANION_PROTOCOL.md.
 */
#pragma once
#include <stdint.h>

// Call every loop() iteration, awake or asleep.
void phone_link_update();

// True while the companion app is connected AND has said hello on the link
// (an old app version connects but never speaks it).
bool phone_link_active();

// ---- Phone battery --------------------------------------------------------
int  phone_link_battery();        // -1 = unknown
bool phone_link_charging();

// ---- Media ---------------------------------------------------------------
struct PhoneMedia {
    bool     valid;               // the phone reported a media session
    char     title[64];
    char     artist[48];
    bool     playing;
    int      position_s;          // at updated_ms
    int      duration_s;          // 0 = unknown
    int      volume, volume_max;  // phone media volume
    uint32_t updated_ms;
};
const PhoneMedia &phone_link_media();
int  phone_link_media_position();  // position extrapolated to now
// "toggle" | "next" | "prev" | "volUp" | "volDown"
void phone_link_media_cmd(const char *cmd);

// ---- Calls ---------------------------------------------------------------
enum PhoneCallState { CALL_NONE, CALL_RINGING, CALL_ACTIVE };
struct PhoneCall {
    PhoneCallState state;
    uint32_t id;                  // notification id, for answer/decline
    char     name[48];
    uint32_t since_ms;            // when this state started
};
const PhoneCall &phone_link_call();
void phone_link_call_answer();
void phone_link_call_decline();

// ---- Find my phone -------------------------------------------------------
void phone_link_find_phone(bool on);
bool phone_link_finding_phone();

// ---- Notifications (actions on items in app_notifications) ---------------
void phone_link_notif_dismiss(uint32_t id);
bool phone_link_notif_reply(uint32_t id, const char *text);   // false = not sent

// Ask the phone to resend its state (battery, media) - e.g. when a screen
// that shows it opens.
void phone_link_request_state();
