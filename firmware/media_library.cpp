#include "media_library.h"
#include "hal_sd.h"
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>

#define PS_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void MediaList::clear() {
    free(off); free(tag); free(pool);
    off = nullptr; tag = nullptr; pool = nullptr;
    n = cap = 0; pool_len = pool_cap = 0;
}

bool MediaList::add(const char *path, uint8_t t) {
    size_t L = strlen(path) + 1;
    if (pool_len + L > pool_cap) {
        uint32_t nc = pool_cap ? pool_cap * 2 : 16384;
        while (nc < pool_len + L) nc *= 2;
        char *np = (char *)PS_REALLOC(pool, nc);
        if (!np) return false;
        pool = np; pool_cap = nc;
    }
    if (n == cap) {
        int nc = cap ? cap * 2 : 256;
        uint32_t *no = (uint32_t *)PS_REALLOC(off, (size_t)nc * 4);
        if (!no) return false;
        off = no;
        uint8_t *nt = (uint8_t *)PS_REALLOC(tag, nc);
        if (!nt) return false;
        tag = nt;
        cap = nc;
    }
    memcpy(pool + pool_len, path, L);
    off[n] = pool_len;
    tag[n] = t;
    pool_len += L;
    n++;
    return true;
}

const char *MediaList::path(int i) const { return (i >= 0 && i < n) ? pool + off[i] : ""; }
const char *MediaList::name(int i) const { const char *p = path(i), *s = strrchr(p, '/'); return s ? s + 1 : p; }

void MediaList::reverse() {
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        uint32_t t = off[i]; off[i] = off[j]; off[j] = t;
        uint8_t v = tag[i]; tag[i] = tag[j]; tag[j] = v;
    }
}

static const MediaList *s_sort_list;
static int cmp_names(const void *a, const void *b) {
    const char *x = s_sort_list->name(*(const int *)a), *y = s_sort_list->name(*(const int *)b);
    return strcasecmp(x, y);
}

void MediaList::sort_by_name() {
    if (n < 2) return;
    int *idx = (int *)malloc((size_t)n * sizeof(int));
    uint32_t *no = (uint32_t *)malloc((size_t)n * 4);
    uint8_t *nt = (uint8_t *)malloc(n);
    if (!idx || !no || !nt) { free(idx); free(no); free(nt); return; }
    for (int i = 0; i < n; i++) idx[i] = i;
    s_sort_list = this;
    qsort(idx, n, sizeof(int), cmp_names);
    for (int i = 0; i < n; i++) { no[i] = off[idx[i]]; nt[i] = tag[idx[i]]; }
    memcpy(off, no, (size_t)n * 4);
    memcpy(tag, nt, n);
    free(idx); free(no); free(nt);
}

// ---- scanner --------------------------------------------------------------------

static bool skip_dir(const char *name) {
    return name[0] == '.' || !strcasecmp(name, "System Volume Information") ||
           !strcasecmp(name, "$RECYCLE.BIN") || !strcasecmp(name, "LOST.DIR") ||
           !strcasecmp(name, "Android");
}

void MediaScanner::cancel() {
    for (int i = 0; i < MAX_DEPTH; i++) {
        if (dirs_[i]) { File *f = (File *)dirs_[i]; f->close(); delete f; dirs_[i] = nullptr; }
    }
    depth_ = -1;
    active_ = false;
}

void MediaScanner::begin(MediaList *list, MediaFilter filter, int max_items) {
    cancel();
    list_ = list; filter_ = filter; max_ = max_items;
    list_->clear();
    if (!sd_is_mounted()) return;
    File root = SD.open("/");
    if (!root) return;
    dirs_[0] = new File(root);
    path_[0][0] = 0;
    depth_ = 0;
    active_ = true;
}

bool MediaScanner::step(uint32_t budget_ms) {
    uint32_t t0 = millis();
    while (active_ && millis() - t0 < budget_ms) {
        if (depth_ < 0) { active_ = false; break; }
        File *dir = (File *)dirs_[depth_];
        File e = dir->openNextFile();
        if (!e) {
            dir->close(); delete dir; dirs_[depth_] = nullptr;
            depth_--;
            if (depth_ < 0) active_ = false;
            continue;
        }
        const char *nm = e.name();
        const char *base = strrchr(nm, '/'); base = base ? base + 1 : nm;
        char full[160];
        snprintf(full, sizeof(full), "%s/%s", path_[depth_], base);
        if (e.isDirectory()) {
            if (!skip_dir(base) && depth_ + 1 < MAX_DEPTH) {
                File d = SD.open(full);
                if (d) {
                    depth_++;
                    dirs_[depth_] = new File(d);
                    strncpy(path_[depth_], full, sizeof(path_[0]) - 1);
                    path_[depth_][sizeof(path_[0]) - 1] = 0;
                }
            }
        } else if (base[0] != '.' && list_->n < max_) {
            int t = filter_ ? filter_(base) : 0;
            if (t >= 0) list_->add(full, (uint8_t)t);
        }
        e.close();
    }
    return !active_;
}

bool media_is_audio(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav"));
}
