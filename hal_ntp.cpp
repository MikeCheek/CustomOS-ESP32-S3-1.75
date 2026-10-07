#include "hal_ntp.h"
#include "config.h"

#if FEATURE_WIFI

#include <WiFi.h>
#include <time.h>
#include "hal_wifi.h"
#include "hal_rtc.h"

static NtpStatus s_status = NTP_IDLE;
static uint32_t s_start_ms = 0;
static const uint32_t NTP_TIMEOUT_MS = 15000;
static const char *NTP_SERVER = "pool.ntp.org";
static const long GMT_OFFSET_SEC = 0;    // adjust for your timezone
static const int DST_OFFSET_SEC = 0;

void ntp_init() {
    s_status = NTP_IDLE;
}

void ntp_sync() {
    if (s_status == NTP_CONNECTING || s_status == NTP_SYNCING) return;

    if (!wifi_has_credentials()) {
        s_status = NTP_FAILED;
        DEBUG_PRINTF("[ntp] no WiFi credentials saved\n");
        return;
    }

    if (!wifi_is_enabled()) {
        wifi_enable();
    }

    if (!wifi_is_connected()) {
        // Wait for connection
    }

    s_status = NTP_CONNECTING;
    s_start_ms = millis();
    DEBUG_PRINTF("[ntp] starting sync...\n");
}

NtpStatus ntp_get_status() {
    return s_status;
}

void ntp_update() {
    if (s_status == NTP_CONNECTING) {
        if (wifi_is_connected()) {
            s_status = NTP_SYNCING;
            configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
            s_start_ms = millis();
            DEBUG_PRINTF("[ntp] WiFi connected, querying NTP...\n");
        } else if (millis() - s_start_ms > NTP_TIMEOUT_MS) {
            s_status = NTP_FAILED;
            wifi_disable();
            DEBUG_PRINTF("[ntp] WiFi connection timed out\n");
        }
    } else if (s_status == NTP_SYNCING) {
        struct tm t;
        if (getLocalTime(&t, 100) && t.tm_year > (2024 - 1900)) {
            rtc_set(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec);
            s_status = NTP_DONE;
            wifi_disable();
            DEBUG_PRINTF("[ntp] sync OK: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                         t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                         t.tm_hour, t.tm_min, t.tm_sec);
        } else if (millis() - s_start_ms > NTP_TIMEOUT_MS) {
            s_status = NTP_FAILED;
            wifi_disable();
            DEBUG_PRINTF("[ntp] NTP query timed out\n");
        }
    }
}

#else

void ntp_init() {}
void ntp_sync() {}
NtpStatus ntp_get_status() { return NTP_FAILED; }
void ntp_update() {}

#endif
