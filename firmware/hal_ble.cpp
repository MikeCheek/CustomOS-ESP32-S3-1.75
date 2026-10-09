#include "hal_ble.h"
#include <Preferences.h>
#include "config.h"
#include "hal_controller.h"
#include "hal_ota.h"
#include "phone_images.h"
#include <string.h>

// Normalizes UTF-8 text from the phone in place for the watch fonts:
// Latin-1 letters (U+00A1..U+00FF: è à ñ ü ß ...) are kept - the smooth UI
// fonts draw them (and ui_font folds them for the old font). Typographic
// quotes/dashes/ellipsis become ASCII, emoji variation selectors and
// joiners are dropped, anything else (emoji, CJK) becomes '?'.
void ble_fold_utf8(char *s) {
    unsigned char *r = (unsigned char *)s, *w = (unsigned char *)s;
    while (*r) {
        unsigned c = *r;
        if (c < 0x80) { *w++ = *r++; continue; }
        unsigned char *start = r;
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        unsigned cp = n == 3 ? (c & 0x07) : n == 2 ? (c & 0x0F) : (c & 0x1F);
        r++;
        bool ok = n > 0;
        for (int i = 0; i < n; i++) {
            if ((*r & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (*r++ & 0x3F);
        }
        if (ok && cp >= 0xA1 && cp <= 0xFF) {   // keep as-is (2 bytes)
            *w++ = start[0];
            *w++ = start[1];
            continue;
        }
        char out = '?';
        if (ok) {
            if (cp == 0xA0) out = ' ';
            else if (cp == 0x2018 || cp == 0x2019) out = '\'';
            else if (cp == 0x201C || cp == 0x201D) out = '"';
            else if (cp == 0x2013 || cp == 0x2014) out = '-';
            else if (cp == 0x2026) out = '.';
            else if (cp == 0x20AC) out = 'E';
            else if (cp >= 0xFE00 && cp <= 0xFE0F) continue; // emoji variation selector
            else if (cp == 0x200D) continue;                  // zero-width joiner
        }
        *w++ = (unsigned char)out;
    }
    *w = 0;
}

#if FEATURE_BLE

#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#if FEATURE_NVS
#include "hal_nvs.h"
#endif

// --- GATT UUIDs ---
static NimBLEUUID svcUUID("19B10000-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID batCharUUID("19B10001-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID notifCharUUID("19B10002-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID timeCharUUID("19B10003-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID contactsCharUUID("19B10004-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID mediaCharUUID("19B10005-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID fileCharUUID("19B10006-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID notesCharUUID("19B10007-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID controllerCharUUID("19B10008-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID linkCharUUID("19B10009-E8F2-537E-4F6C-D104768A1214");
static NimBLEUUID otaCharUUID("19B1000A-E8F2-537E-4F6C-D104768A1214");

// Pairing (fw 3.0): a random 6-digit code per pairing, shown on the watch
// (pairing_screen) and typed on the phone; LE Secure Connections + MITM +
// bonding. Every writable characteristic needs an encrypted link, so an
// unpaired phone can't push notifications, contacts, files, Wi-Fi
// passwords or firmware. Set to 0 to go back to the open link.
#define BLE_REQUIRE_ENCRYPTION 1
#if BLE_REQUIRE_ENCRYPTION
// _AUTHEN = the key must come from a code-entry pairing: a phone that
// claims no keyboard/display gets Just Works keys (encrypted but
// unauthenticated), which must not be enough.
#define W_ENC (NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN)
#define R_ENC (NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN)
#else
#define W_ENC 0
#define R_ENC 0
#endif
static volatile uint32_t s_pair_code = 0;     // code being shown, 0 = none
static volatile int8_t s_pair_result = 0;     // 1 paired, -1 failed (taken by the UI)
static volatile bool s_link_encrypted = false;

static NimBLEServer *s_server = nullptr;
static NimBLECharacteristic *s_batChar = nullptr;
static NimBLECharacteristic *s_notifChar = nullptr;
static NimBLECharacteristic *s_timeChar = nullptr;
static NimBLECharacteristic *s_contactsChar = nullptr;
static NimBLECharacteristic *s_mediaChar = nullptr;
static NimBLECharacteristic *s_fileChar = nullptr;
static NimBLECharacteristic *s_notesChar = nullptr;
static NimBLECharacteristic *s_controllerChar = nullptr;
static NimBLECharacteristic *s_linkChar = nullptr;
static NimBLECharacteristic *s_otaChar = nullptr;
// Power: slow connection interval + slower advertising while the screen is
// off and nothing is being transferred (see ble_update()).
static volatile uint32_t s_last_rx_ms = 0;   // any write from the phone
static bool s_low_power_req = false;
static bool s_conn_slow = false;             // params currently requested
static uint32_t s_conn_param_ms = 0;
static bool s_adv_slow = false;
static bool s_connected = false;
static bool s_enabled = false;

// Notes sync command state
static volatile BleNotesCmd s_notes_cmd = BLE_NOTES_CMD_NONE;
static volatile int s_notes_chunk_credits = 0;   // chunks the app is ready for
static char s_notes_cmd_arg[64] = {0};
// Transcripts are long (minutes of speech), far beyond one ATT write, so
// the app streams them: 0x06 + total(2 LE) + data, then 0x07 + data until
// total is reached. 0x04 + data (single packet) is still accepted.
#define BLE_TRANSCRIPT_MAX_LEN 8192
static char *s_notes_transcript = nullptr;      // PSRAM, BLE_TRANSCRIPT_MAX_LEN
static volatile int s_notes_transcript_len = 0; // >0 = complete, unconsumed
static char *s_tr_stage = nullptr;              // in-flight chunked transcript
static int s_tr_stage_len = 0, s_tr_stage_total = 0;
static bool s_has_time = false;
static int s_phone_year, s_phone_month, s_phone_day;
static int s_phone_hour, s_phone_minute, s_phone_second;

// Connection tracking
static int s_connect_count = 0;
static uint32_t s_last_connect_ms = 0;
static char s_connected_device_name[64] = {0};
static uint16_t s_conn_handle = 0;

// Notification queue
static BleNotification s_notif_queue[BLE_NOTIF_QUEUE_SIZE];
static volatile int s_notif_head = 0;
static volatile int s_notif_tail = 0;

// "Find My Watch" - see the declaration in hal_ble.h for the full
// story. Must match BleProtocol's sentinel constant in the app exactly.
static const char *BLE_FINDME_SENTINEL = "\x01__FINDME__\x01";
static volatile bool s_findme_requested = false;

// Contacts (JSON array from phone). A full list doesn't fit in one ATT
// write (max ~509 bytes), so the app streams it: 0x01 + total(2 LE) +
// data, then 0x02 + data until total is reached. A write starting with
// '[' is the old single-packet form and is still accepted. Both buffers
// live in PSRAM; the visible one only changes once a list is complete.
#define BLE_CONTACTS_MAX_LEN 8192
static char *s_contacts_buf = nullptr;
static volatile int s_contacts_len = 0;
static char *s_contacts_stage = nullptr;
static int s_contacts_stage_len = 0, s_contacts_stage_total = 0;

// Phone link buffers (characteristic 0x0009, see LinkWriteCallback below).
#define LINK_MSG_MAX   1024
#define LINK_QUEUE_LEN 8
static char *s_link_queue = nullptr;          // LINK_QUEUE_LEN * LINK_MSG_MAX
static volatile int s_link_head = 0, s_link_tail = 0;
static char *s_link_stage = nullptr;
static int s_link_stage_len = 0, s_link_stage_total = 0;

static char *ps_buf(char **p, size_t n) {
    if (!*p) *p = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return *p;
}

// Media state from phone
#define BLE_MEDIA_MAX_LEN 256
static char s_media_buf[BLE_MEDIA_MAX_LEN];
static volatile int s_media_len = 0;

// Weather data from phone (prefix 0x02 on file char)
#define BLE_WEATHER_MAX_LEN 512
static char s_weather_buf[BLE_WEATHER_MAX_LEN];
static volatile int s_weather_len = 0;

// Fitness data from phone (prefix 0x03 on file char)
#define BLE_FITNESS_MAX_LEN 256
static char s_fitness_buf[BLE_FITNESS_MAX_LEN];
static volatile int s_fitness_len = 0;

// File transfer buffer: allocated in PSRAM per transfer (sized to the
// announced file), freed once the file is saved or the transfer dies.
#define BLE_FILE_MAX_LEN (4 * 1024 * 1024)   // buffered in PSRAM; bigger files go over Wi-Fi
#define BLE_REC_MAX_LEN  (4 * 1024 * 1024)   // "rec:<name>" = a recording for /Recordings (same limit)
static uint8_t *s_file_buf = nullptr;
static volatile int s_file_len = 0;
static uint32_t s_file_last_ms = 0;      // last packet seen (drains a denied transfer)
static volatile bool s_file_too_big = false;

static void file_free_buf() {
    if (s_file_buf) { heap_caps_free(s_file_buf); s_file_buf = nullptr; }
}

// File transfer state machine
static volatile BleFileState s_file_state = BLE_FILE_IDLE;
static char s_file_name[64] = {0};
static int s_file_total_size = 0;

// Watchface layout buffer (raw JSON from phone, chunked)
#define BLE_WATCHFACE_MAX_LEN 4096
static char *s_watchface_buf = nullptr; // PSRAM, allocated on first layout
static volatile int s_watchface_len = 0;
static volatile int s_watchface_total = 0;
static uint32_t s_watchface_last_ms = 0;

class NotesWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        if (val.length() < 1) return;
        const uint8_t *d = (const uint8_t *)val.data();

        uint8_t cmd = d[0];
        switch (cmd) {
            case 0x01: // Request file list
                s_notes_cmd = BLE_NOTES_CMD_LIST_FILES;
                DEBUG_PRINTF("[ble] notes: request file list\n");
                break;
            case 0x02: // Request file download (arg = filename)
                if (val.length() > 1) {
                    int nameLen = val.length() - 1;
                    if (nameLen > (int)sizeof(s_notes_cmd_arg) - 1) nameLen = sizeof(s_notes_cmd_arg) - 1;
                    memcpy(s_notes_cmd_arg, d + 1, nameLen);
                    s_notes_cmd_arg[nameLen] = '\0';
                    s_notes_cmd = BLE_NOTES_CMD_DOWNLOAD_FILE;
                    DEBUG_PRINTF("[ble] notes: request download '%s'\n", s_notes_cmd_arg);
                }
                break;
            case 0x03: // Delete file (arg = filename)
                if (val.length() > 1) {
                    int nameLen = val.length() - 1;
                    if (nameLen > (int)sizeof(s_notes_cmd_arg) - 1) nameLen = sizeof(s_notes_cmd_arg) - 1;
                    memcpy(s_notes_cmd_arg, d + 1, nameLen);
                    s_notes_cmd_arg[nameLen] = '\0';
                    s_notes_cmd = BLE_NOTES_CMD_DELETE_FILE;
                    DEBUG_PRINTF("[ble] notes: request delete '%s'\n", s_notes_cmd_arg);
                }
                break;
            case 0x04: // Transcript text from phone (single packet)
                if (val.length() > 1 && ps_buf(&s_notes_transcript, BLE_TRANSCRIPT_MAX_LEN)) {
                    int tLen = val.length() - 1;
                    if (tLen > BLE_TRANSCRIPT_MAX_LEN - 1) tLen = BLE_TRANSCRIPT_MAX_LEN - 1;
                    memcpy(s_notes_transcript, d + 1, tLen);
                    s_notes_transcript[tLen] = '\0';
                    s_notes_transcript_len = tLen;
                    s_notes_cmd = BLE_NOTES_CMD_TRANSCRIPT;
                    DEBUG_PRINTF("[ble] notes: transcript received (%d bytes)\n", tLen);
                }
                break;
            case 0x06: // Chunked transcript: first packet, total length (2 LE) + data
            case 0x07: // Chunked transcript: continuation
                if (!ps_buf(&s_tr_stage, BLE_TRANSCRIPT_MAX_LEN) ||
                    !ps_buf(&s_notes_transcript, BLE_TRANSCRIPT_MAX_LEN)) break;
                if (cmd == 0x06) {
                    if (val.length() < 3) break;
                    s_tr_stage_total = d[1] | (d[2] << 8);
                    if (s_tr_stage_total > BLE_TRANSCRIPT_MAX_LEN - 1) s_tr_stage_total = BLE_TRANSCRIPT_MAX_LEN - 1;
                    s_tr_stage_len = 0;
                    d += 3;
                } else {
                    if (s_tr_stage_total <= 0) break; // no transfer in progress
                    d += 1;
                }
                {
                    int n = (int)val.length() - (cmd == 0x06 ? 3 : 1);
                    if (n > s_tr_stage_total - s_tr_stage_len) n = s_tr_stage_total - s_tr_stage_len;
                    if (n > 0) { memcpy(s_tr_stage + s_tr_stage_len, d, n); s_tr_stage_len += n; }
                }
                if (s_tr_stage_len >= s_tr_stage_total) {
                    memcpy(s_notes_transcript, s_tr_stage, s_tr_stage_len);
                    s_notes_transcript[s_tr_stage_len] = '\0';
                    s_notes_transcript_len = s_tr_stage_len;
                    s_tr_stage_total = 0;
                    s_notes_cmd = BLE_NOTES_CMD_TRANSCRIPT;
                    DEBUG_PRINTF("[ble] notes: transcript received (%d bytes, chunked)\n", s_tr_stage_len);
                }
                break;
            case 0x05: // Request next chunk (flow control)
                // 0x05 alone = one more chunk (old apps); 0x05 + n = n more
                // (windowed: the app keeps several chunks in flight).
                __atomic_fetch_add(&s_notes_chunk_credits, val.length() >= 2 ? d[1] : 1, __ATOMIC_SEQ_CST);
                break;
        }
    }
};

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
        s_connected = true;
        s_connect_count++;
        s_last_connect_ms = millis();
        s_conn_handle = connInfo.getConnHandle();
        s_conn_slow = false;            // the phone starts us on its own (fast) params
        s_conn_param_ms = millis();
        // Bigger link-layer packets (one 251-byte frame instead of ten
        // 27-byte ones) and the 2 Mbit PHY: several times the throughput
        // for recordings, files and updates, if the phone supports them.
        pServer->setDataLen(s_conn_handle, 251);
        pServer->updatePhy(s_conn_handle, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
        s_link_encrypted = connInfo.isEncrypted();
        memset(s_connected_device_name, 0, sizeof(s_connected_device_name));
        NimBLEAddress addr = connInfo.getAddress();
        snprintf(s_connected_device_name, sizeof(s_connected_device_name), "%s",
                 addr.toString().c_str());
        DEBUG_PRINTF("[ble] connected to: %s (total: %d)\n", s_connected_device_name, s_connect_count);
    }
    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
        (void)pServer; (void)connInfo; (void)reason;
        s_connected = false;
        s_conn_handle = 0;
        s_link_encrypted = false;
        if (s_pair_code) { s_pair_code = 0; s_pair_result = -1; }   // walked away mid-pairing
        memset(s_connected_device_name, 0, sizeof(s_connected_device_name));
        // Cancel any active file download
        // The download's File belongs to the main loop: only ask it to stop.
        extern void notes_request_cancel();
        notes_request_cancel();
        __atomic_store_n(&s_notes_chunk_credits, 0, __ATOMIC_SEQ_CST);
        // A transfer that was still arriving can never finish now - drop
        // it so the next one isn't blocked (a COMPLETE one is left for the
        // main loop to save).
        if (s_file_state == BLE_FILE_PENDING || s_file_state == BLE_FILE_IN_PROGRESS ||
            s_file_state == BLE_FILE_DENIED) {
            s_file_state = BLE_FILE_IDLE;
            s_file_len = 0;
            s_file_total_size = 0;
            file_free_buf();
        }
        if (s_watchface_total > 0) { s_watchface_total = 0; s_watchface_len = 0; } // half-sent
        s_contacts_stage_total = 0;
        s_tr_stage_total = 0;
        s_link_stage_total = 0;
        DEBUG_PRINTF("[ble] disconnected\n");
        if (s_enabled) NimBLEDevice::startAdvertising();
    }

    // The phone is pairing: make up a code and show it on the watch.
    uint32_t onPassKeyDisplay() override {
        uint32_t code = 100000 + (esp_random() % 900000);
        s_pair_code = code;
        s_pair_result = 0;
        DEBUG_PRINTF("[ble] pairing - code shown on the watch\n");
        return code;
    }

    void onConfirmPassKey(NimBLEConnInfo &connInfo, uint32_t pin) override {
        // Numeric comparison isn't used (DISPLAY_ONLY), but answer anyway.
        NimBLEDevice::injectConfirmPasskey(connInfo, pin == s_pair_code);
    }

    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
        s_link_encrypted = connInfo.isEncrypted();
        bool was_pairing = s_pair_code != 0;
        s_pair_code = 0;
        // Also called when a bonded phone reconnects (encryption restored
        // from the stored key), not only after a fresh pairing.
        if (!connInfo.isEncrypted() || !connInfo.isAuthenticated()) {
            DEBUG_PRINTF("[ble] pairing/encryption failed (encrypted %d, authenticated %d)\n",
                         connInfo.isEncrypted(), connInfo.isAuthenticated());
            s_link_encrypted = false;
            if (was_pairing) s_pair_result = -1;
#if BLE_REQUIRE_ENCRYPTION
            if (connInfo.isEncrypted()) {
                // Just Works keys (the phone claimed no keyboard/display, so
                // no code was asked): don't keep them, drop the link.
                NimBLEDevice::deleteBond(connInfo.getIdAddress());
                NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
            } else if (was_pairing) {
                NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
            }
            // A failed restore (the phone still has a bond this watch has
            // forgotten) keeps the link: nothing on it works unpaired, and
            // dropping it only makes an auto-reconnecting phone retry forever.
#endif
            return;
        }
        if (was_pairing) s_pair_result = 1;
        DEBUG_PRINTF("[ble] link encrypted (%s)\n", connInfo.isBonded() ? "bonded" : "not bonded");
    }
};

class TimeWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        if (val.length() >= 7) {
            s_phone_year   = 2000 + (uint8_t)val[0];
            s_phone_month  = (uint8_t)val[1];
            s_phone_day    = (uint8_t)val[2];
            s_phone_hour   = (uint8_t)val[3];
            s_phone_minute = (uint8_t)val[4];
            s_phone_second = (uint8_t)val[5];
            s_has_time = true;
            DEBUG_PRINTF("[ble] time received: %04d-%02d-%02d %02d:%02d:%02d\n",
                         s_phone_year, s_phone_month, s_phone_day,
                         s_phone_hour, s_phone_minute, s_phone_second);
        }
    }
};

// 2-byte packet: {dpad_bitmask, buttons_bitmask} - see hal_controller.h
// for the bit meanings. Sent by the companion app's Controller screen
// on every state change (button/d-pad press or release), not
// continuously - so a dropped packet just means input state stays as
// it was until the next real change, not a runaway stuck direction.
class ControllerWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        if (val.length() >= 2) {
            controller_set_state((uint8_t)val[0], (uint8_t)val[1]);
        }
    }
};

class NotifWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        if (val.length() > 0) {
            // Reserved sentinel for "Find My Watch" (see ble_consume_
            // findme_request() in hal_ble.h) - checked first so it's
            // never accidentally shown as a real notification.
            if (val == BLE_FINDME_SENTINEL) {
                s_findme_requested = true;
                DEBUG_PRINTF("[ble] find-my-watch requested\n");
                return;
            }
            int next = (s_notif_head + 1) % BLE_NOTIF_QUEUE_SIZE;
            if (next != s_notif_tail) {
                snprintf(s_notif_queue[s_notif_head].text, BLE_NOTIF_MAX_LEN, "%s", val.c_str());
                ble_fold_utf8(s_notif_queue[s_notif_head].text);
                // The app formats notifications as "[App] Title: text" -
                // use the app name as the source when it's there.
                const char *t = s_notif_queue[s_notif_head].text;
                const char *rb = t[0] == '[' ? strchr(t, ']') : nullptr;
                if (rb && rb - t - 1 > 0 && rb - t - 1 < (int)sizeof(s_notif_queue[0].source)) {
                    snprintf(s_notif_queue[s_notif_head].source, sizeof(s_notif_queue[0].source), "%.*s", (int)(rb - t - 1), t + 1);
                    const char *body = rb + 1;
                    while (*body == ' ') body++;
                    memmove(s_notif_queue[s_notif_head].text, body, strlen(body) + 1);
                } else {
                    snprintf(s_notif_queue[s_notif_head].source, sizeof(s_notif_queue[0].source), "Phone");
                }
                s_notif_head = next;
                DEBUG_PRINTF("[ble] notification queued: %s\n", val.c_str());
            }
        }
    }
};

class ContactsWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        int L = (int)val.length();
        if (L < 1) return;
        const uint8_t *d = (const uint8_t *)val.data();
        if (!ps_buf(&s_contacts_buf, BLE_CONTACTS_MAX_LEN) ||
            !ps_buf(&s_contacts_stage, BLE_CONTACTS_MAX_LEN)) return;
        if (d[0] == '[') { // legacy: whole list in one write
            if (L > BLE_CONTACTS_MAX_LEN - 1) L = BLE_CONTACTS_MAX_LEN - 1;
            memcpy(s_contacts_buf, d, L);
            s_contacts_buf[L] = 0;
            s_contacts_len = L;
            DEBUG_PRINTF("[ble] contacts received: %d bytes\n", L);
            return;
        }
        int n;
        if (d[0] == 0x01 && L >= 3) {
            s_contacts_stage_total = d[1] | (d[2] << 8);
            if (s_contacts_stage_total > BLE_CONTACTS_MAX_LEN - 1) s_contacts_stage_total = BLE_CONTACTS_MAX_LEN - 1;
            s_contacts_stage_len = 0;
            d += 3; n = L - 3;
        } else if (d[0] == 0x02 && s_contacts_stage_total > 0) {
            d += 1; n = L - 1;
        } else {
            return;
        }
        if (n > s_contacts_stage_total - s_contacts_stage_len) n = s_contacts_stage_total - s_contacts_stage_len;
        if (n > 0) { memcpy(s_contacts_stage + s_contacts_stage_len, d, n); s_contacts_stage_len += n; }
        if (s_contacts_stage_len >= s_contacts_stage_total) {
            memcpy(s_contacts_buf, s_contacts_stage, s_contacts_stage_len);
            s_contacts_buf[s_contacts_stage_len] = 0;
            s_contacts_len = s_contacts_stage_len;
            s_contacts_stage_total = 0;
            DEBUG_PRINTF("[ble] contacts received: %d bytes (chunked)\n", s_contacts_len);
        }
    }
};

class MediaWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        if (val.length() > 0 && val.length() <= BLE_MEDIA_MAX_LEN) {
            memcpy(s_media_buf, val.data(), val.length());
            s_media_len = val.length();
            DEBUG_PRINTF("[ble] media state received: %d bytes\n", (int)val.length());
        }
    }
};

class FileWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        if (val.length() < 1) return;

        const uint8_t *d = (const uint8_t *)val.data();
        const uint32_t now = millis();

        // File-transfer packets are framed as len(4 LE) + offset(4 LE) +
        // len bytes of payload, so their length is always exactly len+8.
        // Their first byte is the low byte of that length, which can be
        // 0x01-0x03 (a 1-3 character name, or a 257-byte last chunk) - the
        // same values as the watchface/weather/fitness prefixes - so the
        // framing is checked first. JSON packets can never match it: their
        // bytes 1..3 are text, which makes the decoded length enormous.
        const bool framed = val.length() >= 8 &&
            (uint64_t)(d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24)) + 8 == val.length();
        if (framed && handle_file_packet(d, (int)val.length(), now)) return;

        // Type prefix byte: 0x01 = watchface JSON (chunked)
        if (d[0] == 0x01 && val.length() > 1) {
            // A transfer that stalled (phone went away mid-send) must not
            // swallow the next one as its "continuation".
            if (s_watchface_total > 0 && now - s_watchface_last_ms > 3000) {
                s_watchface_total = 0;
                s_watchface_len = 0;
            }
            s_watchface_last_ms = now;
            if (!ps_buf(&s_watchface_buf, BLE_WATCHFACE_MAX_LEN)) return;
            if (s_watchface_total == 0 && val.length() >= 3) {
                // First chunk: 2-byte total length (LE) + data
                s_watchface_total = d[1] | (d[2] << 8);
                // Clamped to MAX_LEN-1, not MAX_LEN: a null terminator
                // gets written at s_watchface_buf[s_watchface_len] once
                // the transfer completes below, and if content were
                // allowed to fill the buffer exactly, that write would
                // land one byte past the end of s_watchface_buf.
                if (s_watchface_total > BLE_WATCHFACE_MAX_LEN - 1) s_watchface_total = BLE_WATCHFACE_MAX_LEN - 1;
                int dataLen = (int)val.length() - 3;
                if (dataLen > s_watchface_total) dataLen = s_watchface_total;
                memcpy(s_watchface_buf, d + 3, dataLen);
                s_watchface_len = dataLen;
                DEBUG_PRINTF("[ble] watchface first chunk: %d bytes (total expected: %d)\n", dataLen, s_watchface_total);
            } else if (s_watchface_total > 0) {
                // Continuation chunk: just data
                int dataLen = (int)val.length() - 1;
                if (s_watchface_len + dataLen > BLE_WATCHFACE_MAX_LEN)
                    dataLen = BLE_WATCHFACE_MAX_LEN - s_watchface_len;
                if (s_watchface_len + dataLen > s_watchface_total)
                    dataLen = s_watchface_total - s_watchface_len;
                memcpy(s_watchface_buf + s_watchface_len, d + 1, dataLen);
                s_watchface_len += dataLen;
                DEBUG_PRINTF("[ble] watchface cont chunk: %d bytes (total: %d/%d)\n", dataLen, s_watchface_len, s_watchface_total);
            }
            if (s_watchface_total > 0 && s_watchface_len >= s_watchface_total) {
                s_watchface_buf[s_watchface_len] = '\0';
                DEBUG_PRINTF("[ble] watchface complete: %d bytes\n", s_watchface_len);
                s_watchface_total = 0;
#if FEATURE_NVS
                nvs_save_watchface_json(s_watchface_buf, s_watchface_len);
#endif
            }
            return;
        }

        // Type prefix byte: 0x02 = weather JSON
        if (d[0] == 0x02 && val.length() > 1) {
            int jsonLen = (int)val.length() - 1;
            // Clamped to MAX_LEN-1 to leave room for the '\0' written below.
            if (jsonLen > BLE_WEATHER_MAX_LEN - 1) jsonLen = BLE_WEATHER_MAX_LEN - 1;
            memcpy(s_weather_buf, d + 1, jsonLen);
            s_weather_buf[jsonLen] = '\0';
            s_weather_len = jsonLen;
            DEBUG_PRINTF("[ble] weather data received: %d bytes\n", jsonLen);
            return;
        }

        // Type prefix byte: 0x03 = fitness JSON
        if (d[0] == 0x03 && val.length() > 1) {
            int jsonLen = (int)val.length() - 1;
            // Clamped to MAX_LEN-1 to leave room for the '\0' written below.
            if (jsonLen > BLE_FITNESS_MAX_LEN - 1) jsonLen = BLE_FITNESS_MAX_LEN - 1;
            memcpy(s_fitness_buf, d + 1, jsonLen);
            s_fitness_buf[jsonLen] = '\0';
            s_fitness_len = jsonLen;
            DEBUG_PRINTF("[ble] fitness data received: %d bytes\n", jsonLen);
            return;
        }

    }

    // Returns true if the packet was consumed as part of a file transfer.
    static bool handle_file_packet(const uint8_t *d, int L, uint32_t now) {
        uint32_t len = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
        uint32_t offset = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);

        if (s_file_state == BLE_FILE_PENDING || s_file_state == BLE_FILE_IN_PROGRESS) {
            // Chunk: chunkLen(4) + offset(4) + data. Buffered during PENDING
            // too - when the user approves, data may already be complete.
            s_file_last_ms = now;
            uint8_t *buf = s_file_buf;
            if (buf && len > 0 && len <= (uint32_t)s_file_total_size &&
                offset <= (uint32_t)s_file_total_size - len) {
                memcpy(buf + offset, d + 8, len);
                int newLen = (int)(offset + len);
                if (newLen > s_file_len) s_file_len = newLen;
            }
            if (s_file_state == BLE_FILE_IN_PROGRESS && s_file_total_size > 0 &&
                s_file_len >= s_file_total_size) {
                s_file_state = BLE_FILE_COMPLETE;
                DEBUG_PRINTF("[ble] file transfer complete: '%s' (%d bytes)\n", s_file_name, s_file_len);
            }
            return true;
        }
        if (s_file_state == BLE_FILE_COMPLETE) return true; // waiting to be saved

        // DENIED: the phone doesn't know and keeps streaming the rest of the
        // file. Those chunks are framed exactly like a new header, so swallow
        // everything until the line has been quiet for 2 s.
        if (s_file_state == BLE_FILE_DENIED && now - s_file_last_ms < 2000) {
            s_file_last_ms = now;
            return true;
        }

        // IDLE: header = nameLen(4) + totalSize(4) + name. Names are short,
        // so the upper three length bytes are zero.
        if (len == 0 || len > 255) return false;
        uint32_t totalSize = offset;
        char name[64];
        uint32_t nameLen = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
        memcpy(name, d + 8, nameLen);
        name[nameLen] = 0;
        s_file_last_ms = now;
        file_free_buf();
        s_file_len = 0;
        uint32_t max_len = strncmp(name, "rec:", 4) == 0 ? BLE_REC_MAX_LEN : BLE_FILE_MAX_LEN;
        if (totalSize == 0 || totalSize > max_len ||
            !(s_file_buf = (uint8_t *)heap_caps_malloc(totalSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
            DEBUG_PRINTF("[ble] file '%s' rejected: %u bytes (max %u)\n", name, (unsigned)totalSize, (unsigned)max_len);
            s_file_too_big = totalSize > 0;
            s_file_total_size = 0;
            s_file_state = BLE_FILE_DENIED;
            return true;
        }
        memcpy(s_file_name, name, nameLen + 1);
        s_file_total_size = (int)totalSize;
        s_file_state = BLE_FILE_PENDING;
        DEBUG_PRINTF("[ble] file transfer requested: '%s' (%d bytes)\n", s_file_name, s_file_total_size);
        return true;
    }
};

static ServerCallbacks s_serverCallbacks;
static TimeWriteCallback s_timeCallback;
static NotifWriteCallback s_notifCallback;
static ControllerWriteCallback s_controllerCallback;

// ---- Phone link (characteristic 0x0009) ----------------------------------
// JSON messages in both directions (see phone_link.h for the vocabulary).
// Framing, same as contacts: 0x01 + total length (2 LE) + data starts a
// message, 0x02 + data continues it; a write starting with '{' is a
// whole message on its own. Complete messages from the phone wait in a
// small queue in PSRAM until the main loop picks them up.

static void link_push(const char *d, int n) {
    if (!s_link_queue || n <= 0) return;
    int next = (s_link_head + 1) % LINK_QUEUE_LEN;
    if (next == s_link_tail) { DEBUG_PRINTF("[ble] link queue full, message dropped\n"); return; }
    if (n > LINK_MSG_MAX - 1) n = LINK_MSG_MAX - 1;
    char *slot = s_link_queue + s_link_head * LINK_MSG_MAX;
    memcpy(slot, d, n);
    slot[n] = 0;
    s_link_head = next;
}

class LinkWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        s_last_rx_ms = millis();
        (void)connInfo;
        std::string val = pChar->getValue();
        int L = (int)val.length();
        if (L < 1) return;
        const char *d = val.data();
        // 0x03: a frame of an app icon / album cover (phone_images.h)
        if ((uint8_t)d[0] == 0x03) { phone_images_on_frame((const uint8_t *)d, L); return; }
        if (!s_link_queue) s_link_queue = (char *)heap_caps_malloc(LINK_QUEUE_LEN * LINK_MSG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ps_buf(&s_link_stage, LINK_MSG_MAX) || !s_link_queue) return;
        if (d[0] == '{') { link_push(d, L); return; }
        int n;
        if ((uint8_t)d[0] == 0x01 && L >= 3) {
            s_link_stage_total = (uint8_t)d[1] | ((uint8_t)d[2] << 8);
            if (s_link_stage_total > LINK_MSG_MAX - 1) s_link_stage_total = LINK_MSG_MAX - 1;
            s_link_stage_len = 0;
            d += 3; n = L - 3;
        } else if ((uint8_t)d[0] == 0x02 && s_link_stage_total > 0) {
            d += 1; n = L - 1;
        } else {
            return;
        }
        if (n > s_link_stage_total - s_link_stage_len) n = s_link_stage_total - s_link_stage_len;
        if (n > 0) { memcpy(s_link_stage + s_link_stage_len, d, n); s_link_stage_len += n; }
        if (s_link_stage_len >= s_link_stage_total) {
            link_push(s_link_stage, s_link_stage_len);
            s_link_stage_total = 0;
        }
    }
};
static LinkWriteCallback s_linkCallback;

class OtaWriteCallback : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
        (void)connInfo;
        s_last_rx_ms = millis();
        std::string val = pChar->getValue();
        ota_on_ble_write((const uint8_t *)val.data(), (int)val.length());
    }
};
static OtaWriteCallback s_otaCallback;
static ContactsWriteCallback s_contactsCallback;
static MediaWriteCallback s_mediaCallback;
static FileWriteCallback s_fileCallback;
static NotesWriteCallback s_notesCallback;

// The BLE controller needs a few tens of KB of INTERNAL RAM (it can't use
// PSRAM). If that isn't there, esp_bt_controller_init() fails and NimBLE
// used to carry on regardless and hit an assert (crash + reboot) on the
// first mutex. Now we check first, and bail out cleanly on any failure.
#define BLE_MIN_FREE_INTERNAL   (40 * 1024)
#define BLE_MIN_LARGEST_BLOCK   (12 * 1024)
static bool s_low_memory = false;

static bool ble_stack_start() {
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t big_int  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    DEBUG_PRINTF("[ble] starting: internal free %u, largest block %u\n", (unsigned)free_int, (unsigned)big_int);
    if (free_int < BLE_MIN_FREE_INTERNAL || big_int < BLE_MIN_LARGEST_BLOCK) {
        DEBUG_PRINTF("[ble] not enough internal RAM for Bluetooth - not starting\n");
        s_low_memory = true;
        return false;
    }
    if (!NimBLEDevice::init("AmoledWatch")) {
        DEBUG_PRINTF("[ble] controller init failed - Bluetooth stays off\n");
        s_low_memory = true;
        return false;
    }
    s_low_memory = false;
#if BLE_REQUIRE_ENCRYPTION
    // First start of fw 3.0: drop bonds made under the old fixed public
    // code, so every phone pairs once with a random one.
    {
        Preferences p;
        if (p.begin("blesec", false)) {
            if (p.getUChar("v", 0) < 3) {
                NimBLEDevice::deleteAllBonds();
                p.putUChar("v", 3);
                DEBUG_PRINTF("[ble] fw 3.0: old bonds cleared - phones pair again with a code\n");
            }
            p.end();
        }
    }
#endif
    NimBLEDevice::setMTU(512);
    NimBLEDevice::setDefaultPhy(BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK,
                                BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK);

    // Passkey pairing instead of Just Works: this link now carries
    // contacts, notifications, and file transfers, so an active
    // attacker being able to complete pairing silently (Just Works has
    // no protection against that, only against passive eavesdropping
    // after the fact) is worth closing off.
    // fw 3.0: random code per pairing via onPassKeyDisplay() (the old fixed
    // 246813 was in the public source), and LE Secure Connections.
    // NimBLE only asks onPassKeyDisplay() while the static passkey is left
    // at its default (123456): never call setSecurityPasskey() here.
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
    NimBLEDevice::setSecurityAuth(true, true, true);  // bond, mitm, secure connections

    s_server = NimBLEDevice::createServer();
    s_server->setCallbacks(&s_serverCallbacks);

    NimBLEService *svc = s_server->createService(svcUUID);

    s_batChar = svc->createCharacteristic(
        batCharUUID, NIMBLE_PROPERTY::READ | R_ENC | NIMBLE_PROPERTY::NOTIFY);
    s_batChar->setValue(0);

    s_timeChar = svc->createCharacteristic(
        timeCharUUID, NIMBLE_PROPERTY::WRITE | W_ENC);
    s_timeChar->setCallbacks(&s_timeCallback);

    s_notifChar = svc->createCharacteristic(
        notifCharUUID, NIMBLE_PROPERTY::WRITE | W_ENC);
    s_notifChar->setCallbacks(&s_notifCallback);

    s_contactsChar = svc->createCharacteristic(
        contactsCharUUID, NIMBLE_PROPERTY::WRITE | W_ENC);
    s_contactsChar->setCallbacks(&s_contactsCallback);

    s_mediaChar = svc->createCharacteristic(
        mediaCharUUID, NIMBLE_PROPERTY::WRITE | W_ENC);
    s_mediaChar->setCallbacks(&s_mediaCallback);

    s_fileChar = svc->createCharacteristic(
        fileCharUUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | W_ENC);   // NR: fast uploads
    s_fileChar->setCallbacks(&s_fileCallback);

    s_notesChar = svc->createCharacteristic(
        notesCharUUID, NIMBLE_PROPERTY::WRITE | W_ENC | NIMBLE_PROPERTY::NOTIFY);
    s_notesChar->setCallbacks(&s_notesCallback);

    s_controllerChar = svc->createCharacteristic(
        controllerCharUUID, NIMBLE_PROPERTY::WRITE | W_ENC);
    s_controllerChar->setCallbacks(&s_controllerCallback);

    s_linkChar = svc->createCharacteristic(
        linkCharUUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | W_ENC | NIMBLE_PROPERTY::NOTIFY);
    s_linkChar->setCallbacks(&s_linkCallback);

    s_otaChar = svc->createCharacteristic(
        otaCharUUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | W_ENC | NIMBLE_PROPERTY::NOTIFY);
    s_otaChar->setCallbacks(&s_otaCallback);

    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName("AmoledWatch");
    adv->enableScanResponse(true);
    adv->addServiceUUID(svcUUID);
    s_adv_slow = false;
    adv->setMinInterval(160);   // 100 ms while the screen is on
    adv->setMaxInterval(240);
    adv->start();

    s_enabled = true;
    DEBUG_PRINTF("[ble] advertising as 'AmoledWatch'\n");
    return true;
}

bool ble_init() {
    return ble_stack_start();
}

bool ble_enable() {
    if (s_enabled) return true;
    return ble_stack_start();
}

bool ble_failed_low_memory() {
    return s_low_memory;
}

void ble_disable() {
    if (!s_enabled) return;
    // Must set false BEFORE deinit — the disconnect callback checks this
    // to avoid calling startAdvertising() on deleted objects.
    s_enabled = false;
    s_connected = false;
    if (s_pair_code) { s_pair_code = 0; s_pair_result = -1; }   // no disconnect callback after this
    // deinit(false) shuts down the BLE stack but keeps NimBLE objects
    // (server, advertising, etc.) alive so deinit doesn't free memory
    // that the disconnect callback or other code still references.
    NimBLEDevice::deinit(false);
    DEBUG_PRINTF("[ble] stack shut down\n");
}

bool ble_is_enabled() {
    return s_enabled;
}

// Connection interval is the biggest BLE power lever: with the screen off
// and nothing moving, ask the phone for ~150 ms intervals plus a slave
// latency of 4 (the watch may skip up to 4 events when it has nothing to
// send), so the radio wakes about once a second instead of every 15-30 ms.
// Any write from the phone (link messages, notes sync, files, OTA) puts it
// back on fast parameters for a while. Disconnected: advertise every
// ~550 ms instead of ~100 ms while the screen is off.
void ble_set_low_power(bool low) { s_low_power_req = low; }

void ble_update() {
    if (!s_enabled) return;
    uint32_t now = millis();
    if (s_file_state == BLE_FILE_DENIED && s_file_buf && now - s_file_last_ms > 2500) file_free_buf();
    bool busy = now - s_last_rx_ms < 8000 || ota_active() ||
                s_file_state == BLE_FILE_PENDING || s_file_state == BLE_FILE_IN_PROGRESS;
    if (s_connected && s_server) {
        bool want_slow = s_low_power_req && !busy && now - s_last_connect_ms > 15000;
        // Going fast is urgent; going slow can wait (and is rate-limited).
        if (want_slow != s_conn_slow && (!want_slow || now - s_conn_param_ms > 5000)) {
            s_conn_slow = want_slow;
            s_conn_param_ms = now;
            if (want_slow) s_server->updateConnParams(s_conn_handle, 96, 128, 4, 600);  // 120-160 ms, latency 4, 6 s timeout
            else           s_server->updateConnParams(s_conn_handle, 12, 24, 0, 400);   // 15-30 ms, 4 s timeout
            DEBUG_PRINTF("[ble] connection params -> %s\n", want_slow ? "low power" : "fast");
        }
    } else {
        bool want_slow = s_low_power_req;
        if (want_slow != s_adv_slow) {
            NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
            s_adv_slow = want_slow;
            bool was = adv->isAdvertising();
            if (was) adv->stop();
            adv->setMinInterval(want_slow ? 874 : 160);   // 546 ms (Apple-recommended step) / 100 ms
            adv->setMaxInterval(want_slow ? 960 : 240);
            if (was) adv->start();
        }
    }
}

// Sends `data` to the connected phone now. A bare notify() only marks the
// value as changed and the stack sends whatever the value is when it gets
// to it - two quick setValue()+notify() pairs then collapse into one
// notification with the second value (a file list's header vanished that
// way and the app's sync timed out). This hands NimBLE its own copy of the
// bytes instead, and retries briefly while the stack is out of buffers.
static bool notify_now(NimBLECharacteristic *c, const uint8_t *data, int len) {
    if (!c || !s_connected) return false;
    c->setValue(data, len);   // what a read returns
    for (int tries = 0; tries < 25; tries++) {
        if (c->notify(data, len, s_conn_handle)) return true;
        if (!s_connected) return false;
        delay(4);
    }
    DEBUG_PRINTF("[ble] notify failed (%d bytes)\n", len);
    return false;
}

bool ble_ota_notify(const uint8_t *data, int len) {
    if (!s_connected || !s_otaChar) return false;
    return notify_now(s_otaChar, data, len);
}

void ble_update_battery(uint8_t pct) {
    if (s_batChar && s_connected) {
        notify_now(s_batChar, &pct, 1);
    }
}

bool ble_is_connected() {
    return s_connected;
}

bool ble_has_phone_time() {
    return s_has_time;
}

bool ble_get_phone_time(int &year, int &month, int &day, int &hour, int &minute, int &second) {
    if (!s_has_time) return false;
    year = s_phone_year;
    month = s_phone_month;
    day = s_phone_day;
    hour = s_phone_hour;
    minute = s_phone_minute;
    second = s_phone_second;
    s_has_time = false;
    return true;
}

bool ble_queue_notification(const char *text, const char *source) {
    int next = (s_notif_head + 1) % BLE_NOTIF_QUEUE_SIZE;
    if (next == s_notif_tail) return false;
    snprintf(s_notif_queue[s_notif_head].text, BLE_NOTIF_MAX_LEN, "%s", text);
    snprintf(s_notif_queue[s_notif_head].source, sizeof(s_notif_queue[0].source), "%s", source);
    s_notif_head = next;
    return true;
}

bool ble_get_notification(BleNotification &notif) {
    if (s_notif_head == s_notif_tail) return false;
    notif = s_notif_queue[s_notif_tail];
    s_notif_tail = (s_notif_tail + 1) % BLE_NOTIF_QUEUE_SIZE;
    return true;
}

int ble_get_connect_count() {
    return s_connect_count;
}

uint32_t ble_get_passkey() {
    return s_pair_code;
}

int ble_take_pairing_result() {
    return __atomic_exchange_n(&s_pair_result, (int8_t)0, __ATOMIC_SEQ_CST);   // set on the NimBLE task
}

bool ble_link_encrypted() { return s_connected && s_link_encrypted; }

int ble_bond_count() { return s_enabled ? NimBLEDevice::getNumBonds() : 0; }

bool ble_forget_bonds() {
    if (!s_enabled) return false;
    if (s_connected) s_server->disconnect(s_conn_handle);
    bool ok = NimBLEDevice::deleteAllBonds();
    DEBUG_PRINTF("[ble] bonds cleared: %s\n", ok ? "ok" : "failed");
    return ok;
}

uint32_t ble_get_last_connect_ms() {
    return s_last_connect_ms;
}

const char *ble_get_connected_device_name() {
    return s_connected_device_name;
}

void ble_disconnect() {
    if (s_connected && s_server && s_conn_handle) {
        s_server->disconnect(s_conn_handle);
        s_connected = false;
        DEBUG_PRINTF("[ble] manually disconnected\n");
    }
}

bool ble_get_contacts(const char **out_data, int *out_len) {
    if (s_contacts_len == 0) return false;
    *out_data = s_contacts_buf;
    *out_len = s_contacts_len;
    return true;
}

void ble_consume_contacts() {
    s_contacts_len = 0;
}

bool ble_get_media_state(const char **out_data, int *out_len) {
    if (s_media_len == 0) return false;
    *out_data = s_media_buf;
    *out_len = s_media_len;
    return true;
}

void ble_consume_media() {
    s_media_len = 0;
}

BleFileState ble_file_state() {
    return s_file_state;
}

const char *ble_file_name() {
    return s_file_name;
}

int ble_file_total_size() {
    return s_file_total_size;
}

int ble_file_bytes_received() {
    return s_file_len;
}

const uint8_t *ble_file_data() {
    return s_file_buf;
}

void ble_file_approve() {
    if (s_file_state == BLE_FILE_PENDING) {
        // If all data was already buffered during PENDING, go straight to COMPLETE
        if (s_file_len >= s_file_total_size && s_file_total_size > 0) {
            s_file_state = BLE_FILE_COMPLETE;
            DEBUG_PRINTF("[ble] file transfer approved (already complete): '%s' (%d bytes)\n", s_file_name, s_file_len);
        } else {
            s_file_state = BLE_FILE_IN_PROGRESS;
            DEBUG_PRINTF("[ble] file transfer approved (waiting for remaining chunks)\n");
        }
    }
}

void ble_file_deny() {
    if (s_file_state == BLE_FILE_PENDING) {
        s_file_last_ms = millis(); // start draining the rest of the stream
        s_file_state = BLE_FILE_DENIED;
        s_file_len = 0;
        s_file_total_size = 0;
        memset(s_file_name, 0, sizeof(s_file_name));
        // The buffer is freed by ble_update() once the phone has stopped
        // streaming - the BLE task may be copying a chunk into it right now.
        DEBUG_PRINTF("[ble] file transfer denied\n");
    }
}

bool ble_consume_file_too_big() {
    if (!s_file_too_big) return false;
    s_file_too_big = false;
    return true;
}

int ble_notify_payload() {
    if (!s_connected || !s_server) return 20;
    int mtu = s_server->getPeerMTU(s_conn_handle);
    if (mtu < 23) mtu = 23;
    return mtu - 3;
}

void ble_consume_file() {
    file_free_buf();
    s_file_state = BLE_FILE_IDLE;
    s_file_len = 0;
    s_file_total_size = 0;
    memset(s_file_name, 0, sizeof(s_file_name));
}

bool ble_get_watchface_data(const char **out_data, int *out_len) {
    if (s_watchface_len == 0) return false;
    *out_data = s_watchface_buf;
    *out_len = s_watchface_len;
    return true;
}

bool ble_watchface_ready() {
    return s_watchface_len > 0 && s_watchface_total == 0;
}

void ble_consume_watchface() {
    s_watchface_len = 0;
}

bool ble_get_weather(const char **out_data, int *out_len) {
    if (s_weather_len == 0) return false;
    *out_data = s_weather_buf;
    *out_len = s_weather_len;
    return true;
}

void ble_consume_weather() {
    s_weather_len = 0;
}

bool ble_get_fitness(const char **out_data, int *out_len) {
    if (s_fitness_len == 0) return false;
    *out_data = s_fitness_buf;
    *out_len = s_fitness_len;
    return true;
}

void ble_consume_fitness() {
    s_fitness_len = 0;
}

bool ble_consume_findme_request() {
    if (!s_findme_requested) return false;
    s_findme_requested = false;
    return true;
}

// ---- Phone link ----

bool ble_link_recv(char *out, int cap) {
    if (!s_link_queue || s_link_head == s_link_tail || cap <= 0) return false;
    const char *slot = s_link_queue + s_link_tail * LINK_MSG_MAX;
    strncpy(out, slot, cap - 1);
    out[cap - 1] = 0;
    s_link_tail = (s_link_tail + 1) % LINK_QUEUE_LEN;
    return true;
}

bool ble_link_send(const char *json) {
    if (!s_connected || !s_linkChar || !json) return false;
#if BLE_REQUIRE_ENCRYPTION
    if (!s_link_encrypted) return false;   // the subscription (CCCD) itself needs no pairing
#endif
    int len = (int)strlen(json);
    int room = ble_notify_payload();
    if (room > 500) room = 500;
    uint8_t pkt[503];
    if (len <= room) {
        return notify_now(s_linkChar, (const uint8_t *)json, len);
    }
    // Longer than one notification: 0x01 + total + data, then 0x02 + data.
    int off = 0;
    bool first = true;
    while (off < len) {
        int hdr = first ? 3 : 1;
        int n = len - off;
        if (n > room - hdr) n = room - hdr;
        pkt[0] = first ? 0x01 : 0x02;
        if (first) { pkt[1] = len & 0xFF; pkt[2] = (len >> 8) & 0xFF; }
        memcpy(pkt + hdr, json + off, n);
        if (!notify_now(s_linkChar, pkt, hdr + n)) return false;
        off += n;
        first = false;
    }
    return true;
}

// ---- Notes sync ----

BleNotesCmd ble_notes_get_cmd() {
    return s_notes_cmd;
}

const char *ble_notes_cmd_arg() {
    return s_notes_cmd_arg;
}

void ble_notes_consume_cmd() {
    s_notes_cmd = BLE_NOTES_CMD_NONE;
    memset(s_notes_cmd_arg, 0, sizeof(s_notes_cmd_arg));
}

int ble_notes_take_chunk_credits() {
    int cur = __atomic_load_n(&s_notes_chunk_credits, __ATOMIC_SEQ_CST);
    for (;;) {
        int take = cur > 64 ? 64 : cur;
        if (take <= 0) return 0;
        if (__atomic_compare_exchange_n(&s_notes_chunk_credits, &cur, cur - take, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return take;
    }
}

void ble_notes_clear_chunk_credits() {
    __atomic_store_n(&s_notes_chunk_credits, 0, __ATOMIC_SEQ_CST);
}

void ble_notes_return_chunk_credits(int n) {
    if (n > 0) __atomic_fetch_add(&s_notes_chunk_credits, n, __ATOMIC_SEQ_CST);
}

bool ble_notes_send_file_list(const char *json_data) {
    if (!s_connected || !s_notesChar) return false;
    // Send as 0x10 prefix + JSON, chunked via notifications
    int len = strlen(json_data);
    // Each notification carries 3 header bytes; a notification longer than
    // the negotiated MTU is silently cut, so size chunks to it.
    int maxChunk = ble_notify_payload() - 3;
    if (maxChunk > 478) maxChunk = 478;
    if (maxChunk < 1) maxChunk = 1;
    int totalChunks = (len + maxChunk - 1) / maxChunk;

    // Send header: 0x10 + totalChunks(2 LE) + totalLen(2 LE)
    uint8_t hdr[6];
    hdr[0] = 0x10;
    hdr[1] = totalChunks & 0xFF;
    hdr[2] = (totalChunks >> 8) & 0xFF;
    hdr[3] = len & 0xFF;
    hdr[4] = (len >> 8) & 0xFF;
    hdr[5] = (len >> 16) & 0xFF;
    if (!notify_now(s_notesChar, hdr, 6)) return false;

    // Send data chunks
    for (int i = 0; i < totalChunks; i++) {
        int offset = i * maxChunk;
        int chunkLen = len - offset;
        if (chunkLen > maxChunk) chunkLen = maxChunk;

        // Build packet: 0x11 + chunkIdx(2 LE) + data
        uint8_t pkt[3 + 478];
        pkt[0] = 0x11;
        pkt[1] = i & 0xFF;
        pkt[2] = (i >> 8) & 0xFF;
        memcpy(pkt + 3, json_data + offset, chunkLen);
        if (!notify_now(s_notesChar, pkt, 3 + chunkLen)) return false;
    }

    DEBUG_PRINTF("[ble] notes: sent file list (%d bytes, %d chunks)\n", len, totalChunks);
    return true;
}

bool ble_notes_send_file_chunk(int chunk_idx, int total_chunks, const uint8_t *data, int len) {
    if (!s_connected || !s_notesChar) return false;
    // Packet: 0x12 + chunkIdx(2 LE) + totalChunks(2 LE) + data
    uint8_t pkt[5 + 500];
    pkt[0] = 0x12;
    pkt[1] = chunk_idx & 0xFF;
    pkt[2] = (chunk_idx >> 8) & 0xFF;
    pkt[3] = total_chunks & 0xFF;
    pkt[4] = (total_chunks >> 8) & 0xFF;
    int copyLen = len;
    if (copyLen > 500) copyLen = 500;
    memcpy(pkt + 5, data, copyLen);
    // false = the stack is out of buffers right now; the caller retries.
    return s_notesChar->notify(pkt, 5 + copyLen);
}

bool ble_notes_send_transfer_complete() {
    if (!s_connected || !s_notesChar) return false;
    uint8_t pkt[1] = {0x13};
    return notify_now(s_notesChar, pkt, 1);
}

const char *ble_notes_get_transcript() {
    if (s_notes_transcript_len == 0) return nullptr;
    return s_notes_transcript;
}

void ble_notes_consume_transcript() {
    s_notes_transcript_len = 0;
    if (s_notes_transcript) s_notes_transcript[0] = 0;
}

#else

bool ble_init() { return false; }
bool ble_enable() { return false; }
bool ble_failed_low_memory() { return false; }
void ble_disable() {}
bool ble_is_enabled() { return false; }
void ble_update() {}
void ble_update_battery(uint8_t) {}
bool ble_is_connected() { return false; }
bool ble_has_phone_time() { return false; }
bool ble_get_phone_time(int &, int &, int &, int &, int &, int &) { return false; }
bool ble_queue_notification(const char *, const char *) { return false; }
bool ble_get_notification(BleNotification &) { return false; }
int ble_get_connect_count() { return 0; }
uint32_t ble_get_passkey() { return 0; }
int ble_take_pairing_result() { return 0; }
bool ble_link_encrypted() { return false; }
int ble_bond_count() { return 0; }
bool ble_forget_bonds() { return false; }
uint32_t ble_get_last_connect_ms() { return 0; }
const char *ble_get_connected_device_name() { return ""; }
void ble_disconnect() {}
bool ble_get_contacts(const char **, int *) { return false; }
void ble_consume_contacts() {}
bool ble_get_media_state(const char **, int *) { return false; }
void ble_consume_media() {}
BleFileState ble_file_state() { return BLE_FILE_IDLE; }
const char *ble_file_name() { return ""; }
int ble_file_total_size() { return 0; }
int ble_file_bytes_received() { return 0; }
const uint8_t *ble_file_data() { return nullptr; }
void ble_file_approve() {}
void ble_file_deny() {}
void ble_consume_file() {}
bool ble_consume_file_too_big() { return false; }
bool ble_link_recv(char *, int) { return false; }
bool ble_link_send(const char *) { return false; }
bool ble_ota_notify(const uint8_t *, int) { return false; }
void ble_set_low_power(bool) {}
int ble_notify_payload() { return 20; }
bool ble_get_watchface_data(const char **, int *) { return false; }
void ble_consume_watchface() {}
bool ble_get_weather(const char **, int *) { return false; }
void ble_consume_weather() {}
bool ble_get_fitness(const char **, int *) { return false; }
void ble_consume_fitness() {}
bool ble_consume_findme_request() { return false; }
BleNotesCmd ble_notes_get_cmd() { return BLE_NOTES_CMD_NONE; }
const char *ble_notes_cmd_arg() { return ""; }
void ble_notes_consume_cmd() {}
int ble_notes_take_chunk_credits() { return 0; }
void ble_notes_clear_chunk_credits() {}
void ble_notes_return_chunk_credits(int) {}
bool ble_notes_send_file_list(const char *) { return false; }
bool ble_notes_send_file_chunk(int, int, const uint8_t *, int) { return false; }
bool ble_notes_send_transfer_complete() { return false; }
const char *ble_notes_get_transcript() { return nullptr; }
void ble_notes_consume_transcript() {}

#endif
