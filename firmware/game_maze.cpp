/*
 * game_maze.cpp
 * "Maze Raider" - an original raycasting (DDA) raid through a new maze
 * every level: collect every gem to open the exit portal, then reach it.
 * Not a port of any existing engine or game.
 *
 * Controls (unchanged): drag left/right to turn - the view continues
 * from where it was, it doesn't snap - and you walk the way you face;
 * a quick tap stops / walks. The blaster fires by itself at whatever is
 * in the reticle (in range, in sight). A gamepad: d-pad turns, A walks.
 *
 * A level: a maze carved by a recursive backtracker on a 7x7 grid of
 * cells (15x15 tiles, 1-tile corridors) with a few walls knocked out
 * so it has loops; gems in the dead ends; a health pack or two; the
 * portal in the cell furthest from the start. Enemies find their way
 * to you along the corridors (a distance field from the player, every
 * quarter second), so they don't grind against walls: red ones bite,
 * purple ones keep their distance and shoot. Every 4th level is a boss
 * arena: an open room, a big one, the portal opens when it falls.
 *
 * The view fills the round screen (each column's height from the
 * circle's chord); walls shade with distance and show brick joints;
 * floor and ceiling are graded bands; sprites (enemies, gems, packs,
 * the portal) are sorted far to near and hidden where a wall is closer
 * (a depth buffer per ray column). A blaster at the bottom kicks back
 * and flashes when it fires; a red rim flashes when you're hit; a
 * compass at the top points to the next gem (or the open portal).
 *
 * Best score is kept in the store ("games/maze_best").
 * Physics runs in fixed 16 ms steps (see game_plane.cpp).
 */
#include "game_maze.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "hal_controller.h"
#include "store.h"
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
#define FOV_DEG 60.0f
#define DEG2RAD (3.14159265f / 180.0f)

#define CELLS 7                    // maze cells per side
#define MAP_W (CELLS * 2 + 1)      // 15 tiles
#define MAP_H MAP_W
#define MAX_ENEMIES 7
#define MAX_ITEMS 10
#define MAX_BULLETS 4

#define PHYS_STEP_MS 16
#define MOVE_SPEED     1.05f  // tiles/sec
#define TURN_GAIN_DEG 130.0f
#define TURN_SENSITIVITY_DEG_PER_PX (2.0f * TURN_GAIN_DEG / (float)(VIEW_RIGHT - VIEW_LEFT))
#define PLAYER_RADIUS  0.22f

#define FIRE_INTERVAL_MS 450
#define FIRE_RANGE        6.0f
#define FIRE_CONE_DEG     9.0f
#define BULLET_TRAVEL_MS  140

#define RANGED_MIN_DIST     2.2f
#define RANGED_MAX_DIST     4.5f
#define RANGED_FIRE_MS   1500
#define RANGED_FIRE_RANGE   6.0f
#define RANGED_DAMAGE          6
#define WAKE_STEPS            9   // enemies further than this (corridor steps) wait

#define TAP_MOVE_THRESHOLD_PX 10
#define TAP_MAX_MS           350

#define BOSS_EVERY_N_LEVELS 4
#define START_HEALTH      100

enum MState { M_TITLE, M_PLAYING, M_LEVELUP, M_BOSS_INTRO, M_BOSS_CLEAR, M_GAMEOVER };
enum EnemyBehavior { EBEH_MELEE, EBEH_RANGED };
enum ItemKind : uint8_t { IT_GEM, IT_HEALTH, IT_PORTAL };

struct Enemy {
    float x, y;
    int hp, hp_max;
    bool active, is_boss;
    EnemyBehavior behavior;
    uint32_t last_hit_ms, last_shot_ms, hurt_ms;
};
struct Item { float x, y; ItemKind kind; bool active; };
// Cosmetic - a hit resolves when fired; this only animates the shot.
struct Bullet { int sx0, sy0, sx1, sy1; uint32_t start_ms; bool active, enemy_shot; };

// A colour scheme per level: walls, floor, ceiling.
struct Theme { uint16_t wall, alt, floor, ceil; const char *name; };
static const Theme THEMES[4] = {
    {COLOR565(0x6a, 0x70, 0x9a), COLOR565(0x8a, 0x80, 0x60), COLOR565(0x2c, 0x2a, 0x30), COLOR565(0x10, 0x12, 0x22), "Stone halls"},
    {COLOR565(0x4c, 0x80, 0x52), COLOR565(0x6a, 0x5a, 0x3a), COLOR565(0x24, 0x2c, 0x20), COLOR565(0x0c, 0x16, 0x10), "Mossy ruins"},
    {COLOR565(0xa0, 0x52, 0x40), COLOR565(0x70, 0x6a, 0x66), COLOR565(0x30, 0x24, 0x20), COLOR565(0x1a, 0x0e, 0x0c), "Red keep"},
    {COLOR565(0x78, 0xa8, 0xc8), COLOR565(0xc8, 0xd8, 0xe8), COLOR565(0x28, 0x30, 0x3a), COLOR565(0x0c, 0x14, 0x20), "Ice caves"},
};

static MState s_state;
static uint8_t s_map[MAP_H][MAP_W];     // 0 floor, 1 wall, 2 the theme's other wall
static uint8_t s_dist[MAP_H][MAP_W];    // corridor steps to the player (255: far / unreached)
static float s_px, s_py, s_pangle_deg;
static int s_health, s_score, s_level, s_best;
static int s_gems_left, s_gems_total;
static bool s_portal_open;
static Enemy s_enemies[MAX_ENEMIES];
static Item s_items[MAX_ITEMS];
static Bullet s_bullets[MAX_BULLETS];
static float s_zbuf[NUM_RAYS + 1];
static uint32_t s_last_fire_ms, s_last_phys_step, s_state_enter_ms, s_last_path_ms, s_hurt_ms, s_level_start_ms;
static bool s_aim_locked;
static bool s_walking = true;
static int s_touch_start_x, s_touch_start_y;
static uint32_t s_touch_start_ms;
static bool s_prev_pressed, s_touch_moved;
static float s_drag_start_angle = 0.0f;

static const Theme &theme() { return THEMES[(s_level - 1) % 4]; }
static bool is_boss_level(int level) { return level % BOSS_EVERY_N_LEVELS == 0; }
static bool is_wall(int mx, int my) {
    if (mx < 0 || mx >= MAP_W || my < 0 || my >= MAP_H) return true;
    return s_map[my][mx] != 0;
}
static void enter_state(MState st) { s_state = st; s_state_enter_ms = millis(); }

// Steps along the line between two points looking for a wall between them.
static bool has_los(float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < 0.001f) return true;
    int steps = (int)(dist / 0.12f) + 1;
    for (int i = 1; i < steps; i++) {
        float t = (float)i / steps;
        if (is_wall((int)(x0 + dx * t), (int)(y0 + dy * t))) return false;
    }
    return true;
}

static void spawn_bullet(int sx0, int sy0, int sx1, int sy1, bool enemy_shot) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (s_bullets[i].active) continue;
        s_bullets[i] = {sx0, sy0, sx1, sy1, millis(), true, enemy_shot};
        return;
    }
}

static void spawn_enemy(float x, float y, bool boss) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (s_enemies[i].active) continue;
        Enemy &e = s_enemies[i];
        e.active = true;
        e.is_boss = boss;
        e.x = x; e.y = y;
        e.hp_max = boss ? (16 + 2 * s_level) : (s_level >= 6 ? 3 : 2);
        e.hp = e.hp_max;
        e.last_hit_ms = 0;
        e.last_shot_ms = millis();
        e.hurt_ms = 0;
        e.behavior = (!boss && random(100) < 40) ? EBEH_RANGED : EBEH_MELEE;
        return;
    }
}

static void add_item(float x, float y, ItemKind k) {
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (s_items[i].active) continue;
        s_items[i] = {x, y, k, true};
        return;
    }
}

// ---- the maze: a recursive backtracker on the cells, then a few loops -------------
static void carve_maze() {
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) s_map[y][x] = random(100) < 18 ? 2 : 1;
    bool seen[CELLS][CELLS] = {};
    int stack[CELLS * CELLS], sp = 0;
    stack[sp++] = 0;
    seen[0][0] = true;
    s_map[1][1] = 0;
    static const int DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
    while (sp) {
        int c = stack[sp - 1], cx = c % CELLS, cy = c / CELLS;
        int opts[4], n = 0;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx >= 0 && nx < CELLS && ny >= 0 && ny < CELLS && !seen[ny][nx]) opts[n++] = d;
        }
        if (!n) { sp--; continue; }
        int d = opts[random(n)];
        int nx = cx + DX[d], ny = cy + DY[d];
        s_map[cy * 2 + 1 + DY[d]][cx * 2 + 1 + DX[d]] = 0;   // the wall between
        s_map[ny * 2 + 1][nx * 2 + 1] = 0;
        seen[ny][nx] = true;
        stack[sp++] = ny * CELLS + nx;
    }
    // knock out a few inner walls: loops, so enemies can be gone round
    for (int k = 0, tries = 0; k < 6 && tries < 200; tries++) {
        int x = 1 + random(MAP_W - 2), y = 1 + random(MAP_H - 2);
        if (s_map[y][x] == 0) continue;
        bool horiz = !is_wall(x - 1, y) && !is_wall(x + 1, y) && is_wall(x, y - 1) && is_wall(x, y + 1);
        bool vert = !is_wall(x, y - 1) && !is_wall(x, y + 1) && is_wall(x - 1, y) && is_wall(x + 1, y);
        if (horiz || vert) { s_map[y][x] = 0; k++; }
    }
}

static void boss_arena() {
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            bool edge = x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1;
            bool pillar = (x == 4 || x == 10) && (y == 4 || y == 10);
            s_map[y][x] = edge ? 1 : pillar ? 2 : 0;
        }
}

// Corridor steps from the player to every floor tile (a breadth-first fill).
static void update_paths() {
    memset(s_dist, 255, sizeof(s_dist));
    static uint8_t qx[MAP_W * MAP_H], qy[MAP_W * MAP_H];
    int h = 0, t = 0;
    int sx = (int)s_px, sy = (int)s_py;
    if (is_wall(sx, sy)) return;
    s_dist[sy][sx] = 0;
    qx[t] = sx; qy[t++] = sy;
    while (h < t) {
        int x = qx[h], y = qy[h++];
        uint8_t d = s_dist[y][x];
        if (d >= 250) continue;
        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto &o : nb) {
            int nx = x + o[0], ny = y + o[1];
            if (is_wall(nx, ny) || s_dist[ny][nx] != 255) continue;
            s_dist[ny][nx] = d + 1;
            qx[t] = nx; qy[t++] = ny;
        }
    }
}

static void load_level(int level) {
    for (auto &e : s_enemies) e.active = false;
    for (auto &it : s_items) it.active = false;
    for (auto &b : s_bullets) b.active = false;
    s_px = 1.5f; s_py = 1.5f; s_pangle_deg = 0.0f;
    s_gems_left = s_gems_total = 0;
    s_portal_open = false;
    s_level_start_ms = millis();

    if (is_boss_level(level)) {
        boss_arena();
        s_pangle_deg = 45.0f;
        spawn_enemy(MAP_W - 3.5f, MAP_H - 3.5f, true);
        add_item(MAP_W / 2 + 0.5f, 2.5f, IT_HEALTH);
        add_item(MAP_W - 2.5f, MAP_H - 2.5f, IT_PORTAL);
        update_paths();
        return;
    }
    carve_maze();
    if (!is_wall(2, 1)) s_pangle_deg = 0.0f; else s_pangle_deg = 90.0f;   // face down the first corridor
    s_px = 1.5f; s_py = 1.5f;
    update_paths();
    // the portal: the cell furthest from the start; gems: dead ends, far ones first
    int far_x = 1, far_y = 1;
    for (int cy = 0; cy < CELLS; cy++)
        for (int cx = 0; cx < CELLS; cx++) {
            int x = cx * 2 + 1, y = cy * 2 + 1;
            if (s_dist[y][x] != 255 && s_dist[y][x] > s_dist[far_y][far_x]) { far_x = x; far_y = y; }
        }
    add_item(far_x + 0.5f, far_y + 0.5f, IT_PORTAL);
    int want = 3 + (level > 5 ? 2 : level / 3);
    for (int pass = 0; pass < 2 && s_gems_total < want; pass++)
        for (int tries = 0; tries < 120 && s_gems_total < want; tries++) {
            int x = random(CELLS) * 2 + 1, y = random(CELLS) * 2 + 1;
            if (s_dist[y][x] < 6 || (x == far_x && y == far_y)) continue;
            int open = !is_wall(x + 1, y) + !is_wall(x - 1, y) + !is_wall(x, y + 1) + !is_wall(x, y - 1);
            if (pass == 0 && open != 1) continue;   // dead ends first
            bool taken = false;
            for (auto &it : s_items) if (it.active && (int)it.x == x && (int)it.y == y) taken = true;
            if (taken) continue;
            add_item(x + 0.5f, y + 0.5f, IT_GEM);
            s_gems_total++;
        }
    s_gems_left = s_gems_total;
    for (int k = 0, tries = 0; k < (level > 2 ? 2 : 1) && tries < 80; tries++) {
        int x = random(CELLS) * 2 + 1, y = random(CELLS) * 2 + 1;
        if (s_dist[y][x] < 5) continue;
        add_item(x + 0.5f, y + 0.5f, IT_HEALTH);
        k++;
    }
    int count = 2 + level;
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    for (int i = 0, tries = 0; i < count && tries < 200; tries++) {
        int x = 1 + random(MAP_W - 2), y = 1 + random(MAP_H - 2);
        if (is_wall(x, y) || s_dist[y][x] < 7) continue;   // not right next to the start
        spawn_enemy(x + 0.5f, y + 0.5f, false);
        i++;
    }
}

static void load_best() {
    s_best = 0;
    store_read("games/maze_best", &s_best, sizeof(s_best));
}
static void save_best() {
    if (s_score <= s_best) return;
    s_best = s_score;
    store_write("games/maze_best", &s_best, sizeof(s_best));
}

static void maze_reset() {
    s_health = START_HEALTH;
    s_score = 0;
    s_level = 1;
    s_last_fire_ms = s_last_phys_step = s_last_path_ms = millis();
    s_hurt_ms = 0;
    s_walking = true;
    s_touch_start_ms = 0;
    load_level(1);
}

static void maze_create() {
    enter_state(M_TITLE);
    load_best();
    maze_reset();
    s_prev_pressed = false;
}

static void maze_destroy() {}

static void die() {
    save_best();
    enter_state(M_GAMEOVER);
    game_sfx_gameover();
}

static void hurt(int dmg) {
    s_health -= dmg;
    s_hurt_ms = millis();
    game_sfx_hit();
    if (s_health <= 0) die();
}

static float angle_diff_deg(float a, float b) { return fmodf(a - b + 540.0f, 360.0f) - 180.0f; }

// Moves (x,y) toward (nx,ny), stopping at walls on each axis (it slides along them).
static void move_with_collision(float *x, float *y, float nx, float ny, float r = PLAYER_RADIUS) {
    if (!is_wall((int)(nx + (nx > *x ? r : -r)), (int)*y)) *x = nx;
    if (!is_wall((int)*x, (int)(ny + (ny > *y ? r : -r)))) *y = ny;
}

// Toward the player along the corridors: the neighbouring tile closer by the distance field.
static void chase(Enemy &e, float speed) {
    int ex = (int)e.x, ey = (int)e.y;
    float tx = s_px, ty = s_py;
    if (!has_los(e.x, e.y, s_px, s_py)) {
        uint8_t best = s_dist[ey][ex];
        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto &o : nb) {
            int nx = ex + o[0], ny = ey + o[1];
            if (!is_wall(nx, ny) && s_dist[ny][nx] < best) { best = s_dist[ny][nx]; tx = nx + 0.5f; ty = ny + 0.5f; }
        }
    }
    float dx = tx - e.x, dy = ty - e.y, d = sqrtf(dx * dx + dy * dy);
    if (d < 0.01f) return;
    move_with_collision(&e.x, &e.y, e.x + dx / d * speed, e.y + dy / d * speed, 0.2f);
}

// Corridors are one tile wide and steering is a drag on a small round
// screen, so walking helps: it keeps you in the middle of the corridor,
// and at a bend with only one way on (not back) it turns you into it. At
// a junction it leaves the choice to you; a dead end stops you.
static void corridor_assist(float dt) {
    if (s_touch_start_ms) return;   // the finger is steering
    float a = fmodf(s_pangle_deg, 360.0f);
    if (a < 0) a += 360.0f;
    int axis = ((int)((a + 45.0f) / 90.0f)) % 4;   // 0 east, 1 south, 2 west, 3 north
    float off = angle_diff_deg(a, axis * 90.0f);
    static const int AX[4] = {1, 0, -1, 0}, AY[4] = {0, 1, 0, -1};
    int tx = (int)s_px, ty = (int)s_py;
    // centring, across the way you're going
    if (fabsf(off) < 35.0f) {
        if (AX[axis]) s_py += (ty + 0.5f - s_py) * 2.5f * dt;
        else s_px += (tx + 0.5f - s_px) * 2.5f * dt;
    }
    // the tile ahead closed and near its middle: is there one way on?
    bool ahead_open = !is_wall(tx + AX[axis], ty + AY[axis]);
    float along = AX[axis] ? (s_px - tx) : (s_py - ty);
    if (AX[axis] < 0 || AY[axis] < 0) along = 1.0f - along;
    if (ahead_open || along < 0.45f) return;
    int l = (axis + 3) % 4, r = (axis + 1) % 4;
    bool lo = !is_wall(tx + AX[l], ty + AY[l]), ro = !is_wall(tx + AX[r], ty + AY[r]);
    if (lo == ro) return;   // a junction (your call) or a dead end
    float target = (lo ? l : r) * 90.0f;
    float d = angle_diff_deg(target, a);
    float stepd = 260.0f * dt;
    s_pangle_deg += fabsf(d) < stepd ? d : (d > 0 ? stepd : -stepd);
}

static void finish_level() {
    s_score += 50 + (s_health > 0 ? s_health / 4 : 0);
    uint32_t secs = (millis() - s_level_start_ms) / 1000;
    if (secs < 120) s_score += (120 - secs) / 2;   // a bonus for a quick raid
    game_sfx_levelup();
    enter_state(is_boss_level(s_level) ? M_BOSS_CLEAR : is_boss_level(s_level + 1) ? M_BOSS_INTRO : M_LEVELUP);
}

static void physics_step() {
    float dt = PHYS_STEP_MS / 1000.0f;
    uint32_t now = millis();

    if (s_walking) {
        float rad = s_pangle_deg * DEG2RAD;
        move_with_collision(&s_px, &s_py, s_px + cosf(rad) * MOVE_SPEED * dt, s_py + sinf(rad) * MOVE_SPEED * dt);
        corridor_assist(dt);
    }
    if (now - s_last_path_ms > 250) { s_last_path_ms = now; update_paths(); }

    // pick-ups and the portal
    for (auto &it : s_items) {
        if (!it.active) continue;
        float dx = it.x - s_px, dy = it.y - s_py;
        if (dx * dx + dy * dy > 0.45f * 0.45f) continue;
        if (it.kind == IT_GEM) {
            it.active = false;
            s_gems_left--;
            s_score += 25;
            game_sfx_powerup();
            if (s_gems_left == 0 && !is_boss_level(s_level)) { s_portal_open = true; game_sfx_levelup(); }
        } else if (it.kind == IT_HEALTH && s_health < START_HEALTH) {
            it.active = false;
            s_health = s_health + 35 > START_HEALTH ? START_HEALTH : s_health + 35;
            game_sfx_score();
        } else if (it.kind == IT_PORTAL && s_portal_open) {
            finish_level();
            return;
        }
    }

    bool boss_alive = false;
    for (auto &e : s_enemies) {
        if (!e.active) continue;
        if (e.is_boss) boss_alive = true;
        float edx = s_px - e.x, edy = s_py - e.y;
        float dist = sqrtf(edx * edx + edy * edy);
        uint8_t steps = s_dist[(int)e.y][(int)e.x];
        if (!e.is_boss && steps > WAKE_STEPS && !has_los(e.x, e.y, s_px, s_py)) continue;   // asleep, far away

        if (e.behavior == EBEH_RANGED && !e.is_boss) {
            bool sees = has_los(e.x, e.y, s_px, s_py);
            if (sees && dist < RANGED_MIN_DIST && dist > 0.05f) {
                move_with_collision(&e.x, &e.y, e.x - edx / dist * 0.3f * dt, e.y - edy / dist * 0.3f * dt, 0.2f);
            } else if (!sees || dist > RANGED_MAX_DIST) {
                chase(e, 0.32f * dt);
            }
            if (sees && dist <= RANGED_FIRE_RANGE && now - e.last_shot_ms >= RANGED_FIRE_MS) {
                e.last_shot_ms = now;
                float rad2 = s_pangle_deg * DEG2RAD, pl = tanf((FOV_DEG * DEG2RAD) / 2.0f);
                float dx2 = cosf(rad2), dy2 = sinf(rad2), plx = -dy2 * pl, ply = dx2 * pl;
                float inv = 1.0f / (plx * dy2 - dx2 * ply);
                float ex = e.x - s_px, ey = e.y - s_py;
                float tX = inv * (dy2 * ex - dx2 * ey), tY = inv * (-ply * ex + plx * ey);
                if (tY > 0.1f) spawn_bullet((int)(ARENA_CX + (tX / tY) * ARENA_R), ARENA_CY, ARENA_CX, ARENA_CY + 40, true);
                hurt(RANGED_DAMAGE);
                if (s_state != M_PLAYING) return;
            }
            continue;
        }
        if (dist > 0.6f) {
            chase(e, (e.is_boss ? 0.42f : 0.36f + 0.02f * (s_level > 8 ? 8 : s_level)) * dt);
        } else if (now - e.last_hit_ms > 600) {
            e.last_hit_ms = now;
            hurt(e.is_boss ? 9 : 5);
            if (s_state != M_PLAYING) return;
        }
    }

    // auto-fire: the nearest enemy in the cone, in range and in sight
    int best = -1;
    float best_dist = 1e9f;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy &e = s_enemies[i];
        if (!e.active) continue;
        float edx = e.x - s_px, edy = e.y - s_py, dist = sqrtf(edx * edx + edy * edy);
        if (dist > FIRE_RANGE || dist < 0.01f) continue;
        float ang = atan2f(edy, edx) / DEG2RAD;
        float cone = FIRE_CONE_DEG + (dist < 1.5f ? 8.0f : 0.0f);   // up close, a little wider
        if (fabsf(angle_diff_deg(ang, s_pangle_deg)) > cone) continue;
        if (!has_los(s_px, s_py, e.x, e.y)) continue;
        if (dist < best_dist) { best_dist = dist; best = i; }
    }
    s_aim_locked = best >= 0;
    if (best >= 0 && now - s_last_fire_ms >= FIRE_INTERVAL_MS) {
        s_last_fire_ms = now;
        Enemy &e = s_enemies[best];
        e.hp--;
        e.hurt_ms = now;
        game_sfx_shoot();
        spawn_bullet(ARENA_CX, ARENA_CY + 150, ARENA_CX, ARENA_CY, false);
        if (e.hp <= 0) {
            e.active = false;
            s_score += e.is_boss ? 150 : 15;
            game_sfx_explosion();
        }
    }

    if (is_boss_level(s_level) && !boss_alive && !s_portal_open) { s_portal_open = true; game_sfx_levelup(); }
}

// ---- drawing -----------------------------------------------------------------------
static float chord_half_h_at(int screen_x) {
    float dx = (float)screen_x - ARENA_CX;
    float under = ARENA_R * ARENA_R - dx * dx;
    return under <= 0.0f ? 0.0f : sqrtf(under);
}

// Floor and ceiling: bands, darker toward the horizon, each clipped to the circle.
static void draw_floor_ceiling(Arduino_GFX *g) {
    const Theme &t = theme();
    const int BANDS = 12;
    for (int i = 0; i < BANDS; i++) {
        int y0 = (int)(ARENA_R * i / BANDS), y1 = (int)(ARENA_R * (i + 1) / BANDS);
        float f = 0.25f + 0.75f * (float)(i + 1) / BANDS;   // near (the edges) brighter
        for (int s = -1; s <= 1; s += 2) {
            int ya = s < 0 ? ARENA_CY - y1 : ARENA_CY + y0;
            int h = y1 - y0;
            int yy = s < 0 ? y1 : y1;   // chord at the outer edge of the band
            float w = sqrtf(ARENA_R * ARENA_R - (float)(yy > ARENA_R ? ARENA_R : yy) * yy);
            g->fillRect(ARENA_CX - (int)w, ya, (int)(2 * w), h, ui_dim(s < 0 ? t.ceil : t.floor, f));
        }
    }
}

struct Sprite { float dist, tx, ty; int kind, idx; };   // kind: 0 enemy, 1 item

static void draw_enemy(Arduino_GFX *g, const Enemy &e, int sx, int sy, int h, float fog, uint32_t now) {
    int w = e.is_boss ? h * 7 / 10 : h * 9 / 20;   // h: the full sprite, top to the floor
    uint16_t c = e.is_boss ? COLOR565(0xb0, 0x18, 0x30) : e.behavior == EBEH_RANGED ? COLOR565(0x9a, 0x50, 0xe0)
                                                                                    : COLOR565(0xe0, 0x40, 0x40);
    if (now - e.hurt_ms < 90) c = COLOR_TEXT;   // a white flash when hit
    c = ui_dim(c, fog);
    int bob = (int)(sinf(now / 180.0f + e.x * 3) * h / 30);
    int r = w / 2;
    int cy = sy + h - r * 7 / 4 - h / 12 + bob;     // standing on the floor, hovering a little
    if (r < 2) { g->fillCircle(sx, cy, 2, c); return; }
    g->fillCircle(sx, cy, r, c);                                   // body
    g->fillRect(sx - r, cy, 2 * r, r * 3 / 4, c);
    for (int k = 0; k < 3; k++)                                    // ragged hem
        g->fillCircle(sx - r + r / 3 + k * 2 * r / 3, cy + r * 3 / 4, r / 3, c);
    uint16_t eye = ui_dim(COLOR_TEXT, fog), pupil = COLOR_BG;
    if (e.behavior == EBEH_RANGED && !e.is_boss) {                  // one big eye
        g->fillCircle(sx, cy - r / 5, r / 2, eye);
        g->fillCircle(sx, cy - r / 5, r / 4, pupil);
    } else {
        g->fillCircle(sx - r / 3, cy - r / 5, r / 4, eye);
        g->fillCircle(sx + r / 3, cy - r / 5, r / 4, eye);
        g->fillCircle(sx - r / 3, cy - r / 6, r / 8 + 1, pupil);
        g->fillCircle(sx + r / 3, cy - r / 6, r / 8 + 1, pupil);
    }
    if (e.is_boss) {                                                // horns
        g->fillTriangle(sx - r / 2, cy - r * 4 / 5, sx - r, cy - r * 3 / 2, sx - r / 5, cy - r, ui_dim(COLOR_WARN, fog));
        g->fillTriangle(sx + r / 2, cy - r * 4 / 5, sx + r, cy - r * 3 / 2, sx + r / 5, cy - r, ui_dim(COLOR_WARN, fog));
    }
    if (e.hp < e.hp_max) {
        int bw = 2 * r, fill = bw * e.hp / e.hp_max;
        g->fillRect(sx - r, cy - r - 10, bw, 3, COLOR_PANEL);
        if (fill > 0) g->fillRect(sx - r, cy - r - 10, fill, 3, COLOR_BAD);
    }
}

static void draw_item(Arduino_GFX *g, const Item &it, int sx, int sy, int h, float fog, uint32_t now) {
    int s = h / 6;
    if (s < 2) s = 2;
    int bob = (int)(sinf(now / 260.0f + it.x) * h / 24);
    int cy = sy + h / 4 + bob;                       // on the floor, a little above it
    if (it.kind == IT_GEM) {
        float spin = fabsf(sinf(now / 300.0f + it.y));
        int hw = (int)(s * (0.35f + 0.65f * spin));
        uint16_t c = ui_dim(COLOR565(0x40, 0xe0, 0xf0), fog), c2 = ui_dim(COLOR565(0xa0, 0xf8, 0xff), fog);
        g->fillTriangle(sx - hw, cy, sx + hw, cy, sx, cy - s, c2);
        g->fillTriangle(sx - hw, cy, sx + hw, cy, sx, cy + s * 3 / 2, c);
    } else if (it.kind == IT_HEALTH) {
        g->fillRoundRect(sx - s, cy - s, 2 * s, 2 * s, s / 3 + 1, ui_dim(COLOR_TEXT, fog));
        g->fillRect(sx - s * 2 / 3, cy - s / 5, s * 4 / 3, s * 2 / 5 + 1, ui_dim(COLOR_BAD, fog));
        g->fillRect(sx - s / 5, cy - s * 2 / 3, s * 2 / 5 + 1, s * 4 / 3, ui_dim(COLOR_BAD, fog));
    } else {                                         // the portal: a ring, swirling when open
        int r = h / 3;
        uint16_t c = s_portal_open ? COLOR565(0x50, 0xf0, 0x90) : COLOR565(0x70, 0x70, 0x78);
        int ccy = sy + h / 2 - r;
        for (int k = 0; k < 3; k++) g->drawCircle(sx, ccy, r - k, ui_dim(c, fog));
        if (s_portal_open) {
            float a = now / 200.0f;
            for (int k = 0; k < 6; k++) {
                float b = a + k * 1.047f;
                g->fillCircle(sx + (int)(cosf(b) * r * 0.6f), ccy + (int)(sinf(b) * r * 0.6f), r / 8 + 1, ui_dim(c, fog));
            }
        }
    }
}

static void draw_view(Arduino_GFX *g) {
    const Theme &t = theme();
    float rad = s_pangle_deg * DEG2RAD;
    float plane_len = tanf((FOV_DEG * DEG2RAD) / 2.0f);
    float dirx = cosf(rad), diry = sinf(rad);
    float planex = -diry * plane_len, planey = dirx * plane_len;
    const float VIEW_HALF_H = ARENA_R;   // the perspective's scale - the chord only clips

    draw_floor_ceiling(g);

    for (int col = 0; col < NUM_RAYS; col++) {
        int sx = VIEW_LEFT + col * COL_STEP;
        s_zbuf[col] = 1e9f;
        float half_h = chord_half_h_at(sx + COL_STEP / 2);
        if (half_h <= 1.0f) continue;
        float camx = 2.0f * col / (float)NUM_RAYS - 1.0f;
        float raydx = dirx + planex * camx, raydy = diry + planey * camx;
        int mapx = (int)s_px, mapy = (int)s_py;
        float ddx = raydx == 0 ? 1e30f : fabsf(1.0f / raydx), ddy = raydy == 0 ? 1e30f : fabsf(1.0f / raydy);
        int stepx, stepy;
        float sdx, sdy;
        if (raydx < 0) { stepx = -1; sdx = (s_px - mapx) * ddx; } else { stepx = 1; sdx = (mapx + 1.0f - s_px) * ddx; }
        if (raydy < 0) { stepy = -1; sdy = (s_py - mapy) * ddy; } else { stepy = 1; sdy = (mapy + 1.0f - s_py) * ddy; }
        int side = 0, hitval = 1, guard = 64;
        while (guard-- > 0) {
            if (sdx < sdy) { sdx += ddx; mapx += stepx; side = 0; } else { sdy += ddy; mapy += stepy; side = 1; }
            if (is_wall(mapx, mapy)) { hitval = (mapx >= 0 && mapx < MAP_W && mapy >= 0 && mapy < MAP_H) ? s_map[mapy][mapx] : 1; break; }
        }
        float perp = side == 0 ? sdx - ddx : sdy - ddy;
        if (perp < 0.05f) perp = 0.05f;
        s_zbuf[col] = perp;

        int lineH = (int)((2.0f * VIEW_HALF_H) / perp);
        int top = ARENA_CY - lineH / 2, bot = ARENA_CY + lineH / 2;
        int ceil_y = (int)(ARENA_CY - half_h), floor_y = (int)(ARENA_CY + half_h);
        int y0 = top < ceil_y ? ceil_y : top, y1 = bot > floor_y ? floor_y : bot;
        if (y1 <= y0) continue;

        float fog = 1.0f - perp / 10.0f;   // further: darker
        if (fog < 0.22f) fog = 0.22f;
        if (side == 1) fog *= 0.72f;
        uint16_t base = hitval == 2 ? t.alt : t.wall;
        uint16_t c = ui_dim(base, fog);
        // where on the wall this ray lands: vertical joints every half tile
        float wx = side == 0 ? s_py + perp * raydy : s_px + perp * raydx;
        wx -= floorf(wx);
        bool joint = lineH > 24 && (wx < 0.035f || fabsf(wx - 0.5f) < 0.02f);
        g->fillRect(sx, y0, COL_STEP, y1 - y0, joint ? ui_dim(base, fog * 0.55f) : c);
        if (lineH > 30) {   // three courses of bricks
            uint16_t m = ui_dim(base, fog * 0.55f);
            for (int k = 1; k < 4; k++) {
                int yy = top + lineH * k / 4;
                if (yy > y0 && yy < y1) g->drawFastHLine(sx, yy, COL_STEP, m);
            }
        }
    }
    s_zbuf[NUM_RAYS] = 1e9f;

    // sprites, far to near, hidden where the wall in their column is nearer
    Sprite sp[MAX_ENEMIES + MAX_ITEMS];
    int n = 0;
    float invDet = 1.0f / (planex * diry - dirx * planey);
    auto add = [&](float x, float y, int kind, int idx) {
        float ex = x - s_px, ey = y - s_py;
        float tX = invDet * (diry * ex - dirx * ey), tY = invDet * (-planey * ex + planex * ey);
        if (tY <= 0.15f) return;
        sp[n++] = {tY, tX, tY, kind, idx};
    };
    for (int i = 0; i < MAX_ENEMIES; i++) if (s_enemies[i].active) add(s_enemies[i].x, s_enemies[i].y, 0, i);
    for (int i = 0; i < MAX_ITEMS; i++) if (s_items[i].active) add(s_items[i].x, s_items[i].y, 1, i);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && sp[j].dist > sp[j - 1].dist; j--) { Sprite tmp = sp[j]; sp[j] = sp[j - 1]; sp[j - 1] = tmp; }
    uint32_t now = millis();
    for (int i = 0; i < n; i++) {
        int screenX = (int)(ARENA_CX + (sp[i].tx / sp[i].ty) * ARENA_R);
        int col = (screenX - VIEW_LEFT) / COL_STEP;
        if (col < 0 || col >= NUM_RAYS || chord_half_h_at(screenX) <= 1.0f) continue;
        if (s_zbuf[col] < sp[i].ty) continue;   // behind a wall
        int h = (int)((2.0f * VIEW_HALF_H) / sp[i].ty);
        if (h > 420) h = 420;
        int topY = ARENA_CY - h / 2;
        float fog = 1.0f - sp[i].ty / 11.0f;
        if (fog < 0.3f) fog = 0.3f;
        if (sp[i].kind == 0) draw_enemy(g, s_enemies[sp[i].idx], screenX, topY, h, fog, now);
        else draw_item(g, s_items[sp[i].idx], screenX, topY, h, fog, now);
    }

    for (auto &b : s_bullets) {
        if (!b.active) continue;
        float tt = (float)(now - b.start_ms) / BULLET_TRAVEL_MS;
        if (tt >= 1.0f) { b.active = false; continue; }
        int bx = b.sx0 + (int)((b.sx1 - b.sx0) * tt), by = b.sy0 + (int)((b.sy1 - b.sy0) * tt);
        g->fillCircle(bx, by, b.enemy_shot ? 5 : 3, b.enemy_shot ? COLOR_BAD : COLOR_WARN);
    }

    uint16_t rc = s_aim_locked ? COLOR_BAD : COLOR_TEXT_DIM;
    g->drawCircle(ARENA_CX, ARENA_CY, 10, rc);
    g->drawFastHLine(ARENA_CX - 18, ARENA_CY, 6, rc);
    g->drawFastHLine(ARENA_CX + 13, ARENA_CY, 6, rc);
    g->drawFastVLine(ARENA_CX, ARENA_CY - 18, 6, rc);
    g->drawFastVLine(ARENA_CX, ARENA_CY + 13, 6, rc);
}

// The blaster at the bottom: kicks back and flashes when it fires.
static void draw_gun(Arduino_GFX *g) {
    uint32_t since = millis() - s_last_fire_ms;
    int kick = since < 120 ? (int)(14 * (1.0f - since / 120.0f)) : 0;
    int bob = s_walking ? (int)(sinf(millis() / 140.0f) * 4) : 0;
    int x = ARENA_CX, y = LCD_HEIGHT - 70 + kick + bob;
    if (since < 70) {
        g->fillCircle(x, y - 46, 16, COLOR_WARN);
        g->fillCircle(x, y - 46, 8, COLOR_TEXT);
    }
    g->fillRoundRect(x - 9, y - 40, 18, 46, 5, COLOR565(0x50, 0x56, 0x60));    // barrel
    g->fillRoundRect(x - 26, y, 52, 40, 10, COLOR565(0x38, 0x3c, 0x46));       // body
    g->fillRoundRect(x - 18, y + 6, 36, 8, 3, s_aim_locked ? COLOR_BAD : COLOR_ACCENT2);
    g->fillRect(x - 4, y - 38, 8, 6, COLOR565(0x80, 0x88, 0x96));
}

static void draw_minimap(Arduino_GFX *g) {
    const int cell = 4, mw = MAP_W * cell;
    int mx0 = 318, my0 = 74;
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            g->fillRect(mx0 + x * cell, my0 + y * cell, cell, cell, s_map[y][x] ? COLOR_PANEL : COLOR_BG);
    g->drawRect(mx0 - 1, my0 - 1, mw + 2, mw + 2, COLOR_TEXT_DIM);
    for (auto &it : s_items) {
        if (!it.active) continue;
        uint16_t c = it.kind == IT_GEM ? COLOR565(0x40, 0xe0, 0xf0) : it.kind == IT_HEALTH ? COLOR_TEXT
                   : s_portal_open ? COLOR_GOOD : COLOR_TEXT_DIM;
        g->fillRect(mx0 + (int)(it.x * cell) - 1, my0 + (int)(it.y * cell) - 1, 3, 3, c);
    }
    for (auto &e : s_enemies)
        if (e.active) g->fillRect(mx0 + (int)(e.x * cell) - 1, my0 + (int)(e.y * cell) - 1, 3, 3, COLOR_BAD);
    int ppx = mx0 + (int)(s_px * cell), ppy = my0 + (int)(s_py * cell);
    float rad = s_pangle_deg * DEG2RAD;
    g->drawLine(ppx, ppy, ppx + (int)(cosf(rad) * 7), ppy + (int)(sinf(rad) * 7), COLOR_GOOD);
    g->fillCircle(ppx, ppy, 2, COLOR_GOOD);
}

// A compass arrow at the top: toward the nearest gem, or the portal once it's open.
static void draw_compass(Arduino_GFX *g) {
    float bx = 0, by = 0, bd = 1e9f;
    for (auto &it : s_items) {
        if (!it.active) continue;
        bool want = s_portal_open ? it.kind == IT_PORTAL : it.kind == IT_GEM;
        if (!want) continue;
        float dx = it.x - s_px, dy = it.y - s_py, d = dx * dx + dy * dy;
        if (d < bd) { bd = d; bx = dx; by = dy; }
    }
    if (bd > 1e8f) return;
    float rel = (atan2f(by, bx) / DEG2RAD - s_pangle_deg) * DEG2RAD;   // 0 = straight ahead
    int cx = ARENA_CX, cy = 104;
    float ax = sinf(rel), ay = -cosf(rel);
    uint16_t c = s_portal_open ? COLOR_GOOD : COLOR565(0x40, 0xe0, 0xf0);
    g->fillCircle(cx, cy, 15, COLOR_BG);
    g->drawCircle(cx, cy, 15, COLOR_TEXT_DIM);
    g->fillTriangle(cx + (int)(ax * 12), cy + (int)(ay * 12), cx + (int)(-ay * 6 - ax * 5), cy + (int)(ax * 6 - ay * 5),
                    cx + (int)(ay * 6 - ax * 5), cy + (int)(-ax * 6 - ay * 5), c);
}

static void maze_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    char buf[32];

    if (s_state == M_TITLE) {
        g->fillScreen(COLOR_BG);
        // a little scene: a corridor in perspective, a gem in it
        for (int i = 0; i < 6; i++) {
            int w = 220 - i * 34, h = 150 - i * 23;
            g->drawRect(ARENA_CX - w / 2, 150 - h / 2, w, h, ui_dim(THEMES[0].wall, 1.0f - i * 0.13f));
        }
        g->fillTriangle(ARENA_CX - 14, 150, ARENA_CX + 14, 150, ARENA_CX, 132, COLOR565(0xa0, 0xf8, 0xff));
        g->fillTriangle(ARENA_CX - 14, 150, ARENA_CX + 14, 150, ARENA_CX, 172, COLOR565(0x40, 0xe0, 0xf0));
        ui_draw_centered_text(250, COLOR_TEXT, "MAZE RAIDER", 3);
        ui_draw_centered_text(292, COLOR_TEXT_DIM, "Drag to turn - tap to stop/walk", 1);
        ui_draw_centered_text(312, COLOR_TEXT_DIM, "Take every gem, then reach the portal", 1);
        ui_draw_centered_text(332, COLOR_TEXT_DIM, "The blaster fires by itself", 1);
        if (s_best > 0) {
            snprintf(buf, sizeof(buf), "Best %d", s_best);
            ui_draw_centered_text(360, COLOR_WARN, buf, 1);
        }
        ui_draw_centered_text(392, COLOR_ACCENT2, "Tap to start", 2);
        return;
    }

    draw_view(g);
    draw_gun(g);
    draw_minimap(g);
    if (s_state == M_PLAYING) draw_compass(g);

    // hit: a red rim for a moment
    uint32_t hs = millis() - s_hurt_ms;
    if (s_hurt_ms && hs < 260) ui_arc(ARENA_CX, ARENA_CY, 232, 18 - hs / 20, 0, 360, COLOR_BAD, false);

    snprintf(buf, sizeof(buf), "%d", s_score);
    ui_text_center(ARENA_CX - 120, 52, COLOR_TEXT, buf, 2);
    int bar_w = 120, bar_x = ARENA_CX - bar_w / 2, bar_y = 40;
    g->drawRoundRect(bar_x, bar_y, bar_w, 12, 4, COLOR_TEXT_DIM);
    int fill_w = bar_w * (s_health < 0 ? 0 : s_health) / START_HEALTH;
    if (fill_w > 0) g->fillRoundRect(bar_x, bar_y, fill_w, 12, 4, s_health > 30 ? COLOR_GOOD : COLOR_BAD);
    if (is_boss_level(s_level)) snprintf(buf, sizeof(buf), s_portal_open ? "Boss down - the portal!" : "BOSS - level %d", s_level);
    else if (s_portal_open) snprintf(buf, sizeof(buf), "Portal open!");
    else snprintf(buf, sizeof(buf), "Gems %d/%d - %s", s_gems_total - s_gems_left, s_gems_total, theme().name);
    ui_draw_centered_text(64, s_portal_open ? COLOR_GOOD : COLOR_TEXT_DIM, buf, 1);
    if (!s_walking && s_state == M_PLAYING) ui_draw_centered_text(LCD_HEIGHT - 128, COLOR_WARN, "STOPPED - tap to walk", 1);

    if (s_state == M_LEVELUP) {
        snprintf(buf, sizeof(buf), "LEVEL %d", s_level + 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 30, COLOR_GOOD, buf, 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 6, COLOR_TEXT, THEMES[s_level % 4].name, 2);
    } else if (s_state == M_BOSS_INTRO) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_BAD, "BOSS AHEAD", 3);
    } else if (s_state == M_BOSS_CLEAR) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, "BOSS DEFEATED!", 3);
    } else if (s_state == M_GAMEOVER) {
        g->fillRoundRect(ARENA_CX - 140, LCD_HEIGHT / 2 - 60, 280, 140, 20, COLOR_BG);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 36, COLOR_BAD, "GAME OVER", 3);
        snprintf(buf, sizeof(buf), "Score %d - level %d", s_score, s_level);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 4, COLOR_TEXT, buf, 2);
        snprintf(buf, sizeof(buf), s_score >= s_best && s_score > 0 ? "New best!" : "Best %d", s_best);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 30, COLOR_WARN, buf, 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 56, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

static void maze_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (s_state == M_TITLE || s_state == M_GAMEOVER) {
        if (tap_edge && (s_state == M_TITLE || millis() - s_state_enter_ms > 700)) {
            maze_reset();
            enter_state(M_PLAYING);
        }
        return;
    }
    if (s_state != M_PLAYING) return;
    if (pressed) {
        if (s_touch_start_ms == 0) {
            s_touch_start_x = x;
            s_touch_start_y = y;
            s_touch_start_ms = millis();
            s_touch_moved = false;
            s_drag_start_angle = s_pangle_deg;
        } else if (abs(x - s_touch_start_x) > TAP_MOVE_THRESHOLD_PX || abs(y - s_touch_start_y) > TAP_MOVE_THRESHOLD_PX) {
            s_touch_moved = true;
        }
        // relative to this touch's start, from the angle it had - never a snap
        s_pangle_deg = s_drag_start_angle + (float)(x - s_touch_start_x) * TURN_SENSITIVITY_DEG_PER_PX;
    } else {
        if (s_touch_start_ms != 0 && !s_touch_moved && millis() - s_touch_start_ms <= TAP_MAX_MS) s_walking = !s_walking;
        s_touch_start_ms = 0;
    }
}

static void maze_tick() {
    uint32_t now = millis();
    if (s_state == M_LEVELUP || s_state == M_BOSS_INTRO || s_state == M_BOSS_CLEAR) {
        if (now - s_state_enter_ms > (s_state == M_LEVELUP ? 1400u : 1500u)) {
            s_level++;
            load_level(s_level);
            enter_state(M_PLAYING);
            s_last_phys_step = now;
        }
        return;
    }
    if (s_state != M_PLAYING) return;

    if (controller_connected()) {
        float dt2 = PHYS_STEP_MS / 1000.0f;
        if (controller_dpad(CTRL_LEFT)) s_pangle_deg -= 130.0f * dt2;
        if (controller_dpad(CTRL_RIGHT)) s_pangle_deg += 130.0f * dt2;
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
    if (now - s_last_phys_step > 200) s_last_phys_step = now;   // after a stall, don't run to catch up
}

Screen maze_screen = {
    nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    maze_create, maze_draw, maze_touch, maze_tick, maze_destroy, nullptr,
    0, true // never dim or lock while a game is active
};
