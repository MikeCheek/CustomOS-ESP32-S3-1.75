/*
 * game_breakout.cpp
 * Brick breaker redesigned around the round display's actual shape,
 * rather than a rectangle dropped in the middle of it:
 *  - The paddle is an arc that travels along the BOTTOM half of the
 *    rim, tilt-controlled (with a quick per-run calibration, same
 *    pattern as game_plane.cpp - hold level, tap to start).
 *  - Bricks fill three concentric rings across the TOP half of the
 *    rim, as angular sectors rather than rectangles.
 *  - The ball moves in ordinary 2D space; collisions are evaluated in
 *    polar terms (radius/angle from the arena center) against the
 *    outer rim, the brick rings, and the paddle's current arc.
 *  - A boss arc replaces the bricks every 4th level: a wide glowing
 *    core in the top rings that absorbs hits and fires projectiles
 *    inward that the paddle has to be positioned to block.
 *
 * The "ball is lost" condition only applies within the paddle's home
 * arc at the bottom - reaching the rim anywhere else (the sides, or a
 * cleared patch of the brick rings) is just a normal elastic bounce
 * off a solid wall, not a miss.
 *
 * Physics runs in fixed 16ms steps (see game_plane.cpp for why).
 */
#include "game_breakout.h"
#include "config.h"
#include "board_pins.h"
#include "hal_imu.h"
#include "hal_controller.h"
#include "ui.h"
#include "game_audio.h"
#include "hal_save.h"
#include "app_settings_state.h"
#include "hal_nvs.h"
#include "hal_audio.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#define DEG2RAD (3.14159265f / 180.0f)

// ---- Arena geometry -------------------------------------------------------
#define ARENA_CX      (LCD_WIDTH / 2)
#define ARENA_CY      (LCD_HEIGHT / 2)
#define ARENA_R       215.0f // outer wall - safely inside the physical bezel

#define TOP_ARC_CENTER_DEG     270.0f // straight up
#define TOP_ARC_HALFSPAN_DEG   80.0f  // bricks span 190..350 deg
#define BOTTOM_ARC_CENTER_DEG  90.0f  // straight down
#define BOTTOM_ARC_HALFSPAN_DEG 75.0f // paddle can travel 15..165 deg

#define PADDLE_ARC_WIDTH_DEG   26.0f
#define PADDLE_RADIUS          205.0f
#define PADDLE_THICK            10.0f

#define BALL_R        6.0f
// Small forgiveness margin added to hit detection for both the ball
// (against bricks/helpers) and the paddle (against the ball) - makes
// collisions feel a little more generous than a pixel-exact check,
// which otherwise reads as "sometimes it just doesn't register" on a
// fast-moving ball.
#define HITBOX_PADDING 3.0f
#define BALL_SPEED_BASE 175.0f // px/sec

#define BRICK_RINGS       3
#define BRICKS_PER_RING   8
#define RING_THICK        26.0f
#define RING_GAP          5.0f
// Ring 0 = innermost (hit first by a ball heading up from the paddle).
// Not const: full-mobility levels use a compressed set of radii (set
// in load_level_bricks() below) so the outermost ring stays a real
// distance from PADDLE_RADIUS (205) - the default set's outer ring
// (198) left only a 7px gap, smaller than the ball's own diameter,
// which is playable when bricks and paddle are on opposite sides of
// the arena (normal levels) but far too tight once the paddle can be
// anywhere, including right next to the bricks.
static float RING_R_IN[BRICK_RINGS]  = { 110.0f, 141.0f, 172.0f };
static float RING_R_OUT[BRICK_RINGS] = { 136.0f, 167.0f, 198.0f };
static const float RING_R_IN_DEFAULT[BRICK_RINGS]  = { 110.0f, 141.0f, 172.0f };
static const float RING_R_OUT_DEFAULT[BRICK_RINGS] = { 136.0f, 167.0f, 198.0f };
#define FULL_MOBILITY_MIN_PADDLE_GAP 35.0f
static const float RING_R_IN_FULL[BRICK_RINGS]  = { 78.0f, 103.0f, 128.0f };
static const float RING_R_OUT_FULL[BRICK_RINGS] = { 97.0f, 122.0f, 147.0f }; // 205 - 147 = 58px gap to the paddle, comfortably over the 35px minimum

#define MAX_BALLS         3
#define MAX_POWERUPS       4
#define MAX_BOSS_SHOTS     4

#define PHYS_STEP_MS  16
#define START_LIVES   3
#define TOTAL_LEVELS  50 // the level map shows exactly this many islands
#define CAL_HOLD_MS   900
#define TILT_GAIN_DEG 70.0f

// A level is a boss level if it's the 5th of its group (5, 10, 15, ...
// up to TOTAL_LEVELS, then the pattern repeats into "loop 2" and
// beyond for endless play past the mapped islands - see
// advance_to_next_level()). This replaces the old fixed BOSS_LEVEL=4
// constant now that levels are persistent and go well past 4.
static bool is_boss_level(int level) {
    int within_loop = ((level - 1) % TOTAL_LEVELS) + 1;
    return within_loop % 5 == 0;
}

enum GState { GS_TITLE, GS_LEVEL_MAP, GS_CALIBRATING, GS_TUTORIAL_POPUP, GS_PLAYING, GS_PAUSED, GS_LEVEL_CLEAR, GS_BOSS_INTRO, GS_GAMEOVER };
enum PowerupType { PU_GROW, PU_MULTIBALL, PU_LIFE, PU_SHOOT, PU_SHRINK };

struct Brick { uint8_t hp; bool active; };
struct Ball { float x, y, vx, vy; bool active; };
// Powerups/boss shots move in straight cartesian lines, not by shrinking
// their polar radius at a fixed angle - the paddle lives at a
// different angle than the bricks/boss do (bottom vs top), so a
// radius-only motion can only ever travel along its own spawn line
// through the center and can never actually reach the paddle's side
// of the arena. This was the actual bug behind "powerups disappear
// near the center" - they were correctly modeled as moving inward,
// just never toward anywhere the paddle could catch them. The same
// mistake made boss shots unable to ever hit the player at all.
struct Powerup { float x, y, vx, vy; PowerupType type; bool active; };
struct BossShot { float x, y, vx, vy; bool active; };
// The paddle's own shots when PU_SHOOT is active - travels straight
// inward (toward arena center) from wherever it was fired, same
// straight-line-not-radius-only reasoning as the comment above
// Powerup explains, so a shot fired while the paddle sits off to one
// side still actually reaches the bricks on the far side of the arena
// rather than just running parallel to them.
struct PaddleShot { float x, y, vx, vy; bool active; };

// Persistent level-map progress - survives power cycles via the same
// generic NVS blob save/load this project already built for Ninja
// Dungeon's roguelite progression (hal_save.h), just under its own
// key so the two games' saves never collide.
#define BREAKOUT_SAVE_MAGIC 0x42524B31u // "BRK1"
struct BreakoutProgress {
    uint32_t magic;
    int highest_unlocked_level; // 1..TOTAL_LEVELS (or beyond, once a loop completes)
    int best_score;
    bool seen_full_mobility_tip;
    bool seen_boss_tip;
};
static BreakoutProgress s_progress;

// Level map - a pannable path of level "islands", drawn as a winding
// S-curve (5 per row, alternating direction each row) rather than one
// long straight line, so 50 levels fit in a reasonably compact,
// draggable area instead of a five-thousand-pixel-long strip.
#define MAP_COLS 5
#define MAP_NODE_SPACING_X 92.0f
#define MAP_NODE_SPACING_Y 104.0f
static float s_map_pan_x, s_map_pan_y;
static bool s_map_dragging;
// Rising-edge tracking for every simple state-transition tap and
// pause-menu button below (GS_TITLE, GS_GAMEOVER, GS_TUTORIAL_POPUP,
// GS_PLAYING, and GS_PAUSED's SFX/Resume/Back-to-map buttons) - these
// used to act on raw `pressed` directly, firing every single frame
// the finger stayed down rather than once. Concretely: tapping to
// pause (GS_PLAYING) could, if the finger lingered even slightly,
// have that same still-down press immediately land on whatever
// pause-menu button ended up under it once GS_PAUSED became active -
// e.g. instantly un-pausing via Resume before the menu was ever
// visibly seen. Drag-based interactions (the level map's pan, the
// pause menu's volume slider) deliberately keep using raw `pressed`.
static bool s_prev_pressed;
static int s_map_drag_start_x, s_map_drag_start_y;
static float s_map_drag_start_pan_x, s_map_drag_start_pan_y;
static bool s_map_moved_significantly;
static int s_selected_level; // set when a node is tapped, consumed once calibration finishes

static void level_node_world_pos(int level, float *wx, float *wy) {
    int idx = level - 1; // 0-based
    int row = idx / MAP_COLS;
    int col = idx % MAP_COLS;
    if (row % 2 == 1) col = MAP_COLS - 1 - col; // alternate direction each row
    *wx = col * MAP_NODE_SPACING_X;
    *wy = row * MAP_NODE_SPACING_Y;
}

static void load_progress() {
    BreakoutProgress loaded;
    if (game_load_blob("breakout_progress", &loaded, sizeof(loaded)) && loaded.magic == BREAKOUT_SAVE_MAGIC) {
        s_progress = loaded;
    } else {
        memset(&s_progress, 0, sizeof(s_progress));
        s_progress.magic = BREAKOUT_SAVE_MAGIC;
        s_progress.highest_unlocked_level = 1;
    }
}
static void save_progress() {
    game_save_blob("breakout_progress", &s_progress, sizeof(s_progress));
}

static GState s_state;
static int s_level, s_cycle, s_lives, s_score;
// Full-mobility levels: bricks ring the whole arena (not just the top
// arc) and the paddle can travel anywhere around the outer edge (not
// just the bottom arc) - a different, occasional level layout rather
// than the default bottom-paddle/top-bricks arrangement. Decided per
// level in load_level_bricks() below.
static bool s_full_mobility = false;
// Where to go once the first-time tutorial popup (GS_TUTORIAL_POPUP)
// is dismissed - GS_PLAYING for the full-mobility tip, GS_BOSS_INTRO
// for the boss tip. Setup (load_level_bricks()/start_boss()) already
// ran by the time the popup shows; only the state transition is
// deferred.
static GState s_tutorial_next_state = GS_PLAYING;

// Sector angular geometry for the brick rings - centralizes the
// "which angle range do bricks span" question so try_damage_brick_at()
// and the drawing code agree, instead of each hardcoding the top-arc
// span directly and needing a separate parallel branch for the full-
// mobility case.
static float brick_sector_width_deg() {
    return s_full_mobility ? (360.0f / BRICKS_PER_RING) : (2.0f * TOP_ARC_HALFSPAN_DEG / BRICKS_PER_RING);
}
static float brick_sector_start_deg() {
    return s_full_mobility ? 0.0f : (TOP_ARC_CENTER_DEG - TOP_ARC_HALFSPAN_DEG);
}
static uint32_t s_state_enter_ms;

// Calibration
static float s_cal_x;
static uint32_t s_cal_start_ms;
static float s_cal_sum_x;
static int s_cal_samples;

static float s_paddle_angle_deg;
static float s_paddle_arc_width_deg;
// Stacking buffs: each collection of the same type adds another stack
// (up to a cap) and refreshes a SHARED expiry for that type - when it
// expires, the whole stack reverts at once rather than each stack
// counting down independently, which would need a per-stack timer
// list for what's a fairly minor gameplay difference.
#define MAX_GROW_STACKS   4
#define MAX_SHRINK_STACKS 3
#define GROW_STACK_MULT   0.35f  // each grow stack: +35% paddle width
#define SHRINK_STACK_MULT 0.22f  // each shrink stack: -22% paddle width
#define BUFF_DURATION_MS  8000
static int s_grow_stacks = 0;
static int s_shrink_stacks = 0;
static uint32_t s_grow_until_ms = 0;
static uint32_t s_shrink_until_ms = 0;

#define SHOOT_FIRE_INTERVAL_MS 450
#define MAX_PADDLE_SHOTS 4
static uint32_t s_shoot_until_ms = 0;
static uint32_t s_last_paddle_shot_ms = 0;
static PaddleShot s_paddle_shots[MAX_PADDLE_SHOTS];

static Brick s_bricks[BRICK_RINGS][BRICKS_PER_RING];
static int s_bricks_remaining;

static Ball s_balls[MAX_BALLS];
static bool s_ball_launched;
static uint32_t s_ball_launch_at_ms;

static Powerup s_powerups[MAX_POWERUPS];

static float s_boss_angle_deg, s_boss_dir;
static float s_boss_arc_width_deg;
static int s_boss_hp, s_boss_hp_max;
enum BossType { BOSS_SWEEPER, BOSS_SPINNER, BOSS_TANK, BOSS_TYPE_COUNT };
static BossType s_boss_type;
static float s_boss_speed_mult;   // how fast it sweeps side to side (0 = stationary)
static float s_boss_fire_mult;    // multiplies the base fire interval (<1 = fires more often)
#define MAX_HELPERS 3
struct Helper { float angle_deg, r; int hp; bool active; };
static Helper s_helpers[MAX_HELPERS];
static uint32_t s_boss_last_fire_ms;
static BossShot s_boss_shots[MAX_BOSS_SHOTS];

static uint32_t s_last_phys_step;

// ---- Small helpers --------------------------------------------------------

static float angle_diff_deg(float a, float b) {
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f; // range: -180 to 180
    return d;
}

// Combines active grow/shrink stacks into the actual paddle width -
// the single source of truth for this, called any time a stack count
// changes rather than scattering the multiplication at each call site.
static void recompute_paddle_width() {
    float mult = 1.0f + GROW_STACK_MULT * s_grow_stacks - SHRINK_STACK_MULT * s_shrink_stacks;
    if (mult < 0.35f) mult = 0.35f; // never shrink to something unplayable
    if (mult > 3.0f) mult = 3.0f;   // sane cap regardless of stack count
    s_paddle_arc_width_deg = PADDLE_ARC_WIDTH_DEG * mult;
}

static float speed_mult() { return 1.0f + 0.12f * (float)s_cycle; }

static void polar_to_xy(float angle_deg, float r, float *x, float *y) {
    float a = angle_deg * DEG2RAD;
    *x = ARENA_CX + r * cosf(a);
    *y = ARENA_CY + r * sinf(a);
}

static void reflect_radial(float angle_deg, float *vx, float *vy) {
    float a = angle_deg * DEG2RAD;
    float nx = cosf(a), ny = sinf(a);
    float dot = (*vx) * nx + (*vy) * ny;
    *vx -= 2.0f * dot * nx;
    *vy -= 2.0f * dot * ny;
}

static int count_active_balls() {
    int n = 0;
    for (int i = 0; i < MAX_BALLS; i++) if (s_balls[i].active) n++;
    return n;
}

static void spawn_powerup(float x, float y) {
    for (int i = 0; i < MAX_POWERUPS; i++) {
        if (s_powerups[i].active) continue;
        s_powerups[i].active = true;
        s_powerups[i].x = x;
        s_powerups[i].y = y;
        if (s_full_mobility) {
            // Drift radially outward from the arena center rather
            // than straight down - "down" in screen space doesn't
            // consistently move toward the paddle when bricks ring
            // the whole center and the paddle can be anywhere around
            // the edge, unlike normal levels where bricks are always
            // above the paddle.
            float ddx = x - ARENA_CX, ddy = y - ARENA_CY;
            float dlen = sqrtf(ddx * ddx + ddy * ddy);
            if (dlen < 1.0f) dlen = 1.0f;
            const float FALL_SPEED = 55.0f;
            s_powerups[i].vx = ddx / dlen * FALL_SPEED;
            s_powerups[i].vy = ddy / dlen * FALL_SPEED;
        } else {
            s_powerups[i].vx = 0.0f;
            s_powerups[i].vy = 55.0f; // falls straight down, like gravity
        }
        int rr = random(100);
        s_powerups[i].type = (rr < 30) ? PU_GROW : (rr < 50) ? PU_MULTIBALL :
                              (rr < 62) ? PU_LIFE : (rr < 85) ? PU_SHOOT : PU_SHRINK;
        return;
    }
}

static void place_brick(int ring, int sector, int hp) {
    if (ring < 0 || ring >= BRICK_RINGS || sector < 0 || sector >= BRICKS_PER_RING) return;
    s_bricks[ring][sector].hp = (uint8_t)hp;
    s_bricks[ring][sector].active = true;
    s_bricks_remaining++;
}

// Forward-declared: load_level_bricks() above needs it, but the
// actual definition (with its difficulty-scaling comment) reads
// better kept near the falling-tiles shift mechanic that also uses
// it, further down.
static void generate_ring_pattern(int ring);

static void load_level_bricks(int level) {
    memset(s_bricks, 0, sizeof(s_bricks));
    s_bricks_remaining = 0;

    // Escalating layout within each group of 5 (position 5 is always a
    // boss level and never reaches load_level_bricks() at all - see
    // is_boss_level()): ring 0 alone at position 1, ring 1 joins at
    // position 2, ring 2 at position 3+. Same progression feel the
    // original hand-authored 3-level cycle had, generalized across
    // however many groups of 5 the level path now has instead of only
    // ever covering 3 distinct levels. Overall difficulty (gap chance,
    // hp) comes from s_cycle inside generate_ring_pattern() itself,
    // which enter_level() already derives from the level number.
    int pos_in_group = ((level - 1) % 5) + 1; // 1-4
    // Position 3 of each group of 5 is a full-mobility level instead
    // of the default layout - occasional variety, not every level.
    s_full_mobility = (pos_in_group == 3);
    for (int i = 0; i < BRICK_RINGS; i++) {
        RING_R_IN[i]  = s_full_mobility ? RING_R_IN_FULL[i]  : RING_R_IN_DEFAULT[i];
        RING_R_OUT[i] = s_full_mobility ? RING_R_OUT_FULL[i] : RING_R_OUT_DEFAULT[i];
    }
    generate_ring_pattern(0);
    if (pos_in_group >= 2) generate_ring_pattern(1);
    if (pos_in_group >= 3) generate_ring_pattern(2);

    int remaining = 0;
    for (int r = 0; r < BRICK_RINGS; r++)
        for (int s = 0; s < BRICKS_PER_RING; s++)
            if (s_bricks[r][s].active) remaining++;
    s_bricks_remaining = remaining;
}

// Generates a fresh random pattern for a single ring - used by
// load_level_bricks() to procedurally build each level. Difficulty
// (fewer empty gaps, higher hp) scales gently with s_cycle, so
// repeated trips through the level loop stay meaningfully harder
// rather than looping the exact same pattern forever.
static void generate_ring_pattern(int ring) {
    for (int s = 0; s < BRICKS_PER_RING; s++) s_bricks[ring][s].active = false;
    int gap_chance = 25 - (s_cycle * 3);
    if (gap_chance < 5) gap_chance = 5;
    for (int s = 0; s < BRICKS_PER_RING; s++) {
        int roll = random(100);
        if (roll < gap_chance) continue; // leave this sector empty
        int hp = 1 + (s_cycle > 0 && random(100) < 30 + s_cycle * 5 ? 1 : 0);
        if (hp > 3) hp = 3;
        place_brick(ring, s, hp);
    }
}

static void reset_ball_on_paddle() {
    for (int i = 1; i < MAX_BALLS; i++) s_balls[i].active = false;
    s_balls[0].active = true;
    float x, y;
    polar_to_xy(s_paddle_angle_deg, PADDLE_RADIUS - BALL_R - 6, &x, &y);
    s_balls[0].x = x;
    s_balls[0].y = y;
    s_balls[0].vx = 0;
    s_balls[0].vy = 0;
    s_ball_launched = false;
    s_ball_launch_at_ms = millis() + 1200;
}

static void start_boss() {
    // Boss levels never call load_level_bricks(), so without this
    // s_full_mobility would leak whatever value the previous (non-
    // boss) level left it at - boss fights always use the standard
    // bottom-arc paddle, regardless of what came before.
    s_full_mobility = false;
    // Same reasoning for the ring radii themselves - the boss uses
    // RING_R_IN[0]/RING_R_OUT[0] for its own arc band and collision,
    // so if the previous level was full-mobility (compressed radii),
    // the boss would be drawn and hit-tested at the wrong radius
    // without this reset.
    for (int i = 0; i < BRICK_RINGS; i++) {
        RING_R_IN[i]  = RING_R_IN_DEFAULT[i];
        RING_R_OUT[i] = RING_R_OUT_DEFAULT[i];
    }

    // Randomly pick which boss shows up this time, each with genuinely
    // different stats and behavior rather than just a difficulty
    // scale-up of the same fight:
    //  - Sweeper: the original boss - moves steadily, moderate
    //    everything.
    //  - Spinner: stays put but fires fast and is a narrower target -
    //    a glass cannon that punishes hesitation.
    //  - Tank: barely moves and fires rarely, but has much more hp and
    //    a wide arc - a damage-sponge war of attrition.
    s_boss_type = (BossType)random(BOSS_TYPE_COUNT);
    switch (s_boss_type) {
        case BOSS_SPINNER:
            s_boss_hp_max = 20 + s_cycle * 6;
            s_boss_arc_width_deg = 40.0f;
            s_boss_speed_mult = 0.0f;
            s_boss_fire_mult = 0.55f;
            break;
        case BOSS_TANK:
            s_boss_hp_max = 42 + s_cycle * 12;
            s_boss_arc_width_deg = 85.0f;
            s_boss_speed_mult = 0.4f;
            s_boss_fire_mult = 1.6f;
            break;
        case BOSS_SWEEPER:
        default:
            s_boss_hp_max = 26 + s_cycle * 8;
            s_boss_arc_width_deg = 60.0f;
            s_boss_speed_mult = 1.0f;
            s_boss_fire_mult = 1.0f;
            break;
    }
    s_boss_hp = s_boss_hp_max;
    s_boss_angle_deg = TOP_ARC_CENTER_DEG;
    s_boss_dir = 1.0f;
    s_boss_last_fire_ms = millis();
    for (int i = 0; i < MAX_BOSS_SHOTS; i++) s_boss_shots[i].active = false;

    // Little helpers flanking the boss - stationary, low-hp targets in
    // ring 1 that don't attack on their own (kept simple deliberately,
    // see the chat), but a boss fight isn't just the boss anymore -
    // there's always something else demanding the ball's attention.
    float helper_angles[MAX_HELPERS] = { TOP_ARC_CENTER_DEG - 45.0f, TOP_ARC_CENTER_DEG + 45.0f, TOP_ARC_CENTER_DEG };
    for (int i = 0; i < MAX_HELPERS; i++) {
        s_helpers[i].active = (i < 2); // the 3rd (dead-center) only appears on tougher cycles
        if (i == 2 && s_cycle >= 1) s_helpers[i].active = true;
        s_helpers[i].angle_deg = helper_angles[i];
        s_helpers[i].r = (RING_R_IN[1] + RING_R_OUT[1]) / 2.0f;
        s_helpers[i].hp = 2;
    }
}

static void enter_state(GState st) {
    s_state = st;
    s_state_enter_ms = millis();
}

static void enter_calibrating() {
    s_state = GS_CALIBRATING;
    s_cal_start_ms = millis();
    s_cal_sum_x = 0.0f;
    s_cal_samples = 0;
}

// Shared by both advance_to_next_level() and start_new_game(level) -
// the actual "set up and enter level N" logic (difficulty tier,
// unlock-progress saving, boss-vs-normal dispatch) only lives here
// once instead of being duplicated between "next level" and "player
// picked a level from the map" paths.
static void enter_level(int level) {
    s_level = level;
    // Difficulty tier within the current 50-level loop (0-9), plus a
    // bonus per full completed loop so looping back past TOTAL_LEVELS
    // stays meaningfully harder than level 1 was the first time.
    int within_loop = ((s_level - 1) % TOTAL_LEVELS);
    int loops_completed = (s_level - 1) / TOTAL_LEVELS;
    s_cycle = within_loop / 5 + loops_completed * 2;

    if (s_level > s_progress.highest_unlocked_level) {
        s_progress.highest_unlocked_level = s_level;
        save_progress();
    }

    s_grow_stacks = 0; s_shrink_stacks = 0; s_shoot_until_ms = 0;
    for (int i = 0; i < MAX_PADDLE_SHOTS; i++) s_paddle_shots[i].active = false;
    recompute_paddle_width();
    if (is_boss_level(s_level)) {
        start_boss();
        s_tutorial_next_state = GS_BOSS_INTRO;
        if (!s_progress.seen_boss_tip) {
            s_progress.seen_boss_tip = true;
            save_progress();
            enter_state(GS_TUTORIAL_POPUP);
        } else {
            enter_state(GS_BOSS_INTRO);
        }
    } else {
        load_level_bricks(s_level);
        s_tutorial_next_state = GS_PLAYING;
        if (s_full_mobility && !s_progress.seen_full_mobility_tip) {
            s_progress.seen_full_mobility_tip = true;
            save_progress();
            enter_state(GS_TUTORIAL_POPUP);
        } else {
            enter_state(GS_PLAYING);
        }
    }
    reset_ball_on_paddle();
}

static void advance_to_next_level() {
    enter_level(s_level + 1);
}

static void start_new_game(int starting_level) {
    s_lives = START_LIVES;
    s_score = 0;
    s_paddle_angle_deg = BOTTOM_ARC_CENTER_DEG;
    for (int i = 0; i < MAX_POWERUPS; i++) s_powerups[i].active = false;
    enter_level(starting_level);
    game_music_start("/music/breakout.mp3"); // no-ops silently if missing
}

static void lose_life() {
    s_lives--;
    if (s_lives <= 0) {
        enter_state(GS_GAMEOVER);
        game_sfx_gameover();
        if (s_score > s_progress.best_score) {
            s_progress.best_score = s_score;
            save_progress();
        }
    } else {
        game_sfx_hit();
        reset_ball_on_paddle();
    }
}

static void breakout_create() {
    s_state = GS_TITLE;
    s_prev_pressed = false; // don't let a stale true from a previous session swallow this session's first tap
    load_progress();
    s_paddle_angle_deg = BOTTOM_ARC_CENTER_DEG;
    s_grow_stacks = 0; s_shrink_stacks = 0; s_shoot_until_ms = 0;
    for (int i = 0; i < MAX_PADDLE_SHOTS; i++) s_paddle_shots[i].active = false;
    recompute_paddle_width();
    s_last_phys_step = millis();
    for (int i = 0; i < MAX_BALLS; i++) s_balls[i].active = false;
    for (int i = 0; i < MAX_POWERUPS; i++) s_powerups[i].active = false;
}

static void breakout_destroy() {
    game_music_stop();
}

// ---- Physics ---------------------------------------------------------

static float paddle_min_deg() { return BOTTOM_ARC_CENTER_DEG - BOTTOM_ARC_HALFSPAN_DEG + s_paddle_arc_width_deg / 2.0f; }
static float paddle_max_deg() { return BOTTOM_ARC_CENTER_DEG + BOTTOM_ARC_HALFSPAN_DEG - s_paddle_arc_width_deg / 2.0f; }

// Finds the brick (if any) at the given polar position and damages it
// by 1 hp - shared by the ball's bounce-and-damage collision AND the
// paddle shots' hit-and-consume collision, so the destroy/score/drop-
// chance logic only lives in one place instead of being duplicated
// for the new shooting powerup. Returns true if a brick was found and
// hit (destroyed or not).
static bool try_damage_brick_at(float theta_deg, float r, float r_tolerance) {
    if (is_boss_level(s_level)) return false;
    float sector_w = brick_sector_width_deg();
    float start_deg = brick_sector_start_deg();
    for (int ring = 0; ring < BRICK_RINGS; ring++) {
        if (r < RING_R_IN[ring] - r_tolerance || r > RING_R_OUT[ring] + r_tolerance) continue;
        float rel;
        if (s_full_mobility) {
            // Full circle: normalize straight to the 0-360 range
            // rather than using angle_diff_deg's -180..180 range,
            // which can't represent a span wider than 180 degrees on
            // its own.
            rel = fmodf(theta_deg - start_deg + 360.0f, 360.0f);
        } else {
            rel = angle_diff_deg(theta_deg, start_deg);
            if (rel < 0 || rel > 2.0f * TOP_ARC_HALFSPAN_DEG) continue;
        }
        int sector = (int)(rel / sector_w);
        if (sector < 0 || sector >= BRICKS_PER_RING) continue;
        if (!s_bricks[ring][sector].active) continue;

        s_bricks[ring][sector].hp--;
        if (s_bricks[ring][sector].hp <= 0) {
            s_bricks[ring][sector].active = false;
            s_bricks_remaining--;
            s_score += 10;
            game_sfx_explosion();
            if (random(100) < 15) {
                float mid_a = start_deg + (sector + 0.5f) * sector_w;
                float px, py;
                polar_to_xy(mid_a, (RING_R_IN[ring] + RING_R_OUT[ring]) / 2.0f, &px, &py);
                spawn_powerup(px, py);
            }
        } else {
            game_sfx_hit();
        }
        return true;
    }
    return false;
}

static void physics_step() {
    float dt = PHYS_STEP_MS / 1000.0f;
    uint32_t now = millis();

    // Tilt steering, relative to the calibrated center - one axis only
    // (left/right roll), direct position mapping like game_plane.cpp.
    // Full-mobility levels use a different mapping entirely - see
    // below.
    ImuSample s = imu_read();
    if (s.valid) {
        float tx, ty;
        imu_get_calibrated_tilt(s, tx, ty);
        if (s_full_mobility) {
            // Full 360 mapping: the paddle's angle directly follows
            // the watch's own orientation (which way gravity points
            // relative to the device) - the same idea auto-rotate
            // uses - so rotating the device moves the paddle anywhere
            // around the circle. This is a genuinely different input
            // scheme from normal levels' left-right tilt-steering,
            // which is physically capped at roughly 150 degrees of
            // total travel (BOTTOM_ARC_HALFSPAN_DEG*2) since a wrist
            // can only roll side to side so far - not something that
            // could reach 360 degrees no matter how the gain constant
            // was tuned.
            //
            // Honesty note: the exact rotation direction/sign here
            // hasn't been verified against real hardware, the same
            // caveat as the auto-rotate angle fix earlier in this
            // project - if turning the watch moves the paddle the
            // "wrong" way, negating this angle (360 - angle) is the
            // fix to try first.
            s_paddle_angle_deg = atan2f(ty, tx) * 180.0f / (float)M_PI;
            if (s_paddle_angle_deg < 0.0f) s_paddle_angle_deg += 360.0f;
        } else {
            s_paddle_angle_deg = BOTTOM_ARC_CENTER_DEG + (tx - s_cal_x) * TILT_GAIN_DEG;
        }
    }
    // Controller d-pad - continuous angle nudge (no "absolute tilt
    // position" for a digital d-pad to map to directly, unlike touch/
    // tilt, so this is a speed-based adjustment instead), additive on
    // top of whatever tilt already set this frame.
    if (controller_connected()) {
        const float CTRL_PADDLE_SPEED = 90.0f; // deg/sec
        if (controller_dpad(CTRL_LEFT)) s_paddle_angle_deg -= CTRL_PADDLE_SPEED * dt;
        if (controller_dpad(CTRL_RIGHT)) s_paddle_angle_deg += CTRL_PADDLE_SPEED * dt;
    }
    if (s_full_mobility) {
        // No bottom-arc clamp - free 360 travel. Still normalize into
        // a bounded range so the value doesn't grow without limit as
        // the player rotates around multiple times, which would
        // eventually lose float precision on a long play session.
        s_paddle_angle_deg = fmodf(s_paddle_angle_deg, 360.0f);
        if (s_paddle_angle_deg < 0.0f) s_paddle_angle_deg += 360.0f;
    } else {
        float lo = paddle_min_deg(), hi = paddle_max_deg();
        if (s_paddle_angle_deg < lo) s_paddle_angle_deg = lo;
        if (s_paddle_angle_deg > hi) s_paddle_angle_deg = hi;
    }

    if (s_grow_stacks > 0 && now >= s_grow_until_ms) {
        s_grow_stacks = 0;
        recompute_paddle_width();
    }
    if (s_shrink_stacks > 0 && now >= s_shrink_until_ms) {
        s_shrink_stacks = 0;
        recompute_paddle_width();
    }

    if (!s_ball_launched && now >= s_ball_launch_at_ms) {
        s_ball_launched = true;
        float a = s_paddle_angle_deg * DEG2RAD;
        float speed = BALL_SPEED_BASE * speed_mult();
        s_balls[0].vx = -cosf(a) * speed + sinf(a) * 30.0f;
        s_balls[0].vy = -sinf(a) * speed - cosf(a) * 30.0f;
    }

    for (int i = 0; i < MAX_BALLS; i++) {
        if (!s_balls[i].active) continue;
        if (!s_ball_launched) {
            float x, y;
            polar_to_xy(s_paddle_angle_deg, PADDLE_RADIUS - BALL_R - 6, &x, &y);
            s_balls[i].x = x;
            s_balls[i].y = y;
            continue;
        }

        s_balls[i].x += s_balls[i].vx * dt;
        s_balls[i].y += s_balls[i].vy * dt;

        float dx = s_balls[i].x - ARENA_CX, dy = s_balls[i].y - ARENA_CY;
        float r = sqrtf(dx * dx + dy * dy);
        float theta_deg = atan2f(dy, dx) / DEG2RAD;
        if (theta_deg < 0) theta_deg += 360.0f;

        bool hit_something = false;
        if (!is_boss_level(s_level)) {
            if (try_damage_brick_at(theta_deg, r, BALL_R + HITBOX_PADDING)) {
                reflect_radial(theta_deg, &s_balls[i].vx, &s_balls[i].vy);
                hit_something = true;
            }
        } else if (r >= RING_R_IN[0] - BALL_R && r <= RING_R_OUT[0] + BALL_R) {
            float rel = angle_diff_deg(theta_deg, s_boss_angle_deg);
            if (fabsf(rel) <= s_boss_arc_width_deg / 2.0f) {
                reflect_radial(theta_deg, &s_balls[i].vx, &s_balls[i].vy);
                s_boss_hp--;
                s_score += 5;
                game_sfx_hit();
                hit_something = true;
            }
        }

        // Helper collision - point targets, so a straightforward
        // cartesian distance check rather than the ring/arc math the
        // bricks and boss use, which assumes a whole band/sector, not
        // one small spot.
        if (!hit_something && is_boss_level(s_level)) {
            for (int h = 0; h < MAX_HELPERS; h++) {
                if (!s_helpers[h].active) continue;
                float hx, hy;
                polar_to_xy(s_helpers[h].angle_deg, s_helpers[h].r, &hx, &hy);
                float hdx = s_balls[i].x - hx, hdy = s_balls[i].y - hy;
                if (hdx * hdx + hdy * hdy > (BALL_R + 10.0f) * (BALL_R + 10.0f)) continue;
                reflect_radial(theta_deg, &s_balls[i].vx, &s_balls[i].vy);
                s_helpers[h].hp--;
                if (s_helpers[h].hp <= 0) {
                    s_helpers[h].active = false;
                    s_score += 15;
                    game_sfx_explosion();
                } else {
                    game_sfx_hit();
                }
                hit_something = true;
                break;
            }
        }

        if (!hit_something && r >= ARENA_R - BALL_R - HITBOX_PADDING) {
            // Full-mobility levels: the paddle can be anywhere around
            // the circle, so the whole outer wall is "paddle
            // territory" - there's no separate safe zone to just
            // bounce off elsewhere the way normal levels' non-bottom-
            // arc wall segments work.
            bool in_paddle_zone;
            if (s_full_mobility) {
                in_paddle_zone = true;
            } else {
                float rel_bottom = angle_diff_deg(theta_deg, BOTTOM_ARC_CENTER_DEG);
                in_paddle_zone = fabsf(rel_bottom) <= BOTTOM_ARC_HALFSPAN_DEG;
            }
            if (in_paddle_zone) {
                float rel_paddle = angle_diff_deg(theta_deg, s_paddle_angle_deg);
                if (fabsf(rel_paddle) <= s_paddle_arc_width_deg / 2.0f + 4.0f + HITBOX_PADDING) {
                    reflect_radial(theta_deg, &s_balls[i].vx, &s_balls[i].vy);
                    float speed = sqrtf(s_balls[i].vx * s_balls[i].vx + s_balls[i].vy * s_balls[i].vy);
                    float offset = rel_paddle / (s_paddle_arc_width_deg / 2.0f + 4.0f + HITBOX_PADDING);
                    float ta = s_paddle_angle_deg * DEG2RAD;
                    float tx = -sinf(ta), ty = cosf(ta);
                    s_balls[i].vx += offset * speed * 0.6f * tx;
                    s_balls[i].vy += offset * speed * 0.6f * ty;
                    float ns = sqrtf(s_balls[i].vx * s_balls[i].vx + s_balls[i].vy * s_balls[i].vy);
                    if (ns > 0.001f) {
                        s_balls[i].vx = s_balls[i].vx / ns * speed;
                        s_balls[i].vy = s_balls[i].vy / ns * speed;
                    }
                    game_sfx_hit();
                } else {
                    s_balls[i].active = false;
                }
            } else {
                reflect_radial(theta_deg, &s_balls[i].vx, &s_balls[i].vy);
            }
            float clamp_r = ARENA_R - BALL_R - 1.0f;
            if (r > clamp_r && s_balls[i].active) {
                float nx = dx / r, ny = dy / r;
                s_balls[i].x = ARENA_CX + nx * clamp_r;
                s_balls[i].y = ARENA_CY + ny * clamp_r;
            }
        }
    }

    if (count_active_balls() == 0) {
        lose_life();
        return;
    }

    if (!is_boss_level(s_level) && s_bricks_remaining <= 0) {
        s_score += 100;
        game_sfx_levelup();
        enter_state(GS_LEVEL_CLEAR);
        return;
    }

    if (is_boss_level(s_level)) {
        float lo = TOP_ARC_CENTER_DEG - TOP_ARC_HALFSPAN_DEG + s_boss_arc_width_deg / 2.0f;
        float hi = TOP_ARC_CENTER_DEG + TOP_ARC_HALFSPAN_DEG - s_boss_arc_width_deg / 2.0f;
        s_boss_angle_deg += s_boss_dir * (35.0f + s_cycle * 5.0f) * s_boss_speed_mult * dt;
        if (s_boss_angle_deg < lo) { s_boss_angle_deg = lo; s_boss_dir = 1.0f; }
        if (s_boss_angle_deg > hi) { s_boss_angle_deg = hi; s_boss_dir = -1.0f; }

        uint32_t fire_interval = (uint32_t)((1900 - s_cycle * 150) * s_boss_fire_mult);
        if (fire_interval < 400) fire_interval = 400;
        if (now - s_boss_last_fire_ms >= fire_interval) {
            s_boss_last_fire_ms = now;
            for (int i = 0; i < MAX_BOSS_SHOTS; i++) {
                if (s_boss_shots[i].active) continue;
                s_boss_shots[i].active = true;
                float bx, by;
                polar_to_xy(s_boss_angle_deg, (RING_R_IN[0] + RING_R_OUT[0]) / 2.0f, &bx, &by);
                // Aimed at the paddle's position right now - a straight
                // line from the boss (top) toward the paddle (bottom),
                // rather than a radius-only motion that could only ever
                // travel back along the boss's own spawn angle and
                // could never reach the opposite side of the arena
                // where the paddle actually lives.
                float px, py;
                polar_to_xy(s_paddle_angle_deg, PADDLE_RADIUS, &px, &py);
                float ddx = px - bx, ddy = py - by;
                float dist = sqrtf(ddx * ddx + ddy * ddy);
                float speed = 90.0f + s_cycle * 12.0f;
                if (dist > 0.001f) {
                    s_boss_shots[i].vx = ddx / dist * speed;
                    s_boss_shots[i].vy = ddy / dist * speed;
                } else {
                    s_boss_shots[i].vx = 0; s_boss_shots[i].vy = speed;
                }
                s_boss_shots[i].x = bx;
                s_boss_shots[i].y = by;
                break;
            }
        }

        for (int i = 0; i < MAX_BOSS_SHOTS; i++) {
            if (!s_boss_shots[i].active) continue;
            s_boss_shots[i].x += s_boss_shots[i].vx * dt;
            s_boss_shots[i].y += s_boss_shots[i].vy * dt;

            float sdx = s_boss_shots[i].x - ARENA_CX, sdy = s_boss_shots[i].y - ARENA_CY;
            float sr = sqrtf(sdx * sdx + sdy * sdy);
            float stheta = atan2f(sdy, sdx) / DEG2RAD;
            if (stheta < 0) stheta += 360.0f;

            if (sr >= PADDLE_RADIUS - 20.0f && sr <= PADDLE_RADIUS + 10.0f) {
                float rel = angle_diff_deg(stheta, s_paddle_angle_deg);
                if (fabsf(rel) > s_paddle_arc_width_deg / 2.0f + 4.0f) {
                    s_boss_shots[i].active = false;
                    lose_life();
                    if (s_state != GS_PLAYING) return;
                } else {
                    s_boss_shots[i].active = false; // blocked by the paddle
                }
            } else if (sr >= ARENA_R) {
                s_boss_shots[i].active = false;
            }
        }

        if (s_boss_hp <= 0) {
            s_score += 500;
            game_sfx_levelup();
            game_sfx_explosion();
            enter_state(GS_LEVEL_CLEAR);
            return;
        }
    }

    for (int i = 0; i < MAX_POWERUPS; i++) {
        if (!s_powerups[i].active) continue;
        s_powerups[i].x += s_powerups[i].vx * dt;
        s_powerups[i].y += s_powerups[i].vy * dt;

        float pdx = s_powerups[i].x - ARENA_CX, pdy = s_powerups[i].y - ARENA_CY;
        float pr = sqrtf(pdx * pdx + pdy * pdy);
        float ptheta = atan2f(pdy, pdx) / DEG2RAD;
        if (ptheta < 0) ptheta += 360.0f;

        if (pr >= PADDLE_RADIUS - 20.0f) {
            float rel = angle_diff_deg(ptheta, s_paddle_angle_deg);
            bool in_paddle_arc = fabsf(rel) <= s_paddle_arc_width_deg / 2.0f + 6.0f;
            // Whole outer edge is paddle territory in full-mobility
            // levels (paddle can be anywhere), same reasoning as the
            // ball-paddle collision fix above.
            bool in_paddle_home = s_full_mobility ? true :
                fabsf(angle_diff_deg(ptheta, BOTTOM_ARC_CENTER_DEG)) <= BOTTOM_ARC_HALFSPAN_DEG;
            if (pr <= PADDLE_RADIUS + 6.0f && in_paddle_arc) {
                game_sfx_powerup();
                switch (s_powerups[i].type) {
                    case PU_GROW:
                        if (s_grow_stacks < MAX_GROW_STACKS) s_grow_stacks++;
                        s_grow_until_ms = now + BUFF_DURATION_MS;
                        recompute_paddle_width();
                        break;
                    case PU_SHRINK:
                        // A bad pickup landing on the paddle still
                        // applies - same as any other powerup falling
                        // into the catch zone, it's not something the
                        // player can selectively avoid catching.
                        if (s_shrink_stacks < MAX_SHRINK_STACKS) s_shrink_stacks++;
                        s_shrink_until_ms = now + BUFF_DURATION_MS;
                        recompute_paddle_width();
                        break;
                    case PU_SHOOT:
                        s_shoot_until_ms = now + BUFF_DURATION_MS;
                        break;
                    case PU_MULTIBALL: {
                        Ball *src = nullptr;
                        for (int b = 0; b < MAX_BALLS; b++) if (s_balls[b].active) { src = &s_balls[b]; break; }
                        for (int b = 0; b < MAX_BALLS && src; b++) {
                            if (s_balls[b].active) continue;
                            s_balls[b] = *src;
                            s_balls[b].vx = -src->vx + ((float)random(60) - 30.0f);
                            s_balls[b].vy = -src->vy + ((float)random(60) - 30.0f);
                            break;
                        }
                        break;
                    }
                    case PU_LIFE:
                        if (s_lives < 5) s_lives++;
                        break;
                }
                s_powerups[i].active = false;
            } else if (pr >= ARENA_R || (in_paddle_home && pr >= PADDLE_RADIUS + 6.0f)) {
                s_powerups[i].active = false; // missed - past the paddle or off the arena
            }
        }
        if (s_powerups[i].y > ARENA_CY + ARENA_R + 20.0f) s_powerups[i].active = false;
    }

    // Shooting powerup: auto-fires straight inward from wherever the
    // paddle currently is, on a fixed interval, for as long as
    // s_shoot_until_ms hasn't passed. Not stacking (the request only
    // asked for size to stack) - collecting it again just refreshes
    // the duration.
    if (now < s_shoot_until_ms && now - s_last_paddle_shot_ms >= SHOOT_FIRE_INTERVAL_MS) {
        s_last_paddle_shot_ms = now;
        for (int i = 0; i < MAX_PADDLE_SHOTS; i++) {
            if (s_paddle_shots[i].active) continue;
            float x, y;
            polar_to_xy(s_paddle_angle_deg, PADDLE_RADIUS - 4.0f, &x, &y);
            s_paddle_shots[i].x = x;
            s_paddle_shots[i].y = y;
            // Straight line toward the arena center - same reasoning
            // as Powerup/BossShot above: a shot fired from off-center
            // still needs to actually cross into the brick rings on
            // the far side, not just travel parallel along its own
            // spawn radius.
            float dirx = ARENA_CX - x, diry = ARENA_CY - y;
            float dlen = sqrtf(dirx * dirx + diry * diry);
            if (dlen < 1.0f) dlen = 1.0f;
            const float SHOT_SPEED = 260.0f;
            s_paddle_shots[i].vx = dirx / dlen * SHOT_SPEED;
            s_paddle_shots[i].vy = diry / dlen * SHOT_SPEED;
            s_paddle_shots[i].active = true;
            game_sfx_shoot();
            break;
        }
    }
    for (int i = 0; i < MAX_PADDLE_SHOTS; i++) {
        if (!s_paddle_shots[i].active) continue;
        s_paddle_shots[i].x += s_paddle_shots[i].vx * dt;
        s_paddle_shots[i].y += s_paddle_shots[i].vy * dt;

        float sdx = s_paddle_shots[i].x - ARENA_CX, sdy = s_paddle_shots[i].y - ARENA_CY;
        float sr = sqrtf(sdx * sdx + sdy * sdy);
        float stheta = atan2f(sdy, sdx) / DEG2RAD;
        if (stheta < 0) stheta += 360.0f;

        if (try_damage_brick_at(stheta, sr, 4.0f)) {
            s_paddle_shots[i].active = false; // consumed on hit, unlike the ball
            continue;
        }
        if (sr > ARENA_R) s_paddle_shots[i].active = false; // missed everything, exited the arena
    }
}

// ---- Draw ---------------------------------------------------------------

// Two circles + a triangle - the standard simple "programmer's heart"
// shape, built from primitives Arduino_GFX already has (fillCircle/
// drawCircle, fillTriangle/drawTriangle). filled=true for a life still
// remaining, false for one already lost (outline only).
static uint16_t brick_color(int hp) {
    if (hp >= 3) return COLOR_BAD;
    if (hp == 2) return COLOR_WARN;
    return COLOR_GOOD;
}

static void draw_sector_brick(Arduino_GFX *g, float a0, float a1, float r_in, float r_out, uint16_t color) {
    float x1, y1, x2, y2, x3, y3, x4, y4;
    polar_to_xy(a0, r_in, &x1, &y1);
    polar_to_xy(a1, r_in, &x2, &y2);
    polar_to_xy(a1, r_out, &x3, &y3);
    polar_to_xy(a0, r_out, &x4, &y4);
    g->fillTriangle((int)x1, (int)y1, (int)x2, (int)y2, (int)x3, (int)y3, color);
    g->fillTriangle((int)x1, (int)y1, (int)x3, (int)y3, (int)x4, (int)y4, color);
}

static void draw_arc_band(Arduino_GFX *g, float center_deg, float halfwidth_deg, float r_in, float r_out, uint16_t color, int steps = 8) {
    float a0 = center_deg - halfwidth_deg;
    float step = (2.0f * halfwidth_deg) / steps;
    for (int i = 0; i < steps; i++) {
        draw_sector_brick(g, a0 + i * step, a0 + (i + 1) * step, r_in, r_out, color);
    }
}

// A life indicator: a short glowing red arc segment near the top edge
// of the display, replacing the earlier heart-icon design. "Glow" is
// approximated the usual way on a display without real alpha
// blending - a wider, dimmer arc drawn first, then a narrower,
// brighter one on top of it. Drawing purely in polar coordinates at a
// fixed radius from center means this is automatically safe on the
// round display regardless of angle, as long as the radius itself
// stays under the display's own radius (233) - no rectangular-element
// safe-zone math needed here. A lost life draws nothing at all
// (empty space where the arc used to be), not a dimmed/outline
// version - "one disappears" as stated.
// A life indicator: a thin glowing red arc segment - five of them
// spread evenly all the way around the display's outer edge (not
// clustered near the top like the earlier design), with real gaps
// between them so they read as distinct segments rather than one
// solid ring. "Glow" is approximated the usual way on a display
// without real alpha blending - a wider, dimmer arc drawn first, then
// a thin, brighter one on top. Drawing purely in polar coordinates at
// a fixed radius from center means this is automatically safe on the
// round display regardless of angle, as long as the radius stays
// under the display's own radius (233). A lost life draws nothing at
// all (empty space where the segment used to be), not a dimmed/
// outline version.
// A life indicator: a soft red glow near the display's outer edge,
// not a bordered shape - "no defined borders, like light" rather than
// the earlier bright-cored arc-band design, which (using the same
// draw_arc_band()/draw_sector_brick() technique as actual bricks)
// read too much like just another tile. Simulated by several
// concentric, progressively dimmer and wider layers with no single
// sharp edge anywhere - even the brightest/innermost layer stays a
// muted red rather than full COLOR_BAD, so nothing in the stack reads
// as a solid, defined shape. Five of them spread evenly all the way
// around the display's outer edge, with real gaps between them so
// they read as distinct rather than one ring.
static void draw_life_arc(Arduino_GFX *g, float center_deg, float halfwidth_deg, bool filled) {
    if (!filled) return;
    draw_arc_band(g, center_deg, halfwidth_deg + 6.0f, ARENA_R, ARENA_R + 7.0f, COLOR565(0x1c, 0, 0), 4);
    draw_arc_band(g, center_deg, halfwidth_deg + 3.5f, ARENA_R + 1.0f, ARENA_R + 6.0f, COLOR565(0x30, 0, 0), 4);
    draw_arc_band(g, center_deg, halfwidth_deg + 1.5f, ARENA_R + 2.0f, ARENA_R + 5.0f, COLOR565(0x48, 0, 0), 4);
    draw_arc_band(g, center_deg, halfwidth_deg, ARENA_R + 3.0f, ARENA_R + 4.5f, COLOR565(0x62, 0, 0), 4);
}

static void breakout_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    g->drawCircle(ARENA_CX, ARENA_CY, (int)ARENA_R, COLOR_PANEL);

    // The two narrow gaps between the brick arc (190-350deg) and the
    // paddle's travel range (15-165deg) are always a solid wall on
    // normal levels - never bricks, never reachable by the paddle
    // either way. Reverted back to two small segments marking the two
    // transition points (rather than one continuous band across the
    // whole top) per feedback that the full semicircle didn't read
    // well. Full-mobility levels have no fixed paddle-zone/wall-zone
    // split at all (the paddle can be anywhere), so this doesn't
    // apply to them.
    if (!s_full_mobility && (s_state == GS_PLAYING || s_state == GS_LEVEL_CLEAR || s_state == GS_BOSS_INTRO)) {
        uint16_t wall_c = COLOR_TEXT_DIM;
        draw_arc_band(g, 177.5f, 12.5f, ARENA_R - 8.0f, ARENA_R, wall_c);
        draw_arc_band(g, 2.5f, 12.5f, ARENA_R - 8.0f, ARENA_R, wall_c);
    }

    if (s_state == GS_TITLE) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 90, COLOR_TEXT, "BRICK BREAKER", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_TEXT_DIM, "Tilt: steer paddle", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_TEXT_DIM, "along the bottom rim", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 30, COLOR_TEXT_DIM, "Break rings, beat the boss", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 80, COLOR_ACCENT2, "Tap to start", 2);
        return;
    }

    if (s_state == GS_LEVEL_MAP) {
        int cx0 = LCD_WIDTH / 2, cy0 = LCD_HEIGHT / 2;

        // Path lines first, so nodes draw on top of them.
        for (int lvl = 1; lvl < TOTAL_LEVELS; lvl++) {
            float wx0, wy0, wx1, wy1;
            level_node_world_pos(lvl, &wx0, &wy0);
            level_node_world_pos(lvl + 1, &wx1, &wy1);
            int x0 = cx0 + (int)(wx0 - s_map_pan_x), y0 = cy0 + (int)(wy0 - s_map_pan_y);
            int x1 = cx0 + (int)(wx1 - s_map_pan_x), y1 = cy0 + (int)(wy1 - s_map_pan_y);
            g->drawLine(x0, y0, x1, y1, COLOR_PANEL);
        }

        for (int lvl = 1; lvl <= TOTAL_LEVELS; lvl++) {
            float wx, wy;
            level_node_world_pos(lvl, &wx, &wy);
            int sx = cx0 + (int)(wx - s_map_pan_x), sy = cy0 + (int)(wy - s_map_pan_y);
            if (sx < -30 || sx > LCD_WIDTH + 30 || sy < -30 || sy > LCD_HEIGHT + 30) continue;

            bool unlocked = lvl <= s_progress.highest_unlocked_level;
            bool is_next = lvl == s_progress.highest_unlocked_level;
            bool boss = is_boss_level(lvl);
            int r = boss ? 22 : 16;
            uint16_t c = !unlocked ? COLOR_PANEL : boss ? COLOR_BAD : (is_next ? COLOR_ACCENT2 : COLOR_GOOD);

            g->fillCircle(sx, sy, r, c);
            g->drawCircle(sx, sy, r, unlocked ? COLOR_TEXT : COLOR_TEXT_DIM);
            if (is_next) g->drawCircle(sx, sy, r + 4, COLOR_ACCENT2); // pulseless "you are here" ring

            char lbuf[4];
            snprintf(lbuf, sizeof(lbuf), "%d", lvl);
            ui_draw_centered_text_at(sx, sy - 6, unlocked ? COLOR_TEXT : COLOR_TEXT_DIM, lbuf, 1);
        }

        ui_draw_centered_text(50, COLOR_TEXT, "Level Map", 2);
        char pbuf[32];
        snprintf(pbuf, sizeof(pbuf), "Furthest: %d / %d", s_progress.highest_unlocked_level, TOTAL_LEVELS);
        ui_draw_centered_text(LCD_HEIGHT - 40, COLOR_TEXT_DIM, pbuf, 1);
        return;
    }

    if (s_state == GS_TUTORIAL_POPUP) {
        // First-time explanation for a level type the player hasn't
        // seen before - shown once ever per type (see
        // seen_full_mobility_tip/seen_boss_tip in BreakoutProgress),
        // dismissed by a tap (handled in breakout_touch()).
        bool boss = (s_tutorial_next_state == GS_BOSS_INTRO);
        if (boss) {
            ui_draw_centered_text(90, COLOR_BAD, "Boss Fight!", 3);
            ui_draw_centered_text(150, COLOR_TEXT, "Hit the boss with the ball", 1);
            ui_draw_centered_text(175, COLOR_TEXT, "to deal damage.", 1);
            ui_draw_centered_text(215, COLOR_TEXT, "Block its shots with your", 1);
            ui_draw_centered_text(240, COLOR_TEXT, "paddle - missing one costs", 1);
            ui_draw_centered_text(265, COLOR_TEXT, "a life.", 1);
            ui_draw_centered_text(305, COLOR_TEXT_DIM, "Watch for helper enemies too.", 1);
        } else {
            ui_draw_centered_text(90, COLOR_ACCENT2, "Free Roam Level", 3);
            ui_draw_centered_text(160, COLOR_TEXT, "Rotate your wrist to move", 1);
            ui_draw_centered_text(185, COLOR_TEXT, "the paddle anywhere around", 1);
            ui_draw_centered_text(210, COLOR_TEXT, "the edge.", 1);
            ui_draw_centered_text(250, COLOR_TEXT_DIM, "Bricks now ring the center", 1);
            ui_draw_centered_text(275, COLOR_TEXT_DIM, "instead of just the top.", 1);
        }
        ui_draw_centered_text(360, COLOR_ACCENT2, "Tap to continue", 2);
        return;
    }

    if (s_state == GS_PAUSED) {
        int bw = 200, bx = LCD_WIDTH / 2 - bw / 2;
        ui_draw_centered_text(90, COLOR_TEXT, "PAUSED", 3);

        ui_draw_centered_text(140, COLOR_TEXT_DIM, "Volume", 1);
        g->drawRoundRect(bx, 150, bw, 30, 8, COLOR_TEXT_DIM);
        int vfill = (int)g_app_settings.volume * bw / 100;
        if (vfill > 0) g->fillRoundRect(bx, 150, vfill, 30, 8, COLOR_ACCENT);
        char vbuf[8];
        snprintf(vbuf, sizeof(vbuf), "%d%%", (int)g_app_settings.volume);
        ui_draw_centered_text_at(LCD_WIDTH / 2, 158, COLOR_TEXT, vbuf, 1);

        bool sfx = g_app_settings.sfx_enabled;
        g->fillRoundRect(bx, 210, bw, 40, 10, sfx ? COLOR_GOOD : COLOR_PANEL);
        g->drawRoundRect(bx, 210, bw, 40, 10, sfx ? COLOR_GOOD : COLOR_TEXT_DIM);
        ui_draw_centered_text_at(LCD_WIDTH / 2, 224, sfx ? COLOR_TEXT : COLOR_TEXT_DIM,
                                 sfx ? "SFX: On" : "SFX: Off", 2);

        g->fillRoundRect(bx, 270, bw, 40, 10, COLOR_ACCENT2);
        ui_draw_centered_text_at(LCD_WIDTH / 2, 284, COLOR_TEXT, "Resume", 2);

        g->drawRoundRect(bx, 330, bw, 40, 10, COLOR_TEXT_DIM);
        ui_draw_centered_text_at(LCD_WIDTH / 2, 344, COLOR_TEXT_DIM, "Back to Map", 2);
        return;
    }

    if (s_state == GS_CALIBRATING) {
        float progress = (float)(millis() - s_cal_start_ms) / (float)CAL_HOLD_MS;
        if (progress > 1.0f) progress = 1.0f;
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_TEXT, "Calibrating", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "Hold the watch level", 2);
        int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2 + 70, r = 40;
        int total_ticks = 28, lit = (int)(progress * total_ticks);
        for (int i = 0; i < total_ticks; i++) {
            float a = (float)i / total_ticks * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
            int x0 = cx + (int)(cosf(a) * (r - 5)), y0 = cy + (int)(sinf(a) * (r - 5));
            int x1 = cx + (int)(cosf(a) * r), y1 = cy + (int)(sinf(a) * r);
            g->drawLine(x0, y0, x1, y1, i < lit ? COLOR_GOOD : COLOR_PANEL);
        }
        return;
    }

    if (!is_boss_level(s_level)) {
        float sector_w = brick_sector_width_deg();
        float a_start = brick_sector_start_deg();
        for (int ring = 0; ring < BRICK_RINGS; ring++) {
            for (int sector = 0; sector < BRICKS_PER_RING; sector++) {
                if (!s_bricks[ring][sector].active) continue;
                float a0 = a_start + sector * sector_w + 1.5f;
                float a1 = a_start + (sector + 1) * sector_w - 1.5f;
                draw_sector_brick(g, a0, a1, RING_R_IN[ring], RING_R_OUT[ring], brick_color(s_bricks[ring][sector].hp));
            }
        }
    } else {
        uint16_t boss_c = s_boss_type == BOSS_SPINNER ? COLOR565(0x88, 0x11, 0x88) :
                          s_boss_type == BOSS_TANK ? COLOR565(0x44, 0x22, 0x11) : COLOR565(0x66, 0x11, 0x33);
        draw_arc_band(g, s_boss_angle_deg, s_boss_arc_width_deg / 2.0f, RING_R_IN[0], RING_R_OUT[0], boss_c);
        uint16_t eye = (millis() / 200) % 6 == 0 ? COLOR_TEXT : COLOR_WARN;
        float ex, ey;
        polar_to_xy(s_boss_angle_deg - 12.0f, (RING_R_IN[0] + RING_R_OUT[0]) / 2.0f, &ex, &ey);
        g->fillCircle((int)ex, (int)ey, 4, eye);
        polar_to_xy(s_boss_angle_deg + 12.0f, (RING_R_IN[0] + RING_R_OUT[0]) / 2.0f, &ex, &ey);
        g->fillCircle((int)ex, (int)ey, 4, eye);

        int bar_w = 140, bar_x = LCD_WIDTH / 2 - bar_w / 2, bar_y = 60;
        g->drawRoundRect(bar_x, bar_y, bar_w, 10, 4, COLOR_TEXT_DIM);
        int fill_w = (int)((float)bar_w * s_boss_hp / (float)s_boss_hp_max);
        if (fill_w > 0) g->fillRoundRect(bar_x, bar_y, fill_w, 10, 4, COLOR_BAD);

        for (int i = 0; i < MAX_BOSS_SHOTS; i++) {
            if (!s_boss_shots[i].active) continue;
            float x = s_boss_shots[i].x, y = s_boss_shots[i].y;
            g->fillCircle((int)x, (int)y, 5, COLOR_BAD);
        }

        for (int i = 0; i < MAX_HELPERS; i++) {
            if (!s_helpers[i].active) continue;
            float hx, hy;
            polar_to_xy(s_helpers[i].angle_deg, s_helpers[i].r, &hx, &hy);
            uint16_t hc = s_helpers[i].hp > 1 ? COLOR_ACCENT2 : COLOR_WARN;
            g->fillCircle((int)hx, (int)hy, 10, hc);
            g->drawCircle((int)hx, (int)hy, 10, COLOR_TEXT_DIM);
        }
    }

    for (int i = 0; i < MAX_POWERUPS; i++) {
        if (!s_powerups[i].active) continue;
        float x = s_powerups[i].x, y = s_powerups[i].y;
        uint16_t c;
        switch (s_powerups[i].type) {
            case PU_GROW:      c = COLOR_ACCENT2; break;
            case PU_MULTIBALL: c = COLOR_ACCENT3; break;
            case PU_SHOOT:     c = COLOR_WARN; break;
            case PU_SHRINK:    c = COLOR_BAD; break;
            default:           c = COLOR565(0xFF, 0x66, 0x99); break; // PU_LIFE
        }
        g->fillCircle((int)x, (int)y, 7, c);
        if (s_powerups[i].type == PU_SHRINK) {
            // Distinguish the "bad" pickup from PU_LIFE's similar-
            // toned pink at a glance - a small downward chevron.
            g->fillTriangle((int)x - 4, (int)y - 3, (int)x + 4, (int)y - 3, (int)x, (int)y + 4, COLOR_TEXT);
        }
    }

    for (int i = 0; i < MAX_PADDLE_SHOTS; i++) {
        if (!s_paddle_shots[i].active) continue;
        g->fillCircle((int)s_paddle_shots[i].x, (int)s_paddle_shots[i].y, 3, COLOR_WARN);
    }

    for (int i = 0; i < MAX_BALLS; i++) {
        if (!s_balls[i].active) continue;
        g->fillCircle((int)s_balls[i].x, (int)s_balls[i].y, (int)BALL_R, COLOR_TEXT);
    }

    uint16_t paddle_c = s_paddle_arc_width_deg > PADDLE_ARC_WIDTH_DEG ? COLOR_ACCENT2 :
                        s_paddle_arc_width_deg < PADDLE_ARC_WIDTH_DEG ? COLOR_BAD : COLOR_ACCENT;
    draw_arc_band(g, s_paddle_angle_deg, s_paddle_arc_width_deg / 2.0f,
                  PADDLE_RADIUS - PADDLE_THICK / 2.0f, PADDLE_RADIUS + PADDLE_THICK / 2.0f, paddle_c);

    char buf[24];
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    snprintf(buf, sizeof(buf), "%d", s_score);
    g->setCursor(16, 14);
    g->print(buf);
    // Centers 72deg apart (5*72=360, full circle), one centered at
    // the top (270) where a player naturally looks first. Halfwidth
    // 23 (46deg wide) leaves a 26deg gap between adjacent segments.
    for (int i = 0; i < 5; i++) draw_life_arc(g, 270.0f + i * 72.0f, 23.0f, i < s_lives);
    if (s_full_mobility && !is_boss_level(s_level)) {
        snprintf(buf, sizeof(buf), "Level %d (free move)", s_level);
    } else {
        snprintf(buf, sizeof(buf), is_boss_level(s_level) ? "BOSS" : "Level %d", s_level);
    }
    ui_draw_centered_text(38, COLOR_TEXT_DIM, buf, 1);

    if (s_state == GS_PLAYING && !s_ball_launched) {
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_ACCENT2, "Ready...", 2);
    }
    if (s_state == GS_LEVEL_CLEAR) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD,
            is_boss_level(s_level) ? "BOSS DEFEATED!" : "LEVEL CLEAR!", 3);
    } else if (s_state == GS_BOSS_INTRO) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_BAD, "BOSS INCOMING", 3);
        const char *boss_name = s_boss_type == BOSS_SPINNER ? "Spinner" :
                                s_boss_type == BOSS_TANK ? "Tank" : "Sweeper";
        ui_draw_centered_text(263, COLOR_TEXT_DIM, boss_name, 2);
    } else if (s_state == GS_GAMEOVER) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 16, COLOR_TEXT, buf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 46, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

// ---- Touch / tick -------------------------------------------------------

static void breakout_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    if (s_state == GS_TITLE) {
        if (tap_edge) {
            enter_state(GS_LEVEL_MAP);
            // Center the view on wherever the player left off, not
            // world origin - re-finding your place on a 50-level path
            // every time would defeat the point of it being saved.
            level_node_world_pos(s_progress.highest_unlocked_level, &s_map_pan_x, &s_map_pan_y);
            s_map_dragging = false;
        }
        return;
    }

    if (s_state == GS_LEVEL_MAP) {
        int cx0 = LCD_WIDTH / 2, cy0 = LCD_HEIGHT / 2;
        if (pressed) {
            if (!s_map_dragging) {
                s_map_dragging = true;
                s_map_drag_start_x = x;
                s_map_drag_start_y = y;
                s_map_drag_start_pan_x = s_map_pan_x;
                s_map_drag_start_pan_y = s_map_pan_y;
                s_map_moved_significantly = false;
            } else {
                int dx = x - s_map_drag_start_x, dy = y - s_map_drag_start_y;
                s_map_pan_x = s_map_drag_start_pan_x - dx;
                s_map_pan_y = s_map_drag_start_pan_y - dy;
                if (abs(dx) > 8 || abs(dy) > 8) s_map_moved_significantly = true;
            }
        } else {
            if (s_map_dragging && !s_map_moved_significantly) {
                // A tap, not a drag - hit-test every visible node.
                for (int lvl = 1; lvl <= TOTAL_LEVELS; lvl++) {
                    if (lvl > s_progress.highest_unlocked_level) continue; // locked
                    float wx, wy;
                    level_node_world_pos(lvl, &wx, &wy);
                    int sx = cx0 + (int)(wx - s_map_pan_x), sy = cy0 + (int)(wy - s_map_pan_y);
                    int r = is_boss_level(lvl) ? 22 : 16;
                    int ddx = x - sx, ddy = y - sy;
                    if (ddx * ddx + ddy * ddy <= r * r) {
                        s_selected_level = lvl;
                        enter_calibrating();
                        break;
                    }
                }
            }
            s_map_dragging = false;
        }
        return;
    }

    if (s_state == GS_GAMEOVER) {
        if (tap_edge) {
            s_selected_level = s_level; // "retry" resumes the level just lost, not the map
            enter_calibrating();
        }
        return;
    }
    if (s_state == GS_TUTORIAL_POPUP) {
        if (tap_edge) enter_state(s_tutorial_next_state);
        return;
    }
    if (s_state == GS_PLAYING) {
        if (tap_edge) enter_state(GS_PAUSED);
        return;
    }

    if (s_state == GS_PAUSED) {
        if (!pressed) return; // act on press, not release
        int bw = 200, bx = LCD_WIDTH / 2 - bw / 2;

        // Volume slider - a continuous drag, deliberately still on raw `pressed`.
        if (y >= 150 && y <= 180 && x >= bx && x <= bx + bw) {
            int pct = (x - bx) * 100 / bw;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            g_app_settings.volume = (uint8_t)pct;
            audio_set_volume(g_app_settings.volume);
            nvs_save_settings(g_app_settings);
            return;
        }
        if (!tap_edge) return; // everything below here is a one-shot button tap, not a drag
        // SFX toggle
        if (y >= 210 && y <= 250 && x >= bx && x <= bx + bw) {
            g_app_settings.sfx_enabled = !g_app_settings.sfx_enabled;
            nvs_save_settings(g_app_settings);
            return;
        }
        // Resume
        if (y >= 270 && y <= 310 && x >= bx && x <= bx + bw) {
            enter_state(GS_PLAYING);
            return;
        }
        // Back to map
        if (y >= 330 && y <= 370 && x >= bx && x <= bx + bw) {
            enter_state(GS_LEVEL_MAP);
            level_node_world_pos(s_progress.highest_unlocked_level, &s_map_pan_x, &s_map_pan_y);
            s_map_dragging = false;
            return;
        }
        return;
    }

    // GS_CALIBRATING/GS_LEVEL_CLEAR/GS_BOSS_INTRO: steering is
    // tilt-only, nothing to do on touch.
}

static void breakout_tick() {
    game_music_poll();
    uint32_t now = millis();

    if (s_state == GS_CALIBRATING) {
        ImuSample s = imu_read();
        if (s.valid) {
            float tx, ty;
            imu_get_calibrated_tilt(s, tx, ty);
            (void)ty;
            s_cal_sum_x += tx;
            s_cal_samples++;
        }
        if (now - s_cal_start_ms >= CAL_HOLD_MS) {
            if (s_cal_samples > 0) s_cal_x = s_cal_sum_x / s_cal_samples;
            start_new_game(s_selected_level);
        }
        return;
    }

    if (s_state == GS_LEVEL_CLEAR) {
        if (now - s_state_enter_ms > 1600) advance_to_next_level();
        return;
    }
    if (s_state == GS_BOSS_INTRO) {
        if (now - s_state_enter_ms > 1400) enter_state(GS_PLAYING);
        return;
    }
    if (s_state != GS_PLAYING) return;

    int guard = 4;
    while (now - s_last_phys_step >= PHYS_STEP_MS && guard-- > 0) {
        s_last_phys_step += PHYS_STEP_MS;
        physics_step();
        if (s_state != GS_PLAYING) break;
    }
}

Screen breakout_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    breakout_create, breakout_draw, breakout_touch, breakout_tick, breakout_destroy, nullptr,
    0, true, true // idle_frame_ms, suppress_idle, needs_tilt_calibration - this game steers by tilt
};
