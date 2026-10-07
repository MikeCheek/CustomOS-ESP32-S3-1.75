/*
 * hal_sd.h
 * microSD card over SPI. Only compiled in if FEATURE_SD_CARD is set.
 */

#pragma once
#include <stdint.h>
#include <Arduino.h>

bool sd_init();
bool sd_is_mounted();
// Unmount + mount again - drops FAT/directory caches after a PC changed
// the card over USB (hal_usb.cpp). Returns the new mounted state.
bool sd_remount();
uint64_t sd_card_size_mb();
uint64_t sd_used_mb();

// Returns a newline-joined listing of the root directory (name + size),
// capped at `max_entries`, for a simple on-screen file browser.
String sd_list_root(int max_entries = 20);

// Round-trip write/read test: writes `contents` to /wm_test.txt, reads
// it back, returns true if it matches.
bool sd_write_read_test(const String &contents, String &readback_out);

// Save binary data to /<filename> on the SD card. Creates or overwrites.
// Returns true on success.
bool sd_save_file(const char *filename, const uint8_t *data, int len);

// Check if a file exists on the SD card.
bool sd_file_exists(const char *filename);

// Returns false if `name` contains a path separator or "..", which
// would let it escape whatever fixed directory prefix it's about to be
// concatenated onto. Filenames arriving from an external source (BLE
// file transfer, notes sync download/delete) should always be checked
// with this before being used to build a file path - without it, a
// crafted filename like "../../something" can read, overwrite, or
// delete files outside the intended folder.
bool sd_is_safe_filename(const char *name);
