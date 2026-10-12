/*
 * game_piano.cpp
 * Piano Tiles, rebuilt for the round screen.
 *
 * Four lanes, one per quadrant. Tiles grow out of the centre and travel
 * to the rim; tap each one, in order, before it leaves the screen - its
 * note plays as you tap, so the song comes out at your own rhythm. A tap
 * in the wrong lane, or a tile that gets away, costs a life. Long notes
 * are hold tiles: keep the finger down while they pass for a bonus.
 *
 * Songs (piano_songs.cpp, plus /piano/*.txt on the SD card) are levels:
 * clearing one unlocks the next, and each has Easy / Normal / Hard -
 * Easy merges the fastest notes and is slower with 3 lives, Hard is the
 * full score, faster, with one life. Stars (3 = no mistakes) and best
 * scores are kept in NVS.
 */
#include "game_piano.h"
#include "piano_songs.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "fx3d.h"
#include "game_audio.h"
#include "hal_audio.h"
#include "hal_nvs.h"
#include "hal_sd.h"
#include "app_settings_state.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// ---- geometry ------------------------------------------------------------------------
static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int R_IN = 54;            // tiles appear here (the score disc is inside)
static const int R_OUT = LCD_WIDTH / 2 - 1;
static const int GAP = 5;              // half-width of the gaps along the axes
static const int TPQ = 48;             // ticks per quarter note

// lane = quadrant: 0 bottom-right, 1 bottom-left, 2 top-left, 3 top-right
static const int8_t LANE_SX[4] = { 1, -1, -1, 1 }, LANE_SY[4] = { 1, 1, -1, -1 };
static const uint16_t LANE_COL[4] = { COLOR565(0x40, 0xC8, 0xFF), COLOR565(0xB0, 0x7C, 0xFF),
                                      COLOR565(0xFF, 0x6E, 0xB4), COLOR565(0x5C, 0xE0, 0x9A) };

// ---- songs ----------------------------------------------------------------------------
#define MAX_SD_SONGS 24
struct SongRef { const char *title, *composer, *notes; uint16_t bpm; bool sd; };
struct Spark { float x, y, vx, vy; uint32_t t; uint16_t c; };
// The game's tables live in PSRAM while it's open (internal RAM is
// what Wi-Fi needs next to Bluetooth).
struct PianoMem {
    SongRef songs[64];
    uint32_t saved[64];       // per song: 2 bits of stars per difficulty, best score above
    Spark sparks[24];
};
static PianoMem *s_m = nullptr;
#define s_songs  (s_m->songs)
#define s_saved  (s_m->saved)
#define s_sparks (s_m->sparks)
static int s_nsongs = 0;
static char *s_sd_text[MAX_SD_SONGS];   // PSRAM copies of the SD files

static int parse_note_name(const char *t, int n) {
    // C4 = 60
    static const int8_t BASE[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A..G
    if (n < 2) return -1;
    char c = t[0] & ~0x20;
    if (c < 'A' || c > 'G') return -1;
    int semi = BASE[c - 'A'], i = 1;
    if (t[i] == '#') { semi++; i++; }
    else if (t[i] == 'b') { semi--; i++; }
    if (i >= n) return -1;
    int oct = t[i] - '0';
    if (oct < 0 || oct > 8 || i + 1 != n) return -1;
    return (oct + 1) * 12 + semi;
}

// ---- tiles ----------------------------------------------------------------------------
#define MAX_TILES 900
struct Tile {
    uint32_t start;    // ticks
    uint16_t len;      // ticks
    uint8_t midi;
    uint8_t lane;
    uint8_t state;     // 0 waiting, 1 hit, 2 missed
    uint8_t hold;      // a hold tile
    uint32_t hit_ms;   // when it was hit (fade-out)
};
static Tile *s_tiles = nullptr;
static int s_ntiles = 0;

// Parses notes text into s_tiles (one per note/chord, rests leave gaps).
static int parse_song(const char *txt) {
    int n = 0;
    uint32_t t = 0;
    int last_len = TPQ;
    const char *p = txt;
    while (*p && n < MAX_TILES) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '|') p++;
        if (!*p) break;
        const char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '|') p++;
        int tl = p - tok;
        // split "pitches/len"
        const char *slash = (const char *)memchr(tok, '/', tl);
        int plen = slash ? slash - tok : tl;
        if (slash) {
            int den = atoi(slash + 1);
            if (den > 0 && den <= 64) {
                int len = 4 * TPQ / den;
                if (tok[tl - 1] == '.') len += len / 2;
                last_len = len;
            }
        }
        bool rest = plen == 1 && (tok[0] == 'R' || tok[0] == 'r');
        if (!rest) {
            int top = -1;
            const char *q = tok;
            while (q < tok + plen) {
                const char *e = (const char *)memchr(q, '+', tok + plen - q);
                if (!e) e = tok + plen;
                int m = parse_note_name(q, e - q);
                if (m > top) top = m;
                q = e + 1;
            }
            if (top >= 0) {
                Tile &tile = s_tiles[n++];
                memset(&tile, 0, sizeof(tile));
                tile.start = t;
                tile.len = last_len;
                tile.midi = top;
            }
        }
        t += last_len;
    }
    return n;
}

// ---- progress (NVS) ---------------------------------------------------------------------

static void save_key(int i, char *key) {
    if (!s_songs[i].sd) { snprintf(key, 8, "pn%02d", i); return; }
    uint32_t h = 2166136261u;                       // SD songs: by title
    for (const char *c = s_songs[i].title; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    snprintf(key, 8, "ps%04x", (unsigned)(h & 0xFFFF));
}
static int stars_of(int i, int d) { return (s_saved[i] >> (d * 2)) & 3; }
static uint32_t best_of(int i) { return s_saved[i] >> 8; }
static bool song_unlocked(int i) {
    if (i == 0 || s_songs[i].sd) return true;
    for (int d = 0; d < 3; d++) if (stars_of(i - 1, d)) return true;
    return false;
}
static bool diff_unlocked(int i, int d) { return song_unlocked(i) && (d == 0 || stars_of(i, d - 1) > 0); }

static void load_progress() {
    char key[8];
    for (int i = 0; i < s_nsongs; i++) {
        save_key(i, key);
        s_saved[i] = nvs_load_high_score(key);
    }
}

// ---- SD songs ---------------------------------------------------------------------------
// /piano/<name>.txt:  "title: ...", "composer: ...", "bpm: ..." lines, then notes.
static void load_sd_songs() {
    for (int i = 0; i < MAX_SD_SONGS; i++) { free(s_sd_text[i]); s_sd_text[i] = nullptr; }
    if (!sd_is_mounted()) return;
    File dir = SD.open("/piano");
    if (!dir || !dir.isDirectory()) return;
    int k = 0;
    for (File f = dir.openNextFile(); f && k < MAX_SD_SONGS && s_nsongs < 64; f = dir.openNextFile()) {
        const char *nm = f.name();
        int l = strlen(nm);
        if (f.isDirectory() || l < 5 || strcasecmp(nm + l - 4, ".txt") || f.size() > 16384) { f.close(); continue; }
        char *buf = (char *)heap_caps_malloc(f.size() + 1, MALLOC_CAP_SPIRAM);
        if (!buf) { f.close(); break; }
        int got = f.read((uint8_t *)buf, f.size());
        buf[got > 0 ? got : 0] = 0;
        f.close();
        s_sd_text[k] = buf;
        SongRef &s = s_songs[s_nsongs];
        s.title = "Song"; s.composer = "SD card"; s.bpm = 100; s.sd = true; s.notes = buf;
        // header lines
        char *line = buf;
        while (*line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            char *colon = strchr(line, ':');
            bool header = colon && colon - line < 10 && !strchr(line, '/');
            if (!header) { if (end) *end = '\n'; s.notes = line; break; }
            char *v = colon + 1;
            while (*v == ' ') v++;
            char *cr = strchr(v, '\r');
            if (cr) *cr = 0;
            if (!strncasecmp(line, "title", 5)) s.title = v;
            else if (!strncasecmp(line, "composer", 8)) s.composer = v;
            else if (!strncasecmp(line, "bpm", 3)) s.bpm = constrain(atoi(v), 30, 300);
            if (!end) { s.notes = ""; break; }
            line = end + 1;
            s.notes = line;
        }
        s_nsongs++;
        k++;
    }
    dir.close();
}

static void build_song_list() {
    s_nsongs = 0;
    for (int i = 0; i < PIANO_SONG_COUNT && s_nsongs < 64; i++)
        s_songs[s_nsongs++] = { PIANO_SONGS[i].title, PIANO_SONGS[i].composer, PIANO_SONGS[i].notes,
                                PIANO_SONGS[i].bpm, false };
    load_sd_songs();
    load_progress();
}

// ---- game state -----------------------------------------------------------------------
enum State : uint8_t { ST_MENU, ST_PICK, ST_PLAY, ST_PAUSE, ST_RESULT };
static State s_state = ST_MENU;
static const char *const DIFF_NAME[3] = { "Easy", "Normal", "Hard" };
static const uint16_t DIFF_COL[3] = { COLOR_GOOD, COLOR_WARN, COLOR_BAD };

static int s_song = 0, s_diff = 0;
static float s_ppt;            // pixels per tick
static float s_scroll;         // px
static float s_speed;          // px/s at the start of the song
static bool s_started;
static int s_next;             // first tile still waiting
static int s_lives, s_mistakes, s_score;
static int s_holding = -1;     // tile being held
static float s_hold_from;      // scroll when the hold began
static uint32_t s_last_ms, s_flash_ms, s_end_ms;
static int s_flash_lane = -1;
static bool s_cleared, s_new_best;
static int s_result_stars;

static int s_spark_i = 0;

static float tile_s(const Tile &t) { return t.start * s_ppt; }
static float tile_l(const Tile &t) { return t.len * s_ppt; }
// outer and inner radius of a tile right now
static float r_front(const Tile &t) { return R_IN + s_scroll - tile_s(t); }

static uint16_t note_ms(const Tile &t) {
    float ms = t.len * 60000.0f / (s_songs[s_song].bpm * TPQ);
    return (uint16_t)constrain(ms * 1.1f, 160.0f, 1600.0f);
}
static uint16_t note_hz(int midi) { return (uint16_t)(440.0f * powf(2.0f, (midi - 69) / 12.0f)); }

static void start_song(int song, int diff) {
    s_song = song;
    s_diff = diff;
    if (!s_tiles) s_tiles = (Tile *)heap_caps_malloc(sizeof(Tile) * MAX_TILES, MALLOC_CAP_SPIRAM);
    if (!s_tiles) { ui_show_toast("Not enough memory", 1500); s_state = ST_MENU; return; }
    int n = parse_song(s_songs[song].notes);
    // Easy: notes shorter than an eighth merge into the ones after them.
    if (diff == 0) {
        int w = 0;
        for (int i = 0; i < n; i++) {
            Tile t = s_tiles[i];
            while (t.len < TPQ / 2 && i + 1 < n && s_tiles[i + 1].start == t.start + t.len) {
                t.len += s_tiles[i + 1].len;
                i++;
            }
            s_tiles[w++] = t;
        }
        n = w;
    }
    // Lanes follow the melody: up = clockwise, down = anticlockwise, a
    // leap jumps across, a repeated note keeps turning the same way -
    // never the same lane twice in a row.
    int lane = 3, prev = n ? s_tiles[0].midi : 60, minlen = 9999, turn = 1;
    for (int i = 0; i < n; i++) {
        Tile &t = s_tiles[i];
        int d = t.midi - prev;
        if (d > 0) turn = 1;
        else if (d < 0) turn = 3;
        lane = (lane + (abs(d) > 4 ? 2 : turn)) & 3;
        if (i == 0) lane = 3;
        t.lane = lane;
        t.hold = t.len >= 2 * TPQ;
        prev = t.midi;
        if (t.len < minlen) minlen = t.len;
    }
    s_ntiles = n;
    if (!n) { ui_show_toast("This song has no notes", 1500); s_state = ST_MENU; return; }
    // The shortest tile is at least 28 px; quarter notes at least 60.
    s_ppt = fmaxf(60.0f / TPQ, 28.0f / minlen);
    static const float MUL[3] = { 0.85f, 1.0f, 1.3f };
    s_speed = s_ppt * TPQ * s_songs[song].bpm / 60.0f * MUL[diff];
    // The first tile waits half way out for the first tap.
    s_scroll = tile_s(s_tiles[0]) + tile_l(s_tiles[0]) + 70;
    s_started = false;
    s_next = 0;
    s_lives = diff == 0 ? 3 : diff == 1 ? 2 : 1;
    s_mistakes = s_score = 0;
    s_holding = -1;
    s_flash_lane = -1;
    s_end_ms = 0;
    memset(s_sparks, 0, sizeof(s_sparks));
    s_last_ms = millis();
    s_state = ST_PLAY;
}

static void finish(bool cleared) {
    s_cleared = cleared;
    s_end_ms = millis();
    s_result_stars = cleared ? (s_mistakes == 0 ? 3 : s_mistakes == 1 ? 2 : 1) : 0;
    uint32_t v = s_saved[s_song];
    int old = stars_of(s_song, s_diff);
    s_new_best = cleared && (uint32_t)s_score > best_of(s_song);
    if (s_result_stars > old) v = (v & ~(3u << (s_diff * 2))) | ((uint32_t)s_result_stars << (s_diff * 2));
    if (s_new_best) v = (v & 0xFF) | ((uint32_t)s_score << 8);
    if (v != s_saved[s_song]) {
        s_saved[s_song] = v;
        char key[8];
        save_key(s_song, key);
        nvs_save_high_score(key, v);
    }
    if (cleared) game_sfx_levelup();
    else game_sfx_gameover();
    s_state = ST_RESULT;
}

static void mistake(int lane) {
    s_mistakes++;
    s_lives--;
    s_flash_lane = lane;
    s_flash_ms = millis();
    game_sfx_hit();
    if (s_lives <= 0) finish(false);
}

static void burst(int x, int y, uint16_t c) {
    for (int k = 0; k < 6; k++) {
        Spark &p = s_sparks[s_spark_i++ % 24];
        float a = random(360) * 0.0174533f, v = 60 + random(120);
        p = { (float)x, (float)y, cosf(a) * v, sinf(a) * v, millis(), c };
    }
}

static int lane_at(int x, int y, float *r) {
    float dx = x - CX, dy = y - CY;
    *r = sqrtf(dx * dx + dy * dy);
    return dy >= 0 ? (dx >= 0 ? 0 : 1) : (dx < 0 ? 2 : 3);
}

static void hit(int i, int x, int y) {
    Tile &t = s_tiles[i];
    t.state = 1;
    t.hit_ms = millis();
    s_score++;
    if (g_app_settings.sfx_enabled) audio_play_note(note_hz(t.midi), note_ms(t));
    if (t.hold) { s_holding = i; s_hold_from = s_scroll; }
    burst(x, y, LANE_COL[t.lane]);
    s_started = true;
    while (s_next < s_ntiles && s_tiles[s_next].state) s_next++;
}

// ---- drawing ---------------------------------------------------------------------------
// The part of a quadrant between radii ri and ro, with GAP-wide gaps
// along the axes: one horizontal line per row.
static void fill_quad_ring(Arduino_GFX *g, int lane, float ri, float ro, uint16_t c) {
    if (ro > R_OUT) ro = R_OUT;
    if (ri < R_IN) ri = R_IN;
    if (ro <= ri) return;
    int sx = LANE_SX[lane], sy = LANE_SY[lane];
    float ro2 = ro * ro, ri2 = ri * ri;
    for (int dy = GAP; dy <= (int)ro; dy++) {
        float d2 = (float)dy * dy;
        if (d2 >= ro2) break;
        int xo = (int)sqrtf(ro2 - d2);
        int xi = d2 < ri2 ? (int)ceilf(sqrtf(ri2 - d2)) : 0;
        if (xi < GAP) xi = GAP;
        if (xo < xi) continue;
        int y = CY + sy * dy;
        if (sx > 0) g->drawFastHLine(CX + xi, y, xo - xi + 1, c);
        else g->drawFastHLine(CX - xo, y, xo - xi + 1, c);
    }
}

static void draw_star(Arduino_GFX *g, int cx, int cy, int r, uint16_t c) {
    float px[10], py[10];
    for (int k = 0; k < 10; k++) {
        float a = -1.5707963f + k * 0.6283185f, rr = (k & 1) ? r * 0.45f : r;
        px[k] = cx + cosf(a) * rr;
        py[k] = cy + sinf(a) * rr;
    }
    for (int k = 0; k < 10; k++) {
        int n = (k + 1) % 10;
        g->fillTriangle(cx, cy, (int)px[k], (int)py[k], (int)px[n], (int)py[n], c);
    }
}

static void draw_stars(Arduino_GFX *g, int cx, int cy, int r, int n, int gap) {
    for (int k = 0; k < 3; k++)
        draw_star(g, cx + (k - 1) * gap, cy, r, k < n ? COLOR565(0xFF, 0xC8, 0x3C) : COLOR_PANEL);
}

static void draw_play(Arduino_GFX *g) {
    uint32_t now = millis();
    // lanes
    for (int l = 0; l < 4; l++) {
        bool fl = l == s_flash_lane && now - s_flash_ms < 350;
        fill_quad_ring(g, l, R_IN, R_OUT, fl ? fx_scale565(COLOR_BAD, 10) : COLOR565(0x08, 0x0A, 0x10));
    }
    // tiles
    for (int i = s_next > 3 ? s_next - 3 : 0; i < s_ntiles; i++) {
        Tile &t = s_tiles[i];
        float fr = r_front(t), br = fr - tile_l(t);
        if (br > R_OUT) continue;
        if (fr < R_IN) break;
        uint16_t lc = LANE_COL[t.lane];
        if (t.state == 0) {
            fill_quad_ring(g, t.lane, br + 2, fr - 2, fx_lerp565(COLOR_TEXT, lc, 0.25f));
            fill_quad_ring(g, t.lane, fr - 6, fr - 2, lc);
            if (t.hold) {   // a stripe down the middle of hold tiles
                fill_quad_ring(g, t.lane, br + 8, fr - 10, fx_lerp565(COLOR_TEXT, lc, 0.55f));
            }
        } else if (t.state == 1) {
            if (t.hold && i == s_holding) {
                float prog = fminf(1.0f, (s_scroll - s_hold_from) / tile_l(t));
                fill_quad_ring(g, t.lane, br + 2, fr - 2, fx_scale565(lc, 10));
                fill_quad_ring(g, t.lane, fr - 2 - (tile_l(t) - 4) * prog, fr - 2, lc);
            } else {
                uint32_t a = now - t.hit_ms;
                uint32_t k = a < 300 ? 14 - a * 14 / 300 : 0;
                if (k) fill_quad_ring(g, t.lane, br + 2, fr - 2, fx_scale565(lc, k));
            }
        } else {
            fill_quad_ring(g, t.lane, br + 2, fr - 2, (now / 120) & 1 ? COLOR_BAD : fx_scale565(COLOR_BAD, 12));
        }
    }
    // "START" on the first tile
    if (!s_started && s_ntiles) {
        Tile &t = s_tiles[0];
        float rm = r_front(t) - tile_l(t) / 2;
        int x = CX + (int)(LANE_SX[t.lane] * rm * 0.7071f), y = CY + (int)(LANE_SY[t.lane] * rm * 0.7071f);
        ui_text_center(x, y, COLOR_BG, "TAP", 2);
    }
    // sparks
    for (Spark &p : s_sparks) {
        uint32_t a = now - p.t;
        if (!p.t || a > 400) continue;
        float tt = a / 1000.0f;
        g->fillCircle((int)(p.x + p.vx * tt), (int)(p.y + p.vy * tt), 2, fx_scale565(p.c, 32 - a * 32 / 400));
    }
    // centre: score, progress ring, lives
    g->fillCircle(CX, CY, R_IN - 6, COLOR_PANEL);
    float prog = s_ntiles ? (float)s_next / s_ntiles : 0;
    for (int k = 0; k < 48; k++) {
        float a = -1.5707963f + k * 0.1309f;
        g->fillCircle(CX + (int)(cosf(a) * (R_IN - 2)), CY + (int)(sinf(a) * (R_IN - 2)), 1,
                      k < prog * 48 ? DIFF_COL[s_diff] : COLOR_BG);
    }
    char b[12];
    snprintf(b, sizeof(b), "%d", s_score);
    ui_text_center(CX, CY - 6, COLOR_TEXT, b, s_score < 1000 ? 3 : 2);
    for (int k = 0; k < (s_diff == 0 ? 3 : s_diff == 1 ? 2 : 1); k++)
        g->fillCircle(CX - 10 * ((s_diff == 0 ? 3 : s_diff == 1 ? 2 : 1) - 1) / 2 + k * 10, CY + 22, 3,
                      k < s_lives ? COLOR_BAD : COLOR_BG);
}

// ---- menu ------------------------------------------------------------------------------
static const int ROW_W = 340, ROW_H = 66, ROW_GAP = 8, LIST_TOP = 104;
static const int ROW_X = (LCD_WIDTH - ROW_W) / 2;
static UiScroll s_scroll_ui;
static int s_press = -1;
static bool s_prev = false;

static int list_h() { return LIST_TOP + s_nsongs * (ROW_H + ROW_GAP) + 90; }

static void draw_menu(Arduino_GFX *g) {
    int off = -ui_scroll_offset(&s_scroll_ui);
    int total = 0, max_total = s_nsongs * 9;
    for (int i = 0; i < s_nsongs; i++) for (int d = 0; d < 3; d++) total += stars_of(i, d);
    if (40 + off > -40) {
        ui_text_center(CX, 54 + off, COLOR_TEXT, "Piano Tiles", 3);
        char b[32];
        snprintf(b, sizeof(b), "%d / %d stars", total, max_total);
        draw_star(g, CX - ui_text_width(b, 1) / 2 - 12, 84 + off, 6, COLOR565(0xFF, 0xC8, 0x3C));
        ui_text_center(CX + 4, 84 + off, COLOR_TEXT_DIM, b, 1);
    }
    char t[48];
    for (int i = 0; i < s_nsongs; i++) {
        int y = LIST_TOP + i * (ROW_H + ROW_GAP) + off;
        if (y + ROW_H < 0) continue;
        if (y > LCD_HEIGHT) break;
        bool open = song_unlocked(i);
        uint16_t bg = s_press == i ? ui_dim(COLOR_TEXT, 0.25f) : COLOR_PANEL;
        g->fillRoundRect(ROW_X, y, ROW_W, ROW_H, 20, bg);
        int ix = ROW_X + 34, iy = y + ROW_H / 2;
        if (open) {
            g->fillCircle(ix, iy, 20, LANE_COL[i & 3]);
            snprintf(t, sizeof(t), "%d", i + 1);
            ui_text_center(ix, iy, COLOR_BG, t, 2);
        } else {   // a padlock
            g->fillCircle(ix, iy, 20, COLOR_BG);
            g->fillRoundRect(ix - 9, iy - 2, 18, 13, 3, COLOR_TEXT_DIM);
            g->drawCircle(ix, iy - 4, 6, COLOR_TEXT_DIM);
            g->drawCircle(ix, iy - 4, 5, COLOR_TEXT_DIM);
        }
        snprintf(t, sizeof(t), "%s", s_songs[i].title);
        ui_fit(t, 2, ROW_W - 64 - 96);
        ui_print(ROW_X + 64, y + 13, 2, open ? COLOR_TEXT : COLOR_TEXT_DIM, t);
        snprintf(t, sizeof(t), "%s", s_songs[i].composer);
        ui_fit(t, 1, ROW_W - 64 - 96);
        ui_print(ROW_X + 64, y + 40, 1, COLOR_TEXT_DIM, t);
        // best difficulty's stars, coloured by that difficulty
        int bd = -1;
        for (int d = 0; d < 3; d++) if (stars_of(i, d)) bd = d;
        if (bd >= 0) {
            draw_stars(g, ROW_X + ROW_W - 48, iy - 6, 7, stars_of(i, bd), 17);
            ui_text_center(ROW_X + ROW_W - 48, iy + 14, DIFF_COL[bd], DIFF_NAME[bd], 1);
        }
    }
}

// difficulty picker over the list
static const int PILL_W = 260, PILL_H = 58, PICK_TOP = 168;

static void draw_pick(Arduino_GFX *g) {
    char t[48];
    snprintf(t, sizeof(t), "%s", s_songs[s_song].title);
    ui_fit(t, 3, 360);
    ui_text_center(CX, 92, COLOR_TEXT, t, 3);
    ui_text_center(CX, 126, COLOR_TEXT_DIM, s_songs[s_song].composer, 1);
    for (int d = 0; d < 3; d++) {
        int y = PICK_TOP + d * (PILL_H + 12);
        bool open = diff_unlocked(s_song, d);
        uint16_t c = open ? DIFF_COL[d] : COLOR_PANEL;
        g->fillRoundRect(CX - PILL_W / 2, y, PILL_W, PILL_H, PILL_H / 2,
                         s_press == d ? ui_dim(c, 0.6f) : open ? fx_scale565(c, 9) : COLOR_PANEL);
        g->drawRoundRect(CX - PILL_W / 2, y, PILL_W, PILL_H, PILL_H / 2, c);
        ui_print(CX - PILL_W / 2 + 28, y + PILL_H / 2 - 8, 2, open ? COLOR_TEXT : COLOR_TEXT_DIM, DIFF_NAME[d]);
        if (open) draw_stars(g, CX + 70, y + PILL_H / 2, 8, stars_of(s_song, d), 20);
        else ui_text_center(CX + 70, y + PILL_H / 2, COLOR_TEXT_DIM, d == 1 ? "Clear Easy" : "Clear Normal", 1);
    }
    snprintf(t, sizeof(t), "Best %lu", (unsigned long)best_of(s_song));
    if (best_of(s_song)) ui_text_center(CX, PICK_TOP + 3 * (PILL_H + 12) + 10, COLOR_TEXT_DIM, t, 1);
}

static void draw_result(Arduino_GFX *g) {
    uint32_t a = millis() - s_end_ms;
    ui_text_center(CX, 110, s_cleared ? COLOR_TEXT : COLOR_BAD, s_cleared ? "Cleared!" : "Missed!", 3);
    char t[48];
    snprintf(t, sizeof(t), "%s  -  %s", s_songs[s_song].title, DIFF_NAME[s_diff]);
    ui_fit(t, 1, 320);
    ui_text_center(CX, 144, COLOR_TEXT_DIM, t, 1);
    for (int k = 0; k < 3; k++) {     // stars pop in one by one
        bool on = k < s_result_stars && a > 250u + k * 220u;
        float sc = on ? fminf(1.0f, (a - 250 - k * 220) / 160.0f) : 1.0f;
        draw_star(g, CX + (k - 1) * 62, 200, (int)(24 * (on ? 0.6f + 0.4f * sc : 1.0f)),
                  on ? COLOR565(0xFF, 0xC8, 0x3C) : COLOR_PANEL);
    }
    snprintf(t, sizeof(t), "%d", s_score);
    ui_text_center(CX, 262, COLOR_TEXT, t, 4);
    ui_text_center(CX, 296, s_new_best ? COLOR_GOOD : COLOR_TEXT_DIM,
                   s_new_best ? "New best!" : (snprintf(t, sizeof(t), "Best %lu", (unsigned long)best_of(s_song)), t), 1);
    bool next = s_cleared && s_song + 1 < s_nsongs && song_unlocked(s_song + 1);
    g->fillRoundRect(CX - 150, 330, 140, 50, 25, s_press == 0 ? ui_dim(COLOR_ACCENT, 0.6f) : COLOR_ACCENT);
    ui_text_center(CX - 80, 355, COLOR_BG, "Retry", 2);
    g->fillRoundRect(CX + 10, 330, 140, 50, 25, s_press == 1 ? ui_dim(COLOR_PANEL, 0.6f) : COLOR_PANEL);
    ui_text_center(CX + 80, 355, COLOR_TEXT, next ? "Next" : "Songs", 2);
}

static void draw_pause(Arduino_GFX *g) {
    draw_play(g);
    g->fillCircle(CX, CY, 150, COLOR_BG);
    ui_text_center(CX, CY - 70, COLOR_TEXT, "Paused", 3);
    g->fillRoundRect(CX - 100, CY - 30, 200, 52, 26, COLOR_ACCENT);
    ui_text_center(CX, CY - 4, COLOR_BG, "Resume", 2);
    g->fillRoundRect(CX - 100, CY + 36, 200, 52, 26, COLOR_PANEL);
    ui_text_center(CX, CY + 62, COLOR_TEXT, "Quit song", 2);
}

// ---- screen callbacks -----------------------------------------------------------------------
static void piano_create() {
    if (!s_m) s_m = (PianoMem *)heap_caps_calloc(1, sizeof(PianoMem), MALLOC_CAP_SPIRAM);
    if (!s_m) { s_nsongs = 0; ui_show_toast("Not enough memory", 1500); return; }
    build_song_list();
    s_state = ST_MENU;
    s_press = -1;
    s_prev = false;
    ui_scroll_reset(&s_scroll_ui, list_h() - LCD_HEIGHT, ROW_H + ROW_GAP);
    // open on the first song not yet cleared
    for (int i = 0; i < s_nsongs; i++) {
        if (!stars_of(i, 0)) {
            int y = LIST_TOP + i * (ROW_H + ROW_GAP) - CY + ROW_H / 2;
            if (y > 0) s_scroll_ui.pos = y < s_scroll_ui.max ? y : s_scroll_ui.max;
            break;
        }
    }
}

static void piano_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g || !s_m) return;
    switch (s_state) {
    case ST_MENU: draw_menu(g); break;
    case ST_PICK: draw_pick(g); break;
    case ST_PLAY: draw_play(g); break;
    case ST_PAUSE: draw_pause(g); break;
    case ST_RESULT: draw_result(g); break;
    }
}

static void piano_tick() {
    if (!s_m) return;
    if (s_state == ST_MENU) { ui_scroll_tick(&s_scroll_ui); return; }
    if (s_state != ST_PLAY) { s_last_ms = millis(); return; }
    uint32_t now = millis();
    float dt = (now - s_last_ms) / 1000.0f;
    if (dt > 0.05f) dt = 0.05f;
    s_last_ms = now;
    if (!s_started) return;
    float prog = s_ntiles ? (float)s_next / s_ntiles : 0;
    s_scroll += s_speed * (1.0f + 0.2f * prog) * dt;   // a little faster towards the end
    // a tile that got away
    if (s_next < s_ntiles) {
        Tile &t = s_tiles[s_next];
        if (r_front(t) - tile_l(t) > R_OUT - 4) {
            t.state = 2;
            int lane = t.lane;
            while (s_next < s_ntiles && s_tiles[s_next].state) s_next++;
            mistake(lane);
            if (s_state != ST_PLAY) return;
        }
    }
    // hold finished
    if (s_holding >= 0) {
        Tile &t = s_tiles[s_holding];
        if (s_scroll - s_hold_from >= tile_l(t)) { s_score += 2; s_holding = -1; game_sfx_score(); }
    }
    if (s_next >= s_ntiles && s_holding < 0) {
        // last tile played: wait for it to clear the screen
        Tile &t = s_tiles[s_ntiles - 1];
        if (r_front(t) - tile_l(t) > R_OUT || s_scroll > tile_s(t) + tile_l(t) + 400) finish(true);
    }
}

static void piano_touch(int x, int y, bool pressed) {
    if (!s_m) return;
    bool edge = pressed && !s_prev;
    s_prev = pressed;
    if (s_state == ST_PLAY) {
        if (!pressed && s_holding >= 0) {     // let go of a hold tile early
            Tile &t = s_tiles[s_holding];
            s_score += (int)(2 * fminf(1.0f, (s_scroll - s_hold_from) / tile_l(t)));
            s_holding = -1;
        }
        if (!edge) return;
        float r;
        int lane = lane_at(x, y, &r);
        if (r < R_IN - 4) { s_state = ST_PAUSE; return; }
        if (s_next >= s_ntiles) return;
        Tile &t = s_tiles[s_next];
        if (r_front(t) < R_IN + 6) return;       // not out yet: too early, no harm
        if (lane == t.lane) hit(s_next, x, y);
        else mistake(lane);
        return;
    }
    if (s_state == ST_PAUSE) {
        if (!edge) return;
        if (abs(x - CX) < 100 && y > CY - 30 && y < CY + 22) { s_state = ST_PLAY; s_last_ms = millis(); }
        else if (abs(x - CX) < 100 && y > CY + 36 && y < CY + 88) s_state = ST_PICK;
        return;
    }
    if (s_state == ST_RESULT) {
        if (millis() - s_end_ms < 600) return;
        int b = (y > 330 && y < 380) ? (x < CX ? 0 : 1) : -1;
        if (edge) s_press = b;
        if (pressed) return;
        int p = s_press;
        s_press = -1;
        if (p != b || p < 0) return;
        if (p == 0) start_song(s_song, s_diff);
        else if (s_cleared && s_song + 1 < s_nsongs && song_unlocked(s_song + 1)) { s_song++; s_state = ST_PICK; }
        else s_state = ST_MENU;
        return;
    }
    if (s_state == ST_PICK) {
        int d = -1;
        for (int k = 0; k < 3; k++) {
            int py = PICK_TOP + k * (PILL_H + 12);
            if (abs(x - CX) < PILL_W / 2 && y > py && y < py + PILL_H) d = k;
        }
        if (edge) s_press = d;
        if (pressed) return;
        int p = s_press;
        s_press = -1;
        if (p < 0 || p != d) return;
        if (!diff_unlocked(s_song, p)) { ui_show_toast(p ? "Clear the easier level first" : "Locked", 1500); return; }
        start_song(s_song, p);
        return;
    }
    // menu
    int cy = y + ui_scroll_offset(&s_scroll_ui);
    int row = -1;
    if (x >= ROW_X && x <= ROW_X + ROW_W && cy >= LIST_TOP) {
        int i = (cy - LIST_TOP) / (ROW_H + ROW_GAP);
        if (i < s_nsongs && (cy - LIST_TOP) % (ROW_H + ROW_GAP) < ROW_H) row = i;
    }
    if (edge) s_press = row;
    bool tap = ui_scroll_touch(&s_scroll_ui, y, pressed);
    if (s_scroll_ui.moved) s_press = -1;
    if (pressed) return;
    int p = s_press;
    s_press = -1;
    if (!tap || p < 0) return;
    if (!song_unlocked(p)) { ui_show_toast("Clear the song before it to unlock", 1800); return; }
    s_song = p;
    s_state = ST_PICK;
}

static void piano_gesture(Gesture gst) {
    if (gst != GESTURE_SWIPE_RIGHT) return;
    if (s_state == ST_MENU) ui_pop_screen();
    else if (s_state == ST_PICK || s_state == ST_RESULT) s_state = ST_MENU;
}

static void piano_destroy() {
    free(s_tiles);
    s_tiles = nullptr;
    free(s_m);
    s_m = nullptr;
    for (int i = 0; i < MAX_SD_SONGS; i++) { free(s_sd_text[i]); s_sd_text[i] = nullptr; }
}

Screen piano_screen = {
    nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    piano_create, piano_draw, piano_touch, piano_tick, piano_destroy, piano_gesture,
    0, true,          // never dim or lock mid-song
    false, false,
    true,             // no_drag_back: swipes in the song are taps
    true,             // hide_status: the whole dial is the playfield
    true,             // fast_taps: quick runs of notes
};
