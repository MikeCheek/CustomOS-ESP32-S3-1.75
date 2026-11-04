// Files that outlive a restart - see store.h.
#include "store.h"
#include "config.h"
#include <Arduino.h>
#include <FS.h>
#include <string.h>
#if FEATURE_SD_CARD
#include "hal_sd.h"
#include <SD.h>
#endif
#include <FFat.h>

static fs::FS *s_fs = nullptr;
static const char *s_where = "nowhere";
static const char *DIR_DATA = "/data";

void store_begin() {
#if FEATURE_SD_CARD
    if (sd_is_mounted()) {
        s_fs = &SD;
        s_where = "SD card";
    }
#endif
    if (!s_fs && FFat.begin(true)) {   // formats the partition the first time
        s_fs = &FFat;
        s_where = "watch";
    }
    if (s_fs) s_fs->mkdir(DIR_DATA);
    DEBUG_PRINTF("[store] files on: %s\n", s_where);
}

bool store_ok() { return s_fs != nullptr; }
const char *store_where() { return s_where; }

static void path(char *b, size_t n, const char *name, const char *ext = "") {
    snprintf(b, n, "%s/%s%s", DIR_DATA, name, ext);
}

int store_read(const char *name, void *buf, size_t max) {
    if (!s_fs) return -1;
    char p[64];
    path(p, sizeof(p), name);
    File f = s_fs->open(p, FILE_READ);
    if (!f || f.isDirectory()) return -1;
    int n = (int)f.read((uint8_t *)buf, max);
    f.close();
    return n;
}

int store_size(const char *name) {
    if (!s_fs) return -1;
    char p[64];
    path(p, sizeof(p), name);
    File f = s_fs->open(p, FILE_READ);
    if (!f || f.isDirectory()) return -1;
    int n = (int)f.size();
    f.close();
    return n;
}

bool store_exists(const char *name) {
    if (!s_fs) return false;
    char p[64];
    path(p, sizeof(p), name);
    return s_fs->exists(p);
}

bool store_write(const char *name, const void *buf, size_t len) {
    if (!s_fs) return false;
    char p[64], t[68];
    path(p, sizeof(p), name);
    path(t, sizeof(t), name, ".tmp");
    // names with a folder ("img/1a2b"): make it
    const char *sl = strrchr(name, '/');
    if (sl) {
        char d[64];
        snprintf(d, sizeof(d), "%s/%.*s", DIR_DATA, (int)(sl - name), name);
        s_fs->mkdir(d);
    }
    File f = s_fs->open(t, FILE_WRITE);
    if (!f) return false;
    bool ok = f.write((const uint8_t *)buf, len) == len;
    f.close();
    if (!ok) { s_fs->remove(t); return false; }
    s_fs->remove(p);
    return s_fs->rename(t, p);
}

bool store_remove(const char *name) {
    if (!s_fs) return false;
    char p[64];
    path(p, sizeof(p), name);
    return s_fs->remove(p);
}
