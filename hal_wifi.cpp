#include "hal_wifi.h"
#include "config.h"

#if FEATURE_WIFI

#include <WiFi.h>
#include <esp_heap_caps.h>
#include "hal_nvs.h"
#include "app_settings_state.h"
#include "hal_wifi_xfer.h"

static bool s_enabled = false;
static bool s_connecting = false;
static char s_ssid[64] = {0};
static char s_pass[64] = {0};
static const char *s_error = nullptr;

void wifi_enable() {
    if (s_enabled) return;
    // The Wi-Fi driver needs ~50 KB of internal RAM (its buffers can't go
    // to PSRAM in this core build). With Bluetooth connected there may not
    // be that much: WiFi.mode() then fails and a scan finds nothing, which
    // used to look like "no networks around".
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (!WiFi.mode(WIFI_STA)) {
        s_error = "Not enough memory for Wi-Fi";
        DEBUG_PRINTF("[wifi] radio start FAILED (internal RAM free %u, largest %u)\n", (unsigned)free_int,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        WiFi.mode(WIFI_OFF);
        return;
    }
    s_error = nullptr;
    DEBUG_PRINTF("[wifi] radio on (internal RAM free before: %u)\n", (unsigned)free_int);
    WiFi.setAutoReconnect(false);
    s_enabled = true;

    if (!s_ssid[0]) {
        nvs_load_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));
    }
    if (s_ssid[0]) {
        DEBUG_PRINTF("[wifi] connecting to '%s'...\n", s_ssid);
        WiFi.begin(s_ssid, s_pass);
        s_connecting = true;
    } else {
        DEBUG_PRINTF("[wifi] no saved credentials\n");
    }
}

// (Re)connects with the saved credentials now - after they were changed.
void wifi_connect_now() {
    if (!s_enabled) { wifi_enable(); return; }
    nvs_load_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass));
    if (!s_ssid[0]) return;
    WiFi.disconnect(false);
    delay(50);
    DEBUG_PRINTF("[wifi] connecting to '%s'...\n", s_ssid);
    WiFi.begin(s_ssid, s_pass);
    s_connecting = true;
}

void wifi_disable() {
    if (!s_enabled) return;
    // A recordings transfer owns the radio until it ends (NTP finishing,
    // battery saver...). It calls this itself when done.
    if (wifi_xfer_active()) return;
    WiFi.disconnect(true);
    // Only actually power the radio down if ESP-NOW isn't also using
    // it right now - both features share one physical radio via the
    // same STA mode (see hal_espnow.cpp's matching check).
    if (!g_app_settings.espnow_on) {
        WiFi.mode(WIFI_OFF);
    }
    s_connecting = false;
    s_enabled = false;
    DEBUG_PRINTF("[wifi] radio off\n");
}

bool wifi_is_enabled() {
    return s_enabled;
}

const char *wifi_last_error() { return s_error; }

bool wifi_is_connected() {
    return s_enabled && WiFi.status() == WL_CONNECTED;
}

bool wifi_connecting() {
    return s_connecting && WiFi.status() != WL_CONNECTED;
}

void wifi_set_credentials(const char *ssid, const char *pass) {
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", pass);
    nvs_save_wifi(ssid, pass);
}

bool wifi_has_credentials() {
    if (s_ssid[0] != '\0') return true;
    return nvs_load_wifi(s_ssid, sizeof(s_ssid), s_pass, sizeof(s_pass)) && s_ssid[0] != '\0';
}

#else

void wifi_enable() {}
void wifi_disable() {}
void wifi_connect_now() {}
bool wifi_is_enabled() { return false; }
const char *wifi_last_error() { return nullptr; }
bool wifi_is_connected() { return false; }
bool wifi_connecting() { return false; }
void wifi_set_credentials(const char *, const char *) {}
bool wifi_has_credentials() { return false; }

#endif
