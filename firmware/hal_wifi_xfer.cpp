#include "hal_wifi_xfer.h"
#include "config.h"
#include "hal_ble.h"

#if FEATURE_WIFI && FEATURE_SD_CARD

#include "hal_wifi.h"
#include "hal_sd.h"
#include "hal_ble.h"
#include "hal_usb.h"
#include "ui.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>

#define XFER_PORT        8080
#define XFER_IDLE_MS     120000
#define XFER_CONNECT_MS  20000
#define XFER_STACK       12288
// WiFi needs this much internal RAM on top of what's in use (BLE included).
#define XFER_MIN_INTERNAL (64 * 1024)

enum XState { X_OFF, X_CONNECTING, X_READY, X_STOPPING };

static volatile XState s_state = X_OFF;
static volatile int s_req = 0;            // last request wins: 1 = on, 2 = off
static bool s_radio_was_on = false;
static uint32_t s_t0 = 0;
static volatile uint32_t s_last_req_ms = 0;
static volatile int s_busy = 0;
static volatile bool s_changed = false;
static char s_key[17] = "";

static WebServer *s_srv = nullptr;
static TaskHandle_t s_task = nullptr;
static volatile bool s_task_run = false;
static volatile bool s_task_done = false;      // the task left its loop and suspended itself
static char s_open_path[96] = "";               // file the task has open (delete guard)
static const char *s_stop_st = nullptr, *s_stop_why = nullptr;
static StaticTask_t s_task_tcb;
static uint8_t *s_task_stack = nullptr;   // PSRAM

// ---- link replies --------------------------------------------------------------

static void reply(const char *st, const char *why = nullptr) {
    char msg[200];
    if (!strcmp(st, "ready")) {
        snprintf(msg, sizeof(msg), "{\"e\":\"wifi\",\"st\":\"ready\",\"ip\":\"%s\",\"port\":%d,\"k\":\"%s\"}",
                 WiFi.localIP().toString().c_str(), XFER_PORT, s_key);
    } else if (why) {
        snprintf(msg, sizeof(msg), "{\"e\":\"wifi\",\"st\":\"%s\",\"why\":\"%s\"}", st, why);
    } else {
        snprintf(msg, sizeof(msg), "{\"e\":\"wifi\",\"st\":\"%s\"}", st);
    }
    ble_link_send(msg);
}

// ---- HTTP handlers (server task) ------------------------------------------------------

// Key and name come as headers (X-Key / X-Name) - the body-upload path of
// WebServer doesn't parse the query string - or as ?k= / ?n=.
static String req_key() { String k = s_srv->header("X-Key"); return k.length() ? k : s_srv->arg("k"); }
static String req_name() { String n = s_srv->header("X-Name"); return n.length() ? n : s_srv->arg("n"); }

static bool authorized() {
    s_last_req_ms = millis();
    if (usb_msc_host_active()) { s_srv->send(503, "text/plain", "SD card in use over USB"); return false; }
    if (req_key() != s_key) {
        s_srv->send(403, "text/plain", "forbidden");
        return false;
    }
    return true;
}

static bool is_audio(const char *n) {
    int l = strlen(n);
    return l > 4 && (!strcasecmp(n + l - 4, ".wav") || !strcasecmp(n + l - 4, ".mp3"));
}

static void h_list() {
    s_busy++;
    if (!authorized()) { s_busy--; return; }
    String json = "[";
    File root = SD.open("/Recordings");
    bool first = true;
    if (root && root.isDirectory()) {
        for (File e = root.openNextFile(); e; e = root.openNextFile()) {
            const char *n = e.name();
            if (!e.isDirectory() && n[0] != '_' && n[0] != '.' && is_audio(n)) {
                if (!first) json += ",";
                first = false;
                json += "{\"name\":\"";
                json += n;
                json += "\",\"size\":";
                json += String((unsigned long)e.size());
                json += "}";
            }
            e.close();
        }
        root.close();
    }
    json += "]";
    s_srv->send(200, "application/json", json);
    s_busy--;
}

static void h_file() {
    s_busy++;
    if (!authorized()) { s_busy--; return; }
    String n = req_name();
    if (!sd_is_safe_filename(n.c_str())) { s_srv->send(400, "text/plain", "bad name"); s_busy--; return; }
    String path = String("/Recordings/") + n;
    File f = SD.open(path, FILE_READ);
    if (!f) {
        s_srv->send(404, "text/plain", "not found");
    } else {
        snprintf(s_open_path, sizeof(s_open_path), "%s", path.c_str());
        size_t total = f.size();
        s_srv->setContentLength(total);
        s_srv->send(200, "application/octet-stream", "");
        // Our own loop instead of streamFile(): stops promptly when the
        // transfer is being shut down or the phone goes away.
        NetworkClient client = s_srv->client();
        static uint8_t *buf = nullptr;   // PSRAM, kept for the next file
        if (!buf) buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!buf) { f.close(); s_open_path[0] = 0; s_busy--; return; }
        size_t sent = 0;
        while (sent < total && s_task_run && client.connected()) {
            int r = f.read(buf, 4096);
            if (r <= 0) break;
            if (client.write(buf, r) != (size_t)r) break;
            sent += r;
            s_last_req_ms = millis();
        }
        f.close();
        s_open_path[0] = 0;
    }
    s_last_req_ms = millis();
    s_busy--;
}

static File s_up;
static String s_up_tmp, s_up_final;
static bool s_up_ok = false;
static bool s_up_generic = false;          // /put: any file, into the card's root
static volatile int s_files_received = 0;  // /put uploads completed (toast)

static void h_upload_body() {
    HTTPRaw &raw = s_srv->raw();
    if (raw.status == RAW_START) {
        s_up_ok = false;
        s_last_req_ms = millis();
        if (req_key() != s_key || usb_msc_host_active()) return;
        String n = req_name();
        s_up_generic = s_srv->uri() == "/put";
        if (!sd_is_safe_filename(n.c_str()) || n[0] == '.' || n.length() > 60) return;
        if (!s_up_generic && (n[0] == '_' || !is_audio(n.c_str()))) return;
        s_busy++;
        if (s_up_generic) {
            // Same place and rule as a Bluetooth file transfer (card root,
            // replaces a file of the same name) - just any size.
            s_up_final = String("/") + n;
            s_up_tmp = "/.up_" + String(esp_random() & 0xFFFF, HEX);
        } else {
            SD.mkdir("/Recordings");
            // Never overwrite: name.wav, name_2.wav, ...
            String base = n.substring(0, n.length() - 4), ext = n.substring(n.length() - 4);
            s_up_final = String("/Recordings/") + n;
            for (int i = 2; SD.exists(s_up_final) && i < 100; i++) s_up_final = "/Recordings/" + base + "_" + i + ext;
            s_up_tmp = "/Recordings/.up_" + String(esp_random() & 0xFFFF, HEX);
        }
        snprintf(s_open_path, sizeof(s_open_path), "%s", s_up_final.c_str());
        s_up = SD.open(s_up_tmp, FILE_WRITE);
        s_up_ok = (bool)s_up;
        if (!s_up_ok) s_busy--;
    } else if (raw.status == RAW_WRITE) {
        if (s_up_ok && s_up.write(raw.buf, raw.currentSize) != raw.currentSize) s_up_ok = false;
        s_last_req_ms = millis();
    } else if (raw.status == RAW_END || raw.status == RAW_ABORTED) {
        if (!s_up) return;
        s_up.close();
        s_open_path[0] = 0;
        if (raw.status == RAW_END && s_up_ok && s_up_generic && SD.exists(s_up_final)) SD.remove(s_up_final);
        if (raw.status == RAW_END && s_up_ok && SD.rename(s_up_tmp, s_up_final)) {
            if (s_up_generic) s_files_received++;
            else s_changed = true;
        } else {
            SD.remove(s_up_tmp);
            s_up_ok = false;
        }
        s_busy--;
    }
}

static void h_upload_done() {
    if (req_key() != s_key) { s_srv->send(403, "text/plain", "forbidden"); return; }
    if (!s_up_ok) { s_srv->send(500, "text/plain", "upload failed"); return; }
    String name = s_up_final.substring(s_up_final.lastIndexOf('/') + 1);
    s_srv->send(200, "application/json", String("{\"name\":\"") + name + "\"}");
}

static void server_task(void *) {
    s_srv = new WebServer(XFER_PORT);
    const char *hdrs[] = { "X-Key", "X-Name" };
    s_srv->collectHeaders(hdrs, 2);
    s_srv->on("/list", HTTP_GET, h_list);
    s_srv->on("/f", HTTP_GET, h_file);
    s_srv->on("/up", HTTP_POST, h_upload_done, h_upload_body);
    s_srv->on("/put", HTTP_POST, h_upload_done, h_upload_body);
    s_srv->onNotFound([]() { s_srv->send(404, "text/plain", "not found"); });
    s_srv->begin();
    while (s_task_run) {
        s_srv->handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    s_srv->stop();
    delete s_srv;
    s_srv = nullptr;
    // The main loop deletes this task (its stack and TCB are static and
    // get reused by the next start - they must not be while it exists).
    s_task_done = true;
    vTaskSuspend(nullptr);
}

// ---- state machine (main loop) -----------------------------------------------------------

// Begins shutting down; finish_stop() completes it once the task is gone.
static void stop(const char *st, const char *why) {
    s_stop_st = st;
    s_stop_why = why;
    s_task_run = false;
    s_state = X_STOPPING;
    s_t0 = millis();
}

static void finish_stop() {
    if (s_task) {
        if (!s_task_done) {
            if (millis() - s_t0 < 15000) return;    // a request is still finishing
            DEBUG_PRINTF("[wifi-xfer] server task stuck - deleting it\n");
        }
        vTaskDelete(s_task);
        s_task = nullptr;
        s_task_done = false;
        s_open_path[0] = 0;
    }
    s_state = X_OFF;                 // before wifi_disable(): it refuses while active
    if (!s_radio_was_on) wifi_disable();
    if (s_stop_st) reply(s_stop_st, s_stop_why);
    DEBUG_PRINTF("[wifi-xfer] stopped%s%s\n", s_stop_why ? ": " : "", s_stop_why ? s_stop_why : "");
}

void wifi_xfer_request(bool on) {
    s_req = on ? 1 : 2;
}

void wifi_xfer_update() {
    if (s_state == X_STOPPING) { finish_stop(); return; }   // requests wait until it's done
    int req = __atomic_exchange_n(&s_req, 0, __ATOMIC_SEQ_CST);
    if (req == 2 && s_state != X_OFF) { stop("off", nullptr); return; }
    if (req == 1) {
        if (s_state == X_READY) { s_last_req_ms = millis(); reply("ready"); }
        else if (s_state == X_OFF) {
            if (!sd_is_mounted()) { reply("err", "No SD card in the watch"); return; }
            if (usb_msc_host_active()) { reply("err", "The SD card is in use over USB"); return; }
            if (!wifi_has_credentials()) { reply("err", "Set up Wi-Fi on the watch first (Settings > WiFi Setup)"); return; }
            s_radio_was_on = wifi_is_enabled();
            if (!s_radio_was_on && (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < XFER_MIN_INTERNAL ||
                                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16 * 1024)) {
                reply("err", "Not enough memory for Wi-Fi next to Bluetooth");
                return;
            }
            if (!s_radio_was_on) wifi_enable();
            s_state = X_CONNECTING;
            s_t0 = millis();
            DEBUG_PRINTF("[wifi-xfer] joining Wi-Fi\n");
        }
    }

    if (s_state == X_CONNECTING) {
        if (wifi_is_connected()) {
            if (!s_task_stack) s_task_stack = (uint8_t *)heap_caps_malloc(XFER_STACK, MALLOC_CAP_SPIRAM);
            snprintf(s_key, sizeof(s_key), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
            s_task_run = true;
            s_task_done = false;
            s_task = s_task_stack ? xTaskCreateStaticPinnedToCore(server_task, "wifi_xfer", XFER_STACK, nullptr, 2,
                                                                  s_task_stack, &s_task_tcb, 0) : nullptr;
            if (!s_task) { stop("err", "Couldn't start the transfer server"); return; }
            s_state = X_READY;
            s_last_req_ms = millis();
            reply("ready");
            ui_show_toast("Wi-Fi transfer on", 1500);
            DEBUG_PRINTF("[wifi-xfer] serving on %s:%d\n", WiFi.localIP().toString().c_str(), XFER_PORT);
        } else if (millis() - s_t0 > XFER_CONNECT_MS) {
            stop("err", "Couldn't join the watch's Wi-Fi network");
        }
    } else if (s_state == X_READY) {
        uint32_t last = s_last_req_ms;                      // the task updates it
        int32_t idle = (int32_t)(millis() - last);
        if (!wifi_is_connected()) stop("err", "Wi-Fi dropped");
        else if (!s_busy && idle > (int32_t)XFER_IDLE_MS) stop("off", nullptr);
        else if (!ble_is_connected() && !s_busy && idle > 15000) stop(nullptr, nullptr);
    }
}

bool wifi_xfer_active() { return s_state != X_OFF; }

bool wifi_xfer_file_busy(const char *path) {
    return s_state != X_OFF && s_open_path[0] && !strcmp(path, s_open_path);
}

int wifi_xfer_take_files_received() {
    return __atomic_exchange_n(&s_files_received, 0, __ATOMIC_SEQ_CST);
}

bool wifi_xfer_take_changes() {
    if (!s_changed) return false;
    s_changed = false;
    return true;
}

#else

void wifi_xfer_request(bool on) {
    if (on) ble_link_send("{\"e\":\"wifi\",\"st\":\"err\",\"why\":\"Wi-Fi not available\"}");
}
void wifi_xfer_update() {}
bool wifi_xfer_active() { return false; }
bool wifi_xfer_take_changes() { return false; }
bool wifi_xfer_file_busy(const char *) { return false; }
int wifi_xfer_take_files_received() { return 0; }

#endif
