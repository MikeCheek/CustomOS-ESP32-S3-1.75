/*
 * hal_ota.h
 * Firmware updates over Bluetooth from the companion app.
 *
 * The app streams the .bin over characteristic 19B1000A (see
 * COMPANION_PROTOCOL.md). Packets arrive on the BLE task and are only
 * copied into a PSRAM ring here; ota_update() (main loop) writes them to
 * the inactive OTA slot with Update.h, so flash writes never block the
 * BLE stack. When the image is complete and verified the watch restarts
 * into it. The new image must survive a minute (diag_loop() then marks it
 * good) or the bootloader rolls back to the previous firmware.
 */
#pragma once
#include <stdint.h>

enum OtaState : uint8_t {
    OTA_IDLE,
    OTA_RECEIVING,
    OTA_DONE,      // verified - restarting
    OTA_FAILED,
};

// Called by hal_ble.cpp's characteristic callback (BLE task).
void ota_on_ble_write(const uint8_t *data, int len);

// Main loop, awake or asleep.
void ota_update();

OtaState    ota_state();
bool        ota_active();          // receiving
uint32_t    ota_total();
uint32_t    ota_written();
const char *ota_error();
void        ota_cancel();          // user cancelled on the watch

// Shared with the Wi-Fi updater (hal_fwupdate.cpp).
// ECDSA P-256 check of an image's SHA-256 against ota_pubkey.h; always true
// when no key is set.
bool ota_signature_valid(const uint8_t hash[32], const uint8_t sig[64]);
bool ota_signing_required();       // a key is set in ota_pubkey.h
// -1 / 0 / 1 for "a" older / same / newer than "b" ("3.10.0" > "3.9.2").
int  ota_compare_versions(const char *a, const char *b);
