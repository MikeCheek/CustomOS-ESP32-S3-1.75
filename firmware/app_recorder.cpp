#include "app_recorder.h"
#include "app_media.h"
#include "media_library.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_audio.h"
#include "hal_sd.h"
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// =========================================================================
//  Recorder
//
//  List of /Recordings in the same style as the Music app: a Record pill on
//  top, one row per recording (name, length, size, "transcript" when the
//  phone has sent one back). Tapping a row opens the music player with the
//  recordings as its playlist (seek ring, volume arc, previous/next).
//  Long-press a row to select recordings for deleting.
// =========================================================================

enum RecState { REC_IDLE, REC_RECORDING, REC_SELECT };

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int ROW_W = 350, ROW_X = (LCD_WIDTH - ROW_W) / 2, ROW_H = 62, GAP = 6;
static const int TOP = 84, PILL_H = 52, LIST_TOP = TOP + PILL_H + 14;
static const int REC_MAX = 200;

struct RecInfo { uint32_t size; uint32_t secs; bool transcript; bool approx; bool selected; };

static RecState  s_state = REC_IDLE;
static char      s_cur_filename[48] = {0};
static MediaList s_list;                 // full paths, newest first
static RecInfo  *s_info = nullptr;       // PSRAM, parallel to s_list
static UiScroll  s_scroll;
static bool      s_back = false, s_prev = false;
static int       s_press = -2;           // -1 = Record pill, -3 = Delete button, >= 0 row
static uint32_t  s_press_ms = 0;
static bool      s_long_fired = false;
static int       s_sel_count = 0;

uint32_t recordings_revision();               // AmoledSmartWatchOS.ino
bool recordings_file_busy(const char *path);   // ...: being sent to the phone
static uint32_t s_seen_rev = 0;

static const int DEL_W = 180, DEL_H = 50;
static int del_y() { return LCD_HEIGHT - 84; }

// ---- files -----------------------------------------------------------------------

static void gen_filename(char *buf, int bufsz) {
    for (int idx = 1; idx < 1000; idx++) {
        snprintf(buf, bufsz, "rec_%03d.wav", idx);
        if (!SD.exists(String("/Recordings/") + buf)) return;
    }
    snprintf(buf, bufsz, "rec_%03lu.wav", (unsigned long)(1000 + millis() % 1000));
}

static int rec_filter(const char *name) {
    return name[0] != '_' && name[0] != '.' && media_is_audio(name) ? 0 : -1;
}

// Length from the WAV header (any rate/channels), or a 128 kbit/s guess for MP3.
static void probe(const char *path, RecInfo &ri) {
    ri.secs = 0;
    ri.approx = false;
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    ri.size = f.size();
    int l = strlen(path);
    if (l > 4 && !strcasecmp(path + l - 4, ".wav")) {
        uint8_t h[36];
        if (f.read(h, sizeof(h)) == sizeof(h) && !memcmp(h, "RIFF", 4)) {
            uint32_t byte_rate = h[28] | (h[29] << 8) | (h[30] << 16) | ((uint32_t)h[31] << 24);
            if (byte_rate) ri.secs = (ri.size > 44 ? ri.size - 44 : 0) / byte_rate;
        }
    } else {
        ri.secs = ri.size / 16000;
        ri.approx = true;
    }
    f.close();
}

static void refresh() {
    s_seen_rev = recordings_revision();
    s_list.clear();
    s_sel_count = 0;
    if (!sd_is_mounted()) return;
    File root = SD.open("/Recordings");
    if (root && root.isDirectory()) {
        for (File e = root.openNextFile(); e && s_list.n < REC_MAX; e = root.openNextFile()) {
            if (!e.isDirectory() && rec_filter(e.name()) == 0) {
                String p = String("/Recordings/") + e.name();
                s_list.add(p.c_str(), 0);
            }
            e.close();
        }
        root.close();
    }
    s_list.sort_by_name();
    s_list.reverse();            // rec_012 before rec_011: newest first
    if (!s_info) s_info = (RecInfo *)heap_caps_calloc(REC_MAX, sizeof(RecInfo), MALLOC_CAP_SPIRAM);
    if (!s_info) { s_list.clear(); return; }
    for (int i = 0; i < s_list.n; i++) {
        RecInfo &ri = s_info[i];
        memset(&ri, 0, sizeof(ri));
        probe(s_list.path(i), ri);
        char txt[96];
        snprintf(txt, sizeof(txt), "%s", s_list.path(i));
        char *dot = strrchr(txt, '.');
        if (dot) { strcpy(dot, ".txt"); ri.transcript = SD.exists(txt); }
    }
}

static int content_h() { return LIST_TOP + s_list.n * (ROW_H + GAP) - GAP + (s_state == REC_SELECT ? 170 : 110); }

static void reset_scroll() { ui_scroll_reset(&s_scroll, content_h() - LCD_HEIGHT, ROW_H + GAP); }

// ---- drawing ------------------------------------------------------------------------

static void mic_glyph(Arduino_GFX *g, int cx, int cy, uint16_t c) {
    g->fillRoundRect(cx - 5, cy - 12, 10, 17, 5, c);
    g->fillRect(cx - 1, cy + 6, 3, 5, c);
    g->fillRect(cx - 6, cy + 10, 13, 2, c);
}

static void fmt_len(const RecInfo &ri, char *out, int n) {
    char sz[12];
    if (ri.size >= 1024 * 1024) snprintf(sz, sizeof(sz), "%.1f MB", ri.size / 1048576.0f);
    else snprintf(sz, sizeof(sz), "%lu KB", (unsigned long)(ri.size / 1024));
    snprintf(out, n, "%s%lu:%02lu  ·  %s%s", ri.approx ? "~" : "", (unsigned long)(ri.secs / 60),
             (unsigned long)(ri.secs % 60), sz, ri.transcript ? "  ·  transcript" : "");
}

// Live dual-mic waveform behind the recording UI (mic1 = left, mic2 = right).
static void draw_mic_waveform(Arduino_GFX *g) {
    int16_t wl[128], wr[128];
    int n = audio_get_mic_waveform(wl, wr, 128);
    if (n < 2) return;
    const int baseline = 210, amp_px = 90, x0 = 30, x1 = LCD_WIDTH - 30, span = x1 - x0;
    for (int i = 1; i < n; i++) {
        int px0 = x0 + (int)((int32_t)(i - 1) * span / (n - 1));
        int px1 = x0 + (int)((int32_t)i * span / (n - 1));
        g->drawLine(px0, baseline - (int)((int32_t)wl[i - 1] * amp_px / 32768), px1,
                    baseline - (int)((int32_t)wl[i] * amp_px / 32768), COLOR565(0x33, 0xCC, 0xFF));
        g->drawLine(px0, baseline - (int)((int32_t)wr[i - 1] * amp_px / 32768), px1,
                    baseline - (int)((int32_t)wr[i] * amp_px / 32768), COLOR565(0xFF, 0x99, 0x33));
    }
}

static void draw_recording(Arduino_GFX *g) {
    draw_mic_waveform(g);
    uint32_t el = audio_record_duration_s();
    float pulse = 0.5f + 0.5f * sinf((float)millis() * 0.005f);
    g->fillCircle(CX, 132, 44 + (int)(pulse * 6), ui_dim(COLOR_BAD, 0.3f));
    g->fillCircle(CX, 132, 40, COLOR_BAD);
    mic_glyph(g, CX, 132, COLOR_TEXT);
    char timer[16];
    snprintf(timer, sizeof(timer), "%02lu:%02lu", (unsigned long)(el / 60), (unsigned long)(el % 60));
    ui_text_center(CX, 262, COLOR_TEXT, timer, 5);
    // level bar
    int lvl = audio_mic_level_percent();
    int bw = 220, bx = CX - bw / 2, by = 304;
    g->fillRoundRect(bx, by, bw, 10, 5, COLOR_PANEL);
    int fw = lvl * bw / 100;
    if (fw > 6) g->fillRoundRect(bx, by, fw, 10, 5, lvl > 80 ? COLOR_BAD : lvl > 50 ? COLOR_WARN : COLOR_GOOD);
    // stop
    g->fillCircle(CX, 372, 34, s_press == -1 ? ui_dim(COLOR_TEXT, 0.7f) : COLOR_TEXT);
    g->fillRoundRect(CX - 12, 360, 24, 24, 5, COLOR_BAD);
}

static void recorder_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_state == REC_RECORDING) { draw_recording(g); return; }
    if (!sd_is_mounted()) {
        ui_text_center(CX, CY - 10, COLOR_TEXT, "No SD card", 2);
        ui_text_center(CX, CY + 18, COLOR_TEXT_DIM, "Recordings are saved on the card", 1);
        return;
    }

    int off = -ui_scroll_offset(&s_scroll);
    const char *now = media_now_playing_path();
    bool sel = s_state == REC_SELECT;

    // Record pill (or the selection header)
    int py = TOP + off;
    if (py + PILL_H > 0) {
        if (sel) {
            char h[32];
            snprintf(h, sizeof(h), s_sel_count ? "%d selected" : "Tap to select", s_sel_count);
            ui_text_center(CX, py + PILL_H / 2, COLOR_WARN, h, 2);
        } else {
            bool pr = s_press == -1;
            g->fillRoundRect(CX - 120, py, 240, PILL_H, PILL_H / 2, pr ? COLOR_TEXT : COLOR_BAD);
            uint16_t c = pr ? COLOR_BAD : COLOR_TEXT;
            g->fillCircle(CX - 64, py + PILL_H / 2, 10, c);
            ui_print(CX - 40, py + PILL_H / 2 - 8, 2, c, "Record");
            char cnt[32];
            snprintf(cnt, sizeof(cnt), s_list.n == 1 ? "1 recording" : "%d recordings", s_list.n);
            ui_text_center(CX, py - 12, COLOR_TEXT_DIM, cnt, 1);
        }
    }

    if (s_list.n == 0) {
        mic_glyph(g, CX, LIST_TOP + 60 + off, COLOR_TEXT_DIM);
        ui_text_center(CX, LIST_TOP + 100 + off, COLOR_TEXT, "No recordings yet", 2);
        ui_text_center(CX, LIST_TOP + 128 + off, COLOR_TEXT_DIM, "Or add some from the companion app", 1);
        return;
    }

    char t[56], sub[48];
    for (int i = 0; i < s_list.n; i++) {
        int y = LIST_TOP + i * (ROW_H + GAP) + off;
        if (y + ROW_H < 0) continue;
        if (y > LCD_HEIGHT) break;
        const RecInfo &ri = s_info[i];
        bool playing = now && !strcmp(now, s_list.path(i));
        bool pressed = s_press == i;
        bool chosen = sel && ri.selected;
        uint16_t border = chosen ? COLOR_BAD : playing ? COLOR_ACCENT : (pressed ? COLOR_TEXT_DIM : COLOR_PANEL);
        g->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 18, border);
        g->fillRoundRect(ROW_X + 2, y + 2, ROW_W - 4, ROW_H - 4, 16,
                         pressed ? ui_dim(COLOR_TEXT, 0.25f) : chosen ? ui_dim(COLOR_BAD, 0.25f) :
                         playing ? ui_dim(COLOR_ACCENT, 0.25f) : COLOR_PANEL);
        int ix = ROW_X + 32, iy = y + ROW_H / 2;
        if (sel) {
            g->fillCircle(ix, iy, 14, chosen ? COLOR_BAD : COLOR_TEXT_DIM);
            if (!chosen) g->fillCircle(ix, iy, 11, COLOR_PANEL);
            else { g->fillRect(ix - 6, iy, 5, 3, COLOR_TEXT); g->fillRect(ix - 2, iy - 6, 3, 9, COLOR_TEXT); }
        } else {
            g->fillCircle(ix, iy, 20, playing ? COLOR_ACCENT : COLOR_BG);
            if (playing) {
                uint32_t ms = millis();
                for (int b = 0; b < 3; b++) {
                    int hgt = 6 + (int)((ms / (90 + b * 37) + b * 5) % 12);
                    g->fillRect(ix - 9 + b * 7, iy + 9 - hgt, 4, hgt, COLOR_TEXT);
                }
            } else {
                mic_glyph(g, ix, iy, COLOR_BAD);
            }
        }
        snprintf(t, sizeof(t), "%s", s_list.name(i));
        char *dot = strrchr(t, '.');
        if (dot && dot != t) *dot = 0;
        ui_fit(t, 2, ROW_W - 64 - 20);
        ui_print(ROW_X + 64, y + 13, 2, COLOR_TEXT, t);
        fmt_len(ri, sub, sizeof(sub));
        ui_fit(sub, 1, ROW_W - 64 - 20);
        ui_print(ROW_X + 64, y + 38, 1, ri.transcript ? COLOR_ACCENT2 : COLOR_TEXT_DIM, sub);
    }

    if (sel) {
        int dy = del_y();
        bool on = s_sel_count > 0;
        g->fillRoundRect(CX - DEL_W / 2, dy, DEL_W, DEL_H, DEL_H / 2,
                         !on ? COLOR_PANEL : s_press == -3 ? ui_dim(COLOR_BAD, 0.6f) : COLOR_BAD);
        char lbl[24];
        snprintf(lbl, sizeof(lbl), on ? "Delete %d" : "Cancel", s_sel_count);
        ui_text_center(CX, dy + DEL_H / 2, COLOR_TEXT, lbl, 2);
    }
}

// ---- actions ------------------------------------------------------------------------

static void start_recording() {
    if (!sd_is_mounted()) { ui_show_toast("No SD card", 1500); return; }
    if (audio_is_playing()) audio_stop_playback();
    gen_filename(s_cur_filename, sizeof(s_cur_filename));
    if (audio_start_record(s_cur_filename)) {
        s_state = REC_RECORDING;
        // The codec didn't answer even after a retry: the file would be silent.
        if (!audio_mic_ok()) ui_show_toast("Microphone not responding - restart the watch", 3000);
    } else {
        ui_show_toast("Record failed", 2000);
    }
}

static void stop_recording() {
    audio_stop_record();
    s_state = REC_IDLE;
    refresh();
    reset_scroll();
    ui_show_toast("Recording saved", 1500);
}

static void delete_selected() {
    int deleted = 0;
    int busy = 0;
    for (int i = 0; i < s_list.n; i++) {
        if (!s_info[i].selected) continue;
        if (recordings_file_busy(s_list.path(i))) { busy++; continue; }
        SD.remove(s_list.path(i));
        char txt[96];
        snprintf(txt, sizeof(txt), "%s", s_list.path(i));
        char *dot = strrchr(txt, '.');
        if (dot) { strcpy(dot, ".txt"); SD.remove(txt); }
        deleted++;
    }
    s_state = REC_IDLE;
    refresh();
    reset_scroll();
    char msg[64];
    if (busy) snprintf(msg, sizeof(msg), "Deleted %d - %d being sent to the phone", deleted, busy);
    else snprintf(msg, sizeof(msg), "Deleted %d recording%s", deleted, deleted == 1 ? "" : "s");
    ui_show_toast(msg, 1500);
}

static void enter_select(int first) {
    s_state = REC_SELECT;
    for (int i = 0; i < s_list.n; i++) s_info[i].selected = false;
    s_sel_count = 0;
    if (first >= 0 && first < s_list.n) { s_info[first].selected = true; s_sel_count = 1; }
    ui_scroll_set_max(&s_scroll, content_h() - LCD_HEIGHT);
}

// ---- screen callbacks -----------------------------------------------------------------

static void recorder_create() {
    s_state = REC_IDLE;
    s_back = s_prev = false;
    s_press = -2;
    refresh();
    reset_scroll();
}

static void recorder_tick() {
    ui_scroll_tick(&s_scroll);
    // Long-press a row: selection mode with that row picked.
    if (s_state == REC_IDLE && s_press >= 0 && !s_long_fired && !s_scroll.moved && millis() - s_press_ms > 650) {
        s_long_fired = true;
        enter_select(s_press);
        s_press = -2;
    }
    if (s_state == REC_RECORDING && !audio_is_recording()) { s_state = REC_IDLE; refresh(); reset_scroll(); }
    // A recording finished while this screen was covered, or one arrived
    // from the phone: re-read the list.
    if (s_state == REC_IDLE && s_seen_rev != recordings_revision()) {
        refresh();
        ui_scroll_set_max(&s_scroll, content_h() - LCD_HEIGHT);
    }
}

static void recorder_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_prev;
    s_prev = pressed;

    if (s_state == REC_RECORDING) {
        bool on = (x - CX) * (x - CX) + (y - 372) * (y - 372) < 44 * 44;
        if (edge) s_press = on ? -1 : -2;
        if (!pressed) { if (s_press == -1 && on) stop_recording(); s_press = -2; }
        return;
    }

    if (pressed && !s_scroll.dragging && edge) s_back = (x < 54 && y < 54);
    if (s_back) {
        if (!pressed) { s_back = false; if (s_state == REC_SELECT) s_state = REC_IDLE; else ui_pop_screen(); }
        return;
    }

    int cy = y + ui_scroll_offset(&s_scroll);
    if (edge) {
        s_press = -2;
        s_long_fired = false;
        s_press_ms = millis();
        if (s_state == REC_SELECT && y >= del_y() && y <= del_y() + DEL_H && abs(x - CX) <= DEL_W / 2) s_press = -3;
        else if (s_state == REC_IDLE && cy >= TOP && cy <= TOP + PILL_H && abs(x - CX) <= 120) s_press = -1;
        else if (x >= ROW_X && x <= ROW_X + ROW_W && cy >= LIST_TOP) {
            int i = (cy - LIST_TOP) / (ROW_H + GAP);
            if (i < s_list.n && (cy - LIST_TOP) % (ROW_H + GAP) < ROW_H) s_press = i;
        }
    }
    bool tap = s_press == -3 ? !pressed : ui_scroll_touch(&s_scroll, y, pressed);
    if (s_press != -3 && s_scroll.moved) s_press = -2;
    if (pressed) return;

    int p = s_press;
    s_press = -2;
    if (!tap || s_long_fired) { s_long_fired = false; return; }
    if (s_state == REC_SELECT) {
        if (p == -3) {
            if (s_sel_count > 0) delete_selected();
            else s_state = REC_IDLE;
        } else if (p >= 0) {
            s_info[p].selected = !s_info[p].selected;
            s_sel_count += s_info[p].selected ? 1 : -1;
        }
        return;
    }
    if (p == -1) start_recording();
    else if (p >= 0) media_play_playlist(&s_list, nullptr, s_list.n, p);   // the music player
}

static void recorder_destroy() {
    // Covered or closed. A recording in progress is finished and kept.
    if (s_state == REC_RECORDING) {
        audio_stop_record();
        s_state = REC_IDLE;
    }
}

static void recorder_gesture(Gesture g) {
    if (s_state == REC_RECORDING) return;
    if (g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_LEFT) {
        if (s_state == REC_SELECT) s_state = REC_IDLE;
        else ui_pop_screen();
    }
}

Screen recorder_screen = {
    "Recorder", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    recorder_create, recorder_draw, recorder_touch, recorder_tick, recorder_destroy, recorder_gesture,
    250, false, false, false, true,   // swipe right leaves selection mode first
};
