/*
 * game_piano.cpp
 * Piano Tiles, rebuilt for the round screen.
 *
 * Four lanes, one per quadrant. Tiles grow out of the centre and travel to
 * the rim; tap each one, in order, and its note plays - the song comes out
 * at your own rhythm. Each tap is judged by where the tile's front edge is
 * against the timing ring: Perfect / Great / Good, and a streak of good
 * taps builds a combo that multiplies the points. A tap in the wrong lane,
 * or a tile that gets away, is a Miss and costs a life. Long notes are
 * hold tiles: hold them right through for a bonus.
 *
 * Stars come from accuracy (how well every tile was timed); the song's
 * progress runs round the edge of the screen.
 *
 * Songs (piano_songs.cpp, plus /piano/*.txt on the SD card) are grouped by
 * genre; in each, clearing one unlocks the next. Every song can be played
 * on Easy (slower, fastest notes merged, 3 lives), Normal (2) or Hard
 * (full score, faster, 1). Stars, best score and accuracy are kept in NVS
 * per song and difficulty.
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
#include "hal_sd.h"
#include "app_settings_state.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// ---- geometry --------------------------------------------------------------------------
static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int R_IN = 54;              // tiles appear here (the score disc is inside)
static const int R_OUT = LCD_WIDTH / 2 - 1;
static const int R_HIT = 168;            // the timing ring
static const int R_PROG = R_OUT - 5;     // the progress ring round the edge
static const int GAP = 5;                // half-width of the gaps along the axes
static const int TPQ = 48;               // ticks per quarter note
static const float PI2 = 6.2831853f;

// lane = quadrant: 0 bottom-right, 1 bottom-left, 2 top-left, 3 top-right
static const int8_t LANE_SX[4] = { 1, -1, -1, 1 }, LANE_SY[4] = { 1, 1, -1, -1 };
static const uint16_t LANE_COL[4] = { COLOR565(0x40, 0xC8, 0xFF), COLOR565(0xB0, 0x7C, 0xFF),
                                      COLOR565(0xFF, 0x6E, 0xB4), COLOR565(0x5C, 0xE0, 0x9A) };
static const uint16_t GOLD = COLOR565(0xFF, 0xC8, 0x3C);
static const uint16_t GENRE_COL[PG_COUNT + 1] = { COLOR565(0x8C, 0x9E, 0xFF), COLOR565(0x5C, 0xE0, 0x9A),
                                                  COLOR565(0xFF, 0x6E, 0x6E), COLOR565(0xFF, 0xB0, 0x40),
                                                  COLOR565(0xB0, 0xB0, 0xB0) };

static const char *const DIFF_NAME[3] = { "Easy", "Normal", "Hard" };
static const uint16_t DIFF_COL[3] = { COLOR_GOOD, COLOR_WARN, COLOR_BAD };
static const int LIVES[3] = { 3, 2, 1 };

// judgements
enum Judge : uint8_t { J_PERFECT, J_GREAT, J_GOOD, J_MISS };
static const char *const JUDGE_TXT[4] = { "PERFECT", "GREAT", "GOOD", "MISS" };
static const uint16_t JUDGE_COL[4] = { GOLD, COLOR565(0x5C, 0xE0, 0xFF), COLOR_TEXT, COLOR_BAD };
static const int JUDGE_PTS[4] = { 100, 70, 40, 0 };
static const float JUDGE_ACC[4] = { 1.0f, 0.75f, 0.45f, 0.0f };
static const int WIN_PERFECT_MS = 70, WIN_GREAT_MS = 150;

// ---- songs -----------------------------------------------------------------------------
#define MAX_SONGS 80
#define MAX_SD_SONGS 24
#define G_SD PG_COUNT              // the SD card's "genre"
struct SongRef { const char *title, *composer, *notes; uint16_t bpm; uint8_t genre; };
struct Spark { float x, y, vx, vy; uint32_t t; uint16_t c; };
struct Popup { char txt[12]; int x, y; uint32_t t; uint16_t c; uint8_t big; };
struct Ripple { int16_t x, y; uint32_t t; uint16_t c; };
struct Flake { float x, y, vy, vx; uint16_t c; };

// The game's tables live in PSRAM while it's open (internal RAM is what
// Wi-Fi needs next to Bluetooth).
struct PianoMem {
    SongRef songs[MAX_SONGS];
    uint32_t saved[MAX_SONGS][3];   // per song and difficulty: stars | accuracy % | best score
    int16_t genre_list[PG_COUNT + 1][MAX_SONGS];
    int16_t genre_n[PG_COUNT + 1];
    char *sd_text[MAX_SD_SONGS];
    Spark sparks[32];
    Popup popups[5];
    Ripple ripples[8];
    Flake flakes[48];
    float ring_c[120], ring_s[120];  // the progress ring's dots
};
static PianoMem *s_m = nullptr;
static int s_nsongs = 0;

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

// ---- tiles -----------------------------------------------------------------------------
#define MAX_TILES 900
struct Tile {
    uint32_t start;    // ticks
    uint16_t len;      // ticks
    uint8_t midi;
    uint8_t lane;
    uint8_t state;     // 0 waiting, 1 hit, 2 missed
    uint8_t hold;      // a hold tile
    uint8_t judge;
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

// ---- progress (NVS "piano") ------------------------------------------------------------
// One key per song (by title, so adding songs never shuffles progress) and
// difficulty: bits 0-1 stars, 2-8 accuracy %, 9-31 best score.
static int stars_of(int i, int d) { return s_m->saved[i][d] & 3; }
static int acc_of(int i, int d) { return (s_m->saved[i][d] >> 2) & 127; }
static uint32_t best_of(int i, int d) { return s_m->saved[i][d] >> 9; }
static int best_stars(int i) {
    int b = 0;
    for (int d = 0; d < 3; d++) b = stars_of(i, d) > b ? stars_of(i, d) : b;
    return b;
}

static void save_key(int i, int d, char *key) {
    uint32_t h = 2166136261u;
    for (const char *c = s_m->songs[i].title; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    snprintf(key, 12, "%07x%d", (unsigned)(h & 0xFFFFFFF), d);
}

static void load_progress() {
    Preferences p;
    p.begin("piano", true);
    char key[12];
    for (int i = 0; i < s_nsongs; i++)
        for (int d = 0; d < 3; d++) {
            save_key(i, d, key);
            s_m->saved[i][d] = p.getUInt(key, 0);
        }
    p.end();
}

static void store_progress(int i, int d) {
    Preferences p;
    p.begin("piano", false);
    char key[12];
    save_key(i, d, key);
    p.putUInt(key, s_m->saved[i][d]);
    p.end();
}

static int s_diff = 1;   // the last one picked, remembered
static void load_diff() { Preferences p; p.begin("piano", true); s_diff = constrain(p.getUChar("diff", 1), 0, 2); p.end(); }
static void save_diff() { Preferences p; p.begin("piano", false); p.putUChar("diff", s_diff); p.end(); }

// position of song i within its genre
static int index_in_genre(int i) {
    int g = s_m->songs[i].genre;
    for (int k = 0; k < s_m->genre_n[g]; k++) if (s_m->genre_list[g][k] == i) return k;
    return 0;
}
static bool song_unlocked(int i) {
    int g = s_m->songs[i].genre;
    if (g == G_SD) return true;
    int k = index_in_genre(i);
    return k == 0 || best_stars(s_m->genre_list[g][k - 1]) > 0;
}

// ---- SD songs ----------------------------------------------------------------------------
// /piano/<name>.txt:  "title: ...", "composer: ...", "bpm: ..." lines, then notes.
static void load_sd_songs() {
    if (!sd_is_mounted()) return;
    File dir = SD.open("/piano");
    if (!dir || !dir.isDirectory()) return;
    int k = 0;
    for (File f = dir.openNextFile(); f && k < MAX_SD_SONGS && s_nsongs < MAX_SONGS; f = dir.openNextFile()) {
        const char *nm = f.name();
        int l = strlen(nm);
        if (f.isDirectory() || l < 5 || strcasecmp(nm + l - 4, ".txt") || f.size() > 16384) { f.close(); continue; }
        char *buf = (char *)heap_caps_malloc(f.size() + 1, MALLOC_CAP_SPIRAM);
        if (!buf) { f.close(); break; }
        int got = f.read((uint8_t *)buf, f.size());
        buf[got > 0 ? got : 0] = 0;
        f.close();
        s_m->sd_text[k++] = buf;
        SongRef &s = s_m->songs[s_nsongs++];
        s.title = "Song"; s.composer = "SD card"; s.bpm = 100; s.genre = G_SD; s.notes = buf;
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
    }
    dir.close();
}

static void build_song_list() {
    s_nsongs = 0;
    for (int i = 0; i < PIANO_SONG_COUNT && s_nsongs < MAX_SONGS; i++) {
        const PianoSong &p = PIANO_SONGS[i];
        s_m->songs[s_nsongs++] = { p.title, p.composer, p.notes, p.bpm, (uint8_t)(p.genre < PG_COUNT ? p.genre : 0) };
    }
    load_sd_songs();
    memset(s_m->genre_n, 0, sizeof(s_m->genre_n));
    for (int i = 0; i < s_nsongs; i++) {
        int g = s_m->songs[i].genre;
        s_m->genre_list[g][s_m->genre_n[g]++] = i;
    }
    load_progress();
    for (int k = 0; k < 120; k++) {
        float a = -1.5707963f + k * PI2 / 120;
        s_m->ring_c[k] = cosf(a);
        s_m->ring_s[k] = sinf(a);
    }
}

// ---- game state --------------------------------------------------------------------------
enum State : uint8_t { ST_MENU, ST_PICK, ST_PLAY, ST_PAUSE, ST_RESULT };
static State s_state = ST_MENU;
static int s_genre = 0;
static int s_song = 0;
static float s_ppt;            // pixels per tick
static float s_scroll;         // px
static float s_speed;          // px/s at the start of the song
static bool s_started;
static int s_next;             // first tile still waiting
static int s_lives, s_score, s_combo, s_max_combo;
static int s_count[4];         // judgements
static float s_acc_sum;        // accuracy points so far
static int s_holding = -1;     // tile being held
static float s_hold_from;      // scroll when the hold began
static uint32_t s_last_ms, s_end_ms, s_ring_flash_ms;
static uint16_t s_ring_flash_c;
static uint32_t s_lane_glow_ms[4];
static uint16_t s_lane_glow_c[4];
static bool s_cleared, s_new_best;
static int s_result_stars, s_result_acc;
static int s_spark_i = 0, s_popup_i = 0, s_ripple_i = 0;

static float tile_s(const Tile &t) { return t.start * s_ppt; }
static float tile_l(const Tile &t) { return t.len * s_ppt; }
static float r_front(const Tile &t) { return R_IN + s_scroll - tile_s(t); }
static float cur_speed() { return s_speed * (1.0f + 0.2f * (s_ntiles ? (float)s_next / s_ntiles : 0)); }

static uint16_t note_ms(const Tile &t) {
    float ms = t.len * 60000.0f / (s_m->songs[s_song].bpm * TPQ);
    return (uint16_t)constrain(ms * 1.1f, 160.0f, 1600.0f);
}
static uint16_t note_hz(int midi) { return (uint16_t)(440.0f * powf(2.0f, (midi - 69) / 12.0f)); }

// ---- effects -------------------------------------------------------------------------------
static void burst(int x, int y, uint16_t c, int n, float vmax) {
    for (int k = 0; k < n; k++) {
        Spark &p = s_m->sparks[s_spark_i++ % 32];
        float a = random(360) * 0.0174533f, v = vmax * (0.4f + random(60) / 100.0f);
        p = { (float)x, (float)y, cosf(a) * v, sinf(a) * v, millis(), c };
    }
}
static void popup(const char *txt, int x, int y, uint16_t c, bool big) {
    Popup &p = s_m->popups[s_popup_i++ % 5];
    snprintf(p.txt, sizeof(p.txt), "%s", txt);
    p.x = x; p.y = y; p.t = millis(); p.c = c; p.big = big;
}
static void ripple(int x, int y, uint16_t c) {
    Ripple &r = s_m->ripples[s_ripple_i++ % 8];
    r = { (int16_t)x, (int16_t)y, millis(), c };
}
// where in a lane a radius is
static void lane_point(int lane, float r, int *x, int *y) {
    *x = CX + (int)(LANE_SX[lane] * r * 0.7071f);
    *y = CY + (int)(LANE_SY[lane] * r * 0.7071f);
}

// ---- song start / end ----------------------------------------------------------------------
static void start_song(int song) {
    s_song = song;
    int diff = s_diff;
    if (!s_tiles) s_tiles = (Tile *)heap_caps_malloc(sizeof(Tile) * MAX_TILES, MALLOC_CAP_SPIRAM);
    if (!s_tiles) { ui_show_toast("Not enough memory", 1500); s_state = ST_MENU; return; }
    int n = parse_song(s_m->songs[song].notes);
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
    s_speed = s_ppt * TPQ * s_m->songs[song].bpm / 60.0f * MUL[diff];
    // The first tile waits with its front on the timing ring.
    s_scroll = tile_s(s_tiles[0]) + (R_HIT - R_IN);
    s_started = false;
    s_next = 0;
    s_lives = LIVES[diff];
    s_score = s_combo = s_max_combo = 0;
    memset(s_count, 0, sizeof(s_count));
    s_acc_sum = 0;
    s_holding = -1;
    s_end_ms = s_ring_flash_ms = 0;
    memset(s_lane_glow_ms, 0, sizeof(s_lane_glow_ms));
    memset(s_m->sparks, 0, sizeof(s_m->sparks));
    memset(s_m->popups, 0, sizeof(s_m->popups));
    memset(s_m->ripples, 0, sizeof(s_m->ripples));
    s_last_ms = millis();
    s_state = ST_PLAY;
}

static void finish(bool cleared) {
    s_cleared = cleared;
    s_end_ms = millis();
    s_result_acc = s_ntiles ? (int)(100.0f * s_acc_sum / s_ntiles + 0.5f) : 0;
    s_result_stars = !cleared ? 0 : s_result_acc >= 92 ? 3 : s_result_acc >= 75 ? 2 : 1;
    uint32_t &v = s_m->saved[s_song][s_diff];
    uint32_t stars = v & 3, acc = (v >> 2) & 127, best = v >> 9;
    s_new_best = cleared && (uint32_t)s_score > best;
    if (cleared) {
        if ((uint32_t)s_result_stars > stars) stars = s_result_stars;
        if ((uint32_t)s_result_acc > acc) acc = s_result_acc;
        if (s_new_best) best = s_score;
        uint32_t nv = stars | (acc << 2) | (best << 9);
        if (nv != v) { v = nv; store_progress(s_song, s_diff); }
    }
    // confetti for three stars
    for (int k = 0; k < 48; k++) {
        Flake &f = s_m->flakes[k];
        f = { (float)random(LCD_WIDTH), (float)(-random(300)), 60.0f + random(90), (random(40) - 20) * 1.0f,
              k % 3 == 0 ? GOLD : LANE_COL[k & 3] };
    }
    if (cleared) game_sfx_levelup();
    else game_sfx_gameover();
    s_state = ST_RESULT;
}

static void judge(int lane, Judge j, int x, int y) {
    s_count[j]++;
    if (j == J_MISS) {
        s_combo = 0;
        s_lives--;
        s_lane_glow_ms[lane] = millis();
        s_lane_glow_c[lane] = COLOR_BAD;
        s_ring_flash_ms = millis();
        s_ring_flash_c = COLOR_BAD;
        popup("MISS", x, y, COLOR_BAD, true);
        game_sfx_hit();
        if (s_lives <= 0) finish(false);
        return;
    }
    // points: the judgement, times the combo (up to 2x at 40)
    s_score += (int)(JUDGE_PTS[j] * (1.0f + fminf(s_combo, 40) / 40.0f));
    s_combo++;
    if (s_combo > s_max_combo) s_max_combo = s_combo;
    s_lane_glow_ms[lane] = millis();
    s_lane_glow_c[lane] = j == J_PERFECT ? GOLD : LANE_COL[lane];
    if (j == J_PERFECT) { s_ring_flash_ms = millis(); s_ring_flash_c = GOLD; }
    popup(JUDGE_TXT[j], x, y, JUDGE_COL[j], j == J_PERFECT);
    if (s_combo > 0 && s_combo % 25 == 0) {
        char b[12];
        snprintf(b, sizeof(b), "%d COMBO", s_combo);
        popup(b, CX, CY - 90, GOLD, true);
        game_sfx_powerup();
    }
}

static void hit(int i, int x, int y) {
    Tile &t = s_tiles[i];
    t.state = 1;
    t.hit_ms = millis();
    if (g_app_settings.sfx_enabled) audio_play_note(note_hz(t.midi), note_ms(t));
    // timing against the ring
    Judge j = J_PERFECT;
    if (s_started) {
        float ms = (r_front(t) - R_HIT) / (cur_speed() / 1000.0f);
        float a = fabsf(ms);
        j = a <= WIN_PERFECT_MS ? J_PERFECT : a <= WIN_GREAT_MS ? J_GREAT : J_GOOD;
    }
    t.judge = j;
    if (!t.hold) s_acc_sum += JUDGE_ACC[j];
    else { s_holding = i; s_hold_from = s_scroll; }
    judge(t.lane, j, x, y);
    ripple(x, y, j == J_PERFECT ? GOLD : LANE_COL[t.lane]);
    burst(x, y, j == J_PERFECT ? GOLD : LANE_COL[t.lane], j == J_PERFECT ? 10 : 6, j == J_PERFECT ? 220 : 160);
    s_started = true;
    while (s_next < s_ntiles && s_tiles[s_next].state) s_next++;
}

// A hold ends: let go (frac < 1) or held right through (frac = 1).
static void end_hold(float frac) {
    Tile &t = s_tiles[s_holding];
    s_holding = -1;
    s_acc_sum += JUDGE_ACC[t.judge] * (0.5f + 0.5f * frac);
    int beats = t.len / TPQ;
    int x, y;
    lane_point(t.lane, R_HIT - 30, &x, &y);
    if (frac >= 1.0f) {
        int bonus = 40 * (beats > 0 ? beats : 1) * (t.judge == J_PERFECT ? 2 : 1);
        s_score += bonus;
        char b[12];
        snprintf(b, sizeof(b), "HOLD +%d", bonus);
        popup(b, x, y, GOLD, true);
        burst(x, y, GOLD, 14, 240);
        game_sfx_score();
    } else {
        s_score += (int)(20 * beats * frac);
    }
}

// ---- drawing -------------------------------------------------------------------------------
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
    for (int k = 0; k < 3; k++) draw_star(g, cx + (k - 1) * gap, cy, r, k < n ? GOLD : COLOR_PANEL);
}

static void draw_tile(Arduino_GFX *g, const Tile &t, int i, uint32_t now) {
    float fr = r_front(t), br = fr - tile_l(t);
    uint16_t lc = LANE_COL[t.lane];
    if (t.state == 0) {
        // body: darker at the back, bright towards the front, a coloured edge
        float mid = br + (fr - br) * 0.45f;
        fill_quad_ring(g, t.lane, br + 2, mid, fx_lerp565(COLOR_TEXT, lc, 0.55f));
        fill_quad_ring(g, t.lane, mid, fr - 2, fx_lerp565(COLOR_TEXT, lc, 0.2f));
        // the next tile's edge glows inside the Perfect window
        bool ripe = i == s_next && fabsf(fr - R_HIT) < cur_speed() * WIN_PERFECT_MS / 1000.0f;
        fill_quad_ring(g, t.lane, fr - (ripe ? 9 : 6), fr - 2, ripe ? GOLD : lc);
        if (t.hold) fill_quad_ring(g, t.lane, br + 8, fr - 12, fx_lerp565(COLOR_TEXT, lc, 0.75f));
    } else if (t.state == 1) {
        if (t.hold && i == s_holding) {
            float prog = fminf(1.0f, (s_scroll - s_hold_from) / tile_l(t));
            fill_quad_ring(g, t.lane, br + 2, fr - 2, fx_scale565(lc, 9));
            fill_quad_ring(g, t.lane, fr - 2 - (tile_l(t) - 4) * prog, fr - 2, (now / 80) & 1 ? GOLD : lc);
        } else {
            uint32_t a = now - t.hit_ms;
            uint32_t k = a < 260 ? 16 - a * 16 / 260 : 0;
            if (k) fill_quad_ring(g, t.lane, br + 2, fr - 2, fx_scale565(t.judge == J_PERFECT ? GOLD : lc, k));
        }
    } else {
        fill_quad_ring(g, t.lane, br + 2, fr - 2, (now / 110) & 1 ? COLOR_BAD : fx_scale565(COLOR_BAD, 12));
    }
}

static void draw_play(Arduino_GFX *g) {
    uint32_t now = millis();
    // lanes, glowing for a moment after a hit (red after a miss)
    for (int l = 0; l < 4; l++) {
        uint32_t a = now - s_lane_glow_ms[l];
        uint16_t bg = COLOR565(0x07, 0x09, 0x0F);
        if (s_lane_glow_ms[l] && a < 300) bg = fx_lerp565(bg, s_lane_glow_c[l], 0.22f * (1.0f - a / 300.0f));
        fill_quad_ring(g, l, R_IN, R_OUT, bg);
    }
    // the timing ring
    {
        uint32_t a = now - s_ring_flash_ms;
        bool fl = s_ring_flash_ms && a < 200;
        uint16_t c = fl ? fx_lerp565(COLOR_TEXT_DIM, s_ring_flash_c, 1.0f - a / 200.0f) : fx_scale565(COLOR_TEXT, 12);
        for (int k = 0; k < 120; k += fl ? 1 : 2)
            g->fillCircle(CX + (int)(s_m->ring_c[k] * R_HIT), CY + (int)(s_m->ring_s[k] * R_HIT), fl ? 2 : 1, c);
    }
    // tiles
    for (int i = s_next > 4 ? s_next - 4 : 0; i < s_ntiles; i++) {
        Tile &t = s_tiles[i];
        float fr = r_front(t);
        if (fr - tile_l(t) > R_OUT) continue;
        if (fr < R_IN) break;
        draw_tile(g, t, i, now);
    }
    // "TAP" on the first tile
    if (!s_started && s_ntiles) {
        Tile &t = s_tiles[0];
        int x, y;
        lane_point(t.lane, r_front(t) - tile_l(t) / 2, &x, &y);
        float pu = 0.5f + 0.5f * sinf(now * 0.008f);
        ui_text_center(x, y, fx_lerp565(COLOR_BG, LANE_COL[t.lane], pu * 0.6f), "TAP", 2);
    }
    // ripples
    for (Ripple &r : s_m->ripples) {
        uint32_t a = now - r.t;
        if (!r.t || a > 320) continue;
        int rad = 8 + a * 46 / 320;
        uint16_t c = fx_scale565(r.c, 32 - a * 32 / 320);
        g->drawCircle(r.x, r.y, rad, c);
        g->drawCircle(r.x, r.y, rad + 1, c);
    }
    // sparks
    for (Spark &p : s_m->sparks) {
        uint32_t a = now - p.t;
        if (!p.t || a > 450) continue;
        float tt = a / 1000.0f;
        g->fillCircle((int)(p.x + p.vx * tt), (int)(p.y + p.vy * tt + 120 * tt * tt), a < 200 ? 2 : 1,
                      fx_scale565(p.c, 32 - a * 32 / 450));
    }
    // progress round the edge: a track, the part played, a glowing head
    {
        float prog = s_ntiles ? (float)s_next / s_ntiles : 0;
        int nfill = (int)(prog * 120 + 0.5f);
        bool fever = s_combo >= 20;
        for (int k = 0; k < 120; k++) {
            int x = CX + (int)(s_m->ring_c[k] * R_PROG), y = CY + (int)(s_m->ring_s[k] * R_PROG);
            if (k < nfill) {
                uint16_t c = fever ? LANE_COL[(k / 8 + now / 120) & 3] : fx_lerp565(DIFF_COL[s_diff], COLOR_TEXT, (float)k / 120);
                g->fillCircle(x, y, 2, c);
            } else {
                g->fillCircle(x, y, 1, fx_scale565(COLOR_TEXT, 6));
            }
        }
        if (nfill > 0) {
            int k = nfill - 1;
            float pu = 0.5f + 0.5f * sinf(now * 0.01f);
            g->fillCircle(CX + (int)(s_m->ring_c[k] * R_PROG), CY + (int)(s_m->ring_s[k] * R_PROG), 3 + (int)(pu * 2),
                          fx_lerp565(COLOR_TEXT, GOLD, pu));
        }
    }
    // centre: score, combo, lives - pulsing on the beat
    float beat = s_started ? fmodf(s_scroll / (s_ppt * TPQ), 1.0f) : 0.5f;
    int disc = R_IN - 7 + (int)(3 * powf(1.0f - beat, 3));
    g->fillCircle(CX, CY, disc, COLOR_PANEL);
    g->drawCircle(CX, CY, disc, s_combo >= 20 ? GOLD : fx_scale565(COLOR_TEXT, 8));
    char b[16];
    snprintf(b, sizeof(b), "%d", s_score);
    ui_text_center(CX, CY - 10, COLOR_TEXT, b, s_score < 10000 ? 2 : 1);
    if (s_combo >= 2) {
        snprintf(b, sizeof(b), "x%d", s_combo);
        ui_text_center(CX, CY + 12, s_combo >= 20 ? GOLD : COLOR_ACCENT, b, 1);
    }
    int nl = LIVES[s_diff];
    for (int k = 0; k < nl; k++)
        g->fillCircle(CX - 8 * (nl - 1) / 2 + k * 8, CY + 30, 2, k < s_lives ? COLOR_BAD : COLOR_BG);
    // judgement popups, rising and fading
    for (Popup &p : s_m->popups) {
        uint32_t a = now - p.t;
        if (!p.t || a > 600) continue;
        uint16_t c = fx_scale565(p.c, a < 400 ? 32 : 32 - (a - 400) * 32 / 200);
        ui_text_center(p.x, p.y - (int)(a * 0.05f), c, p.txt, p.big && a < 120 ? 2 : (p.big ? 2 : 1));
    }
}

// ---- menu ----------------------------------------------------------------------------------
static const int ROW_W = 340, ROW_H = 64, ROW_GAP = 8, LIST_TOP = 124;
static const int ROW_X = (LCD_WIDTH - ROW_W) / 2;
static UiScroll s_scroll_ui;
static int s_press = -1;
static bool s_prev = false;
static uint32_t s_genre_ms = 0;          // genre switch animation

static int n_genres() { return s_m->genre_n[G_SD] ? PG_COUNT + 1 : PG_COUNT; }
static const char *genre_name(int g) { return g == G_SD ? "SD card" : PIANO_GENRES[g]; }
static int list_h() { return LIST_TOP + s_m->genre_n[s_genre] * (ROW_H + ROW_GAP) + 90; }

static void open_genre(int g) {
    s_genre = g;
    s_genre_ms = millis();
    ui_scroll_reset(&s_scroll_ui, list_h() - LCD_HEIGHT, ROW_H + ROW_GAP);
    // scroll to the first song not yet cleared
    for (int k = 0; k < s_m->genre_n[g]; k++) {
        if (!best_stars(s_m->genre_list[g][k])) {
            int y = LIST_TOP + k * (ROW_H + ROW_GAP) - CY + ROW_H / 2;
            if (y > 0) s_scroll_ui.pos = y < s_scroll_ui.max ? y : s_scroll_ui.max;
            break;
        }
    }
}

static void draw_menu(Arduino_GFX *g) {
    uint32_t now = millis();
    int off = -ui_scroll_offset(&s_scroll_ui);
    int stars = 0;
    for (int i = 0; i < s_nsongs; i++) for (int d = 0; d < 3; d++) stars += stars_of(i, d);
    // header
    if (off > -110) {
        ui_text_center(CX, 40 + off, COLOR_TEXT, "Piano Tiles", 2);
        char b[24];
        snprintf(b, sizeof(b), "%d", stars);
        draw_star(g, CX - 14 - ui_text_width(b, 1) / 2, 64 + off, 6, GOLD);
        ui_text_center(CX + 2, 64 + off, COLOR_TEXT_DIM, b, 1);
        // genre switcher: < Classical >
        uint16_t gc = GENRE_COL[s_genre];
        float in = fminf(1.0f, (now - s_genre_ms) / 220.0f);
        g->fillRoundRect(CX - 140, 78 + off, 280, 34, 17, fx_scale565(gc, 6));
        ui_text_center(CX + (int)((1.0f - fx_ease_out_cubic(in)) * 30), 95 + off, fx_lerp565(COLOR_BG, gc, in),
                       genre_name(s_genre), 2);
        g->fillTriangle(CX - 126, 95 + off, CX - 116, 87 + off, CX - 116, 103 + off, s_press == -10 ? COLOR_TEXT : gc);
        g->fillTriangle(CX + 126, 95 + off, CX + 116, 87 + off, CX + 116, 103 + off, s_press == -11 ? COLOR_TEXT : gc);
        for (int k = 0; k < n_genres(); k++)
            g->fillCircle(CX - 7 * (n_genres() - 1) + k * 14, 116 + off, k == s_genre ? 3 : 2,
                          k == s_genre ? gc : COLOR_PANEL);
    }
    char t[48];
    int n = s_m->genre_n[s_genre];
    for (int k = 0; k < n; k++) {
        int i = s_m->genre_list[s_genre][k];
        int y = LIST_TOP + k * (ROW_H + ROW_GAP) + off;
        if (y + ROW_H < 0) continue;
        if (y > LCD_HEIGHT) break;
        // rows slide in after a genre switch
        int slide = (int)((1.0f - fx_ease_out_cubic(fminf(1.0f, (now - s_genre_ms) / (200.0f + k * 40)))) * 60);
        int rx = ROW_X + slide;
        bool open = song_unlocked(i);
        g->fillRoundRect(rx, y, ROW_W, ROW_H, 20, s_press == k ? ui_dim(COLOR_TEXT, 0.25f) : COLOR_PANEL);
        int ix = rx + 34, iy = y + ROW_H / 2;
        if (open) {
            g->fillCircle(ix, iy, 20, LANE_COL[k & 3]);
            snprintf(t, sizeof(t), "%d", k + 1);
            ui_text_center(ix, iy, COLOR_BG, t, 2);
        } else {   // a padlock
            g->fillCircle(ix, iy, 20, COLOR_BG);
            g->fillRoundRect(ix - 9, iy - 2, 18, 13, 3, COLOR_TEXT_DIM);
            g->drawCircle(ix, iy - 4, 6, COLOR_TEXT_DIM);
            g->drawCircle(ix, iy - 4, 5, COLOR_TEXT_DIM);
        }
        snprintf(t, sizeof(t), "%s", s_m->songs[i].title);
        ui_fit(t, 2, ROW_W - 64 - 92);
        ui_print(rx + 64, y + 12, 2, open ? COLOR_TEXT : COLOR_TEXT_DIM, t);
        snprintf(t, sizeof(t), "%s", s_m->songs[i].composer);
        ui_fit(t, 1, ROW_W - 64 - 92);
        ui_print(rx + 64, y + 39, 1, COLOR_TEXT_DIM, t);
        int bs = best_stars(i);
        if (bs) {
            draw_stars(g, rx + ROW_W - 46, iy - 8, 7, bs, 17);
            int ba = 0;
            for (int d = 0; d < 3; d++) ba = acc_of(i, d) > ba ? acc_of(i, d) : ba;
            snprintf(t, sizeof(t), "%d%%", ba);
            ui_text_center(rx + ROW_W - 46, iy + 13, COLOR_TEXT_DIM, t, 1);
        }
    }
}

// song card: difficulty and Play
static const int PILL_W = 112, PILL_H = 64, PILL_Y = 184, PLAY_Y = 352, PLAY_R = 46;
static int pill_x(int d) { return CX + (d - 1) * (PILL_W + 10); }

static void draw_pick(Arduino_GFX *g) {
    uint32_t now = millis();
    const SongRef &s = s_m->songs[s_song];
    uint16_t gc = GENRE_COL[s.genre];
    char t[48];
    ui_text_center(CX, 66, gc, genre_name(s.genre), 1);
    snprintf(t, sizeof(t), "%s", s.title);
    ui_fit(t, 3, 360);
    ui_text_center(CX, 100, COLOR_TEXT, t, 3);
    snprintf(t, sizeof(t), "%s  -  %d bpm", s.composer, s.bpm);
    ui_text_center(CX, 134, COLOR_TEXT_DIM, t, 1);
    for (int d = 0; d < 3; d++) {
        int x = pill_x(d) - PILL_W / 2;
        bool sel = d == s_diff;
        uint16_t c = DIFF_COL[d];
        g->fillRoundRect(x, PILL_Y, PILL_W, PILL_H, 18, sel ? fx_scale565(c, 10) : COLOR_PANEL);
        if (sel) { g->drawRoundRect(x, PILL_Y, PILL_W, PILL_H, 18, c); g->drawRoundRect(x + 1, PILL_Y + 1, PILL_W - 2, PILL_H - 2, 17, c); }
        ui_text_center(pill_x(d), PILL_Y + 20, sel ? COLOR_TEXT : COLOR_TEXT_DIM, DIFF_NAME[d], 2);
        draw_stars(g, pill_x(d), PILL_Y + 46, 7, stars_of(s_song, d), 18);
    }
    if (best_of(s_song, s_diff)) {
        snprintf(t, sizeof(t), "Best %lu  -  %d%%", (unsigned long)best_of(s_song, s_diff), acc_of(s_song, s_diff));
        ui_text_center(CX, PILL_Y + PILL_H + 22, COLOR_TEXT_DIM, t, 1);
    } else {
        snprintf(t, sizeof(t), "%d %s  -  %s", LIVES[s_diff], LIVES[s_diff] == 1 ? "life" : "lives",
                 s_diff == 0 ? "slower" : s_diff == 2 ? "faster, every note" : "full speed");
        ui_text_center(CX, PILL_Y + PILL_H + 22, COLOR_TEXT_DIM, t, 1);
    }
    // Play, breathing
    float pu = 0.5f + 0.5f * sinf(now * 0.005f);
    g->fillCircle(CX, PLAY_Y, PLAY_R + 4 + (int)(pu * 4), fx_scale565(DIFF_COL[s_diff], 6));
    g->fillCircle(CX, PLAY_Y, PLAY_R, s_press == 5 ? ui_dim(DIFF_COL[s_diff], 0.6f) : DIFF_COL[s_diff]);
    g->fillTriangle(CX - 12, PLAY_Y - 18, CX - 12, PLAY_Y + 18, CX + 20, PLAY_Y, COLOR_BG);
}

static void draw_result(Arduino_GFX *g) {
    uint32_t now = millis(), a = now - s_end_ms;
    // confetti for three stars
    if (s_result_stars == 3) {
        float dt = 0.016f;
        for (Flake &f : s_m->flakes) {
            f.y += f.vy * dt;
            f.x += f.vx * dt + sinf((now + f.vy * 10) * 0.004f) * 0.6f;
            if (f.y > LCD_HEIGHT) { f.y = -10; f.x = random(LCD_WIDTH); }
            if (f.y > 0) g->fillRect((int)f.x, (int)f.y, 4, 3, f.c);
        }
    }
    ui_text_center(CX, 74, s_cleared ? COLOR_TEXT : COLOR_BAD, s_cleared ? "Cleared!" : "Missed!", 3);
    char t[48];
    snprintf(t, sizeof(t), "%s  -  %s", s_m->songs[s_song].title, DIFF_NAME[s_diff]);
    ui_fit(t, 1, 320);
    ui_text_center(CX, 106, COLOR_TEXT_DIM, t, 1);
    for (int k = 0; k < 3; k++) {     // stars pop in one by one
        uint32_t at = 300 + k * 260;
        bool on = k < s_result_stars && a > at;
        float sc = on ? fminf(1.0f, (a - at) / 180.0f) : 1.0f;
        float pop = on ? 1.0f + 0.35f * sinf(sc * 3.14159f) : 1.0f;
        draw_star(g, CX + (k - 1) * 62, 156 - (k == 1 ? 8 : 0), (int)(22 * pop), on ? GOLD : COLOR_PANEL);
    }
    // score counts up
    float up = fminf(1.0f, a / 900.0f);
    snprintf(t, sizeof(t), "%d", (int)(s_score * fx_ease_out_cubic(up)));
    ui_text_center(CX, 214, COLOR_TEXT, t, 4);
    snprintf(t, sizeof(t), "%d%% accuracy  -  best combo %d", s_result_acc, s_max_combo);
    ui_text_center(CX, 248, s_new_best ? COLOR_GOOD : COLOR_TEXT_DIM, s_new_best ? "New best!" : t, 1);
    if (s_new_best) ui_text_center(CX, 266, COLOR_TEXT_DIM, t, 1);
    // judgement breakdown
    for (int j = 0; j < 4; j++) {
        int x = CX - 135 + j * 90;
        snprintf(t, sizeof(t), "%d", s_count[j]);
        ui_text_center(x, 296, JUDGE_COL[j], t, 2);
        ui_text_center(x, 318, COLOR_TEXT_DIM, JUDGE_TXT[j], 1);
    }
    bool next = s_cleared && index_in_genre(s_song) + 1 < s_m->genre_n[s_m->songs[s_song].genre];
    g->fillRoundRect(CX - 150, 344, 140, 50, 25, s_press == 0 ? ui_dim(COLOR_ACCENT, 0.6f) : COLOR_ACCENT);
    ui_text_center(CX - 80, 369, COLOR_BG, "Retry", 2);
    g->fillRoundRect(CX + 10, 344, 140, 50, 25, s_press == 1 ? ui_dim(COLOR_PANEL, 0.6f) : COLOR_PANEL);
    ui_text_center(CX + 80, 369, COLOR_TEXT, next ? "Next" : "Songs", 2);
}

static void draw_pause(Arduino_GFX *g) {
    draw_play(g);
    g->fillCircle(CX, CY, 150, COLOR_BG);
    g->drawCircle(CX, CY, 150, COLOR_PANEL);
    ui_text_center(CX, CY - 70, COLOR_TEXT, "Paused", 3);
    g->fillRoundRect(CX - 100, CY - 30, 200, 52, 26, COLOR_ACCENT);
    ui_text_center(CX, CY - 4, COLOR_BG, "Resume", 2);
    g->fillRoundRect(CX - 100, CY + 36, 200, 52, 26, COLOR_PANEL);
    ui_text_center(CX, CY + 62, COLOR_TEXT, "Quit song", 2);
}

// ---- screen callbacks --------------------------------------------------------------------------
static void piano_create() {
    if (!s_m) s_m = (PianoMem *)heap_caps_calloc(1, sizeof(PianoMem), MALLOC_CAP_SPIRAM);
    if (!s_m) { ui_show_toast("Not enough memory", 1500); return; }
    build_song_list();
    load_diff();
    s_state = ST_MENU;
    s_press = -1;
    s_prev = false;
    if (s_genre >= n_genres()) s_genre = 0;
    open_genre(s_genre);
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
    s_scroll += cur_speed() * dt;     // a little faster towards the end
    // a tile that got away
    if (s_next < s_ntiles) {
        Tile &t = s_tiles[s_next];
        if (r_front(t) - tile_l(t) > R_OUT - 4) {
            t.state = 2;
            int lane = t.lane, x, y;
            lane_point(lane, R_HIT, &x, &y);
            while (s_next < s_ntiles && s_tiles[s_next].state) s_next++;
            judge(lane, J_MISS, x, y);
            if (s_state != ST_PLAY) return;
        }
    }
    // hold held right through
    if (s_holding >= 0 && s_scroll - s_hold_from >= tile_l(s_tiles[s_holding])) end_hold(1.0f);
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
            float frac = (s_scroll - s_hold_from) / tile_l(t);
            end_hold(frac >= 0.9f ? 1.0f : frac);     // a hair early still counts
        }
        if (!edge) return;
        float dx = x - CX, dy = y - CY, r = sqrtf(dx * dx + dy * dy);
        int lane = dy >= 0 ? (dx >= 0 ? 0 : 1) : (dx < 0 ? 2 : 3);
        if (r < R_IN - 4) { s_state = ST_PAUSE; return; }
        if (s_next >= s_ntiles) return;
        Tile &t = s_tiles[s_next];
        if (r_front(t) < R_IN + 6) return;       // not out yet: too early, no harm
        if (lane == t.lane) hit(s_next, x, y);
        else judge(lane, J_MISS, x, y);           // wrong lane
        return;
    }
    if (s_state == ST_PAUSE) {
        if (!edge) return;
        if (abs(x - CX) < 100 && y > CY - 30 && y < CY + 22) { s_state = ST_PLAY; s_last_ms = millis(); }
        else if (abs(x - CX) < 100 && y > CY + 36 && y < CY + 88) s_state = ST_PICK;
        return;
    }
    if (s_state == ST_RESULT) {
        if (millis() - s_end_ms < 700) return;
        int b = (y > 344 && y < 394) ? (x < CX ? 0 : 1) : -1;
        if (edge) s_press = b;
        if (pressed) return;
        int p = s_press;
        s_press = -1;
        if (p != b || p < 0) return;
        if (p == 0) { start_song(s_song); return; }
        int g = s_m->songs[s_song].genre, k = index_in_genre(s_song);
        if (s_cleared && k + 1 < s_m->genre_n[g]) { s_song = s_m->genre_list[g][k + 1]; s_state = ST_PICK; }
        else { s_state = ST_MENU; open_genre(s_genre); }
        return;
    }
    if (s_state == ST_PICK) {
        int hitp = -1;
        for (int d = 0; d < 3; d++)
            if (abs(x - pill_x(d)) < PILL_W / 2 && y > PILL_Y && y < PILL_Y + PILL_H) hitp = d;
        if ((x - CX) * (x - CX) + (y - PLAY_Y) * (y - PLAY_Y) < (PLAY_R + 8) * (PLAY_R + 8)) hitp = 5;
        if (edge) s_press = hitp;
        if (pressed) return;
        int p = s_press;
        s_press = -1;
        if (p < 0 || p != hitp) return;
        if (p < 3) { s_diff = p; save_diff(); game_sfx_simon_note(p); return; }
        start_song(s_song);
        return;
    }
    // menu: genre arrows, then the list
    int off = ui_scroll_offset(&s_scroll_ui);
    int hy = 95 - off;
    int arrow = -1;
    if (abs(y - hy) < 22) {
        if (abs(x - (CX - 121)) < 28) arrow = -10;
        else if (abs(x - (CX + 121)) < 28) arrow = -11;
    }
    int cy = y + off, row = -1;
    if (arrow == -1 && x >= ROW_X && x <= ROW_X + ROW_W && cy >= LIST_TOP) {
        int k = (cy - LIST_TOP) / (ROW_H + ROW_GAP);
        if (k < s_m->genre_n[s_genre] && (cy - LIST_TOP) % (ROW_H + ROW_GAP) < ROW_H) row = k;
    }
    if (edge) s_press = arrow != -1 ? arrow : row;
    bool tap = ui_scroll_touch(&s_scroll_ui, y, pressed);
    if (s_scroll_ui.moved) s_press = -1;
    if (pressed) return;
    int p = s_press;
    s_press = -1;
    if (!tap) return;
    if (p == -10) { open_genre((s_genre + n_genres() - 1) % n_genres()); return; }
    if (p == -11) { open_genre((s_genre + 1) % n_genres()); return; }
    if (p < 0) return;
    int i = s_m->genre_list[s_genre][p];
    if (!song_unlocked(i)) { ui_show_toast("Clear the song before it to unlock", 1800); return; }
    s_song = i;
    s_state = ST_PICK;
}

static void piano_gesture(Gesture gst) {
    if (!s_m) { if (gst == GESTURE_SWIPE_RIGHT) ui_pop_screen(); return; }
    if (s_state == ST_MENU) {
        if (gst == GESTURE_SWIPE_LEFT) open_genre((s_genre + 1) % n_genres());
        else if (gst == GESTURE_SWIPE_RIGHT) {
            if (s_genre == 0) ui_pop_screen();
            else open_genre(s_genre - 1);
        }
    } else if (gst == GESTURE_SWIPE_RIGHT && (s_state == ST_PICK || s_state == ST_RESULT)) {
        s_state = ST_MENU;
        open_genre(s_genre);
    }
}

static void piano_destroy() {
    free(s_tiles);
    s_tiles = nullptr;
    if (s_m) for (int i = 0; i < MAX_SD_SONGS; i++) free(s_m->sd_text[i]);
    free(s_m);
    s_m = nullptr;
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
