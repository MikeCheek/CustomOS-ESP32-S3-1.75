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
#include <ArduinoJson.h>
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
    uint32_t art;                 // album cover (phone_images.h), 0 = none
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
// {"e":"pf"}: the photo faces on the card (after a change, or when the app asks)
void phone_link_send_photo_faces();
bool phone_link_finding_phone();

// ---- Notifications (actions on items in app_notifications) ---------------
void phone_link_notif_dismiss(uint32_t id);
bool phone_link_notif_reply(uint32_t id, const char *text);   // false = not sent
// A fall wasn't answered on the watch (ok = false), or the person said it's fine after all.
bool phone_link_fall(bool ok);
// Ask the phone's LLM: a summary of the recent notifications, a message translated.
bool phone_link_notif_digest();
bool phone_link_notif_translate(uint32_t id);
// Ask the phone to say something on the watch's speaker (its voices, the
// "say:" file comes back): {"e":"say","w":"notif","id":..} a message read
// aloud, {"e":"say","w":"briefing","x":".."} the morning briefing,
// {"e":"say","w":"brief"} the Today card. The phone answers
// {"t":"say","err":".."} when it can't (no voice installed...).
bool phone_link_say(const char *what, uint32_t id = 0, const char *text = nullptr);

// Ask the phone to resend its state (battery, media) - e.g. when a screen
// that shows it opens.
void phone_link_request_state();
// Any watch -> phone event ({"e":...}); false when it couldn't be sent.
bool phone_link_send(JsonDocument &doc);
