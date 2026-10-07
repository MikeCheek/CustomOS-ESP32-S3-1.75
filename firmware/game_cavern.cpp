/*
 * game_cavern.cpp - "Crystal Cavern"
 *
 * An original cascading-grid cluster-pays game with virtual currency:
 * 5 columns x 3 rows (the standard video-slot shape), connected-
 * cluster wins, winning symbols visibly shrink away before anything
 * new appears, survivors then drop and new symbols fall in from
 * above, a growing cascade multiplier shown as a ring of dots around
 * the top edge, an explosive Wild, and a Scatter that - on 3 or more
 * anywhere on the grid - triggers a dedicated celebration (pulsing
 * scatters, a banner, a pause) before awarding free spins with a
 * multiplier that carries over and climbs across them. A Treasure
 * Chest "burst" bonus rounds out a large chest cluster. Original
 * theme, symbol set and paytable throughout - not a reproduction of
 * any specific commercial product.
 *
 * Cascade animation, in order: PH_HIGHLIGHT flashes the winning
 * cells in place (and runs the scatter celebration when triggered);
 * PH_CLEAR then visibly shrinks those same cells down to nothing -
 * this is the step that makes symbols actually leave before anything
 * new shows up, rather than snapping straight to the post-clear
 * grid; only once that finishes does apply_clear_and_fall() run and
 * PH_FALL drop the survivors and new symbols into place.
 *
 * Drawing note: only fillRect/fillRoundRect/fillCircle/fillTriangle
 * are used - no drawRoundRect (documented elsewhere in this project
 * as unreliable on this display's CO5300 driver), no drawCircle
 * outline, no drawBitmap. The ring indicator is many small fillCircle
 * dots at a fixed radius from screen center - safe by construction
 * since that radius is smaller than the screen's own. Every other
 * position below was checked against the display's actual safe-area
 * radius before being chosen - the grid's own corner distance was
 * recomputed for its new 5x3 shape and larger cell size, not assumed
 * to still be safe just because the old square grid was.
 *
 * Touch handling: every tap is rising-edge gated (tap_edge), not raw
 * `pressed` - see this session's project-wide touch-handling fixes
 * for why.
 */
#include "game_cavern.h"
#include "config.h"
#include "board_pins.h"
#include "hal_controller.h"
#include "hal_save.h"
#include "game_audio.h"
#include "ui.h"
#include <Arduino_GFX_Library.h>
#include <string.h>
#include <math.h>

#define GRID_W 5
#define GRID_H 3
#define NUM_REGULAR 7
#define MOON_SYMBOL 6
#define WILD_SYMBOL 7
#define SCATTER_SYMBOL 8
#define TREASURE_SYMBOL 5
#define EMPTY_CELL (-1)
#define START_MONEY 500
#define BET_MIN 5
#define FREESPIN_TRIGGER 3
#define CELL_SIZE 64
#define CELL_GAP 6
#define GRID_TOTAL_W (GRID_W * CELL_SIZE + (GRID_W - 1) * CELL_GAP)
#define GRID_TOTAL_H (GRID_H * CELL_SIZE + (GRID_H - 1) * CELL_GAP)
#define GRID_LEFT ((LCD_WIDTH - GRID_TOTAL_W) / 2)
#define GRID_TOP ((LCD_HEIGHT - GRID_TOTAL_H) / 2)
#define FALL_DURATION_MS 350
#define CLEAR_DURATION_MS 280
#define DEG2RAD 0.0174533f

enum SlotState { SL_IDLE, SL_PLAYING, SL_ROUND_END, SL_GAMEOVER };
enum Phase { PH_FALL, PH_HIGHLIGHT, PH_CLEAR };

static int8_t s_grid[GRID_H][GRID_W];
static bool s_marked[GRID_H][GRID_W];
static int s_fall_offset[GRID_H][GRID_W]; // pixels above final resting position at the start of a fall animation

static SlotState s_state;
static Phase s_phase;
static uint32_t s_phase_ms;

static int32_t s_money;
static int32_t s_bet = 20;
static int32_t s_round_win;
static int32_t s_step_win;
static int s_cascade_step;
static int s_wild_detonations;
static bool s_chest_burst_this_step;
static bool s_scatter_bonus_this_step;
static int s_scatter_bonus_spins_awarded;

static bool s_free_spins_active;
static int s_free_spins_left;
static int s_free_spins_total;   // spins awarded this free-spin session (for the bottom arc)
static int s_free_spin_mult_base; // the "minimum multiplier" floor - rises as natural crystals win during free spins (evaluate_clusters()), can jump to 60-70x on the money milestone (check_freespin_milestone())
static int32_t s_freespin_session_total; // accumulated winnings across the whole free-spin session (not just the current spin - s_round_win resets every spin, this doesn't), for the milestone check
static bool s_milestone_triggered_this_session;
#define MILESTONE_THRESHOLD_MULT 50 // accumulated free-spin winnings reaching this many multiples of bet triggers the big floor jump

static bool s_prev_pressed;

// ---- Persistence --------------------------------------------------------
#define CAVERN_SAVE_MAGIC 0x43415633u // "CAV3" - bumped from the 5x5 grid's save format
struct CavernSave { uint32_t magic; int32_t money; };

static void load_money() {
    CavernSave s;
    if (game_load_blob("cavern_money", &s, sizeof(s)) && s.magic == CAVERN_SAVE_MAGIC) s_money = s.money;
    else s_money = START_MONEY;
}
static void save_money() {
    CavernSave s;
    s.magic = CAVERN_SAVE_MAGIC;
    s.money = s_money;
    game_save_blob("cavern_money", &s, sizeof(s));
}

// ---- Symbol data --------------------------------------------------------
static const uint16_t SYMBOL_COLOR[NUM_REGULAR] = {
    (uint16_t)COLOR565(0x50, 0x90, 0xFF), // blue gem
    (uint16_t)COLOR565(0x40, 0xD0, 0x70), // green gem
    (uint16_t)COLOR565(0x38, 0x28, 0x48), // bat
    (uint16_t)COLOR565(0x2A, 0x2A, 0x30), // spider
    (uint16_t)COLOR565(0xE8, 0xE0, 0xC8), // skull
    (uint16_t)COLOR565(0x9A, 0x62, 0x30), // treasure chest
    (uint16_t)COLOR565(0xC8, 0xD8, 0xF0), // moon crystal - pale silver-blue
};
static const float SYMBOL_BASE_VALUE[NUM_REGULAR] = { 0.3f, 0.3f, 0.4f, 0.5f, 0.8f, 1.5f, 2.5f };
static const uint16_t WILD_COLOR = (uint16_t)COLOR565(0xFF, 0xFF, 0xFF);
static const uint16_t SCATTER_COLOR = (uint16_t)COLOR565(0x30, 0xE0, 0xD0);
#define WILD_BASE_VALUE 3.0f

// Explosion resistance: Treasure Chest and Moon Crystal are the two
// "premium" tiers strong enough to withstand a neighboring crystal's
// blast, same as Wild/Scatter - see explode_from_matched() below.
// Everything else (the five lower symbols) is destructible.
static bool symbol_resists_explosion(int8_t sym) {
    return sym == TREASURE_SYMBOL || sym == MOON_SYMBOL || sym == WILD_SYMBOL || sym == SCATTER_SYMBOL;
}

static int8_t random_symbol() {
    int r = random(100);
    if (r < 3) return WILD_SYMBOL;
    if (r < 5) return SCATTER_SYMBOL;
    if (r < 7) return MOON_SYMBOL; // rare premium tier, similar odds to Wild/Scatter
    r = random(93);
    if (r < 20) return 0;
    if (r < 39) return 1;
    if (r < 56) return 2;
    if (r < 71) return 3;
    if (r < 83) return 4;
    return 5;
}

static float size_multiplier(int size) {
    if (size >= 11) return 8.0f;
    if (size >= 9) return 4.0f;
    if (size >= 7) return 2.0f;
    return 1.0f;
}
static int cascade_tier(int step) { return step >= 4 ? 4 : step; }
static const int CASCADE_MULT[5] = { 1, 2, 3, 5, 8 };

// ---- Grid helpers -------------------------------------------------------
static void clear_marks() {
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++)
            s_marked[r][c] = false;
}

static int flood_collect(int r0, int c0, int8_t sym, bool visited[GRID_H][GRID_W], int out_r[GRID_W * GRID_H], int out_c[GRID_W * GRID_H]) {
    int stack_r[GRID_W * GRID_H], stack_c[GRID_W * GRID_H];
    int sp = 0, count = 0;
    stack_r[sp] = r0; stack_c[sp] = c0; sp++;
    visited[r0][c0] = true;
    while (sp > 0) {
        sp--;
        int r = stack_r[sp], c = stack_c[sp];
        out_r[count] = r; out_c[count] = c; count++;
        int dr[4] = { -1, 1, 0, 0 }, dc[4] = { 0, 0, -1, 1 };
        for (int k = 0; k < 4; k++) {
            int nr = r + dr[k], nc = c + dc[k];
            if (nr < 0 || nr >= GRID_H || nc < 0 || nc >= GRID_W) continue;
            if (visited[nr][nc]) continue;
            if (s_grid[nr][nc] != sym) continue;
            visited[nr][nc] = true;
            stack_r[sp] = nr; stack_c[sp] = nc; sp++;
        }
    }
    return count;
}

// Iterative flood-fill (explicit stack, not recursion - same pattern
// as flood_collect() above) chain-destroying every destructible
// crystal reachable, through shared edges, from any already-matched
// cell. Treasure Chest and Moon Crystal (and Wild/Scatter) act as a
// firewall: they stop the chain from propagating through them and are
// never destroyed by it themselves - only their own winning cluster
// removes them. This is what makes a win potentially ripple across
// the whole grid rather than stay confined to the cells that actually
// matched.
static int explode_from_matched(bool marked[GRID_H][GRID_W]) {
    int stack_r[GRID_W * GRID_H], stack_c[GRID_W * GRID_H];
    int sp = 0;
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++)
            if (marked[r][c]) { stack_r[sp] = r; stack_c[sp] = c; sp++; }

    int destroyed = 0;
    int dr[8] = { -1, -1, -1, 0, 0, 1, 1, 1 }, dc[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    while (sp > 0) {
        sp--;
        int r = stack_r[sp], c = stack_c[sp];
        for (int k = 0; k < 8; k++) {
            int nr = r + dr[k], nc = c + dc[k];
            if (nr < 0 || nr >= GRID_H || nc < 0 || nc >= GRID_W) continue;
            if (marked[nr][nc]) continue;
            int8_t sym = s_grid[nr][nc];
            if (sym == EMPTY_CELL) continue;
            if (symbol_resists_explosion(sym)) continue; // firewall - blocks the chain, isn't destroyed
            marked[nr][nc] = true;
            destroyed++;
            stack_r[sp] = nr; stack_c[sp] = nc; sp++;
        }
    }
    return destroyed;
}

static bool evaluate_clusters() {
    clear_marks();
    s_step_win = 0;
    s_chest_burst_this_step = false;
    bool visited[GRID_H][GRID_W];
    memset(visited, 0, sizeof(visited));
    int out_r[GRID_W * GRID_H], out_c[GRID_W * GRID_H];
    int mult = CASCADE_MULT[cascade_tier(s_cascade_step)];
    if (s_free_spins_active && mult < s_free_spin_mult_base) mult = s_free_spin_mult_base; // the "minimum multiplier" floor - see end_round()/check_milestone() for how it rises

    int natural_wins_this_step = 0;

    for (int r = 0; r < GRID_H; r++) {
        for (int c = 0; c < GRID_W; c++) {
            if (visited[r][c]) continue;
            int8_t sym = s_grid[r][c];
            if (sym == SCATTER_SYMBOL) { visited[r][c] = true; continue; }
            int count = flood_collect(r, c, sym, visited, out_r, out_c);
            if (count >= 5) {
                float base = (sym == WILD_SYMBOL) ? WILD_BASE_VALUE : SYMBOL_BASE_VALUE[sym];
                float payout = s_bet * base * size_multiplier(count) * mult;
                s_step_win += (int32_t)payout;
                for (int i = 0; i < count; i++) s_marked[out_r[i]][out_c[i]] = true;
                if (sym == TREASURE_SYMBOL && count >= 9) {
                    s_step_win += s_bet * 2;
                    s_chest_burst_this_step = true;
                }
                if (sym != WILD_SYMBOL) natural_wins_this_step++;
            }
        }
    }

    bool any = false;
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++)
            if (s_marked[r][c]) any = true;

    if (any) {
        // Natural-crystal chain explosion first, so a Wild sitting
        // next to the blast zone (checked right below) can still
        // detect and join in - the two mechanics reinforce each
        // other rather than compete for the same cells.
        explode_from_matched(s_marked);

        if (s_free_spins_active && natural_wins_this_step > 0) {
            s_free_spin_mult_base += natural_wins_this_step;
        }

        for (int r = 0; r < GRID_H; r++) {
            for (int c = 0; c < GRID_W; c++) {
                if (s_grid[r][c] != WILD_SYMBOL || s_marked[r][c]) continue;
                int dr[4] = { -1, 1, 0, 0 }, dc[4] = { 0, 0, -1, 1 };
                bool touches = false;
                for (int k = 0; k < 4 && !touches; k++) {
                    int nr = r + dr[k], nc = c + dc[k];
                    if (nr < 0 || nr >= GRID_H || nc < 0 || nc >= GRID_W) continue;
                    if (s_marked[nr][nc]) touches = true;
                }
                if (!touches) continue;
                s_marked[r][c] = true;
                s_wild_detonations++;
                int extra = 1 + random(2);
                for (int k = 0; k < extra; k++) {
                    int tries = 0;
                    while (tries < 10) {
                        int rr = random(GRID_H), cc = random(GRID_W);
                        if (!s_marked[rr][cc]) { s_marked[rr][cc] = true; break; }
                        tries++;
                    }
                }
            }
        }
    }
    return any;
}

static void check_scatter_bonus() {
    s_scatter_bonus_this_step = false;
    int count = 0;
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++)
            if (s_grid[r][c] == SCATTER_SYMBOL) count++;
    if (count < FREESPIN_TRIGGER) return;

    int spins, cash_mult;
    if (count >= 5) { spins = 12; cash_mult = 5; }
    else if (count == 4) { spins = 8; cash_mult = 3; }
    else { spins = 5; cash_mult = 2; }

    s_step_win += s_bet * cash_mult;
    if (!s_free_spins_active) {
        s_free_spins_active = true;
        s_free_spins_total = 0;
        s_free_spin_mult_base = 1;
        s_freespin_session_total = 0;
        s_milestone_triggered_this_session = false;
    }
    s_free_spins_left += spins;
    s_free_spins_total += spins;
    s_scatter_bonus_spins_awarded = spins;
    s_scatter_bonus_this_step = true;
    game_sfx_levelup();
}

static void apply_clear_and_fall() {
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++)
            if (s_marked[r][c]) s_grid[r][c] = EMPTY_CELL;

    for (int c = 0; c < GRID_W; c++) {
        int write_r = GRID_H - 1;
        for (int r = GRID_H - 1; r >= 0; r--) {
            if (s_grid[r][c] != EMPTY_CELL) {
                int moved_by = write_r - r;
                s_grid[write_r][c] = s_grid[r][c];
                s_fall_offset[write_r][c] = moved_by * (CELL_SIZE + CELL_GAP);
                if (write_r != r) s_grid[r][c] = EMPTY_CELL;
                write_r--;
            }
        }
        for (int r = write_r; r >= 0; r--) {
            s_grid[r][c] = random_symbol();
            s_fall_offset[r][c] = (write_r - r + 1) * (CELL_SIZE + CELL_GAP);
        }
    }
}

// ---- Round flow -------------------------------------------------------
static void begin_spin() {
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++) {
            s_grid[r][c] = random_symbol();
            s_fall_offset[r][c] = (r + 1) * (CELL_SIZE + CELL_GAP);
        }
    s_round_win = 0;
    s_cascade_step = 0;
    s_wild_detonations = 0;
    s_state = SL_PLAYING;
    s_phase = PH_FALL;
    s_phase_ms = millis();
    game_sfx_hit();
}

static bool s_milestone_just_hit; // one-shot flag for the SL_ROUND_END banner, see cavern_draw()

static void end_round() {
    s_money += s_round_win;
    s_milestone_just_hit = false;
    if (s_free_spins_active) {
        s_free_spin_mult_base += s_wild_detonations;
        s_freespin_session_total += s_round_win;
        if (!s_milestone_triggered_this_session && s_freespin_session_total >= s_bet * MILESTONE_THRESHOLD_MULT) {
            s_milestone_triggered_this_session = true;
            s_free_spin_mult_base = 60 + random(11); // random 60-70x, per the milestone's whole point being a dramatic, one-time floor jump
            s_milestone_just_hit = true;
            game_sfx_levelup();
        }
        s_free_spins_left--;
        if (s_free_spins_left <= 0) s_free_spins_active = false;
    }
    save_money();
    if (s_round_win > 0) game_sfx_score(); else game_sfx_hit();
    s_state = SL_ROUND_END;
}

// ---- Drawing --------------------------------------------------------------
static void draw_symbol(Arduino_GFX *g, int cx, int cy, int8_t sym, float scale) {
    if (sym == EMPTY_CELL || scale <= 0.02f) return;
    int r = (int)((CELL_SIZE / 2 - 8) * scale);
    if (r < 1) r = 1;
    if (sym == SCATTER_SYMBOL) {
        g->fillCircle(cx, cy, r, SCATTER_COLOR);
        g->fillTriangle(cx, cy - r + 4, cx - r + 6, cy, cx + r - 6, cy, COLOR_BG);
        g->fillTriangle(cx, cy + r - 4, cx - r + 6, cy, cx + r - 6, cy, COLOR_BG);
        return;
    }
    if (sym == WILD_SYMBOL) {
        for (int k = 0; k < 4; k++) {
            float ang = k * (3.14159f / 2.0f) + 0.785f;
            int tx = cx + (int)(cosf(ang) * (r + 6)), ty = cy + (int)(sinf(ang) * (r + 6));
            g->fillTriangle(cx, cy, cx + (int)(cosf(ang - 0.3f) * r), cy + (int)(sinf(ang - 0.3f) * r), tx, ty, WILD_COLOR);
        }
        g->fillCircle(cx, cy, r > 10 ? r - 10 : r / 2, WILD_COLOR);
        return;
    }
    uint16_t col = SYMBOL_COLOR[sym];
    switch (sym) {
        case 0: g->fillCircle(cx, cy, r, col); break;
        case 1: g->fillRect(cx - r, cy - r, r * 2, r * 2, col); break;
        case 2:
            g->fillTriangle(cx, cy, cx - r, cy - r + 4, cx - 4, cy + r - 6, col);
            g->fillTriangle(cx, cy, cx + r, cy - r + 4, cx + 4, cy + r - 6, col);
            g->fillCircle(cx, cy, r > 6 ? 6 : r, col);
            break;
        case 3:
            g->fillTriangle(cx - r, cy - r, cx - 6, cy - 4, cx - r + 6, cy + 2, col);
            g->fillTriangle(cx + r, cy - r, cx + 6, cy - 4, cx + r - 6, cy + 2, col);
            g->fillTriangle(cx - r, cy + r, cx - 6, cy + 4, cx - r + 6, cy - 2, col);
            g->fillTriangle(cx + r, cy + r, cx + 6, cy + 4, cx + r - 6, cy - 2, col);
            g->fillCircle(cx, cy, r > 10 ? 10 : r, col);
            g->fillCircle(cx, cy - 12, r > 6 ? 6 : r / 2, col);
            break;
        case 4:
            g->fillCircle(cx, cy - 2, r > 2 ? r - 2 : 1, col);
            g->fillRect(cx - 8, cy + r - 16, 16, 10, col);
            g->fillCircle(cx - 7, cy - 4, 5, COLOR_BG);
            g->fillCircle(cx + 7, cy - 4, 5, COLOR_BG);
            g->fillRect(cx - 2, cy + 6, 4, 6, COLOR_BG);
            break;
        case 5:
            g->fillRoundRect(cx - r, cy - r + 8, r * 2, r * 2 - 8, 4, col);
            g->fillRoundRect(cx - r, cy - r, r * 2, 14, 4, (uint16_t)COLOR565(0x6B, 0x40, 0x1F));
            g->fillRect(cx - 5, cy - 4, 10, 12, (uint16_t)COLOR565(0xFF, 0xD8, 0x30));
            break;
        case 6: // moon crystal - crescent shape plus two small sparkle accents, the top-tier premium symbol
            g->fillCircle(cx, cy, r, col);
            g->fillCircle(cx + r / 2, cy - r / 3, (int)(r * 0.85f), COLOR_BG);
            g->fillCircle(cx - r + 5, cy + r - 7, 3, col);
            g->fillCircle(cx + r - 7, cy - r + 9, 3, col);
            break;
    }
}

static void draw_grid(Arduino_GFX *g) {
    float fall_progress = 1.0f;
    if (s_state == SL_PLAYING && s_phase == PH_FALL) {
        fall_progress = (float)(millis() - s_phase_ms) / (float)FALL_DURATION_MS;
        if (fall_progress > 1.0f) fall_progress = 1.0f;
    }
    float clear_scale = 1.0f;
    if (s_state == SL_PLAYING && s_phase == PH_CLEAR) {
        float p = (float)(millis() - s_phase_ms) / (float)CLEAR_DURATION_MS;
        if (p > 1.0f) p = 1.0f;
        clear_scale = 1.0f - p; // winning symbols visibly shrink to nothing here, before the grid data changes at all
    }
    for (int r = 0; r < GRID_H; r++) {
        for (int c = 0; c < GRID_W; c++) {
            int x = GRID_LEFT + c * (CELL_SIZE + CELL_GAP);
            int y = GRID_TOP + r * (CELL_SIZE + CELL_GAP);
            bool marked_now = s_marked[r][c] && (s_phase == PH_HIGHLIGHT || s_phase == PH_CLEAR);
            uint16_t bg = (marked_now && s_phase == PH_HIGHLIGHT) ? COLOR_PANEL : COLOR_BG;
            g->fillRoundRect(x, y, CELL_SIZE, CELL_SIZE, 8, bg);

            float scale = 1.0f;
            if (s_phase == PH_CLEAR && s_marked[r][c]) scale = clear_scale;
            bool pulsing_scatter = (s_phase == PH_HIGHLIGHT && s_scatter_bonus_this_step && s_grid[r][c] == SCATTER_SYMBOL);
            if (pulsing_scatter) scale = 0.85f + 0.3f * sinf((float)(millis() - s_phase_ms) * 0.012f);

            int draw_y = y;
            if (s_phase == PH_FALL) draw_y = y - (int)(s_fall_offset[r][c] * (1.0f - fall_progress));
            draw_symbol(g, x + CELL_SIZE / 2, draw_y + CELL_SIZE / 2, s_grid[r][c], scale);

            if (marked_now && s_phase == PH_HIGHLIGHT && ((millis() - s_phase_ms) / 120) % 2 == 0) {
                g->fillRoundRect(x, y, CELL_SIZE, 4, 2, COLOR_ACCENT3);
                g->fillRoundRect(x, y + CELL_SIZE - 4, CELL_SIZE, 4, 2, COLOR_ACCENT3);
            }
        }
    }
}

// ---- Round-screen chrome -------------------------------------------------
//
// Edge ring, top: the cascade multiplier as five labelled segments
// (x1 x2 x3 x5 x8) that light up as a cascade climbs. Edge ring, bottom:
// free spins remaining, during a free-spin session. Balance big at the
// top, the win counting up big under the grid, round buttons along the
// bottom curve, banners laid right across the grid where the eye already is.

struct RBtn { int16_t cx, cy, r; };
static const RBtn SPIN_BTN    = { LCD_WIDTH / 2, 402, 36 };
static const RBtn BET_DOWN    = { 148, 384, 26 };
static const RBtn BET_UP      = { LCD_WIDTH - 148, 384, 26 };
static const uint16_t GOLD    = (uint16_t)COLOR565(0xFF, 0xC8, 0x30);

static float    s_win_shown = 0.0f;   // count-up display value
static uint32_t s_win_anim_ms = 0;

static bool rbtn_hit(const RBtn &b, int x, int y, int slack = 8) {
    int dx = x - b.cx, dy = y - b.cy, r = b.r + slack;
    return dx * dx + dy * dy <= r * r;
}

static void draw_rbtn(Arduino_GFX *g, const RBtn &b, uint16_t color, const char *label, int text_sz, bool enabled) {
    uint16_t c = enabled ? color : COLOR_PANEL;
    if (enabled) g->fillCircle(b.cx, b.cy, b.r + 4, ui_dim(color, 0.3f));
    g->fillCircle(b.cx, b.cy, b.r, c);
    ui_text_center(b.cx, b.cy, enabled ? COLOR_TEXT : COLOR_TEXT_DIM, label, text_sz);
}

#define MULT_SEG_START 295.0f
#define MULT_SEG_SPAN  26.0f

static void draw_multiplier_ring(int lit_tier, bool flash) {
    for (int i = 0; i < 5; i++) {
        float start = MULT_SEG_START + i * MULT_SEG_SPAN + 1.5f;
        bool lit = i <= lit_tier;
        uint16_t c = lit ? GOLD : ui_dim(COLOR_PANEL, 1.0f);
        if (lit && flash) c = ui_dim(GOLD, 0.55f + 0.45f * sinf((float)millis() * 0.02f));
        if (lit && i == lit_tier && lit_tier > 0)                  // glow on the newest tier
            ui_edge_ring(start, MULT_SEG_SPAN - 3.0f, ui_dim(GOLD, 0.35f), 20, 1, false);
        ui_edge_ring(start, MULT_SEG_SPAN - 3.0f, c, 16, 3, false);
        char lbl[4];
        snprintf(lbl, sizeof(lbl), "x%d", CASCADE_MULT[i]);
        int lx, ly;
        ui_polar(start + (MULT_SEG_SPAN - 3.0f) / 2.0f, LCD_WIDTH / 2 - 11, &lx, &ly);
        ui_text_center(lx, ly, lit ? COLOR_BG : COLOR_TEXT_DIM, lbl, 1);
    }
}

static void draw_freespin_arc() {
    if (!s_free_spins_active || s_free_spins_total <= 0) return;
    const float start = 240.0f, span = -120.0f; // bottom, left -> right
    int total = s_free_spins_total, left = s_free_spins_left;
    if (total <= 20) {
        float seg = span / total;
        for (int i = 0; i < total; i++) {
            bool on = i < left;
            ui_edge_ring(start + seg * i - 1.0f, seg + 2.0f, on ? SCATTER_COLOR : ui_dim(COLOR_PANEL, 1.0f), 10, 4, false);
        }
    } else {
        ui_edge_ring(start, span, ui_dim(COLOR_PANEL, 1.0f), 10, 4);
        ui_edge_ring(start, span * left / (float)total, SCATTER_COLOR, 10, 4);
    }
}

// Banner pill centred at row cy. Mid-cascade banners go just above the
// grid (cy ~100) so they never cover the winning cells; end-of-round ones
// go across the grid itself, which is static by then.
static void draw_banner(Arduino_GFX *g, int cy, const char *text, uint16_t color, int text_sz) {
    int w = text_sz >= 3 ? 320 : 290, h = text_sz >= 3 ? 52 : 36;
    int x = LCD_WIDTH / 2 - w / 2, y = cy - h / 2;
    g->fillRoundRect(x - 3, y - 3, w + 6, h + 6, h / 2 + 3, ui_dim(color, 0.4f));
    g->fillRoundRect(x, y, w, h, h / 2, color);
    g->fillRoundRect(x + 3, y + 3, w - 6, h - 6, h / 2 - 3, COLOR_BG);
    ui_text_center(LCD_WIDTH / 2, cy, color, text, text_sz);
}

static void cavern_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    char buf[40];

    if (s_state == SL_GAMEOVER) {
        ui_edge_ring(0, 360, ui_dim(COLOR_BAD, 0.5f), 8, 4, false);
        ui_text_center(LCD_WIDTH / 2, 170, COLOR_BAD, "OUT OF CHIPS", 3);
        ui_text_center(LCD_WIDTH / 2, 220, COLOR_TEXT_DIM, "Tap to restart with", 1);
        snprintf(buf, sizeof(buf), "$%d", START_MONEY);
        ui_text_center(LCD_WIDTH / 2, 262, GOLD, buf, 4);
        return;
    }

    // ---- Win count-up ----------------------------------------------------
    int32_t win_target = 0;
    if (s_state == SL_PLAYING) win_target = s_round_win + s_step_win;
    else if (s_state == SL_ROUND_END) win_target = s_round_win;
    uint32_t now = millis();
    float dt = (float)(now - s_win_anim_ms);
    s_win_anim_ms = now;
    if (win_target == 0) s_win_shown = 0.0f;
    else {
        float k = dt / 180.0f; if (k > 1.0f) k = 1.0f;
        s_win_shown += ((float)win_target - s_win_shown) * k;
        if (fabsf((float)win_target - s_win_shown) < 1.0f) s_win_shown = (float)win_target;
    }
    bool big_win = s_state == SL_ROUND_END && s_round_win >= s_bet * 10;

    // ---- Edge rings ----------------------------------------------------
    int tier = (s_state == SL_PLAYING) ? cascade_tier(s_cascade_step) : 0;
    draw_multiplier_ring(tier, big_win);
    draw_freespin_arc();

    // ---- Balance + bet / free spins --------------------------------------
    snprintf(buf, sizeof(buf), "$%ld", (long)s_money);
    ui_text_center(LCD_WIDTH / 2, 62, COLOR_TEXT, buf, 3);
    if (s_free_spins_active) {
        snprintf(buf, sizeof(buf), "FREE SPINS %d   x%d", s_free_spins_left, s_free_spin_mult_base);
        ui_text_center(LCD_WIDTH / 2, 96, SCATTER_COLOR, buf, 1);
    } else if (s_state != SL_IDLE) { // idle shows the bet big at the bottom instead
        snprintf(buf, sizeof(buf), "BET $%ld", (long)s_bet);
        ui_text_center(LCD_WIDTH / 2, 96, COLOR_TEXT_DIM, buf, 1);
    }
    if (s_state == SL_PLAYING && tier > 0) {
        snprintf(buf, sizeof(buf), "CASCADE x%d", CASCADE_MULT[tier]);
        ui_text_center(LCD_WIDTH / 2, 114, GOLD, buf, 1);
    }

    // ---- Grid on a soft panel ------------------------------------------
    g->fillRoundRect(GRID_LEFT - 8, GRID_TOP - 8, GRID_TOTAL_W + 16, GRID_TOTAL_H + 16, 16, ui_dim(COLOR_PANEL, 0.8f));
    draw_grid(g);

    // ---- Banners over the grid -------------------------------------------
    if (s_state == SL_PLAYING && s_phase == PH_HIGHLIGHT && s_scatter_bonus_this_step) {
        snprintf(buf, sizeof(buf), "+%d FREE SPINS!", s_scatter_bonus_spins_awarded);
        uint16_t flash = ((now - s_phase_ms) / 150) % 2 == 0 ? SCATTER_COLOR : COLOR_TEXT;
        draw_banner(g, 101, buf, flash, 2);
    } else if (s_state == SL_PLAYING && s_phase == PH_HIGHLIGHT && s_chest_burst_this_step) {
        draw_banner(g, 101, "CHEST BURST!", COLOR_ACCENT3, 2);
    } else if (s_state == SL_ROUND_END && s_milestone_just_hit) {
        snprintf(buf, sizeof(buf), "MULTIPLIER x%d!", s_free_spin_mult_base);
        draw_banner(g, LCD_HEIGHT / 2, buf, COLOR_ACCENT3, 2);
    } else if (big_win) {
        uint16_t c = ((now / 200) % 2 == 0) ? GOLD : COLOR_TEXT;
        draw_banner(g, LCD_HEIGHT / 2, "BIG WIN!", c, 3);
    }

    // ---- Bottom: win, or controls ----------------------------------------
    if (s_state == SL_PLAYING || s_state == SL_ROUND_END) {
        if (win_target > 0 || s_win_shown > 0.5f) {
            ui_text_center(LCD_WIDTH / 2, 350, COLOR_TEXT_DIM, "WIN", 1);
            snprintf(buf, sizeof(buf), "$%ld", (long)lroundf(s_win_shown));
            ui_text_center(LCD_WIDTH / 2, 374, GOLD, buf, 3);
        } else if (s_state == SL_ROUND_END) {
            ui_text_center(LCD_WIDTH / 2, 370, COLOR_TEXT_DIM, "NO WIN", 2);
        }
        if (s_state == SL_ROUND_END)
            ui_text_center(LCD_WIDTH / 2, 410, COLOR_TEXT, s_free_spins_active ? "TAP FOR NEXT FREE SPIN" : "TAP TO CONTINUE", 1);
    } else { // SL_IDLE
        if (s_free_spins_active) {
            draw_rbtn(g, SPIN_BTN, SCATTER_COLOR, "FREE", 2, true);
        } else {
            snprintf(buf, sizeof(buf), "BET $%ld", (long)s_bet);
            ui_text_center(LCD_WIDTH / 2, 356, COLOR_TEXT, buf, 2);
            draw_rbtn(g, BET_DOWN, COLOR_ACCENT2, "-", 3, s_bet - BET_MIN >= BET_MIN);
            draw_rbtn(g, BET_UP, COLOR_ACCENT2, "+", 3, s_bet + BET_MIN <= s_money);
            draw_rbtn(g, SPIN_BTN, COLOR_GOOD, "SPIN", 2, s_money >= s_bet);
        }
    }
}

// ---- Touch / tick -----------------------------------------------------

static void cavern_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;

    if (s_state == SL_GAMEOVER) {
        s_money = START_MONEY;
        save_money();
        s_free_spins_active = false;
        s_state = SL_IDLE;
        return;
    }

    if (s_state == SL_ROUND_END) {
        // < BET_MIN, not just <= 0 - $1-4 left is exactly "stuck" too:
        // not technically bust, but unable to ever place the smallest
        // bet the game allows, so nothing to do but stare at a Spin
        // button that can never light up.
        if (s_money < BET_MIN && !s_free_spins_active) { s_state = SL_GAMEOVER; return; }
        s_state = SL_IDLE;
        if (s_free_spins_active) begin_spin();
        return;
    }

    if (s_state != SL_IDLE) return;

    if (!s_free_spins_active) {
        if (rbtn_hit(BET_DOWN, x, y)) {
            if (s_bet - BET_MIN >= BET_MIN) s_bet -= BET_MIN;
            return;
        }
        if (rbtn_hit(BET_UP, x, y)) {
            if (s_bet + BET_MIN <= s_money) s_bet += BET_MIN;
            return;
        }
    }

    if (rbtn_hit(SPIN_BTN, x, y)) {
        if (s_free_spins_active || s_money >= s_bet) {
            if (!s_free_spins_active) s_money -= s_bet;
            begin_spin();
        }
        return;
    }
}

static void cavern_tick() {
    if (controller_connected() && s_state == SL_IDLE) {
        static bool s_ctrl_a_prev = false;
        bool a = controller_button(CTRL_BTN_A);
        if (a && !s_ctrl_a_prev && (s_free_spins_active || s_money >= s_bet)) {
            if (!s_free_spins_active) s_money -= s_bet;
            begin_spin();
        }
        s_ctrl_a_prev = a;
    }

    if (s_state != SL_PLAYING) return;
    uint32_t elapsed = millis() - s_phase_ms;

    if (s_phase == PH_FALL) {
        if (elapsed >= FALL_DURATION_MS) {
            s_phase = PH_HIGHLIGHT;
            s_phase_ms = millis();
        }
        return;
    }
    if (s_phase == PH_HIGHLIGHT) {
        static bool evaluated_this_step = false;
        static bool any_cluster_this_step = false;
        if (!evaluated_this_step) {
            any_cluster_this_step = evaluate_clusters();
            check_scatter_bonus();
            evaluated_this_step = true;
            s_round_win += s_step_win;
            if (any_cluster_this_step) game_sfx_hit();
        }
        uint32_t hold_ms = (s_chest_burst_this_step || s_scatter_bonus_this_step) ? 1100 : 450;
        if (!any_cluster_this_step && !s_scatter_bonus_this_step) hold_ms = 0;
        if (elapsed >= hold_ms) {
            evaluated_this_step = false;
            if (!any_cluster_this_step) {
                // Nothing marked - whether truly no win, or a
                // scatter-only trigger with no accompanying cluster -
                // there's nothing to clear or drop, so ending here
                // (rather than looping back to PH_HIGHLIGHT on an
                // identical grid) is what stops a scatter-only step
                // from re-triggering its own bonus forever.
                end_round();
                return;
            }
            s_phase = PH_CLEAR;
            s_phase_ms = millis();
        }
        return;
    }
    if (s_phase == PH_CLEAR) {
        if (elapsed >= CLEAR_DURATION_MS) {
            apply_clear_and_fall();
            s_cascade_step++;
            s_phase = PH_FALL;
            s_phase_ms = millis();
        }
        return;
    }
}

static void cavern_create() {
    load_money();
    s_state = (s_money < BET_MIN) ? SL_GAMEOVER : SL_IDLE; // same "stuck below the minimum bet" reasoning as the SL_ROUND_END check above
    s_free_spins_active = false;
    s_free_spins_total = 0;
    s_win_shown = 0.0f;
    s_milestone_just_hit = false;
    s_milestone_triggered_this_session = false;
    s_freespin_session_total = 0;
    s_round_win = 0;
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++) {
            s_grid[r][c] = random_symbol();
            s_fall_offset[r][c] = 0;
        }
    s_prev_pressed = false;
}

static void cavern_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen cavern_screen = {
    "", GESTURE_MODE_EDGE,   // no header - balance and rings use the whole round screen
    UI_FRAME_MS_DEFAULT,
    cavern_create, cavern_draw, cavern_touch, cavern_tick, nullptr, cavern_gesture,
    0, false, false, false, false,
    true, // hide_status - the edge is the multiplier / free-spin ring
};
