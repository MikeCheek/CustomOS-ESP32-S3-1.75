/*
 * game_maze.cpp
 * "Maze Raider" - an original raycasting mini-shooter (DDA algorithm),
 * not a port of any existing engine.
 *
 * Controls: single-touch-drag only (matching game_breakout.cpp's
 * paddle exactly) - touch x maps directly to facing angle, and the
 * player walks continuously in whatever direction they face, so
 * steering the view also steers movement. Combat is automatic: the
 * player fires at whatever's in a narrow forward cone within range.
 * An aim reticle at screen center shows exactly where that cone is
 * pointed, and lights up when something's actually in it.
 *
 * The 3D view now fills the ENTIRE round display rather than a
 * rectangular inset: each screen column's visible height is derived
 * from the circle's own chord width at that column's x position (the
 * same technique game_flappy.cpp's tunnel uses), so the corridor
 * naturally narrows toward the round bezel instead of being clipped
 * by an arbitrary rectangle. Discrete UI (health bar, minimap, score)
 * still gets placed at positions individually checked against the
 * circle, since unlike the continuous 3D background, those really do
 * become unreadable/untappable if part of them lands outside the
 * visible glass.
 *
 * Levels: 3 wide-corridor mazes with increasing enemy counts, then a
 * boss arena every 4th level, matching this project's other games.
 *
 * Physics/game logic runs in fixed 16ms steps (see game_plane.cpp for
 * why this pattern exists in this codebase).
 */
#include "game_maze.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "hal_controller.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#define ARENA_CX (LCD_WIDTH / 2)
#define ARENA_CY (LCD_HEIGHT / 2)
#define ARENA_R   228.0f // just inside the true bezel edge
#define VIEW_LEFT  (ARENA_CX - (int)ARENA_R)
#define VIEW_RIGHT (ARENA_CX + (int)ARENA_R)
#define COL_STEP 4
#define NUM_RAYS (((int)(2 * ARENA_R)) / COL_STEP)
#define FOV_DEG 50.0f
#define DEG2RAD (3.14159265f / 180.0f)

#define MAP_W 14
#define MAP_H 14
#define MAX_ENEMIES 6
#define MAX_BULLETS 4

#define PHYS_STEP_MS 16
#define MOVE_SPEED     0.9f   // tiles/sec - was 1.6, felt too fast
#define TURN_GAIN_DEG 130.0f
#define TURN_SENSITIVITY_DEG_PER_PX (2.0f * TURN_GAIN_DEG / (float)(VIEW_RIGHT - VIEW_LEFT))
#define PLAYER_RADIUS  0.25f

#define FIRE_INTERVAL_MS 550
#define FIRE_RANGE        5.5f
#define FIRE_CONE_DEG    10.0f
#define BULLET_TRAVEL_MS  160

#define RANGED_MIN_DIST     2.4f  // backs away if the player gets closer than this
#define RANGED_MAX_DIST     4.5f  // closes in if further than this
#define RANGED_FIRE_MS   1400
#define RANGED_FIRE_RANGE   6.0f
#define RANGED_DAMAGE          6

#define TAP_MOVE_THRESHOLD_PX 10
#define TAP_MAX_MS           350

#define BOSS_EVERY_N_LEVELS 4
#define START_HEALTH      100

enum MState { M_TITLE, M_PLAYING, M_LEVELUP, M_BOSS_INTRO, M_BOSS_CLEAR, M_GAMEOVER };
enum EnemyBehavior { EBEH_MELEE, EBEH_RANGED };

struct Enemy {
    float x, y;
    int hp, hp_max;
    bool active;
    bool is_boss;
    EnemyBehavior behavior;
    uint32_t last_hit_ms;
    uint32_t last_shot_ms;
};
// Purely cosmetic - the actual hit is resolved instantly when fired;
// this just gives it a travel animation instead of an instant snap.
// enemy_shot marks it as an incoming (not outgoing) round, drawn in a
// different color, still just a screen-space visual either way.
struct Bullet {
    int sx0, sy0, sx1, sy1;
    uint32_t start_ms;
    bool active;
    bool enemy_shot;
};

// 0 = empty, 1/2/3 = wall variants (color only, same collision).
// 2-wide passages throughout instead of 1-tile mazes - wider corridors.
static const uint8_t MAP1[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,1,1,0,0,0,0,1,1,0,0,1},
    {1,0,0,1,1,0,0,0,0,1,1,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,0,0,1,1,1,1,0,0,1,1,1},
    {1,1,1,0,0,1,1,1,1,0,0,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,2,2,0,0,0,0,2,2,0,0,1},
    {1,0,0,2,2,0,0,0,0,2,2,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};
static const uint8_t MAP2[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,1,1,0,0,1,1,0,0,1},
    {1,0,0,0,0,1,1,0,0,1,1,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,0,0,0,0,0,0,0,0,1,1,1},
    {1,1,1,0,0,0,0,0,0,0,0,1,1,1},
    {1,0,0,0,0,1,1,1,1,0,0,0,0,1},
    {1,0,0,0,0,1,1,1,1,0,0,0,0,1},
    {1,1,1,0,0,0,0,0,0,0,0,1,1,1},
    {1,1,1,0,0,0,0,0,0,0,0,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,3,3,0,0,0,0,3,3,0,0,1},
    {1,0,0,3,3,0,0,0,0,3,3,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};
static const uint8_t MAP3[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,1,1,0,0,2,2,0,0,1,1,0,1},
    {1,0,1,1,0,0,2,2,0,0,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,1,1,1,1,0,0,0,0,1},
    {1,0,0,0,0,1,1,1,1,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,3,3,0,0,0,0,0,0,3,3,0,1},
    {1,0,3,3,0,0,0,0,0,0,3,3,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,1,1,1,1,0,0,0,0,1},
    {1,0,0,0,0,1,1,1,1,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};
// Boss arena: a wide-open room, no maze - the encounter is the point.
static const uint8_t MAPB[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,1,1,0,0,1,1,0,0,0,1},
    {1,0,0,0,1,1,0,0,1,1,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,1,1,0,0,1,1,0,0,0,1},
    {1,0,0,0,1,1,0,0,1,1,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

static MState s_state;
static uint8_t s_map[MAP_H][MAP_W];
static float s_px, s_py, s_pangle_deg;
static int s_health;
static int s_score, s_level;
static Enemy s_enemies[MAX_ENEMIES];
static Bullet s_bullets[MAX_BULLETS];
static uint32_t s_last_fire_ms;
static uint32_t s_last_phys_step;
static uint32_t s_state_enter_ms;
static bool s_aim_locked;
static bool s_walking = true; // toggled by a quick tap (not a drag) - see maze_touch()
static int s_touch_start_x, s_touch_start_y;
static uint32_t s_touch_start_ms;
// Rising-edge tracking for the title/gameover "tap to (re)start"
// transition only - the in-game drag tracking below already guards
// itself correctly via s_touch_start_ms and needs continuous `pressed`.
static bool s_prev_pressed;
static bool s_touch_moved;
static float s_drag_start_angle = 0.0f; // facing angle at the moment the current touch began

static bool is_boss_level(int level) { return level % BOSS_EVERY_N_LEVELS == 0; }
static bool is_wall(int mx, int my) {
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return true;
    return s_map[my][mx] != 0;
}

static void enter_state(MState st) {
    s_state = st;
    s_state_enter_ms = millis();
}

// Steps along the line between two world points checking for a wall in
// between - this was the actual bug behind "seeing and hitting enemies
// behind a wall": the wall RENDERING already correctly stops each ray
// at the first wall it hits, but enemy visibility and auto-fire
// targeting only ever checked distance and angle, never whether a
// wall actually stood between the player and the enemy.
static bool has_los(float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 0.001f) return true;
    int steps = (int)(dist / 0.15f) + 1;
    for (int i = 1; i < steps; i++) {
        float t = (float)i / steps;
        if (is_wall((int)(x0 + dx * t), (int)(y0 + dy * t))) return false;
    }
    return true;
}

static void spawn_bullet(int sx0, int sy0, int sx1, int sy1, bool enemy_shot) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (s_bullets[i].active) continue;
        s_bullets[i].active = true;
        s_bullets[i].sx0 = sx0; s_bullets[i].sy0 = sy0;
        s_bullets[i].sx1 = sx1; s_bullets[i].sy1 = sy1;
        s_bullets[i].start_ms = millis();
        s_bullets[i].enemy_shot = enemy_shot;
        return;
    }
}

static void spawn_enemy(float x, float y, bool boss) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (s_enemies[i].active) continue;
        s_enemies[i].active = true;
        s_enemies[i].is_boss = boss;
        s_enemies[i].x = x;
        s_enemies[i].y = y;
        s_enemies[i].hp_max = boss ? (14 + s_level) : 2;
        s_enemies[i].hp = s_enemies[i].hp_max;
        s_enemies[i].last_hit_ms = 0;
        s_enemies[i].last_shot_ms = millis();
        // Boss stays melee (it already hits hard up close); regular
        // enemies are a mix so a level has both kinds to deal with.
        s_enemies[i].behavior = (!boss && random(100) < 40) ? EBEH_RANGED : EBEH_MELEE;
        return;
    }
}

static void load_level(int level) {
    for (int i = 0; i < MAX_ENEMIES; i++) s_enemies[i].active = false;
    for (int i = 0; i < MAX_BULLETS; i++) s_bullets[i].active = false;

    const uint8_t (*src)[MAP_W] = MAP1;
    if (is_boss_level(level)) src = MAPB;
    else {
        int variant = (level - 1) % 3;
        src = (variant == 0) ? MAP1 : (variant == 1) ? MAP2 : MAP3;
    }
    memcpy(s_map, src, sizeof(s_map));

    s_px = 1.5f; s_py = 1.5f; s_pangle_deg = 0.0f;

    if (is_boss_level(level)) {
        spawn_enemy(MAP_W - 2.5f, MAP_H - 2.5f, true);
    } else {
        int count = 2 + level;
        if (count > MAX_ENEMIES) count = MAX_ENEMIES;
        for (int i = 0; i < count; i++) {
            int mx, my;
            int guard = 40;
            do {
                mx = 1 + random(MAP_W - 2);
                my = 1 + random(MAP_H - 2);
            } while (is_wall(mx, my) && --guard > 0);
            spawn_enemy(mx + 0.5f, my + 0.5f, false);
        }
    }
}

static void maze_reset() {
    s_health = START_HEALTH;
    s_score = 0;
    s_level = 1;
    s_last_fire_ms = millis();
    s_last_phys_step = millis();
    s_walking = true;
    s_touch_start_ms = 0;
    load_level(1);
}

static void maze_create() {
    enter_state(M_TITLE);
    maze_reset();
    s_prev_pressed = false;
}

static void maze_destroy() {}

static void die() {
    enter_state(M_GAMEOVER);
    game_sfx_gameover();
}

static float angle_diff_deg(float a, float b) {
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return d;
}

// Moves (x,y) toward (nx,ny), stopping at walls on each axis
// independently (so it slides along a wall instead of just refusing
// to move at all) - used for both the player and enemies now.
// Enemies previously moved with zero wall-collision checking at all,
// which is exactly why they could walk straight through walls toward
// the player instead of only ever approaching via open corridors.
static void move_with_collision(float *x, float *y, float nx, float ny) {
    if (!is_wall((int)(nx + (nx > *x ? PLAYER_RADIUS : -PLAYER_RADIUS)), (int)*y)) *x = nx;
    if (!is_wall((int)*x, (int)(ny + (ny > *y ? PLAYER_RADIUS : -PLAYER_RADIUS)))) *y = ny;
}

static void physics_step() {
    float dt = PHYS_STEP_MS / 1000.0f;
    uint32_t now = millis();

    if (s_walking) {
        float rad = s_pangle_deg * DEG2RAD;
        float nx = s_px + cosf(rad) * MOVE_SPEED * dt;
        float ny = s_py + sinf(rad) * MOVE_SPEED * dt;
        move_with_collision(&s_px, &s_py, nx, ny);
    }

    bool boss_alive = false;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!s_enemies[i].active) continue;
        if (s_enemies[i].is_boss) boss_alive = true;
        float edx = s_px - s_enemies[i].x, edy = s_py - s_enemies[i].y;
        float dist = sqrtf(edx * edx + edy * edy);

        if (s_enemies[i].behavior == EBEH_RANGED && !s_enemies[i].is_boss) {
            // Keeps its distance rather than closing in for melee:
            // backs away if the player gets too close, closes the gap
            // if too far, otherwise holds position and shoots.
            if (dist < RANGED_MIN_DIST && dist > 0.05f) {
                float speed = 0.26f * dt;
                float enx = s_enemies[i].x - (edx / dist) * speed;
                float eny = s_enemies[i].y - (edy / dist) * speed;
                move_with_collision(&s_enemies[i].x, &s_enemies[i].y, enx, eny);
            } else if (dist > RANGED_MAX_DIST) {
                float speed = 0.22f * dt;
                float enx = s_enemies[i].x + (edx / dist) * speed;
                float eny = s_enemies[i].y + (edy / dist) * speed;
                move_with_collision(&s_enemies[i].x, &s_enemies[i].y, enx, eny);
            }
            if (dist <= RANGED_FIRE_RANGE && now - s_enemies[i].last_shot_ms >= RANGED_FIRE_MS &&
                has_los(s_enemies[i].x, s_enemies[i].y, s_px, s_py)) {
                s_enemies[i].last_shot_ms = now;
                float ex_screen, ey_screen;
                // Reuse the same camera transform as the billboard draw
                // so the incoming-shot bullet visibly comes from where
                // the enemy is actually rendered on screen.
                float rad2 = s_pangle_deg * DEG2RAD;
                float plane_len2 = tanf((FOV_DEG * DEG2RAD) / 2.0f);
                float dirx2 = cosf(rad2), diry2 = sinf(rad2);
                float planex2 = -diry2 * plane_len2, planey2 = dirx2 * plane_len2;
                float invDet2 = 1.0f / (planex2 * diry2 - dirx2 * planey2);
                float ex = s_enemies[i].x - s_px, ey = s_enemies[i].y - s_py;
                float transX = invDet2 * (diry2 * ex - dirx2 * ey);
                float transY = invDet2 * (-planey2 * ex + planex2 * ey);
                if (transY > 0.1f) {
                    ex_screen = ARENA_CX + (transX / transY) * ARENA_R;
                    ey_screen = ARENA_CY;
                    spawn_bullet((int)ex_screen, (int)ey_screen, ARENA_CX, ARENA_CY, true);
                }
                s_health -= RANGED_DAMAGE;
                game_sfx_hit();
                if (s_health <= 0) { die(); return; }
            }
            continue;
        }

        // Melee behavior (and the boss, which stays melee).
        if (dist > 0.6f) {
            float speed = (s_enemies[i].is_boss ? 0.3f : 0.28f) * dt;
            float enx = s_enemies[i].x + (edx / dist) * speed;
            float eny = s_enemies[i].y + (edy / dist) * speed;
            move_with_collision(&s_enemies[i].x, &s_enemies[i].y, enx, eny);
        } else if (now - s_enemies[i].last_hit_ms > 500) {
            s_enemies[i].last_hit_ms = now;
            s_health -= s_enemies[i].is_boss ? 8 : 4;
            if (s_health <= 0) { die(); return; }
        }
    }

    // Auto-fire: nearest active enemy within range/cone AND with a
    // clear line of sight - this is the actual fix for being able to
    // see and hit enemies standing behind a wall, since only distance
    // and angle were ever checked here before.
    int best = -1;
    float best_dist = 1e9f;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!s_enemies[i].active) continue;
        float edx = s_enemies[i].x - s_px, edy = s_enemies[i].y - s_py;
        float dist = sqrtf(edx * edx + edy * edy);
        if (dist > FIRE_RANGE || dist < 0.01f) continue;
        float ang = atan2f(edy, edx) / DEG2RAD;
        if (fabsf(angle_diff_deg(ang, s_pangle_deg)) > FIRE_CONE_DEG) continue;
        if (!has_los(s_px, s_py, s_enemies[i].x, s_enemies[i].y)) continue;
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    s_aim_locked = (best >= 0);

    if (best >= 0 && now - s_last_fire_ms >= FIRE_INTERVAL_MS) {
        s_last_fire_ms = now;
        s_enemies[best].hp--;
        game_sfx_shoot();
        spawn_bullet(ARENA_CX, ARENA_CY + 60, ARENA_CX, ARENA_CY, false);
        if (s_enemies[best].hp <= 0) {
            bool was_boss = s_enemies[best].is_boss;
            s_enemies[best].active = false;
            s_score += was_boss ? 100 : 15;
            game_sfx_explosion();
        } else {
            game_sfx_hit();
        }
    }

    if (is_boss_level(s_level)) {
        if (!boss_alive) {
            s_score += 50;
            game_sfx_levelup();
            enter_state(M_BOSS_CLEAR);
            return;
        }
    } else {
        int remaining = 0;
        for (int i = 0; i < MAX_ENEMIES; i++) if (s_enemies[i].active) remaining++;
        if (remaining == 0) {
            game_sfx_levelup();
            if (is_boss_level(s_level + 1)) {
                enter_state(M_BOSS_INTRO);
            } else {
                enter_state(M_LEVELUP);
            }
        }
    }
}

// Chord half-height of the round display at a given screen x - the
// same technique game_flappy.cpp's tunnel uses, so the corridor
// naturally narrows toward the bezel instead of being hard-clipped by
// a rectangle.
static float chord_half_h_at(int screen_x) {
    float dx = (float)screen_x - ARENA_CX;
    float under = ARENA_R * ARENA_R - dx * dx;
    if (under <= 0.0f) return 0.0f;
    return sqrtf(under);
}

static void draw_view(Arduino_GFX *g) {
    float rad = s_pangle_deg * DEG2RAD;
    float plane_len = tanf((FOV_DEG * DEG2RAD) / 2.0f);
    float dirx = cosf(rad), diry = sinf(rad);
    float planex = -diry * plane_len, planey = dirx * plane_len;

    // Fixed reference used for the actual perspective math everywhere
    // below - chord_half_h_at() is ONLY for clipping/culling against
    // the circle's edge, never for scaling a wall's or sprite's
    // rendered size. Using the (shrinking-toward-the-edges) chord
    // width as a size scale was the actual bug: it meant the exact
    // same wall at the exact same distance would render at a
    // different height depending only on which screen column it
    // landed in, stacking an artificial extra warp on top of the
    // already-correct perspective distance falloff.
    const float VIEW_HALF_H = ARENA_R;

    for (int col = 0; col < NUM_RAYS; col++) {
        int sx = VIEW_LEFT + col * COL_STEP;
        float half_h = chord_half_h_at(sx + COL_STEP / 2);
        if (half_h <= 1.0f) continue;

        float camx = 2.0f * col / (float)NUM_RAYS - 1.0f;
        float raydx = dirx + planex * camx;
        float raydy = diry + planey * camx;

        int mapx = (int)s_px, mapy = (int)s_py;
        float sideDistX, sideDistY;
        float deltaDistX = (raydx == 0) ? 1e30f : fabsf(1.0f / raydx);
        float deltaDistY = (raydy == 0) ? 1e30f : fabsf(1.0f / raydy);
        int stepx, stepy;

        if (raydx < 0) { stepx = -1; sideDistX = (s_px - mapx) * deltaDistX; }
        else { stepx = 1; sideDistX = (mapx + 1.0f - s_px) * deltaDistX; }
        if (raydy < 0) { stepy = -1; sideDistY = (s_py - mapy) * deltaDistY; }
        else { stepy = 1; sideDistY = (mapy + 1.0f - s_py) * deltaDistY; }

        int side = 0, hitval = 1, guard = 48;
        while (guard-- > 0) {
            if (sideDistX < sideDistY) { sideDistX += deltaDistX; mapx += stepx; side = 0; }
            else { sideDistY += deltaDistY; mapy += stepy; side = 1; }
            if (is_wall(mapx, mapy)) { hitval = s_map[mapy][mapx]; break; }
        }

        float perpDist = (side == 0) ? (sideDistX - deltaDistX) : (sideDistY - deltaDistY);
        if (perpDist < 0.05f) perpDist = 0.05f;

        int ceil_y = (int)(ARENA_CY - half_h);
        int floor_y = (int)(ARENA_CY + half_h);
        g->fillRect(sx, ceil_y, COL_STEP, (int)half_h, COLOR_PANEL);
        g->fillRect(sx, ARENA_CY, COL_STEP, (int)half_h, COLOR565(0x22, 0x22, 0x22));

        int lineH = (int)((2.0f * VIEW_HALF_H) / perpDist);
        int y0 = ARENA_CY - lineH / 2;
        int y1 = ARENA_CY + lineH / 2;
        if (y0 < ceil_y) y0 = ceil_y;
        if (y1 > floor_y) y1 = floor_y;

        uint16_t base = (hitval == 2) ? COLOR_TEXT_DIM : (hitval == 3) ? COLOR_ACCENT2 : COLOR_ACCENT;
        uint16_t c = base;
        if (side == 1) {
            uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 0.75f);
            uint8_t gg = (uint8_t)(((c >> 5) & 0x3F) * 0.75f);
            uint8_t b = (uint8_t)((c & 0x1F) * 0.75f);
            c = (uint16_t)((r << 11) | (gg << 5) | b);
        }
        if (y1 > y0) g->fillRect(sx, y0, COL_STEP, y1 - y0, c);
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!s_enemies[i].active) continue;
        if (!has_los(s_px, s_py, s_enemies[i].x, s_enemies[i].y)) continue; // hidden behind a wall
        float ex = s_enemies[i].x - s_px, ey = s_enemies[i].y - s_py;
        float invDet = 1.0f / (planex * diry - dirx * planey);
        float transX = invDet * (diry * ex - dirx * ey);
        float transY = invDet * (-planey * ex + planex * ey);
        if (transY <= 0.1f) continue;
        int screenX = (int)(ARENA_CX + (transX / transY) * ARENA_R);
        float half_h = chord_half_h_at(screenX);
        if (half_h <= 1.0f) continue; // culled - outside the visible circle entirely
        int spriteH = (int)((2.0f * VIEW_HALF_H) / transY); // fixed reference, not the local chord
        if (spriteH > 400) spriteH = 400;
        int topY = ARENA_CY - spriteH / 2;
        int w = spriteH / 2;
        if (screenX + w < VIEW_LEFT || screenX - w > VIEW_RIGHT) continue;
        uint16_t c = s_enemies[i].is_boss ? COLOR565(0x99, 0x11, 0x33) :
                     (s_enemies[i].behavior == EBEH_RANGED) ? COLOR_ACCENT3 : COLOR_BAD;
        int cx0 = screenX - w / 2, cy0 = topY, cw = w, ch = spriteH;
        if (cw > 0 && ch > 0) g->fillRoundRect(cx0, cy0, cw, ch, 4, c);
        if (s_enemies[i].hp < s_enemies[i].hp_max) {
            int bw = w;
            int fill = (int)((float)bw * s_enemies[i].hp / s_enemies[i].hp_max);
            g->fillRect(screenX - w / 2, topY - 6, bw, 2, COLOR_TEXT_DIM);
            if (fill > 0) g->fillRect(screenX - w / 2, topY - 6, fill, 2, COLOR_BAD);
        }
    }

    uint32_t now = millis();
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!s_bullets[i].active) continue;
        float t = (float)(now - s_bullets[i].start_ms) / (float)BULLET_TRAVEL_MS;
        if (t >= 1.0f) { s_bullets[i].active = false; continue; }
        int bx = s_bullets[i].sx0 + (int)((s_bullets[i].sx1 - s_bullets[i].sx0) * t);
        int by = s_bullets[i].sy0 + (int)((s_bullets[i].sy1 - s_bullets[i].sy0) * t);
        g->fillCircle(bx, by, 3, s_bullets[i].enemy_shot ? COLOR_BAD : COLOR_WARN);
    }

    uint16_t reticle_c = s_aim_locked ? COLOR_BAD : COLOR_TEXT_DIM;
    g->drawCircle(ARENA_CX, ARENA_CY, 10, reticle_c);
    g->drawLine(ARENA_CX - 16, ARENA_CY, ARENA_CX - 12, ARENA_CY, reticle_c);
    g->drawLine(ARENA_CX + 12, ARENA_CY, ARENA_CX + 16, ARENA_CY, reticle_c);
    g->drawLine(ARENA_CX, ARENA_CY - 16, ARENA_CX, ARENA_CY - 12, reticle_c);
    g->drawLine(ARENA_CX, ARENA_CY + 12, ARENA_CX, ARENA_CY + 16, reticle_c);
}

static void draw_minimap(Arduino_GFX *g) {
    int mw = 55, mh = 55;
    int mx0 = 300, my0 = 68;
    g->fillRect(mx0, my0, mw, mh, COLOR_BG);
    g->drawRect(mx0, my0, mw, mh, COLOR_TEXT_DIM);
    float cell = (float)mw / MAP_W;
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            if (s_map[y][x] == 0) continue;
            g->fillRect(mx0 + (int)(x * cell), my0 + (int)(y * cell), (int)cell + 1, (int)cell + 1, COLOR_PANEL);
        }
    }
    int ppx = mx0 + (int)(s_px * cell), ppy = my0 + (int)(s_py * cell);
    g->fillCircle(ppx, ppy, 2, COLOR_GOOD);
    float rad = s_pangle_deg * DEG2RAD;
    g->drawLine(ppx, ppy, ppx + (int)(cosf(rad) * 6), ppy + (int)(sinf(rad) * 6), COLOR_GOOD);
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!s_enemies[i].active) continue;
        int ex = mx0 + (int)(s_enemies[i].x * cell), ey = my0 + (int)(s_enemies[i].y * cell);
        g->fillCircle(ex, ey, s_enemies[i].is_boss ? 3 : 2, COLOR_BAD);
    }
}

static void maze_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    if (s_state == M_TITLE) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 90, COLOR_TEXT, "MAZE RAIDER", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_TEXT_DIM, "Drag to steer & walk", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 12, COLOR_TEXT_DIM, "Auto-fires ahead", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 14, COLOR_TEXT_DIM, "Every 4th level: boss", 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_ACCENT2, "Tap to start", 2);
        return;
    }

    draw_view(g);
    draw_minimap(g);

    char buf[24];
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    snprintf(buf, sizeof(buf), "%d", s_score);
    g->setCursor(LCD_WIDTH / 2 - 90, 40);
    g->print(buf);

    int bar_w = 120, bar_x = LCD_WIDTH / 2 - bar_w / 2, bar_y = 40;
    g->drawRoundRect(bar_x, bar_y, bar_w, 12, 4, COLOR_TEXT_DIM);
    int fill_w = (int)((float)bar_w * s_health / START_HEALTH);
    if (fill_w < 0) fill_w = 0;
    if (fill_w > 0) g->fillRoundRect(bar_x, bar_y, fill_w, 12, 4, s_health > 30 ? COLOR_GOOD : COLOR_BAD);

    snprintf(buf, sizeof(buf), is_boss_level(s_level) ? "BOSS - Lvl %d" : "Level %d", s_level);
    ui_draw_centered_text(70, COLOR_TEXT_DIM, buf, 1);
    if (!s_walking && s_state == M_PLAYING) {
        ui_draw_centered_text(LCD_HEIGHT - 60, COLOR_WARN, "STOPPED - tap to walk", 1);
    }

    if (s_state == M_LEVELUP) {
        char lbuf[16];
        snprintf(lbuf, sizeof(lbuf), "LEVEL %d", s_level);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, lbuf, 3);
    } else if (s_state == M_BOSS_INTRO) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_BAD, "BOSS AHEAD", 3);
    } else if (s_state == M_BOSS_CLEAR) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, "BOSS DEFEATED!", 3);
    } else if (s_state == M_GAMEOVER) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 16, COLOR_TEXT, buf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 46, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

static void maze_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    if (s_state == M_TITLE || s_state == M_GAMEOVER) {
        if (tap_edge) {
            maze_reset();
            enter_state(M_PLAYING);
        }
        return;
    }
    if (s_state != M_PLAYING) return;

    if (pressed) {
        if (s_touch_start_ms == 0) {
            // Rising edge of this touch - remember where/when it started
            // (for tap-vs-drag detection) AND the angle it was already
            // facing, so this drag continues from there.
            s_touch_start_x = x;
            s_touch_start_y = y;
            s_touch_start_ms = millis();
            s_touch_moved = false;
            s_drag_start_angle = s_pangle_deg;
        } else if (abs(x - s_touch_start_x) > TAP_MOVE_THRESHOLD_PX ||
                   abs(y - s_touch_start_y) > TAP_MOVE_THRESHOLD_PX) {
            s_touch_moved = true;
        }
        // Relative to how far the finger has moved since THIS touch
        // began, added onto the angle it already had - not an
        // absolute touch-x-to-angle mapping. The old absolute version
        // meant every new touch-down snapped the view to whatever
        // angle that x position mapped to, instead of continuing
        // smoothly from wherever you'd last been steering.
        s_pangle_deg = s_drag_start_angle + (float)(x - s_touch_start_x) * TURN_SENSITIVITY_DEG_PER_PX;
    } else {
        // Release: a quick tap that didn't drag toggles walking on/off.
        if (s_touch_start_ms != 0 && !s_touch_moved &&
            millis() - s_touch_start_ms <= TAP_MAX_MS) {
            s_walking = !s_walking;
        }
        s_touch_start_ms = 0;
    }
}

static void maze_tick() {
    uint32_t now = millis();

    if (s_state == M_LEVELUP || s_state == M_BOSS_INTRO || s_state == M_BOSS_CLEAR) {
        uint32_t wait_ms = (s_state == M_LEVELUP) ? 1100 : 1300;
        if (now - s_state_enter_ms > wait_ms) {
            s_level++;
            load_level(s_level);
            enter_state(M_PLAYING);
            s_last_phys_step = now;
        }
        return;
    }
    if (s_state != M_PLAYING) return;

    if (controller_connected()) {
        const float CTRL_TURN_SPEED = 130.0f; // deg/sec - continuous, since a d-pad has
                                               // no "absolute angle" the way touch-x does
        float dt2 = PHYS_STEP_MS / 1000.0f; // this check runs once per maze_tick() call, not per physics step
        if (controller_dpad(CTRL_LEFT)) s_pangle_deg -= CTRL_TURN_SPEED * dt2;
        if (controller_dpad(CTRL_RIGHT)) s_pangle_deg += CTRL_TURN_SPEED * dt2;
        static bool s_ctrl_a_prev = false;
        bool a = controller_button(CTRL_BTN_A);
        if (a && !s_ctrl_a_prev) s_walking = !s_walking;
        s_ctrl_a_prev = a;
    }

    int guard = 4;
    while (now - s_last_phys_step >= PHYS_STEP_MS && guard-- > 0) {
        s_last_phys_step += PHYS_STEP_MS;
        physics_step();
        if (s_state != M_PLAYING) break;
    }
}

Screen maze_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    maze_create, maze_draw, maze_touch, maze_tick, maze_destroy, nullptr,
    0, true // idle_frame_ms, suppress_idle - never dim or lock while a game is active
};
