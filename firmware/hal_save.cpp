#include "hal_save.h"
#include <Preferences.h>

static Preferences s_prefs;

bool game_save_blob(const char *key, const void *data, size_t len) {
    if (!s_prefs.begin("gamesave", false)) return false;
    size_t written = s_prefs.putBytes(key, data, len);
    s_prefs.end();
    return written == len;
}

bool game_load_blob(const char *key, void *data, size_t len) {
    if (!s_prefs.begin("gamesave", true)) return false;
    size_t stored = s_prefs.getBytesLength(key);
    if (stored != len) { s_prefs.end(); return false; }
    size_t got = s_prefs.getBytes(key, data, len);
    s_prefs.end();
    return got == len;
}
