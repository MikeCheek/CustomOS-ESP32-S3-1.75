#include "hal_fwupdate.h"
#include "config.h"

#if FEATURE_WIFI

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include "hal_wifi.h"
#include "hal_wifi_xfer.h"
#include "hal_ota.h"
#include "hal_power.h"
#include "hal_sleep.h"
#include "app_settings_state.h"
#include "diag.h"
#include "ui.h"
#include "fw_update_ca.h"

extern Screen fwup_screen;     // app_fwupdate.cpp
bool fwup_screen_is_open();

// Trailer sign_firmware.py appends to a *.signed.bin: signature(64) + magic.
static const char SIG_MAGIC[] = "AWSIG001";
static const int  SIG_TRAILER = 64 + 8;

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t STALL_TIMEOUT_MS = 20000;
static const uint32_t FIRST_AUTO_CHECK_MS = 30000;   // let boot (and the NTP sync) settle first

enum Job : uint8_t { JOB_NONE, JOB_CHECK, JOB_DOWNLOAD };

static volatile FwupState s_state = FWUP_IDLE;
static Job s_job = JOB_NONE;
static bool s_manual = false;            // the user asked: show errors, no pop-up
static bool s_prompt_pending = false;    // "update available" pop-up to show when possible
static bool s_failed_installing = false;
static uint32_t s_connect_ms = 0, s_done_ms = 0;
static uint32_t s_last_auto_ms = 0;
static bool s_auto_done_once = false;

static bool s_holding_radio = false, s_radio_was_on = false;
static volatile bool s_task_running = false;
static volatile bool s_cancel = false;

// Filled by the check, read by the download and the UI.
static char s_latest[24] = "";
static char s_asset_url[384] = "";
static bool s_asset_signed = false;
static uint32_t s_asset_size = 0;

static volatile uint32_t s_total = 0, s_written = 0;
static char s_error[64] = "";

// ---- radio ------------------------------------------------------------------------

static void take_radio() {
    if (s_holding_radio) return;
    s_radio_was_on = wifi_is_enabled();
    s_holding_radio = true;
    wifi_hold(true);
    if (wifi_is_connected()) return;
    if (!wifi_is_enabled()) wifi_enable();
    else if (!wifi_connecting()) wifi_connect_now();
}

static void release_radio() {
    if (!s_holding_radio) return;
    s_holding_radio = false;
    wifi_hold(false);   // runs a wifi_disable() someone asked for meanwhile (NTP done...)
    if (!s_radio_was_on && !wifi_xfer_active()) wifi_disable();
}

// ---- worker task (blocking network I/O) -----------------------------------------

static void set_error(const char *why) {
    snprintf(s_error, sizeof(s_error), "%s", why);
    DEBUG_PRINTF("[fwup] %s\n", why);
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void prepare_tls(NetworkClientSecure &client, HTTPClient &http) {
    client.setCACert(FWUP_ROOT_CAS);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // release assets redirect to a CDN
    http.useHTTP10(true);   // no chunked encoding: the body can be read as a plain stream
    http.setTimeout(15000);
    http.setUserAgent("AmoledSmartWatchOS/" FW_VERSION);
}

static const char *http_error(int code) {
    if (code == 403 || code == 429) return "GitHub is busy - try again later";
    if (code < 0) return "Couldn't reach GitHub";
    return nullptr;
}

static FwupState do_check() {
    NetworkClientSecure client;
    HTTPClient http;
    prepare_tls(client, http);
    if (!http.begin(client, "https://api.github.com/repos/" FW_UPDATE_REPO "/releases/latest")) {
        set_error("Couldn't reach GitHub");
        return FWUP_FAILED;
    }
    http.addHeader("Accept", "application/vnd.github+json");
    int code = http.GET();
    if (code == 404) {   // the repo has no (non-draft, non-prerelease) release yet
        http.end();
        s_latest[0] = 0;
        DEBUG_PRINTF("[fwup] no releases published\n");
        return FWUP_UP_TO_DATE;
    }
    if (code != 200) {
        char why[48];
        const char *e = http_error(code);
        if (!e) { snprintf(why, sizeof(why), "GitHub answered %d", code); e = why; }
        set_error(e);
        http.end();
        return FWUP_FAILED;
    }

    // Only the fields we need - a release's JSON (notes, uploader...) can be
    // tens of KB.
    JsonDocument filter;
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    filter["assets"][0]["size"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        set_error("Unexpected answer from GitHub");
        return FWUP_FAILED;
    }

    const char *tag = doc["tag_name"] | "";
    if (*tag == 'v' || *tag == 'V') tag++;
    snprintf(s_latest, sizeof(s_latest), "%s", tag);
    DEBUG_PRINTF("[fwup] latest release %s, installed %s\n", s_latest, diag_fw_version());
    if (!s_latest[0] || ota_compare_versions(s_latest, diag_fw_version()) <= 0) return FWUP_UP_TO_DATE;

    // The firmware asset: AmoledSmartWatchOS-x.y.z.bin, or the
    // .signed.bin made by sign_firmware.py. With signing on only the
    // signed one will do; otherwise the plain one is preferred.
    const char *plain = nullptr, *signd = nullptr;
    uint32_t plain_size = 0, signed_size = 0;
    for (JsonObject a : doc["assets"].as<JsonArray>()) {
        const char *name = a["name"] | "";
        const char *url = a["browser_download_url"] | "";
        if (strncmp(name, FW_UPDATE_ASSET_PREFIX, strlen(FW_UPDATE_ASSET_PREFIX)) != 0 || !url[0]) continue;
        if (ends_with(name, ".signed.bin")) { signd = url; signed_size = a["size"] | 0; }
        else if (ends_with(name, ".bin")) { plain = url; plain_size = a["size"] | 0; }
    }
    const char *pick = nullptr;
    if (ota_signing_required()) {
        pick = signd;
        if (!pick) { set_error(plain ? "The new release isn't signed" : "No firmware in the new release"); return FWUP_FAILED; }
    } else {
        pick = plain ? plain : signd;
        if (!pick) { set_error("No firmware in the new release"); return FWUP_FAILED; }
    }
    s_asset_signed = pick == signd;
    s_asset_size = s_asset_signed ? signed_size : plain_size;
    if (strlen(pick) >= sizeof(s_asset_url)) { set_error("Download link too long"); return FWUP_FAILED; }
    snprintf(s_asset_url, sizeof(s_asset_url), "%s", pick);
    s_total = s_asset_size;
    s_written = 0;
    return FWUP_AVAILABLE;
}

static FwupState download_fail(const char *why) {
    if (Update.isRunning()) Update.abort();
    set_error(why);
    return FWUP_FAILED;
}

static FwupState do_download() {
    NetworkClientSecure client;
    HTTPClient http;
    prepare_tls(client, http);
    if (!http.begin(client, s_asset_url)) return download_fail("Couldn't reach GitHub");
    int code = http.GET();
    if (code != 200) {
        char why[48];
        const char *e = http_error(code);
        if (!e) { snprintf(why, sizeof(why), "Download failed (%d)", code); e = why; }
        http.end();
        return download_fail(e);
    }
    int len = http.getSize();
    if (len <= 0 || (s_asset_size && (uint32_t)len != s_asset_size)) { http.end(); return download_fail("Unexpected download size"); }

    uint32_t total = (uint32_t)len;
    uint32_t image = s_asset_signed ? total - SIG_TRAILER : total;
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    if (total <= (uint32_t)SIG_TRAILER + 1024 || !slot || image > slot->size) { http.end(); return download_fail("Firmware file doesn't fit"); }
    if (!Update.begin(image, U_FLASH)) {
        char why[48];
        snprintf(why, sizeof(why), "Can't start update (%s)", Update.errorString());
        http.end();
        return download_fail(why);
    }
    s_total = total;
    s_written = 0;
    DEBUG_PRINTF("[fwup] downloading %lu bytes (%s)\n", (unsigned long)total, s_asset_signed ? "signed" : "unsigned");

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    const uint32_t BUF = 4096;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(BUF);
    if (!buf) { http.end(); mbedtls_sha256_free(&sha); return download_fail("Not enough memory"); }
    uint8_t trailer[SIG_TRAILER];
    NetworkClient *stream = http.getStreamPtr();
    uint32_t got = 0, last_data = millis();
    const char *why = nullptr;
    while (got < total) {
        if (s_cancel) { why = "Cancelled"; break; }
        int avail = stream->available();
        if (avail <= 0) {
            if (!stream->connected()) { why = "Connection lost"; break; }
            if (millis() - last_data > STALL_TIMEOUT_MS) { why = "Download timed out"; break; }
            delay(5);
            continue;
        }
        uint32_t want = total - got;
        if (want > BUF) want = BUF;
        if ((uint32_t)avail < want) want = avail;
        int n = stream->read(buf, want);
        if (n <= 0) { delay(5); continue; }
        last_data = millis();
        // Image bytes go to flash; a signed file's last 72 bytes are the trailer.
        uint32_t img_n = got < image ? (image - got < (uint32_t)n ? image - got : (uint32_t)n) : 0;
        if (img_n) {
            mbedtls_sha256_update(&sha, buf, img_n);
            if (Update.write(buf, img_n) != img_n) { why = "Flash write failed"; break; }
        }
        if ((uint32_t)n > img_n) memcpy(trailer + (got + img_n - image), buf + img_n, n - img_n);
        got += n;
        s_written = got;
    }
    http.end();
    free(buf);
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    if (why) return download_fail(why);

    if (s_asset_signed) {
        if (memcmp(trailer + 64, SIG_MAGIC, 8) != 0) return download_fail("Signature missing");
        if (!ota_signature_valid(hash, trailer)) return download_fail("Wrong signature");
    } else if (ota_signing_required()) {
        return download_fail("Update not signed");
    }
    if (!Update.end(true)) {
        char e[48];
        snprintf(e, sizeof(e), "Invalid image (%s)", Update.errorString());
        return download_fail(e);
    }
    DEBUG_PRINTF("[fwup] firmware %s installed\n", s_latest);
    return FWUP_DONE;
}

static void worker(void *arg) {
    Job job = (Job)(uintptr_t)arg;
    FwupState result = job == JOB_CHECK ? do_check() : do_download();
    s_state = result;
    __sync_synchronize();   // the loop reads the state once it sees the task gone
    s_task_running = false;
    vTaskDelete(nullptr);
}

// ---- main loop side -----------------------------------------------------------------

static void fail_now(const char *why) {
    set_error(why);
    s_failed_installing = s_job == JOB_DOWNLOAD;
    // An automatic check that fails stays quiet (it's tried again later).
    s_state = (s_job == JOB_CHECK && !s_manual) ? FWUP_IDLE : FWUP_FAILED;
    s_job = JOB_NONE;
    release_radio();
}

static void start(Job job, bool manual) {
    if (fwup_busy() || s_task_running) return;
    s_error[0] = 0;
    s_failed_installing = false;
    s_job = job;
    s_manual = manual;
    s_cancel = false;
    if (!wifi_has_credentials()) { fail_now("Set up Wi-Fi first (WiFi Setup)"); return; }
    if (wifi_xfer_active()) { fail_now("Wi-Fi is busy with a transfer"); return; }
    take_radio();
    if (!wifi_is_enabled()) { fail_now(wifi_last_error() ? wifi_last_error() : "Wi-Fi didn't start"); return; }
    s_connect_ms = millis();
    s_state = FWUP_CONNECTING;
}

void fwup_check() { start(JOB_CHECK, true); }

void fwup_install() {
    if (s_state != FWUP_AVAILABLE || !s_asset_url[0]) return;
    int bat = power_get_battery_percent();
    if (!power_is_charging() && bat >= 0 && bat < 20) {
        set_error("Charge to 20% first");
        s_failed_installing = true;
        s_state = FWUP_FAILED;
        return;
    }
    if (ota_active()) {
        set_error("A Bluetooth update is running");
        s_failed_installing = true;
        s_state = FWUP_FAILED;
        return;
    }
    s_total = s_asset_size;
    s_written = 0;
    start(JOB_DOWNLOAD, true);
}

void fwup_cancel() {
    if (s_state == FWUP_DOWNLOADING) s_cancel = true;
    else if (s_state == FWUP_CONNECTING && s_job == JOB_DOWNLOAD) fail_now("Cancelled");
}

void fwup_dismiss() {
    if (s_state == FWUP_UP_TO_DATE || s_state == FWUP_FAILED || s_state == FWUP_AVAILABLE) s_state = FWUP_IDLE;
    s_prompt_pending = false;
}

static void on_prompt(bool yes) {
    if (!yes) { fwup_dismiss(); return; }
    if (!fwup_screen_is_open()) ui_push(&fwup_screen);
    fwup_install();
}

static bool auto_check_due() {
    uint32_t now = millis();
    if (!s_auto_done_once) return now > FIRST_AUTO_CHECK_MS;
    return now - s_last_auto_ms > (uint32_t)FW_UPDATE_CHECK_HOURS * 3600UL * 1000UL;
}

void fwup_update() {
    FwupState st = s_state;

    // Automatic check: on Wi-Fi now, or Wi-Fi switched on with a saved
    // network (the radio is brought up just for this and switched off again).
    // Looked at every few seconds, not every loop (wifi_has_credentials()
    // reads NVS while no network is saved).
    static uint32_t s_eval_ms = 0;
    if ((st == FWUP_IDLE || st == FWUP_UP_TO_DATE) && !s_task_running && auto_check_due() &&
        millis() - s_eval_ms > 5000) {
        s_eval_ms = millis();
        int bat = power_get_battery_percent();
        bool battery_ok = power_is_charging() || bat < 0 || bat >= 20;
        if (battery_ok && wifi_has_credentials() && (wifi_is_connected() || g_app_settings.wifi_on) &&
            !wifi_xfer_active() && !ota_active()) {
            s_auto_done_once = true;
            s_last_auto_ms = millis();
            DEBUG_PRINTF("[fwup] automatic check\n");
            start(JOB_CHECK, fwup_screen_is_open());   // on that screen it shows the result itself
            st = s_state;
        }
    }

    if (st == FWUP_CONNECTING) {
        if (wifi_is_connected()) {
            s_task_running = true;
            s_state = s_job == JOB_CHECK ? FWUP_CHECKING : FWUP_DOWNLOADING;
            // TLS + HTTP need a deep stack, and flash writes need it in
            // internal RAM. Core 0, away from the UI loop.
            if (xTaskCreatePinnedToCore(worker, "fwup", 12 * 1024, (void *)(uintptr_t)s_job, 1, nullptr, 0) != pdPASS) {
                s_task_running = false;
                fail_now("Not enough memory");
            }
        } else if (millis() - s_connect_ms > CONNECT_TIMEOUT_MS) {
            fail_now("Couldn't connect to Wi-Fi");
        }
        return;
    }

    // Worker finished: hand the radio back and act on the result.
    if (s_job != JOB_NONE && !s_task_running) {
        Job job = s_job;
        s_job = JOB_NONE;
        release_radio();
        st = s_state;
        if (st == FWUP_FAILED) s_failed_installing = job == JOB_DOWNLOAD;
        if (st == FWUP_DONE) s_done_ms = millis();
        if (job == JOB_CHECK && !s_manual) {
            if (st == FWUP_AVAILABLE) s_prompt_pending = true;
            else if (st == FWUP_FAILED || st == FWUP_UP_TO_DATE) s_state = FWUP_IDLE;   // quiet
        }
    }

    if (s_state == FWUP_DONE) {
        if (millis() - s_done_ms > 2500) {
            DEBUG_PRINTF("[fwup] restarting into the new firmware\n");
            delay(100);
            ESP.restart();
        }
        return;
    }

    // "Update available" pop-up - once the screen is on and nothing that
    // shouldn't be interrupted (a game, a Bluetooth update...) is showing.
    if (s_prompt_pending) {
        if (s_state != FWUP_AVAILABLE) { s_prompt_pending = false; return; }
        if (fwup_screen_is_open()) { s_prompt_pending = false; return; }   // the screen shows it already
        if (!sleep_is_asleep() && !ui_confirm_active() && !ui_current_screen_suppresses_idle()) {
            s_prompt_pending = false;
            char body[96];
            snprintf(body, sizeof(body), "Firmware %s is out (you have %s). Download and install it now?",
                     s_latest, diag_fw_version());
            ui_show_confirm("Update available", body, "Update", "Later", on_prompt);
        }
    }
}

FwupState fwup_state() { return s_state; }
bool fwup_busy() {
    FwupState st = s_state;
    return st == FWUP_CONNECTING || st == FWUP_CHECKING || st == FWUP_DOWNLOADING || s_task_running;
}
bool fwup_installing() {
    FwupState st = s_state;
    return st == FWUP_DOWNLOADING || st == FWUP_DONE || (st == FWUP_CONNECTING && s_job == JOB_DOWNLOAD);
}
bool fwup_failed_installing() { return s_failed_installing; }
const char *fwup_latest_version() { return s_latest; }
const char *fwup_error() { return s_error; }
uint32_t fwup_total() { return s_total; }
uint32_t fwup_written() { return s_written; }

#else

void fwup_update() {}
void fwup_check() {}
void fwup_install() {}
void fwup_cancel() {}
void fwup_dismiss() {}
FwupState fwup_state() { return FWUP_IDLE; }
bool fwup_busy() { return false; }
bool fwup_installing() { return false; }
bool fwup_failed_installing() { return false; }
const char *fwup_latest_version() { return ""; }
const char *fwup_error() { return "Wi-Fi is off in this build"; }
uint32_t fwup_total() { return 0; }
uint32_t fwup_written() { return 0; }

#endif
