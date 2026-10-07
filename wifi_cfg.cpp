#include "wifi_cfg.h"
#include "config.h"
#include "hal_wifi.h"
#include "hal_wifi_xfer.h"
#include "hal_nvs.h"
#include "hal_ble.h"
#include "app_settings_state.h"
#include "ui.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

#if FEATURE_WIFI
#include <WiFi.h>

enum CfgOp { C_IDLE, C_SCAN, C_TEST };
static CfgOp    s_op = C_IDLE;
static uint32_t s_t0 = 0;
static bool     s_was_enabled = false;    // radio state before we touched it
static bool     s_scan_started = false;

static const uint32_t TEST_TIMEOUT_MS = 15000;
static const uint32_t SCAN_TIMEOUT_MS = 12000;

static void send(JsonDocument &doc) {
    // Up to 15 networks x ~45 bytes: build in PSRAM, not on the loop stack.
    const size_t N = 1600;
    char *buf = (char *)heap_caps_malloc(N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    size_t n = serializeJson(doc, buf, N);
    if (n > 0 && n < N) ble_link_send(buf);
    heap_caps_free(buf);
}

static void fill_status(JsonDocument &out) {
    char ssid[64] = {0}, pass[64] = {0};
    bool saved = nvs_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0];
    out["saved"] = saved ? ssid : "";
    out["on"] = wifi_is_enabled() ? 1 : 0;
    out["auto"] = g_app_settings.wifi_on ? 1 : 0;
    bool conn = wifi_is_connected();
    out["conn"] = conn ? 1 : 0;
    if (conn) {
        out["ssid"] = WiFi.SSID();
        out["ip"] = WiFi.localIP().toString();
        out["rssi"] = WiFi.RSSI();
    }
    if (wifi_last_error()) out["err"] = wifi_last_error();
    out["ram"] = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static void reply_status(const char *op) {
    JsonDocument out;
    out["e"] = "wcfg";
    out["op"] = op;
    fill_status(out);
    send(out);
}

static void reply_err(const char *op, const char *why) {
    JsonDocument out;
    out["e"] = "wcfg";
    out["op"] = op;
    out["ok"] = 0;
    out["why"] = why;
    send(out);
}

// Back to how the radio was before the scan / test.
static void restore_radio() {
    if (!s_was_enabled && !g_app_settings.wifi_on && !wifi_xfer_active()) wifi_disable();
}

static bool start_radio(const char *op) {
    s_was_enabled = wifi_is_enabled();
    if (!s_was_enabled) wifi_enable();
    if (!wifi_is_enabled()) {
        reply_err(op, wifi_last_error() ? wifi_last_error() : "Wi-Fi radio didn't start");
        return false;
    }
    return true;
}

static void begin_test() {
    if (!start_radio("test")) return;
    wifi_connect_now();
    s_op = C_TEST;
    s_t0 = millis();
}

void wifi_cfg_handle(JsonDocument &doc) {
    const char *op = doc["op"] | "status";
    if (!strcmp(op, "status")) { reply_status("status"); return; }
    if (s_op != C_IDLE) { reply_err(op, "Busy - try again in a few seconds"); return; }
    if (wifi_xfer_active()) { reply_err(op, "A file transfer is using Wi-Fi"); return; }

    if (!strcmp(op, "scan")) {
        if (!start_radio("scan")) return;
        WiFi.scanDelete();
        s_scan_started = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
        s_op = C_SCAN;
        s_t0 = millis();
    } else if (!strcmp(op, "set")) {
        const char *ssid = doc["s"] | "";
        const char *pass = doc["p"] | "";
        if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 63) { reply_err("test", "Invalid network name or password"); return; }
        wifi_set_credentials(ssid, pass);
        DEBUG_PRINTF("[wcfg] credentials for '%s' set from the phone\n", ssid);
        ui_show_toast("Wi-Fi set from phone", 1500);
        begin_test();
    } else if (!strcmp(op, "test")) {
        if (!wifi_has_credentials()) { reply_err("test", "No network saved on the watch"); return; }
        begin_test();
    } else if (!strcmp(op, "forget")) {
        if (wifi_is_enabled() && !wifi_xfer_active()) wifi_disable();
        wifi_set_credentials("", "");
        reply_status("status");
    } else {
        reply_err(op, "Unknown request");
    }
}

void wifi_cfg_update() {
    if (s_op == C_IDLE) return;
    uint32_t el = millis() - s_t0;

    if (s_op == C_SCAN) {
        int n = s_scan_started ? WiFi.scanComplete() : WIFI_SCAN_FAILED;
        if (n == WIFI_SCAN_RUNNING && el < SCAN_TIMEOUT_MS) return;
        JsonDocument out;
        out["e"] = "wcfg";
        out["op"] = "scan";
        if (n < 0) {
            out["ok"] = 0;
            out["why"] = n == WIFI_SCAN_RUNNING ? "Scan timed out" : "Scan failed";
        } else {
            out["ok"] = 1;
            JsonArray l = out["l"].to<JsonArray>();
            // strongest first, no duplicates (several access points per name)
            int taken = 0;
            for (int pass = 0; pass < n && taken < 15; pass++) {
                int best = -1;
                for (int i = 0; i < n; i++) {
                    String s = WiFi.SSID(i);
                    if (s.length() == 0) continue;
                    bool dup = false;
                    for (JsonObject o : l) if (s == (const char *)(o["s"] | "")) { dup = true; break; }
                    if (dup) continue;
                    if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
                }
                if (best < 0) break;
                JsonObject o = l.add<JsonObject>();
                o["s"] = WiFi.SSID(best);
                o["r"] = WiFi.RSSI(best);
                o["o"] = WiFi.encryptionType(best) == WIFI_AUTH_OPEN ? 1 : 0;
                taken++;
            }
        }
        WiFi.scanDelete();
        s_op = C_IDLE;
        send(out);
        restore_radio();
        return;
    }

    // C_TEST
    wl_status_t st = WiFi.status();
    bool done = st == WL_CONNECTED || st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL || el > TEST_TIMEOUT_MS;
    if (!done) return;
    JsonDocument out;
    out["e"] = "wcfg";
    out["op"] = "test";
    out["ok"] = st == WL_CONNECTED ? 1 : 0;
    if (st != WL_CONNECTED) {
        out["why"] = st == WL_NO_SSID_AVAIL ? "Network not found - is it 2.4 GHz and in range?"
                   : st == WL_CONNECT_FAILED ? "Couldn't connect - check the password"
                   : "Timed out - check the password and the signal";
    }
    fill_status(out);
    DEBUG_PRINTF("[wcfg] test: %s\n", st == WL_CONNECTED ? "connected" : "failed");
    s_op = C_IDLE;
    send(out);
    restore_radio();
}

bool wifi_cfg_busy() { return s_op != C_IDLE; }

#else

void wifi_cfg_handle(JsonDocument &) {
    ble_link_send("{\"e\":\"wcfg\",\"op\":\"status\",\"ok\":0,\"why\":\"No Wi-Fi in this build\"}");
}
void wifi_cfg_update() {}
bool wifi_cfg_busy() { return false; }

#endif
