/*
 * hal_wifi.h
 * WiFi connect/disconnect for on-demand NTP sync. Feature-gated on
 * FEATURE_WIFI. Credentials are stored in NVS via hal_nvs.h.
 *
 * WiFi starts OFF at boot. Call wifi_enable() to power up the radio
 * and connect; wifi_disable() tears it all down.
 */

#pragma once

// Powers up the WiFi radio and connects with saved credentials.
// No-op if FEATURE_WIFI is 0.
void wifi_enable();

// Disconnects from WiFi and powers down the radio.
void wifi_disable();

// Connect with the saved credentials right away (starts the radio if off,
// reconnects if it's on). Used after the credentials change.
void wifi_connect_now();

// Returns true if the radio has been powered on (whether connected or
// still connecting). False means the radio is fully off.
bool wifi_is_enabled();

// Why the radio couldn't start ("Not enough memory for Wi-Fi"), or nullptr.
const char *wifi_last_error();

// Status checks.
bool wifi_is_connected();
bool wifi_connecting();

// Credential management.
void wifi_set_credentials(const char *ssid, const char *pass);
bool wifi_has_credentials();
