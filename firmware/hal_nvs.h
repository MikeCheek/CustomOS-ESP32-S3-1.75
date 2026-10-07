/*
 * hal_nvs.h
 * Non-volatile storage via ESP32 Preferences for settings, high scores,
 * step count, and WiFi credentials. Wraps Preferences.h behind a thin
 * API so the rest of the project never touches the NVS namespace directly.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

struct AppSettings;

// Call once from setup(), before anything else reads NVS.
void nvs_init();

// Settings persistence. Caller passes a populated AppSettings struct to
// save, or an empty one to load. nvs_load_settings() returns true if
// valid data was found (struct now populated), false if NVS is empty
// (caller should keep its defaults).
bool nvs_save_settings(const AppSettings &s);
bool nvs_load_settings(AppSettings &s);

// High scores - keyed by a short game name ("snake", "reaction", etc.).
void nvs_save_high_score(const char *game, uint32_t score);
uint32_t nvs_load_high_score(const char *game);

// Step counter persistence (saved periodically, not every step).
void nvs_save_steps(uint32_t steps);
uint32_t nvs_load_steps();
void nvs_save_steps_day(uint32_t day);
uint32_t nvs_load_steps_day();

// WiFi credentials.
void nvs_save_wifi(const char *ssid, const char *pass);
bool nvs_load_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
void nvs_save_bambu_code(const char *code);
bool nvs_load_bambu_code(char *code, size_t code_len);

// Watchface selection index.
void nvs_save_watchface_index(uint8_t idx);
uint8_t nvs_load_watchface_index();

// Custom watchface JSON persistence (saved on BLE receive, loaded on boot).
bool nvs_save_watchface_json(const char *json, int len);
bool nvs_load_watchface_json(char *buf, int buf_size, int *out_len);
bool nvs_has_watchface_json();
void nvs_clear_watchface_json();
