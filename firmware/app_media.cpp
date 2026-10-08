/*
 * app_media.cpp
 * SD card media browser with folder navigation, WAV/MP3 audio player
 * and text viewer. Images open in the Gallery viewer (app_gallery.cpp),
 * videos in the video player (app_video.cpp).
 */

#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "fx3d.h"
#include "ui_font.h"
#include "apps.h"
#include "app_media.h"
#include "hal_audio.h"
#include "hal_touch.h"
#include "hal_sd.h"
#include "hal_nvs.h"
#include "app_settings_state.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#if FEATURE_SD_CARD
#include <SD.h>
#endif
#include "app_gallery.h"
#include "app_video.h"
#include "media_decode.h"
#include "media_library.h"

// ---- Forward declarations for sub-screens ----------------------------------
extern Screen media_audio_screen;
extern Screen media_text_screen;
void media_navigate_to(const char *path);

// ---- Shared state for sub-screen navigation (used by browser) --------------
#define BROWSER_MAX_PATH    128
#define BROWSER_MAX_NAME    48
static char s_aud_path[BROWSER_MAX_PATH];
static char s_aud_name[BROWSER_MAX_NAME];
static char s_txt_path[BROWSER_MAX_PATH];

// ---- Helpers ---------------------------------------------------------------

static void draw_back(Arduino_GFX *g) {
    g->fillRoundRect(10, 10, 44, 44, 8, COLOR_PANEL);
    g->setTextSize(3);
    g->setTextColor(COLOR_TEXT);
    ui_print(20, 18, 3, COLOR_TEXT, "<");
}

static void draw_top_title(Arduino_GFX *g, const char *title) {
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    int tw = ui_text_width(title, 2);
    int cx = LCD_WIDTH / 2;
    ui_print(cx - tw / 2, 22, 2, COLOR_TEXT_DIM, title);
}

// ---- File type classification -----------------------------------------------

enum FileType : uint8_t {
    FTYPE_UNKNOWN = 0,
    FTYPE_FOLDER,
    FTYPE_IMAGE,
    FTYPE_AUDIO,
    FTYPE_TEXT,
    FTYPE_VIDEO,
};

static bool ends_with_ci(const char *str, const char *suffix) {
    int sl = (int)strlen(str);
    int fl = (int)strlen(suffix);
    if (fl > sl) return false;
    for (int i = 0; i < fl; i++) {
        char a = str[sl - fl + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static FileType classify_file(const char *name) {
    if (media_is_image(name))          // .jpg .jpeg .png .bmp
        return FTYPE_IMAGE;
    if (ends_with_ci(name, ".wav"))
        return FTYPE_AUDIO;
    if (ends_with_ci(name, ".mp3") || ends_with_ci(name, ".ogg") ||
        ends_with_ci(name, ".flac") || ends_with_ci(name, ".aac"))
        return FTYPE_AUDIO;  // classified as audio but unsupported format
    if (ends_with_ci(name, ".txt") || ends_with_ci(name, ".json") ||
        ends_with_ci(name, ".csv") || ends_with_ci(name, ".log") ||
        ends_with_ci(name, ".ini") || ends_with_ci(name, ".cfg") ||
        ends_with_ci(name, ".md") || ends_with_ci(name, ".c") ||
        ends_with_ci(name, ".h") || ends_with_ci(name, ".cpp") ||
        ends_with_ci(name, ".ino") || ends_with_ci(name, ".py") ||
        ends_with_ci(name, ".js") || ends_with_ci(name, ".html") ||
        ends_with_ci(name, ".xml") || ends_with_ci(name, ".yml") ||
        ends_with_ci(name, ".yaml") || ends_with_ci(name, ".sh"))
        return FTYPE_TEXT;
    if (media_is_video(name) || ends_with_ci(name, ".mkv") || ends_with_ci(name, ".webm"))
        return FTYPE_VIDEO;            // the player explains unsupported formats
    return FTYPE_UNKNOWN;
}

// ---- Tile icons ------------------------------------------------------------

static void draw_icon_folder(Arduino_GFX *g, int cx, int cy) {
    g->fillRoundRect(cx - 14, cy - 4, 28, 18, 3, COLOR_ACCENT3);
    g->fillRoundRect(cx - 14, cy - 10, 14, 8, 3, COLOR_ACCENT3);
    g->fillRoundRect(cx - 10, cy, 20, 10, 2, COLOR_BG);
}

static void draw_icon_image(Arduino_GFX *g, int cx, int cy) {
    g->fillRoundRect(cx - 12, cy - 10, 24, 22, 4, COLOR565(0x33, 0x99, 0xFF));
    g->fillCircle(cx - 4, cy - 3, 3, COLOR_ACCENT3);
    g->fillTriangle(cx - 10, cy + 8, cx, cy - 2, cx + 10, cy + 8, COLOR_GOOD);
}

static void draw_icon_audio(Arduino_GFX *g, int cx, int cy) {
    g->fillCircle(cx - 4, cy + 6, 5, COLOR_GOOD);
    g->fillRoundRect(cx + 2, cy - 10, 3, 20, 1, COLOR_GOOD);
    g->fillTriangle(cx + 5, cy - 10, cx + 14, cy - 6, cx + 5, cy - 3, COLOR_GOOD);
}

static void draw_icon_text(Arduino_GFX *g, int cx, int cy) {
    g->fillRoundRect(cx - 10, cy - 10, 20, 22, 3, COLOR_TEXT_DIM);
    g->drawFastHLine(cx - 6, cy - 4, 12, COLOR_BG);
    g->drawFastHLine(cx - 6, cy, 10, COLOR_BG);
    g->drawFastHLine(cx - 6, cy + 4, 12, COLOR_BG);
}

static void draw_icon_video(Arduino_GFX *g, int cx, int cy) {
    g->fillRoundRect(cx - 12, cy - 9, 24, 18, 4, COLOR_BAD);
    g->fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 7, cy, COLOR_TEXT);
}

static void draw_icon_unknown(Arduino_GFX *g, int cx, int cy) {
    g->fillRoundRect(cx - 10, cy - 10, 20, 20, 4, COLOR_PANEL);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    ui_print(cx - 5, cy - 3, 2, COLOR_TEXT_DIM, "?");
}

static void draw_type_icon(Arduino_GFX *g, int cx, int cy, FileType t) {
    switch (t) {
        case FTYPE_FOLDER:  draw_icon_folder(g, cx, cy); break;
        case FTYPE_IMAGE:   draw_icon_image(g, cx, cy); break;
        case FTYPE_AUDIO:   draw_icon_audio(g, cx, cy); break;
        case FTYPE_TEXT:    draw_icon_text(g, cx, cy); break;
        case FTYPE_VIDEO:   draw_icon_video(g, cx, cy); break;
        default:            draw_icon_unknown(g, cx, cy); break;
    }
}

// =========================================================================
//  File Browser
// =========================================================================

#define BROWSER_MAX_ENTRIES 30

struct BrowserEntry {
    char name[BROWSER_MAX_NAME];
    FileType type;
    uint32_t size;
};

static BrowserEntry s_br_entries[BROWSER_MAX_ENTRIES];
static int          s_br_count = 0;
static int          s_br_scroll = 0;
static bool         s_br_dragging = false;
static int          s_br_drag_y = 0;
static int          s_br_drag_scroll = 0;

// Path stack for back navigation
#define BR_MAX_DEPTH 8
static char s_br_path_stack[BR_MAX_DEPTH][BROWSER_MAX_PATH];
static int  s_br_path_depth = 0;
static char s_br_cur_path[BROWSER_MAX_PATH] = "/";

// Tile geometry - single column, sized to stay within the round
// display's actual visible circle throughout the whole scrollable
// range. The old 2-column grid (206px tiles) put the right column's
// top-right corner outside the circle for any row near the top of the
// viewport - the round display's chord width there is only ~154px on
// each side of center, well short of what a 2-column layout needed.
// 300px wide, and keeping content within y=95..405, is the same
// safe-bounds pair already verified for the Breakout play field.
static const int BR_COLS = 1;
static const int BR_TILE_W = 300;
static const int BR_TILE_H = 72;
static const int BR_GAP = 10;
static const int BR_GRID_LEFT = (LCD_WIDTH - BR_TILE_W) / 2;
static const int BR_GRID_TOP = 95;
static const int BR_VIEWPORT_BOTTOM = 405;
static const int BR_VISIBLE_H = BR_VIEWPORT_BOTTOM - BR_GRID_TOP;

static int br_rows() { return (s_br_count + BR_COLS - 1) / BR_COLS; }
static int br_content_h() { return br_rows() * (BR_TILE_H + BR_GAP) - BR_GAP; }
static int br_max_scroll() {
    int m = br_content_h() - BR_VISIBLE_H;
    return m > 0 ? m : 0;
}

static void br_load_dir(const char *path) {
    s_br_count = 0;
    s_br_scroll = 0;
#if FEATURE_SD_CARD
    if (!sd_is_mounted()) return;

    // path is always like "/" or "/music" — open directly
    File root = SD.open(path);
    if (!root || !root.isDirectory()) {
        root.close();
        return;
    }

    // Directories first
    File entry = root.openNextFile();
    while (entry && s_br_count < BROWSER_MAX_ENTRIES) {
        if (entry.isDirectory()) {
            // entry.name() may return full path on ESP32 core 3.x — strip to basename
            const char *full = entry.name();
            const char *base = strrchr(full, '/');
            base = base ? base + 1 : full;
            strncpy(s_br_entries[s_br_count].name, base, BROWSER_MAX_NAME - 1);
            s_br_entries[s_br_count].name[BROWSER_MAX_NAME - 1] = '\0';
            s_br_entries[s_br_count].type = FTYPE_FOLDER;
            s_br_entries[s_br_count].size = 0;
            s_br_count++;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    // Reopen for files
    root = SD.open(path);
    if (!root || !root.isDirectory()) { root.close(); return; }
    entry = root.openNextFile();
    while (entry && s_br_count < BROWSER_MAX_ENTRIES) {
        if (!entry.isDirectory()) {
            const char *full = entry.name();
            const char *base = strrchr(full, '/');
            base = base ? base + 1 : full;
            strncpy(s_br_entries[s_br_count].name, base, BROWSER_MAX_NAME - 1);
            s_br_entries[s_br_count].name[BROWSER_MAX_NAME - 1] = '\0';
            s_br_entries[s_br_count].type = classify_file(base);
            s_br_entries[s_br_count].size = entry.size();
            s_br_count++;
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
#endif
}

static void br_navigate_into(int idx) {
    if (idx < 0 || idx >= s_br_count) return;
    BrowserEntry *e = &s_br_entries[idx];

    if (e->type == FTYPE_FOLDER) {
        // Save current path to stack
        if (s_br_path_depth < BR_MAX_DEPTH) {
            strncpy(s_br_path_stack[s_br_path_depth], s_br_cur_path, BROWSER_MAX_PATH - 1);
            s_br_path_depth++;
        }
        // Build new path
        // Built in a temporary: formatting s_br_cur_path into itself is undefined.
        char next[BROWSER_MAX_PATH];
        if (strcmp(s_br_cur_path, "/") == 0)
            snprintf(next, BROWSER_MAX_PATH, "/%s", e->name);
        else
            snprintf(next, BROWSER_MAX_PATH, "%s/%s", s_br_cur_path, e->name);
        strcpy(s_br_cur_path, next);

        br_load_dir(s_br_cur_path);
    } else {
        // Build full VFS path (always starts with /)
        char file_path[BROWSER_MAX_PATH];
        if (strcmp(s_br_cur_path, "/") == 0)
            snprintf(file_path, BROWSER_MAX_PATH, "/%s", e->name);
        else
            snprintf(file_path, BROWSER_MAX_PATH, "%s/%s", s_br_cur_path, e->name);

        // Open in appropriate viewer
        switch (e->type) {
            case FTYPE_IMAGE:
                media_navigate_to(file_path);
                break;
            case FTYPE_AUDIO:
                media_playlist_clear();
                strncpy(s_aud_name, e->name, BROWSER_MAX_NAME - 1);
                strncpy(s_aud_path, file_path, BROWSER_MAX_PATH - 1);
                ui_push(&media_audio_screen);
                break;
            case FTYPE_TEXT:
                strncpy(s_txt_path, file_path, BROWSER_MAX_PATH - 1);
                ui_push(&media_text_screen);
                break;
            case FTYPE_VIDEO:
                video_open(file_path);
                break;
            default:
                ui_show_toast("Unknown file type", 1500);
                break;
        }
    }
}

static bool br_go_back() {
    if (s_br_path_depth > 0) {
        s_br_path_depth--;
        strncpy(s_br_cur_path, s_br_path_stack[s_br_path_depth], BROWSER_MAX_PATH - 1);
        s_br_cur_path[BROWSER_MAX_PATH - 1] = '\0';
        br_load_dir(s_br_cur_path);
        return true;
    }
    return false;
}

static void browser_create() {
    s_br_scroll = 0;
    s_br_dragging = false;
    br_load_dir(s_br_cur_path);
}

static void browser_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    draw_back(g);

    // Title: show current directory name
    const char *dirname = strrchr(s_br_cur_path, '/');
    dirname = dirname ? dirname + 1 : s_br_cur_path;
    if (dirname[0] == '\0') dirname = "Media";
    draw_top_title(g, dirname);

    if (s_br_count == 0) {
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "Empty folder", 2);
        return;
    }

    // Draw tile grid
    for (int i = 0; i < s_br_count; i++) {
        int col = i % BR_COLS;
        int row = i / BR_COLS;
        int x = BR_GRID_LEFT + col * (BR_TILE_W + BR_GAP);
        int y = BR_GRID_TOP + row * (BR_TILE_H + BR_GAP) - s_br_scroll;

        // Cull off-screen tiles
        if (y + BR_TILE_H < BR_GRID_TOP || y > BR_VIEWPORT_BOTTOM) continue;

        // Tile background
        g->fillRoundRect(x, y, BR_TILE_W, BR_TILE_H, 12, COLOR_PANEL);

        // Type icon
        int icon_cx = x + 30;
        int icon_cy = y + BR_TILE_H / 2;
        draw_type_icon(g, icon_cx, icon_cy, s_br_entries[i].type);

        // Filename (truncated)
        g->setTextSize(1);
        g->setTextColor(COLOR_TEXT);
        int name_x = x + 52;
        int name_max_w = BR_TILE_W - 60;
        char nm[sizeof(s_br_entries[i].name)];
        snprintf(nm, sizeof(nm), "%s", s_br_entries[i].name);
        ui_utf8_trim(nm);
        ui_fit(nm, 1, name_max_w);
        ui_print(name_x, y + 16, 1, COLOR_TEXT, nm);

        // File size (for non-folders)
        if (s_br_entries[i].type != FTYPE_FOLDER && s_br_entries[i].size > 0) {
            g->setTextSize(1);
            g->setTextColor(COLOR_TEXT_DIM);
            char size_buf[16];
            uint32_t sz = s_br_entries[i].size;
            if (sz > 1048576)
                snprintf(size_buf, sizeof(size_buf), "%luMB", (unsigned long)(sz / 1048576));
            else if (sz > 1024)
                snprintf(size_buf, sizeof(size_buf), "%luKB", (unsigned long)(sz / 1024));
            else
                snprintf(size_buf, sizeof(size_buf), "%luB", (unsigned long)sz);
            ui_print(name_x, y + 34, 1, COLOR_TEXT_DIM, size_buf);
        } else if (s_br_entries[i].type == FTYPE_FOLDER) {
            g->setTextSize(1);
            g->setTextColor(COLOR_ACCENT3);
            ui_print(name_x, y + 34, 1, COLOR_ACCENT3, "Folder");
        }
    }

    // Scroll indicator - inset within the tile column's own right
    // edge (not out past LCD_WIDTH-8, which is well outside the
    // visible circle at this viewport's y-range) so it never overflows.
    if (br_max_scroll() > 0) {
        int bar_h = BR_VISIBLE_H * BR_VISIBLE_H / br_content_h();
        if (bar_h < 20) bar_h = 20;
        int bar_y = BR_GRID_TOP + (int)((float)s_br_scroll / br_max_scroll() * (BR_VISIBLE_H - bar_h));
        g->fillRoundRect(BR_GRID_LEFT + BR_TILE_W - 6, bar_y, 4, bar_h, 2, COLOR_TEXT_DIM);
    }
}

static void browser_touch(int x, int y, bool pressed) {
    if (pressed) {
        // Back button
        if (x < 60 && y < 60) return; // handle on release

        if (!s_br_dragging) {
            s_br_dragging = true;
            s_br_drag_y = y;
            s_br_drag_scroll = s_br_scroll;
        } else {
            int dy = s_br_drag_y - y;
            s_br_scroll = s_br_drag_scroll + dy;
            if (s_br_scroll < 0) s_br_scroll = 0;
            if (s_br_scroll > br_max_scroll()) s_br_scroll = br_max_scroll();
        }
    } else {
        if (s_br_dragging) {
            s_br_dragging = false;
            int dy = y - s_br_drag_y;
            if (abs(dy) >= 10) {
                touch_cancel_swipes();
                return;
            }
            // Back button tap
            if (x < 60 && y < 60) {
                if (br_go_back()) return;
                audio_stop_playback();
                ui_pop_screen();
                return;
            }
            // Tile tap
            for (int i = 0; i < s_br_count; i++) {
                int col = i % BR_COLS;
                int row = i / BR_COLS;
                int tx = BR_GRID_LEFT + col * (BR_TILE_W + BR_GAP);
                int ty = BR_GRID_TOP + row * (BR_TILE_H + BR_GAP) - s_br_scroll;
                if (x >= tx && x < tx + BR_TILE_W && y >= ty && y < ty + BR_TILE_H) {
                    br_navigate_into(i);
                    return;
                }
            }
        }
    }
}

static void browser_tick() {}

static void browser_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) {
        if (br_go_back()) return;
        audio_stop_playback();
        ui_pop_screen();
    }
}

Screen media_screen = {
    "Media", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    browser_create, browser_draw, browser_touch,
    browser_tick, nullptr, browser_gesture,
    0, false, false, false,
    true, // no_drag_back - swipe right goes up a folder before it leaves
};

// =========================================================================
//  Audio Player - round-screen layout
// =========================================================================
//
//            NOW PLAYING
//        Song title (marquee)          <- edge ring, 280 deg: song progress
//      MP3 | 44.1 kHz | 128 kbps          (tap or drag along it to seek)
//
//        ((( visualizer )))
//        (  big play/pause  )
//
//               1:23
//               3:45
//          [ volume arc ]              <- bottom 66 deg of the edge: volume
//
// Everything interactive sits on the screen edge or is the one big
// button in the middle, so it can be operated without looking closely.

static bool s_aud_was_playing = false;
static bool s_aud_prev_pressed = false; // rising-edge tap detection, see aud_touch()

enum AudDrag : uint8_t { AUD_DRAG_NONE, AUD_DRAG_SEEK, AUD_DRAG_VOL };
static AudDrag  s_aud_drag = AUD_DRAG_NONE;
static float    s_aud_scrub = 0.0f;        // seek preview while dragging, 0..1
static uint32_t s_aud_vol_touch_ms = 0;    // volume label stays bright briefly after a change
static uint32_t s_aud_title_t0 = 0;        // marquee start time
static char     s_aud_title[BROWSER_MAX_NAME];
static char     s_aud_ext[8];

// Ring geometry (clock degrees: 0 = 12 o'clock, clockwise)
#define AUD_RING_OUTER     229
#define AUD_RING_THICK     10
#define AUD_PROG_START     220.0f   // bottom-left, then clockwise over the top...
#define AUD_PROG_SWEEP     280.0f   // ...to bottom-right (140 deg)
#define AUD_VOL_START      213.0f   // volume arc in the bottom gap, left = quiet
#define AUD_VOL_SWEEP      (-66.0f) // counter-clockwise -> left to right
#define AUD_RING_TOUCH_R   172      // touches this far out operate the rings

#define AUD_PLAY_CX        (LCD_WIDTH / 2)
#define AUD_PLAY_CY        236
#define AUD_PLAY_R         54

// ---- Playlist (Music app): previous / next / auto-advance -------------------
static const MediaList *s_pl = nullptr;
static int  *s_pl_order = nullptr;      // play order (indices into s_pl)
static int   s_pl_n = 0, s_pl_pos = 0;
static bool  s_pl_user_stop = false;    // stopped on purpose - don't auto-advance

void media_playlist_clear() {
    s_pl = nullptr;
    free(s_pl_order); s_pl_order = nullptr;
    s_pl_n = s_pl_pos = 0;
}

static bool pl_active() { return s_pl && s_pl_n > 0; }

static void pl_load_current() {
    int i = s_pl_order[s_pl_pos];
    strncpy(s_aud_path, s_pl->path(i), BROWSER_MAX_PATH - 1);
    s_aud_path[BROWSER_MAX_PATH - 1] = '\0';
    strncpy(s_aud_name, s_pl->name(i), BROWSER_MAX_NAME - 1);
    s_aud_name[BROWSER_MAX_NAME - 1] = '\0';
}

void media_play_playlist(const MediaList *list, const int *order, int n, int pos) {
    media_playlist_clear();
    if (!list || n <= 0) return;
    s_pl_order = (int *)malloc((size_t)n * sizeof(int));
    if (!s_pl_order) return;
    for (int i = 0; i < n; i++) s_pl_order[i] = order ? order[i] : i;
    s_pl = list;
    s_pl_n = n;
    s_pl_pos = pos < 0 ? 0 : (pos >= n ? n - 1 : pos);
    pl_load_current();
    ui_push(&media_audio_screen);
}

const char *media_now_playing_path() {
    return audio_is_playing() ? s_aud_path : nullptr;
}

static void aud_fmt_time(uint32_t ms, char *buf, size_t n) {
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static bool aud_start() {
    bool ok = ends_with_ci(s_aud_path, ".mp3") ? audio_play_mp3(s_aud_path) : audio_play_wav(s_aud_path);
    if (ok) s_aud_was_playing = true;
    else ui_show_toast("Can't play this file", 1500);
    return ok;
}

static void aud_load_title() {
    s_aud_title_t0 = millis();
    // Title without the extension; the extension goes in the info line.
    strncpy(s_aud_title, s_aud_name, sizeof(s_aud_title) - 1);
    s_aud_title[sizeof(s_aud_title) - 1] = '\0';
    s_aud_ext[0] = '\0';
    char *dot = strrchr(s_aud_title, '.');
    if (dot && dot != s_aud_title) {
        int i = 0;
        for (const char *e = dot + 1; *e && i < (int)sizeof(s_aud_ext) - 1; e++) s_aud_ext[i++] = (char)toupper((unsigned char)*e);
        s_aud_ext[i] = '\0';
        *dot = '\0';
    }
}

static void aud_create() {
    s_aud_was_playing = false;
    s_aud_prev_pressed = false;
    s_aud_drag = AUD_DRAG_NONE;
    s_aud_vol_touch_ms = 0;
    s_pl_user_stop = false;
    aud_load_title();
    aud_start(); // opening a song plays it
}

// Next/previous in the playlist. Returns false at either end.
static bool aud_go(int delta) {
    if (!pl_active()) return false;
    int p = s_pl_pos + delta;
    if (p < 0 || p >= s_pl_n) return false;
    s_pl_pos = p;
    audio_stop_playback();
    pl_load_current();
    aud_load_title();
    s_pl_user_stop = false;
    aud_start();
    return true;
}

#define AUD_SKIP_DX  118   // prev/next buttons either side of play/pause
#define AUD_SKIP_R   28

static void aud_draw_skip(Arduino_GFX *g, int cx, int cy, int dir, bool enabled) {
    uint16_t bg = enabled ? COLOR_PANEL : ui_dim(COLOR_PANEL, 0.5f);
    uint16_t c = enabled ? COLOR_TEXT : COLOR_TEXT_DIM;
    g->fillCircle(cx, cy, AUD_SKIP_R, bg);
    int s = 10;
    g->fillTriangle(cx - dir * s, cy - s, cx - dir * s, cy + s, cx, cy, c);
    g->fillTriangle(cx, cy - s, cx, cy + s, cx + dir * s, cy, c);
    g->fillRect(dir > 0 ? cx + s : cx - s - 3, cy - s, 3, 2 * s, c);
}

// Song title, size 3, scrolling (marquee) when wider than the window.
static void aud_draw_title(Arduino_GFX *g, int y) {
    const int x0 = 86, x1 = LCD_WIDTH - 86, cw = 18; // window and char width at size 3
    int n = (int)strlen(s_aud_title);
    int text_w = n * cw;
    g->setTextSize(3);
    g->setTextColor(COLOR_TEXT);
    int smooth_w = ui_text_width(s_aud_title, 3);
    if (smooth_w <= x1 - x0) {
        ui_print(LCD_WIDTH / 2 - smooth_w / 2, y, 3, COLOR_TEXT, s_aud_title);
        return;
    }
    if (ui_fonts_smooth()) {
        // Marquee in the smooth font: the whole string, clipped to the window.
        const int sgap = 48;
        uint32_t t = millis() - s_aud_title_t0;
        int off = t < 1500 ? 0 : (int)(((t - 1500) / 22) % (uint32_t)(smooth_w + sgap));
        ui_font_set_clip_x(x0, x1);
        ui_print(x0 - off, y, 3, COLOR_TEXT, s_aud_title);
        ui_print(x0 - off + smooth_w + sgap, y, 3, COLOR_TEXT, s_aud_title);
        ui_font_clear_clip();
        return;
    }
    const int gap = 3 * cw;
    uint32_t t = millis() - s_aud_title_t0;
    int off = t < 1500 ? 0 : (int)(((t - 1500) / 22) % (uint32_t)(text_w + gap));
    for (int copy = 0; copy < 2; copy++) {
        int base = x0 - off + copy * (text_w + gap);
        for (int i = 0; i < n; i++) {
            int cx = base + i * cw;
            if (cx < x0 || cx + cw > x1) continue;
            g->setCursor(cx, y);
            g->print(s_aud_title[i]);
        }
    }
}

static void aud_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    AudioStreamInfo info;
    bool loaded = audio_get_stream_info(&info);
    bool playing = loaded && !info.paused;
    float frac = (loaded && info.duration_ms) ? (float)info.position_ms / (float)info.duration_ms : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (s_aud_drag == AUD_DRAG_SEEK) frac = s_aud_scrub;

    // ---- Edge: progress ring (track, glow, fill, head) --------------------
    ui_edge_ring(AUD_PROG_START, AUD_PROG_SWEEP, ui_dim(COLOR_ACCENT, 0.22f), AUD_RING_THICK, 4);
    if (frac > 0.003f) {
        float sw = AUD_PROG_SWEEP * frac;
        ui_edge_ring(AUD_PROG_START, sw, ui_dim(COLOR_ACCENT, 0.35f), AUD_RING_THICK + 6, 1);
        ui_edge_ring(AUD_PROG_START, sw, COLOR_ACCENT, AUD_RING_THICK, 4);
        int hx, hy;
        ui_polar(AUD_PROG_START + sw, AUD_RING_OUTER - AUD_RING_THICK / 2, &hx, &hy);
        g->fillCircle(hx, hy, s_aud_drag == AUD_DRAG_SEEK ? 11 : 8, COLOR_TEXT);
    }

    // ---- Edge: volume arc in the bottom gap ------------------------------
    int vol = audio_get_volume();
    bool vol_hot = s_aud_drag == AUD_DRAG_VOL || millis() - s_aud_vol_touch_ms < 1500;
    ui_edge_ring(AUD_VOL_START, AUD_VOL_SWEEP, ui_dim(COLOR_ACCENT2, 0.22f), AUD_RING_THICK, 4);
    if (vol > 0) ui_edge_ring(AUD_VOL_START, AUD_VOL_SWEEP * vol / 100.0f, COLOR_ACCENT2, AUD_RING_THICK, 4);
    {
        int kx, ky;
        ui_polar(AUD_VOL_START + AUD_VOL_SWEEP * vol / 100.0f, AUD_RING_OUTER - AUD_RING_THICK / 2, &kx, &ky);
        g->fillCircle(kx, ky, vol_hot ? 10 : 7, COLOR_TEXT);
    }
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d%%", vol);
    // small speaker glyph + percentage, just above the arc
    {
        uint16_t c = vol_hot ? COLOR_TEXT : COLOR_TEXT_DIM;
        int tw = ui_text_width(vbuf, 2);
        int x = LCD_WIDTH / 2 - (tw + 22) / 2, y = 398;
        g->fillRect(x, y + 4, 5, 8, c);
        g->fillTriangle(x + 3, y + 8, x + 12, y, x + 12, y + 16, c);
        g->setTextSize(2);
        g->setTextColor(c);
        ui_print(x + 22, y + 1, 2, c, vbuf);
    }

    // ---- Header: state, title, format ---------------------------------
    const char *state = !loaded ? (s_aud_was_playing ? "FINISHED" : "READY")
                                : (info.paused ? "PAUSED" : "NOW PLAYING");
    ui_text_center(LCD_WIDTH / 2, 64, info.paused ? COLOR_WARN : COLOR_TEXT_DIM, state, 1);
    aud_draw_title(g, 80);
    if (loaded && info.sample_rate) {
        char fbuf[40];
        snprintf(fbuf, sizeof(fbuf), "%s | %lu.%lu kHz | %u kbps", s_aud_ext[0] ? s_aud_ext : (info.is_mp3 ? "MP3" : "WAV"),
                 (unsigned long)(info.sample_rate / 1000), (unsigned long)(info.sample_rate % 1000 / 100), (unsigned)info.kbps);
        ui_text_center(LCD_WIDTH / 2, 124, COLOR_TEXT_DIM, fbuf, 1);
    }

    // ---- Centre: 3D spectrum ring on the real output level + play/pause --
    static FxBeat beat;
    uint32_t now = millis();
    fx_beat_update(beat, loaded ? info.level : 0, playing, now);
    fx_eq_ring_draw(g, AUD_PLAY_CX, AUD_PLAY_CY, FX_EQ_INNER(AUD_PLAY_R), 30.0f, beat, now, COLOR_ACCENT2, COLOR_ACCENT3, 0);
    g->fillCircle(AUD_PLAY_CX, AUD_PLAY_CY, AUD_PLAY_R, playing ? COLOR_ACCENT : COLOR_TEXT);
    uint16_t glyph = playing ? COLOR_TEXT : COLOR_BG;
    if (playing) {
        g->fillRoundRect(AUD_PLAY_CX - 17, AUD_PLAY_CY - 20, 12, 40, 3, glyph);
        g->fillRoundRect(AUD_PLAY_CX + 5, AUD_PLAY_CY - 20, 12, 40, 3, glyph);
    } else {
        g->fillTriangle(AUD_PLAY_CX - 14, AUD_PLAY_CY - 23, AUD_PLAY_CX - 14, AUD_PLAY_CY + 23,
                        AUD_PLAY_CX + 24, AUD_PLAY_CY, glyph);
    }
    fx_eq_ring_draw(g, AUD_PLAY_CX, AUD_PLAY_CY, FX_EQ_INNER(AUD_PLAY_R), 30.0f, beat, now, COLOR_ACCENT2, COLOR_ACCENT3, 1);

    // ---- Playlist: prev / next + position --------------------------------
    if (pl_active()) {
        aud_draw_skip(g, AUD_PLAY_CX - AUD_SKIP_DX, AUD_PLAY_CY, -1, true);   // prev also restarts the song
        aud_draw_skip(g, AUD_PLAY_CX + AUD_SKIP_DX, AUD_PLAY_CY, +1, s_pl_pos < s_pl_n - 1);
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d / %d", s_pl_pos + 1, s_pl_n);
        ui_text_center(LCD_WIDTH / 2, 142, COLOR_TEXT_DIM, pbuf, 1);
    }

    // ---- Time ------------------------------------------------------------
    char tbuf[16], dbuf[16];
    uint32_t pos_ms = loaded ? info.position_ms : 0;
    if (s_aud_drag == AUD_DRAG_SEEK && loaded) pos_ms = (uint32_t)(s_aud_scrub * info.duration_ms);
    aud_fmt_time(pos_ms, tbuf, sizeof(tbuf));
    ui_text_center(LCD_WIDTH / 2, 342, s_aud_drag == AUD_DRAG_SEEK ? COLOR_ACCENT : COLOR_TEXT, tbuf, 3);
    if (loaded && info.duration_ms) {
        aud_fmt_time(info.duration_ms, dbuf, sizeof(dbuf));
        ui_text_center(LCD_WIDTH / 2, 372, COLOR_TEXT_DIM, dbuf, 2);
    }

    // ---- Back chevron (left edge) ---------------------------------------
    g->fillTriangle(30, 233, 44, 219, 44, 247, COLOR_TEXT_DIM);
    g->fillTriangle(37, 233, 44, 226, 44, 240, COLOR_BG);
}

static void aud_touch(int x, int y, bool pressed) {
    // on_touch is called on EVERY frame while the finger is down, not
    // once per tap. Buttons act only on the press edge; the rings follow
    // the held finger.
    bool tap_edge = pressed && !s_aud_prev_pressed;
    s_aud_prev_pressed = pressed;

    if (!pressed) {
        if (s_aud_drag == AUD_DRAG_SEEK) {
            if (audio_is_playing()) audio_seek(s_aud_scrub);
        } else if (s_aud_drag == AUD_DRAG_VOL) {
            // Keep it as the device-wide volume, like Settings > Volume does.
            g_app_settings.volume = audio_get_volume();
            nvs_save_settings(g_app_settings);
        }
        // Dragging along the ring looks like a swipe to the gesture
        // detector - it must not also pop this screen.
        if (s_aud_drag != AUD_DRAG_NONE) touch_cancel_swipes();
        s_aud_drag = AUD_DRAG_NONE;
        return;
    }

    float ang = ui_angle_of(x, y);
    if (s_aud_drag == AUD_DRAG_VOL) {
        float t = (AUD_VOL_START - ang) / -AUD_VOL_SWEEP; // 0 at the left end
        if (ang < 90.0f) t = 1.0f;                         // dragged past the right end
        if (ang > 270.0f) t = 0.0f;                        // ...or past the left end
        if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
        audio_set_volume((uint8_t)(t * 100.0f + 0.5f));
        s_aud_vol_touch_ms = millis();
        return;
    }
    if (s_aud_drag == AUD_DRAG_SEEK) {
        float d = fmodf(ang - AUD_PROG_START + 360.0f, 360.0f);
        float t = d / AUD_PROG_SWEEP;
        if (d > AUD_PROG_SWEEP) t = d > AUD_PROG_SWEEP + 40.0f ? 0.0f : 1.0f; // in the volume gap: clamp to the nearer end
        s_aud_scrub = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return;
    }
    if (!tap_edge) return;

    // Back chevron
    if (x < 72 && abs(y - 233) < 40) {
        audio_stop_playback();
        s_aud_was_playing = false;
        ui_pop_screen();
        return;
    }

    // Rings: the bottom gap is volume, the rest of the edge is the song
    if (ui_radius_of(x, y) >= AUD_RING_TOUCH_R) {
        if (ui_angle_in_arc(ang, 140.0f, 80.0f)) {
            s_aud_drag = AUD_DRAG_VOL;
        } else if (audio_is_playing()) {
            s_aud_drag = AUD_DRAG_SEEK;
        } else {
            return;
        }
        aud_touch(x, y, true); // apply the first position right away
        return;
    }

    // Previous / next (playlist only). Previous restarts the song if it's
    // more than 3 s in, like any music player.
    if (pl_active()) {
        for (int dir = -1; dir <= 1; dir += 2) {
            int sx = AUD_PLAY_CX + dir * AUD_SKIP_DX;
            int dx = x - sx, dy = y - AUD_PLAY_CY;
            if (dx * dx + dy * dy > (AUD_SKIP_R + 10) * (AUD_SKIP_R + 10)) continue;
            AudioStreamInfo si;
            if (dir < 0 && audio_get_stream_info(&si) && si.position_ms > 3000) audio_seek(0);
            else if (!aud_go(dir) && dir < 0 && audio_is_playing()) audio_seek(0);
            return;
        }
    }

    // Play / pause (generous target)
    int pdx = x - AUD_PLAY_CX, pdy = y - AUD_PLAY_CY;
    if (pdx * pdx + pdy * pdy < (AUD_PLAY_R + 18) * (AUD_PLAY_R + 18)) {
        if (audio_is_playing()) audio_set_paused(!audio_is_paused());
        else aud_start();
    }
}

static void aud_tick() {
    if (s_aud_was_playing && !audio_is_playing() && !s_pl_user_stop) {
        // Finished on its own: next song in the playlist, or keep
        // s_aud_was_playing so "FINISHED" shows.
        if (!aud_go(+1)) s_pl_user_stop = true;
    }
}

static void aud_destroy() {
    s_pl_user_stop = true;
    audio_stop_playback();
    s_aud_was_playing = false;
}

static void aud_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) {
        audio_stop_playback();
        s_aud_was_playing = false;
        ui_pop_screen();
    }
}

Screen media_audio_screen = {
    "", GESTURE_MODE_EDGE,   // no header: this screen draws its own chrome
    UI_FRAME_MS_DEFAULT,
    aud_create, aud_draw, aud_touch,
    aud_tick, aud_destroy, aud_gesture,
    0, false, false, false, false,
    true, // hide_status - the edge belongs to the progress/volume rings
};

// =========================================================================
//  Text Viewer
// =========================================================================

#define TXT_MAX_SIZE 4096
static char *s_txt_buf = nullptr;
static int   s_txt_len = 0;
static int   s_txt_lines = 0;
static const char **s_txt_line_ptrs = nullptr;
static int   s_txt_scroll = 0;
static bool  s_txt_dragging = false;
static int   s_txt_drag_y = 0;
static int   s_txt_drag_scroll = 0;

static const int TXT_LINES_PER_PAGE = 25;
static const int TXT_LINE_H = 14;
static const int TXT_TOP = 58;
static const int TXT_VISIBLE = LCD_HEIGHT - TXT_TOP - 20;
static const int TXT_LINES_VISIBLE = TXT_VISIBLE / TXT_LINE_H;

static void txt_parse_lines() {
    s_txt_lines = 0;
    if (!s_txt_buf || s_txt_len == 0) return;

    // Count newlines and build line pointers
    int max_lines = s_txt_len / 4 + 1; // rough estimate
    s_txt_line_ptrs = (const char **)ps_malloc(max_lines * sizeof(char *));
    if (!s_txt_line_ptrs) return;

    s_txt_line_ptrs[0] = s_txt_buf;
    s_txt_lines = 1;
    for (int i = 0; i < s_txt_len - 1; i++) {
        if (s_txt_buf[i] == '\n') {
            s_txt_buf[i] = '\0'; // null-terminate this line
            if (s_txt_lines < max_lines) {
                s_txt_line_ptrs[s_txt_lines++] = &s_txt_buf[i + 1];
            }
        }
        if (s_txt_buf[i] == '\r') {
            s_txt_buf[i] = '\0';
        }
    }
}

static void txt_create() {
    s_txt_scroll = 0;
    s_txt_dragging = false;
    s_txt_len = 0;
    s_txt_lines = 0;
    if (s_txt_line_ptrs) { free(s_txt_line_ptrs); s_txt_line_ptrs = nullptr; }
    if (s_txt_buf) { free(s_txt_buf); s_txt_buf = nullptr; }

#if FEATURE_SD_CARD
    if (!sd_is_mounted()) return;
    // s_txt_path already starts with /
    File f = SD.open(s_txt_path, FILE_READ);
    if (!f) {
        DEBUG_PRINTF("[media] txt_create: open failed for %s\n", s_txt_path);
        return;
    }

    int avail = f.size();
    if (avail > TXT_MAX_SIZE) avail = TXT_MAX_SIZE;
    s_txt_buf = (char *)ps_malloc(avail + 1);
    if (!s_txt_buf) { f.close(); return; }

    s_txt_len = f.read((uint8_t *)s_txt_buf, avail);
    s_txt_buf[s_txt_len] = '\0';
    f.close();

    txt_parse_lines();
#endif
}

static void txt_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    draw_back(g);

    // Show filename as title
    const char *fname = strrchr(s_txt_path, '/');
    fname = fname ? fname + 1 : s_txt_path;
    draw_top_title(g, fname);

    if (!s_txt_buf || s_txt_lines == 0) {
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "Cannot read file", 2);
        return;
    }

    // Draw text lines
    g->setTextSize(1);
    g->setTextColor(COLOR_TEXT);

    int first_line = s_txt_scroll;
    int y = TXT_TOP;

    for (int i = first_line; i < s_txt_lines && y < LCD_HEIGHT - 10; i++) {
        const char *line = s_txt_line_ptrs[i];
        if (!line) continue;

        // Truncate long lines for display (max ~70 chars at size 1 = 420px)
        int max_chars = 70;
        g->setCursor(16, y);
        for (int j = 0; j < max_chars && line[j]; j++) {
            g->print(line[j]);
        }
        y += TXT_LINE_H;
    }

    // Scroll indicator
    int total_lines = s_txt_lines;
    if (total_lines > TXT_LINES_VISIBLE) {
        int bar_h = TXT_VISIBLE * TXT_LINES_VISIBLE / total_lines;
        if (bar_h < 16) bar_h = 16;
        int max_scroll = total_lines - TXT_LINES_VISIBLE;
        int bar_y = TXT_TOP + (int)((float)s_txt_scroll / max_scroll * (TXT_VISIBLE - bar_h));
        g->fillRoundRect(LCD_WIDTH - 8, bar_y, 4, bar_h, 2, COLOR_TEXT_DIM);
    }

    // Line count at bottom
    char info[24];
    snprintf(info, sizeof(info), "%d/%d", s_txt_scroll + 1, s_txt_lines);
    g->setTextSize(1);
    g->setTextColor(COLOR_TEXT_DIM);
    int iw = ui_text_width(info, 1);
    ui_print((LCD_WIDTH - iw) / 2, LCD_HEIGHT - 14, 1, COLOR_TEXT_DIM, info);
}

static void txt_touch(int x, int y, bool pressed) {
    if (pressed) {
        if (x < 60 && y < 60) return;

        if (!s_txt_dragging) {
            s_txt_dragging = true;
            s_txt_drag_y = y;
            s_txt_drag_scroll = s_txt_scroll;
        } else {
            int dy = s_txt_drag_y - y;
            int lines_scrolled = dy / TXT_LINE_H;
            s_txt_scroll = s_txt_drag_scroll + lines_scrolled;
            if (s_txt_scroll < 0) s_txt_scroll = 0;
            int max_scroll = s_txt_lines - TXT_LINES_VISIBLE;
            if (max_scroll < 0) max_scroll = 0;
            if (s_txt_scroll > max_scroll) s_txt_scroll = max_scroll;
        }
    } else {
        if (s_txt_dragging) {
            s_txt_dragging = false;
            int dy = y - s_txt_drag_y;
            if (abs(dy) >= 10) {
                touch_cancel_swipes();
                return;
            }
            // Back button
            if (x < 60 && y < 60) {
                ui_pop_screen();
                return;
            }
        }
    }
}

static void txt_destroy() {
    if (s_txt_buf) { free(s_txt_buf); s_txt_buf = nullptr; }
    if (s_txt_line_ptrs) { free(s_txt_line_ptrs); s_txt_line_ptrs = nullptr; }
    s_txt_len = 0;
    s_txt_lines = 0;
}

static void txt_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) {
        ui_pop_screen();
    } else if (g == GESTURE_SWIPE_DOWN) {
        // Page up
        s_txt_scroll -= TXT_LINES_VISIBLE;
        if (s_txt_scroll < 0) s_txt_scroll = 0;
    }
}

Screen media_text_screen = {
    "Text", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    txt_create, txt_draw, txt_touch,
    nullptr, txt_destroy, txt_gesture
};

// =========================================================================
//  Images and videos open in the Gallery viewer / video player
//  (app_gallery.cpp, app_video.cpp).
// =========================================================================

void media_navigate_to(const char *path) {
    gallery_open_file(path);
}
