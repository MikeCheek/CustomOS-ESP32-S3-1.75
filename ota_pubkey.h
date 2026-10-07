/*
 * ota_pubkey.h - public key that firmware updates over Bluetooth must be
 * signed with (ECDSA P-256 over the SHA-256 of the image).
 *
 * As shipped, no key is set (OTA_PUBKEY_SET 0): updates are accepted
 * unsigned, as before. To require signed updates:
 *   1. python scripts/ota_keygen.py        (writes keys/ota_private.pem - keep it
 *                                           secret, it's git-ignored - and rewrites
 *                                           this file with your public key)
 *   2. flash this build once over USB
 *   3. from then on sign every update:  python scripts/sign_firmware.py build/AmoledSmartWatchOS.ino.bin
 *      and send the resulting *.signed.bin from the app.
 * Flashing over USB always works, signed or not (that's the recovery path).
 */
#pragma once
#include <stdint.h>

#define OTA_PUBKEY_SET 0
// Uncompressed point: 0x04 || X(32) || Y(32)
static const uint8_t OTA_PUBKEY[65] = { 0 };
