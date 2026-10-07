/*
 * hal_ble.h
 * BLE GATT server for companion phone link. Exposes battery level,
 * accepts time sync, notifications, contacts, media control, and
 * file transfers from a connected phone. Feature-gated on FEATURE_BLE.
 */

#pragma once
#include <stdint.h>

// Normalizes UTF-8 text in place for the watch fonts: Latin-1 accents kept,
// smart quotes/dashes mapped to ASCII, other symbols (emoji...) -> '?'.
void ble_fold_utf8(char *s);

// Call once from setup(). Initializes BLE device and starts
// advertising. No-op if FEATURE_BLE is 0.
bool ble_init();

// Full NimBLE shutdown - frees radio + memory. No-op if already off.
void ble_disable();

// Full NimBLE re-init + advertise. No-op if already on. Returns false (and
// leaves Bluetooth off) if there isn't enough internal RAM to start it.
bool ble_enable();

// True if the last start attempt was refused/failed for lack of memory.
bool ble_failed_low_memory();

// Returns true if the BLE stack is powered on.
bool ble_is_enabled();

// Call from loop() to process BLE events.
void ble_update();

// Update the battery level characteristic (call when battery % changes).
void ble_update_battery(uint8_t pct);

// Returns true if a phone is currently connected.
bool ble_is_connected();

// Returns true if time was received from the connected phone.
bool ble_has_phone_time();

// Get the time received from the phone. Returns false if no time received.
bool ble_get_phone_time(int &year, int &month, int &day, int &hour, int &minute, int &second);

// Queue a notification text to display on screen (from phone).
// Returns true if queued, false if buffer full.
bool ble_queue_notification(const char *text, const char *source);

// Notification buffer for display by app_notifications.
#define BLE_NOTIF_MAX_LEN 128
#define BLE_NOTIF_QUEUE_SIZE 8

struct BleNotification {
    char text[BLE_NOTIF_MAX_LEN];
    char source[32];
};

// Returns queued notifications (consumed on read).
bool ble_get_notification(BleNotification &notif);

// --- Connection tracking (for BLE status screen) ---

// Returns how many times a phone has connected since boot.
int ble_get_connect_count();

// Pairing code being shown right now (random per pairing, fw 3.0), 0 = none.
uint32_t ble_get_passkey();
// 1 = a pairing just succeeded, -1 = failed/cancelled, 0 = nothing new (clears it).
int ble_take_pairing_result();
bool ble_link_encrypted();
int ble_bond_count();          // phones paired with this watch
bool ble_forget_bonds();       // unpair every phone (disconnects the current one)

// Returns millis() timestamp of last connect event.
uint32_t ble_get_last_connect_ms();

// Returns the name of the currently connected device (empty if none).
const char *ble_get_connected_device_name();

// Manually disconnect the current BLE connection.
void ble_disconnect();

// --- Contacts data (JSON array from phone, [{"name","phone","email"}]) ---
// Returns pointer to contacts JSON buffer and its length. Returns false
// if no contacts received yet.
bool ble_get_contacts(const char **out_data, int *out_len);
// Marks contacts as consumed so next sync overwrites cleanly.
void ble_consume_contacts();

// --- Media state (JSON object from phone) ---
// Returns pointer to media JSON buffer and its length. Returns false
// if no media state received yet.
bool ble_get_media_state(const char **out_data, int *out_len);
// Marks media as consumed.
void ble_consume_media();

// --- File transfer (chunked binary from phone) ---

enum BleFileState {
    BLE_FILE_IDLE,           // No transfer in progress
    BLE_FILE_PENDING,        // Header received, awaiting user approval
    BLE_FILE_IN_PROGRESS,    // Approved, chunks arriving
    BLE_FILE_COMPLETE,       // All chunks received
    BLE_FILE_DENIED          // User denied the transfer
};

// Returns the current file transfer state.
BleFileState ble_file_state();

// Returns the pending file's name (valid in PENDING/IN_PROGRESS/COMPLETE).
const char *ble_file_name();

// Returns total expected size in bytes (valid after PENDING).
int ble_file_total_size();

// Returns bytes received so far (valid in IN_PROGRESS/COMPLETE).
int ble_file_bytes_received();

// Returns the received data buffer (valid in COMPLETE).
const uint8_t *ble_file_data();

// User approves the pending transfer → transitions to IN_PROGRESS.
void ble_file_approve();

// User denies the pending transfer → transitions to DENIED, then IDLE.
void ble_file_deny();

// Marks file as consumed after saving to SD → transitions to IDLE.
void ble_consume_file();

// True once after the phone offered a file that was refused automatically
// (bigger than the 1 MB receive limit, or out of memory) - show a toast.
bool ble_consume_file_too_big();

// Largest notification payload the connected phone accepts (MTU - 3).
int ble_notify_payload();

// --- Watchface layout (JSON from phone, via file characteristic) ---
// Returns true when all chunks received and data is complete (not mid-transfer).
bool ble_watchface_ready();
// Returns pointer to watchface JSON data and its length. Returns false
// if no watchface data received yet.
bool ble_get_watchface_data(const char **out_data, int *out_len);
// Marks watchface as consumed.
void ble_consume_watchface();

// --- Weather data (JSON from phone, prefix 0x02 on file characteristic) ---
bool ble_get_weather(const char **out_data, int *out_len);
void ble_consume_weather();

// --- Fitness data (JSON from phone, prefix 0x03 on file characteristic) ---
bool ble_get_fitness(const char **out_data, int *out_len);
void ble_consume_fitness();

// --- Phone link (characteristic 0x0009) - see phone_link.h ---
// Next complete JSON message from the phone (consumed on read).
bool ble_link_recv(char *out, int cap);
// Sends a JSON message to the phone (chunked if needed). Main loop only.
bool ble_link_send(const char *json);

// --- Firmware update (characteristic 0x000A) - see hal_ota.h ---
bool ble_ota_notify(const uint8_t *data, int len);

// --- Power ---------------------------------------------------------------
// true while the screen is off: slow connection interval / advertising
// unless something is being transferred (applied in ble_update()).
void ble_set_low_power(bool low);

// --- Find My Watch ---------------------------------------------------
// The companion app's "Find Watch" button writes a reserved sentinel
// string to the notification characteristic instead of a real
// notification (see the sentinel constant in hal_ble.cpp and the
// matching one in the app's ble_service.dart) - NotifWriteCallback
// recognizes it and sets this flag instead of queueing it as a normal
// notification. One-shot consume, same pattern as the getters above;
// call from the main loop (not from a BLE callback - NimBLE callbacks
// don't run on the main thread, and this is what actually pushes a UI
// screen, so it has to happen from code that owns the screen stack).
bool ble_consume_findme_request();

// --- Notes sync (bidirectional, via notes characteristic 0x0007) ---

enum BleNotesCmd {
    BLE_NOTES_CMD_NONE = 0,
    BLE_NOTES_CMD_LIST_FILES,       // Phone requests list of recordings
    BLE_NOTES_CMD_DOWNLOAD_FILE,    // Phone requests a specific file
    BLE_NOTES_CMD_DELETE_FILE,      // Phone requests deleting a file
    BLE_NOTES_CMD_TRANSCRIPT,       // Phone sends transcript text back
    BLE_NOTES_CMD_REQUEST_CHUNK,    // Phone requests next file chunk (flow control)
};

// Check if a new command was received from the phone.
BleNotesCmd ble_notes_get_cmd();
// Get the argument (filename) for DOWNLOAD_FILE or DELETE_FILE commands.
const char *ble_notes_cmd_arg();
// Mark command as consumed after handling.
void ble_notes_consume_cmd();
// Check and consume a chunk request (flow control). Returns true once per 0x05 write.
// Chunks the app asked for since the last call (windowed download).
int  ble_notes_take_chunk_credits();
void ble_notes_return_chunk_credits(int n);   // couldn't send them yet
void ble_notes_clear_chunk_credits();          // a download ended / was replaced

// Send file list JSON to the phone via BLE notifications.
// json_data: JSON array string, e.g. [{"name":"rec_001.wav","size":12345,"date":"2026-07-18"}]
bool ble_notes_send_file_list(const char *json_data);
// Send a file chunk to the phone via BLE notifications.
// chunk_idx: 0-based chunk number, total_chunks: total, data/len: chunk payload
bool ble_notes_send_file_chunk(int chunk_idx, int total_chunks, const uint8_t *data, int len);
// Send file transfer complete notification.
bool ble_notes_send_transfer_complete();
// Get received transcript text (from BLE_NOTES_CMD_TRANSCRIPT).
const char *ble_notes_get_transcript();
void ble_notes_consume_transcript();
