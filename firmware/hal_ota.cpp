#include "hal_ota.h"
#include "config.h"
#include "hal_ble.h"
#include "hal_power.h"
#include "hal_sleep.h"
#include "diag.h"
#include "ui.h"
#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include "ota_pubkey.h"
#include "hal_fwupdate.h"

// Protocol (characteristic 19B1000A):
//  app -> watch  0x01 size(4 LE) [md5 hex(32)]   begin
//                0x02 offset(4 LE) data            data, in order
//                0x03                              all sent
//                0x04                              abort
//                0x05 signature(64)                ECDSA P-256 r||s over SHA-256 of the
//                                                  image, before 0x03 (fw 3.0+, see ota_pubkey.h)
//  watch -> app  0x81 status                       begin result: 0 ok, 1 error,
//                                                  2 too big, 3 battery low
//                0x82 written(4 LE)                progress ack every 16 KB
//                0x84 status                       end: 0 ok (restarting), else error
//                                                  (10 = signature missing or wrong,
//                                                  11 = older than installed, signing on)
//                0x85 code                         failed while receiving
// The app keeps at most OTA_WINDOW bytes in flight beyond the last ack.

extern Screen ota_screen;

#define RING_SIZE   (64 * 1024)
#define ACK_EVERY   (16 * 1024)
#define WRITE_CHUNK 4096

static uint8_t *s_ring = nullptr;                 // PSRAM
static volatile uint32_t s_head = 0, s_tail = 0;  // free-running byte counters

static volatile OtaState s_state = OTA_IDLE;
static volatile uint32_t s_begin_size = 0;        // >0 = begin requested
static char s_begin_md5[33] = "";
static volatile bool s_end_req = false, s_abort_req = false;
static volatile uint32_t s_rx_expected = 0;       // next offset the BLE side expects
static volatile uint32_t s_last_rx_ms = 0;
static volatile uint8_t s_rx_error = 0;           // set on the BLE task, reported by the loop

static uint32_t s_total = 0, s_written = 0, s_last_ack = 0;
static uint32_t s_done_ms = 0;
static char s_error[48] = "";

// Signature check (ota_pubkey.h): SHA-256 of everything written, compared
// with the 0x05 signature before the image is made bootable.
static mbedtls_sha256_context s_sha;
static bool s_sha_on = false;
static uint8_t s_sig[64];
static volatile bool s_has_sig = false;

bool ota_signature_valid(const uint8_t hash[32], const uint8_t sig[64]) {
#if OTA_PUBKEY_SET
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, sv;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&sv);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &q, OTA_PUBKEY, sizeof(OTA_PUBKEY)) == 0 &&
              mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
              mbedtls_mpi_read_binary(&sv, sig + 32, 32) == 0 &&
              mbedtls_ecdsa_verify(&grp, hash, 32, &q, &r, &sv) == 0;
    mbedtls_mpi_free(&sv);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
#else
    (void)hash;
    (void)sig;
    return true;   // no key set: unsigned updates allowed
#endif
}

bool ota_signing_required() { return OTA_PUBKEY_SET; }

static bool signature_ok(const uint8_t hash[32]) {
    if (OTA_PUBKEY_SET && !s_has_sig) return false;
    return ota_signature_valid(hash, s_sig);
}

// Version of the incoming image, read from its "AMOLEDWATCH_FW=" marker as
// it streams by (a window carries the marker across chunk boundaries).
static const char VMARK[] = "AMOLEDWATCH_FW=";
static char s_win[64];
static int s_win_n = 0;
static char s_new_ver[24] = "";

static void scan_version(const uint8_t *d, uint32_t n) {
    if (s_new_ver[0]) return;
    const int ML = sizeof(VMARK) - 1;
    for (uint32_t i = 0; i < n; i++) {
        if (s_win_n == (int)sizeof(s_win)) { memmove(s_win, s_win + 32, 32); s_win_n = 32; }
        s_win[s_win_n++] = (char)d[i];
        // marker + at least a few version characters in the window?
        if (s_win_n < ML + 6) continue;
        int start = s_win_n - ML - 6;
        if (memcmp(s_win + start, VMARK, ML) != 0) continue;
        // read the version from the rest of this chunk (it's short)
        uint32_t j = i - 5;   // first version char in d
        int k = 0;
        while (j < n && k < (int)sizeof(s_new_ver) - 1 && d[j] >= 0x20 && d[j] < 0x7F) s_new_ver[k++] = (char)d[j++];
        s_new_ver[k] = 0;
        return;
    }
}

int ota_compare_versions(const char *a, const char *b) {
    for (int part = 0; part < 3; part++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static void sha_stop() {
    if (s_sha_on) { mbedtls_sha256_free(&s_sha); s_sha_on = false; }
}

static bool notify(uint8_t op, uint32_t v, int vlen) {
    uint8_t p[5] = { op, (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return ble_ota_notify(p, 1 + vlen);
}

extern bool ota_screen_is_open();

// ---- BLE task side: copy only -----------------------------------------------------

void ota_on_ble_write(const uint8_t *d, int len) {
    if (len < 1) return;
    s_last_rx_ms = millis();
    switch (d[0]) {
        case 0x01:
            if (len < 5) return;
            s_begin_md5[0] = 0;
            if (len >= 5 + 32) { memcpy(s_begin_md5, d + 5, 32); s_begin_md5[32] = 0; }
            s_begin_size = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
            break;
        case 0x02: {
            if (s_state != OTA_RECEIVING || len < 5 || s_rx_error) return;
            uint32_t off = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
            int n = len - 5;
            if (off != s_rx_expected) { s_rx_error = 2; return; }       // gap / reorder
            if (s_head - s_tail + n > RING_SIZE) { s_rx_error = 3; return; } // app ignored flow control
            uint32_t h = s_head;
            for (int i = 0; i < n; ) {
                uint32_t pos = (h + i) % RING_SIZE;
                int run = RING_SIZE - pos;
                if (run > n - i) run = n - i;
                memcpy(s_ring + pos, d + 5 + i, run);
                i += run;
            }
            __sync_synchronize();
            s_head = h + n;
            s_rx_expected = off + n;
            break;
        }
        case 0x05:
            if (len >= 65 && s_state == OTA_RECEIVING) { memcpy(s_sig, d + 1, 64); __sync_synchronize(); s_has_sig = true; }
            break;
        case 0x03: s_end_req = true; break;
        case 0x04: s_abort_req = true; break;
    }
}

// ---- main loop side ---------------------------------------------------------------------

static void fail(const char *why, uint8_t op, uint8_t code) {
    if (Update.isRunning()) Update.abort();
    sha_stop();
    snprintf(s_error, sizeof(s_error), "%s", why);
    s_state = OTA_FAILED;
    s_done_ms = millis();
    notify(op, code, 1);
    DEBUG_PRINTF("[ota] failed: %s\n", why);
}

static void handle_begin(uint32_t size) {
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    if (fwup_installing()) { notify(0x81, 1, 1); return; }   // already updating over Wi-Fi
    if (s_state == OTA_RECEIVING) Update.abort();
    s_state = OTA_IDLE;
    if (!slot) { notify(0x81, 1, 1); return; }
    if (size < 1024 || size > slot->size) { notify(0x81, 2, 1); return; }
    int bat = power_get_battery_percent();
    if (!power_is_charging() && bat >= 0 && bat < 20) { notify(0x81, 3, 1); return; }
    if (!s_ring) s_ring = (uint8_t *)heap_caps_malloc(RING_SIZE + WRITE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring || !Update.begin(size, U_FLASH)) {
        DEBUG_PRINTF("[ota] begin failed: %s\n", Update.errorString());
        notify(0x81, 1, 1);
        return;
    }
    if (s_begin_md5[0]) Update.setMD5(s_begin_md5);
    s_total = size;
    s_written = s_last_ack = 0;
    s_head = s_tail = 0;
    s_rx_expected = 0;
    s_rx_error = 0;
    s_end_req = s_abort_req = false;
    s_error[0] = 0;
    s_has_sig = false;
    s_win_n = 0;
    s_new_ver[0] = 0;
    sha_stop();
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    s_sha_on = true;
    s_last_rx_ms = millis();
    s_state = OTA_RECEIVING;
    DEBUG_PRINTF("[ota] receiving %lu bytes into %s\n", (unsigned long)size, slot->label);
    sleep_register_activity();
    if (!ota_screen_is_open()) ui_push(&ota_screen);
    notify(0x81, 0, 1);
}

void ota_update() {
    uint32_t b = s_begin_size;
    if (b) { s_begin_size = 0; handle_begin(b); }

    if (s_state == OTA_DONE) {
        if (millis() - s_done_ms > 2500) {
            DEBUG_PRINTF("[ota] restarting into the new firmware\n");
            delay(100);
            ESP.restart();
        }
        return;
    }
    if (s_state != OTA_RECEIVING) return;

    if (s_abort_req) { s_abort_req = false; fail("Cancelled on the phone", 0x85, 4); return; }
    if (s_rx_error) { fail(s_rx_error == 2 ? "Data out of order" : "Buffer overflow", 0x85, s_rx_error); return; }
    if (!ble_is_connected()) { fail("Phone disconnected", 0x85, 5); return; }
    if (millis() - s_last_rx_ms > 20000) { fail("Timed out", 0x85, 6); return; }

    // Write what has arrived, in sector-sized pieces (a flash erase+write
    // takes a few tens of ms - a bounded amount per loop keeps the UI alive).
    uint8_t *chunk = s_ring + RING_SIZE;   // staging area after the ring
    for (int rounds = 0; rounds < 6; rounds++) {
        uint32_t avail = s_head - s_tail;
        if (avail == 0 || (avail < WRITE_CHUNK && !s_end_req && s_written + avail < s_total)) break;
        uint32_t n = avail < WRITE_CHUNK ? avail : WRITE_CHUNK;
        for (uint32_t i = 0; i < n; ) {
            uint32_t pos = (s_tail + i) % RING_SIZE;
            uint32_t run = RING_SIZE - pos;
            if (run > n - i) run = n - i;
            memcpy(chunk + i, s_ring + pos, run);
            i += run;
        }
        __sync_synchronize();
        s_tail += n;
        if (s_sha_on) mbedtls_sha256_update(&s_sha, chunk, n);
        scan_version(chunk, n);
        if (Update.write(chunk, n) != n) {
            char why[48];
            snprintf(why, sizeof(why), "Flash write failed (%s)", Update.errorString());
            fail(why, 0x85, 7);
            return;
        }
        s_written += n;
    }
    // Progress ack. The app stops sending 32 KB past the last one, so a
    // notify that didn't go out (stack out of buffers) is retried on the
    // next pass instead of being lost.
    if ((s_written - s_last_ack >= ACK_EVERY || (s_written == s_total && s_last_ack != s_total)) &&
        notify(0x82, s_written, 4)) {
        s_last_ack = s_written;
    }

    if (s_end_req && s_head == s_tail) {
        s_end_req = false;
        if (s_written != s_total) { fail("Incomplete image", 0x84, 8); return; }
        uint8_t hash[32];
        mbedtls_sha256_finish(&s_sha, hash);
        sha_stop();
        if (!signature_ok(hash)) {
            fail(s_has_sig ? "Wrong signature" : "Update not signed", 0x84, 10);
            return;
        }
#if OTA_PUBKEY_SET
        // With signing on, an older (validly signed) build can't be sent
        // back to reopen a fixed bug. Downgrades stay possible over USB.
        if (s_new_ver[0] && ota_compare_versions(s_new_ver, diag_fw_version()) < 0) {
            fail("Older than the installed firmware", 0x84, 11);
            return;
        }
#endif
        DEBUG_PRINTF("[ota] image version %s\n", s_new_ver[0] ? s_new_ver : "?");
        if (!Update.end(true)) {
            char why[48];
            snprintf(why, sizeof(why), "Invalid image (%s)", Update.errorString());
            fail(why, 0x84, 9);
            return;
        }
        s_state = OTA_DONE;
        s_done_ms = millis();
        notify(0x84, 0, 1);
        DEBUG_PRINTF("[ota] image verified\n");
    }
}

OtaState ota_state() { return s_state; }
bool ota_active() { return s_state == OTA_RECEIVING; }
uint32_t ota_total() { return s_total; }
uint32_t ota_written() { return s_written; }
const char *ota_error() { return s_error; }

void ota_cancel() {
    if (s_state == OTA_RECEIVING) fail("Cancelled on the watch", 0x85, 4);
    else if (s_state == OTA_FAILED) s_state = OTA_IDLE;
}
