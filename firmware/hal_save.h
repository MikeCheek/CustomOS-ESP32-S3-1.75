/*
 * hal_save.h
 * Generic persistent-progression save/load, reusable by any future
 * game on this hardware - not specific to Ninja Dungeon. Wraps the
 * ESP32 Preferences (NVS) library the same way hal_nvs.cpp already
 * does for system settings, but in its OWN "gamesave" namespace so
 * game progression data never mixes with system settings.
 *
 * Each game defines its own progress struct and just saves/loads it
 * as a raw blob under its own key - this module doesn't know or care
 * what's inside it.
 */
#pragma once
#include <Arduino.h>

bool game_save_blob(const char *key, const void *data, size_t len);
// Returns false (and leaves *data untouched) if the key doesn't exist
// yet or the stored blob is a different size - callers should fall
// back to sensible defaults in that case, not treat it as an error.
bool game_load_blob(const char *key, void *data, size_t len);
