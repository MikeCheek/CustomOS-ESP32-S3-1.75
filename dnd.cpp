#include "dnd.h"
#include "config.h"
#include "app_settings_state.h"
#include "hal_nvs.h"
#include "hal_rtc.h"
#include "hal_ble.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// ---- Do Not Disturb --------------------------------------------------------------

bool dnd_manual() { return g_app_settings.dnd_on; }

bool dnd_bedtime_now() {
    if (!g_app_settings.dnd_bedtime) return false;
    WatchTime t = rtc_now();
    if (!t.valid) return false;
    int now = t.hour * 60 + t.minute;
    int a = g_app_settings.dnd_from_min, b = g_app_settings.dnd_to_min;
    if (a == b) return false;
    return a < b ? (now >= a && now < b) : (now >= a || now < b);   // window across midnight
}

bool dnd_active() { return dnd_manual() || dnd_bedtime_now(); }

void dnd_set(bool on, bool from_phone) {
    if (g_app_settings.dnd_on == on) return;
    g_app_settings.dnd_on = on;
    nvs_save_settings(g_app_settings);
    if (!from_phone && ble_is_connected()) {
        ble_link_send(on ? "{\"e\":\"dnd\",\"on\":1}" : "{\"e\":\"dnd\",\"on\":0}");
    }
}

// ---- Quick replies -----------------------------------------------------------------

static const char *const DEFAULTS[] = {
    "OK", "On my way", "Call you later", "Thanks!", "Yes", "No", "Can't talk now",
};
static const int DEFAULT_N = sizeof(DEFAULTS) / sizeof(DEFAULTS[0]);
#define QR_MAX 10
#define QR_LEN 40
static char s_qr[QR_MAX][QR_LEN];
static int s_qr_n = -1;    // -1 = not loaded / use defaults

void quick_replies_load() {
    Preferences p;
    s_qr_n = -1;
    if (!p.begin("qr", true)) return;
    int n = p.getUChar("n", 0);
    if (n > QR_MAX) n = QR_MAX;
    for (int i = 0; i < n; i++) {
        char k[4];
        snprintf(k, sizeof(k), "%d", i);
        p.getString(k, s_qr[i], QR_LEN);
    }
    p.end();
    if (n > 0) s_qr_n = n;
}

void quick_replies_set(const char *const *items, int n) {
    if (n > QR_MAX) n = QR_MAX;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (!items[i] || !items[i][0]) continue;
        strncpy(s_qr[kept], items[i], QR_LEN - 1);
        s_qr[kept][QR_LEN - 1] = 0;
        ble_fold_utf8(s_qr[kept]);
        kept++;
    }
    s_qr_n = kept > 0 ? kept : -1;
    Preferences p;
    if (!p.begin("qr", false)) return;
    p.clear();
    p.putUChar("n", (uint8_t)kept);
    for (int i = 0; i < kept; i++) {
        char k[4];
        snprintf(k, sizeof(k), "%d", i);
        p.putString(k, s_qr[i]);
    }
    p.end();
    DEBUG_PRINTF("[qr] %d quick replies from the phone\n", kept);
}

int quick_reply_count() { return 1 + (s_qr_n > 0 ? s_qr_n : DEFAULT_N); }

const char *quick_reply(int i) {
    if (i <= 0) return "Dictate";
    i--;
    if (s_qr_n > 0) return i < s_qr_n ? s_qr[i] : "";
    return i < DEFAULT_N ? DEFAULTS[i] : "";
}
