/*
 * media_library.h
 * Whole-card media scanning shared by Gallery and Music: a list of file
 * paths (one PSRAM string pool) and an incremental scanner that walks the
 * SD card a few milliseconds per frame, so the UI stays live while it runs.
 */
#pragma once
#include <stdint.h>

struct MediaList {
    uint32_t *off = nullptr;     // offsets into pool
    uint8_t  *tag = nullptr;     // caller-defined per item (Gallery: 1 = video)
    int n = 0, cap = 0;
    char *pool = nullptr;
    uint32_t pool_len = 0, pool_cap = 0;

    void clear();
    bool add(const char *path, uint8_t tag);
    const char *path(int i) const;
    const char *name(int i) const;      // file name part
    void reverse();
    void sort_by_name();                // case-insensitive, by file name
};

// Returns the tag to store, or -1 to skip the file.
typedef int (*MediaFilter)(const char *file_name);

class MediaScanner {
public:
    void begin(MediaList *list, MediaFilter filter, int max_items);
    // Scans for up to budget_ms; returns true once the whole card is done.
    bool step(uint32_t budget_ms);
    bool scanning() const { return active_; }
    void cancel();
private:
    static const int MAX_DEPTH = 6;
    MediaList *list_ = nullptr;
    MediaFilter filter_ = nullptr;
    int max_ = 0;
    int depth_ = -1;
    bool active_ = false;
    void *dirs_[MAX_DEPTH] = {};          // File* (kept opaque to avoid SD.h here)
    char path_[MAX_DEPTH][160] = {};
};

bool media_is_audio(const char *name);      // .mp3 .wav
