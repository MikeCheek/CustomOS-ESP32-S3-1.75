/*
 * game_plane.cpp
 * Tilt-controlled rail-shooter flight game with perspective depth.
 *
 * Controls:
 *  - Tilt the board: steers an "aim reticle" (the plane) on both axes,
 *    relative to the tilt reading calibrated at the start of each run
 *    (see PSTATE_CALIBRATING below) - not absolute level.
 *  - Quick tap (press+release under BOOST_HOLD_MS): fires a bullet
 *    straight down the aim direction, into the screen.
 *  - Press and hold: engages boost - enemies close faster (more score,
 *    more risk) while boosted.
 *
 * Rendering model: every enemy/bullet/pickup/star lives in 3D space as
 * (world_x, world_y, z), where z is distance from the camera.
 * Z_FAR = just spawned, far away, small on screen.
 * Z_NEAR = arrived at the camera/player.
 * Screen projection is the standard pinhole-camera divide:
 *     scale    = 1 / z
 *     screen_x = VANISH_X + world_x * scale
 *     screen_y = VANISH_Y + world_y * scale
 * Enemies/bullets/the ship stay on this exact fixed frame (that's what
 * makes hit detection correct). Purely cosmetic elements - the tunnel
 * backdrop and starfield - additionally roll and pan with the current
 * steering input (see project_world_visual()), which is what actually
 * sells "the world reacts to the plane" without touching hit detection.
 *
 * Physics runs in fixed 16ms steps (PHYS_STEP_MS) inside on_tick,
 * same pattern as game_dodger.cpp's fall-speed stepping - this keeps
 * gameplay speed constant regardless of how many times per second
 * on_tick happens to be called by the render loop.
 *
 * Same Screen-callback shape as the project's other games - see
 * README.md "Extending it".
 */
#include "game_plane.h"
#include "config.h"
#include "board_pins.h"
#include "hal_imu.h"
#include "hal_controller.h"
#include "ui.h"
#include "game_audio.h"
#include <Arduino_GFX_Library.h>
#include <math.h>

// ---- Perspective / world tunables ----------------------------------------
#define HUD_TOP_MARGIN_PLN 56
#define BOTTOM_MARGIN_PLN  16
#define VANISH_X          (LCD_WIDTH / 2)
#define VANISH_Y          (HUD_TOP_MARGIN_PLN + 244)

#define Z_FAR             2.6f     // spawn depth (far, small, near vanish pt)
#define Z_NEAR            0.35f    // "arrived" depth (large, at the camera)
#define Z_HIT_TOL         0.16f    // bullet/enemy depth match tolerance

#define PHYS_STEP_MS      16       // fixed simulation step, ~60Hz regardless
                                    // of actual render frame rate
#define ENEMY_Z_SPEED     0.012f   // world-z units per phys step (approach)
#define BULLET_Z_SPEED    0.05f    // world-z units per phys step (departure)
#define BOOST_Z_MULT      1.8f

#define WORLD_HIT_RADIUS  14.0f    // bullet<->enemy lateral world distance
#define ARRIVAL_HIT_PX    52       // enemy<->ship screen-space distance at Z_NEAR

// ---- Ship (player) screen-space tunables ----------------------------------
// The ship is drawn at VANISH_X/VANISH_Y + aim offset - the SAME fixed
// frame every enemy/bullet is projected around, which is what keeps
// bullets departing from the nose and hit detection correct. Only the
// purely-cosmetic tunnel/starfield get the extra roll+pan below.
#define AIM_RANGE_X       140.0f
#define AIM_RANGE_Y       46.0f
#define TILT_GAIN         70.0f    // px of aim offset per g of *calibrated* tilt
#define PLANE_W           34
#define PLANE_H           30

// World-space spawn range derived so an enemy at the edge of the spawn
// range, once it reaches Z_NEAR, projects exactly to the edge of the
// ship's reachable aim range - the full spawn envelope is dodgeable.
#define WORLD_RANGE_X     (AIM_RANGE_X * Z_NEAR)
#define WORLD_RANGE_Y     (AIM_RANGE_Y * Z_NEAR)

#define MAX_BULLETS       8
#define MAX_ENEMIES       8
#define MAX_PICKUPS       4
#define MAX_PARTICLES     20
#define STAR_COUNT        24

#define START_LIVES       3
#define MAX_LIVES         5
#define INVULN_MS         1500
#define BOOST_HOLD_MS     220

// Quick per-run tilt calibration (file header + PSTATE_CALIBRATING).
#define CAL_HOLD_MS       900

// World roll/pan reaction to steering - cosmetic only, see file header.
#define WORLD_ROLL_MAX_DEG 14.0f
#define WORLD_PAN_FACTOR    0.28f

// Power-up/down durations.
#define SHIELD_DURATION_MS 6000
#define RAPID_DURATION_MS  7000
#define JAM_DURATION_MS    3000
#define PICKUP_SPAWN_MIN_MS 6000
#define PICKUP_SPAWN_MAX_MS 11000
#define PICKUP_Z_SPEED     0.010f  // slightly slower than enemies - easier to react to

// ---- State ------------------------------------------------------------
enum PlaneState { PSTATE_TITLE, PSTATE_CALIBRATING, PSTATE_PLAYING, PSTATE_GAMEOVER };
static PlaneState s_pstate;

static int     s_score;
static int     s_lives;
static uint32_t s_invuln_until_ms;

// Tilt calibration - averaged over a short hold at the start of each run.
static float    s_cal_x, s_cal_y;
static uint32_t s_cal_start_ms;
static float    s_cal_sum_x, s_cal_sum_y;
static int      s_cal_samples;

// Aim reticle, in screen-space pixel offset from the ship's neutral spot.
static float   s_aim_x, s_aim_y;

enum EnemyType : uint8_t { ENEMY_BASIC, ENEMY_WEAVER };
enum PickupType : uint8_t { PICKUP_SHIELD, PICKUP_RAPID, PICKUP_LIFE, PICKUP_JAM };

struct Bullet  { float wx, wy, z; bool active; };
struct Enemy   { float wx, wy, z; bool active; EnemyType type; float weave_phase; };
struct Pickup  { float wx, wy, z; bool active; PickupType type; };
struct Particle{ float x, y; int ttl; };
struct Star    { float wx, wy, z; };

static Bullet   s_bullets[MAX_BULLETS];
static Enemy    s_enemies[MAX_ENEMIES];
static Pickup   s_pickups[MAX_PICKUPS];
static Particle s_particles[MAX_PARTICLES];
static Star     s_stars[STAR_COUNT];
static int      s_particle_count;

static bool     s_touch_active;
// Rising-edge tracking for the title/gameover "tap to (re)start"
// transition only - the in-flight boost/shoot tracking further down
// already guards itself correctly via s_touch_active and needs
// continuous `pressed`.
static bool     s_prev_pressed;
static uint32_t s_touch_start_ms;
static bool     s_boosting;

static uint32_t s_last_phys_step;
static uint32_t s_last_enemy_spawn;
static uint32_t s_last_boost_score_tick;
static uint32_t s_next_pickup_spawn_ms;

// Power-up/down state.
static bool     s_shield_active;
static uint32_t s_shield_until_ms;
static bool     s_rapid_active;
static uint32_t s_rapid_until_ms;
static bool     s_jammed;
static uint32_t s_jammed_until_ms;

// ---- Helpers ------------------------------------------------------------

static inline float proj_scale(float z) { return 1.0f / z; }

// Fixed-frame projection - used for anything hit detection depends on
// (enemies, bullets, pickups, particles, the ship). Never rolled/panned.
static inline void project_fixed(float wx, float wy, float z, int *sx, int *sy) {
    float sc = proj_scale(z);
    *sx = VANISH_X + (int)(wx * sc);
    *sy = VANISH_Y + (int)(wy * sc);
}

// Cosmetic projection with roll+pan reacting to the current aim, for the
// tunnel backdrop and starfield only - see file header for why this is
// kept separate from project_fixed().
static inline void project_world_visual(float wx, float wy, float z, int *sx, int *sy) {
    float roll = -(s_aim_x / AIM_RANGE_X) * WORLD_ROLL_MAX_DEG * (float)M_PI / 180.0f;
    float pan_x = -s_aim_x * WORLD_PAN_FACTOR;
    float pan_y = -s_aim_y * WORLD_PAN_FACTOR;
    float cr = cosf(roll), sr = sinf(roll);
    float rwx = wx * cr - wy * sr;
    float rwy = wx * sr + wy * cr;
    float sc = proj_scale(z);
    *sx = VANISH_X + (int)pan_x + (int)(rwx * sc);
    *sy = VANISH_Y + (int)pan_y + (int)(rwy * sc);
}

static void spawn_star_at(int i) {
    s_stars[i].wx = (float)(random(200) - 100);
    s_stars[i].wy = (float)(random(200) - 100);
    s_stars[i].z = Z_FAR;
}

static void add_particle(float x, float y) {
    if (s_particle_count >= MAX_PARTICLES) return;
    s_particles[s_particle_count].x = x;
    s_particles[s_particle_count].y = y;
    s_particles[s_particle_count].ttl = 14;
    s_particle_count++;
}

static void spawn_enemy() {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (s_enemies[i].active) continue;
        s_enemies[i].active = true;
        s_enemies[i].type = (random(100) < 30) ? ENEMY_WEAVER : ENEMY_BASIC;
        s_enemies[i].wx = (random(2001) / 1000.0f - 1.0f) * WORLD_RANGE_X;
        s_enemies[i].wy = (random(2001) / 1000.0f - 1.0f) * WORLD_RANGE_Y;
        s_enemies[i].z = Z_FAR;
        s_enemies[i].weave_phase = (float)random(628) / 100.0f;
        return;
    }
}

static void spawn_pickup() {
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (s_pickups[i].active) continue;
        s_pickups[i].active = true;
        int r = random(100);
        s_pickups[i].type = (r < 30) ? PICKUP_SHIELD : (r < 55) ? PICKUP_RAPID :
                             (r < 80) ? PICKUP_LIFE : PICKUP_JAM;
        s_pickups[i].wx = (random(2001) / 1000.0f - 1.0f) * WORLD_RANGE_X;
        s_pickups[i].wy = (random(2001) / 1000.0f - 1.0f) * WORLD_RANGE_Y;
        s_pickups[i].z = Z_FAR;
        return;
    }
}

static void spawn_bullet_at(float wx, float wy) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (s_bullets[i].active) continue;
        s_bullets[i].active = true;
        s_bullets[i].wx = wx;
        s_bullets[i].wy = wy;
        s_bullets[i].z = Z_NEAR;
        return;
    }
}

static void plane_shoot() {
    if (s_jammed) return; // power-down: weapon offline, see file header
    // Depart from the ship's NOSE, not its center: the nose sits
    // PLANE_H/2 px above the ship's screen center, so back-project that
    // exact screen point into world space at the near plane - same
    // fixed frame the ship itself is drawn in.
    float nose_wx = s_aim_x * Z_NEAR;
    float nose_wy = (s_aim_y - (float)(PLANE_H / 2)) * Z_NEAR;
    if (s_rapid_active) {
        // 3-way spread while the RAPID power-up is active.
        spawn_bullet_at(nose_wx - 6.0f, nose_wy);
        spawn_bullet_at(nose_wx, nose_wy);
        spawn_bullet_at(nose_wx + 6.0f, nose_wy);
    } else {
        spawn_bullet_at(nose_wx, nose_wy);
    }
    game_sfx_shoot();
}

static void enter_calibrating() {
    s_pstate = PSTATE_CALIBRATING;
    s_cal_start_ms = millis();
    s_cal_sum_x = s_cal_sum_y = 0.0f;
    s_cal_samples = 0;
}

static void plane_create() {
    s_pstate = PSTATE_TITLE;
    s_score = 0;
    s_lives = START_LIVES;
    s_invuln_until_ms = 0;

    s_cal_x = 0.0f;
    s_cal_y = 0.0f;
    s_aim_x = 0.0f;
    s_aim_y = 0.0f;

    for (int i = 0; i < MAX_BULLETS; i++) s_bullets[i].active = false;
    for (int i = 0; i < MAX_ENEMIES; i++) s_enemies[i].active = false;
    for (int i = 0; i < MAX_PICKUPS; i++) s_pickups[i].active = false;
    s_particle_count = 0;

    for (int i = 0; i < STAR_COUNT; i++) {
        s_stars[i].wx = (float)(random(200) - 100);
        s_stars[i].wy = (float)(random(200) - 100);
        s_stars[i].z = Z_NEAR + random((int)((Z_FAR - Z_NEAR) * 100)) / 100.0f;
    }

    s_touch_active = false;
    s_boosting = false;

    s_shield_active = s_rapid_active = s_jammed = false;
    s_shield_until_ms = s_rapid_until_ms = s_jammed_until_ms = 0;

    uint32_t now = millis();
    s_last_phys_step = now;
    s_last_enemy_spawn = now;
    s_last_boost_score_tick = now;
    s_next_pickup_spawn_ms = now + PICKUP_SPAWN_MIN_MS + (uint32_t)random(PICKUP_SPAWN_MAX_MS - PICKUP_SPAWN_MIN_MS);

    game_music_start("/music/plane.mp3");
}

static void plane_destroy() {
    game_music_stop();
}

// ---- Draw ---------------------------------------------------------------

static void draw_ship(Arduino_GFX *g, int cx, int cy, bool visible) {
    if (!visible) return;
    if (s_boosting) {
        uint16_t flame = (millis() / 60) % 2 ? COLOR_ACCENT3 : COLOR_WARN;
        g->fillTriangle(cx - 6, cy + PLANE_H / 2,
                         cx + 6, cy + PLANE_H / 2,
                         cx,     cy + PLANE_H / 2 + 16, flame);
    }
    if (s_shield_active) {
        uint16_t shieldc = (millis() / 100) % 2 ? COLOR_ACCENT2 : COLOR565(0x66, 0xCC, 0xFF);
        g->drawCircle(cx, cy, PLANE_W, shieldc);
        g->drawCircle(cx, cy, PLANE_W - 1, shieldc);
    }
    g->fillTriangle(cx,          cy - PLANE_H / 2,
                     cx - PLANE_W / 2, cy + PLANE_H / 2,
                     cx + PLANE_W / 2, cy + PLANE_H / 2, COLOR_GOOD);
    g->fillTriangle(cx - PLANE_W / 2 - 10, cy + 6,
                     cx,                    cy - 4,
                     cx - PLANE_W / 2 + 4,  cy + 10, COLOR_GOOD);
    g->fillTriangle(cx + PLANE_W / 2 + 10, cy + 6,
                     cx,                    cy - 4,
                     cx + PLANE_W / 2 - 4,  cy + 10, COLOR_GOOD);
    g->fillCircle(cx, cy, 4, COLOR_TEXT_DIM);
}

// Perspective tunnel backdrop: horizon + converging guide lines + a
// couple of depth rings, panned to react to steering (see file header).
static void draw_tunnel(Arduino_GFX *g) {
    float pan_x = -s_aim_x * WORLD_PAN_FACTOR;
    float pan_y = -s_aim_y * WORLD_PAN_FACTOR;
    int vx = VANISH_X + (int)pan_x;
    int vy = VANISH_Y + (int)pan_y;

    g->drawLine(0, vy, LCD_WIDTH, vy, COLOR_PANEL);
    g->drawLine(vx, HUD_TOP_MARGIN_PLN, vx, LCD_HEIGHT - BOTTOM_MARGIN_PLN, COLOR_PANEL);
    g->drawLine(vx, vy, 0, HUD_TOP_MARGIN_PLN, COLOR_PANEL);
    g->drawLine(vx, vy, LCD_WIDTH, HUD_TOP_MARGIN_PLN, COLOR_PANEL);
    g->drawLine(vx, vy, 0, LCD_HEIGHT - BOTTOM_MARGIN_PLN, COLOR_PANEL);
    g->drawLine(vx, vy, LCD_WIDTH, LCD_HEIGHT - BOTTOM_MARGIN_PLN, COLOR_PANEL);
    for (float ringz = 0.6f; ringz < Z_FAR; ringz += 0.7f) {
        int r = (int)(90.0f * proj_scale(ringz));
        g->drawCircle(vx, vy, r, COLOR_PANEL);
    }
}

static void draw_calibrating(Arduino_GFX *g) {
    float progress = (float)(millis() - s_cal_start_ms) / (float)CAL_HOLD_MS;
    if (progress > 1.0f) progress = 1.0f;

    ui_draw_centered_text(LCD_HEIGHT / 2 - 60, COLOR_TEXT, "Calibrating", 3);
    ui_draw_centered_text(LCD_HEIGHT / 2 - 15, COLOR_TEXT_DIM, "Hold the watch level", 2);

    int cx = LCD_WIDTH / 2, cy = LCD_HEIGHT / 2 + 60, r = 46;
    int total_ticks = 32, lit = (int)(progress * total_ticks);
    for (int i = 0; i < total_ticks; i++) {
        float a = (float)i / total_ticks * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
        int x0 = cx + (int)(cosf(a) * (r - 6)), y0 = cy + (int)(sinf(a) * (r - 6));
        int x1 = cx + (int)(cosf(a) * r), y1 = cy + (int)(sinf(a) * r);
        g->drawLine(x0, y0, x1, y1, i < lit ? COLOR_GOOD : COLOR_PANEL);
    }
}

static void draw_pickup_icon(Arduino_GFX *g, int px, int py, int r, PickupType type) {
    switch (type) {
        case PICKUP_SHIELD:
            g->drawCircle(px, py, r, COLOR565(0x66, 0xCC, 0xFF));
            g->drawCircle(px, py, r > 2 ? r - 2 : r, COLOR565(0x66, 0xCC, 0xFF));
            break;
        case PICKUP_RAPID:
            g->fillTriangle(px - r, py + r / 2, px + r / 3, py + r / 2, px, py - r, COLOR_WARN);
            g->fillTriangle(px - r / 3, py - r / 2, px + r, py - r / 2, px, py + r, COLOR_WARN);
            break;
        case PICKUP_LIFE:
            g->fillRoundRect(px - r, py - r / 3, 2 * r, (2 * r) / 3, 2, COLOR565(0xFF, 0x66, 0x99));
            g->fillRoundRect(px - r / 3, py - r, (2 * r) / 3, 2 * r, 2, COLOR565(0xFF, 0x66, 0x99));
            break;
        case PICKUP_JAM:
            g->fillCircle(px, py, r, COLOR565(0x66, 0x33, 0x88));
            g->fillCircle(px, py, r > 3 ? r - 3 : r, COLOR_BG);
            break;
    }
}

static void plane_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    draw_tunnel(g);

    // Starfield - depth-only motion cue, drawn behind everything else,
    // rolled+panned so it visibly reacts to steering.
    for (int i = 0; i < STAR_COUNT; i++) {
        int sx, sy;
        project_world_visual(s_stars[i].wx, s_stars[i].wy, s_stars[i].z, &sx, &sy);
        int r = 1 + (int)(2.0f * proj_scale(s_stars[i].z));
        if (sx >= 0 && sx < LCD_WIDTH && sy >= HUD_TOP_MARGIN_PLN && sy < LCD_HEIGHT - BOTTOM_MARGIN_PLN) {
            g->fillCircle(sx, sy, r, COLOR_TEXT_DIM);
        }
    }

    if (s_pstate == PSTATE_TITLE) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 70, COLOR_TEXT, "PLANE", 4);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 25, COLOR_TEXT_DIM, "Tilt to fly", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 5, COLOR_TEXT_DIM, "Tap: shoot", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 35, COLOR_TEXT_DIM, "Hold: boost", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 75, COLOR_ACCENT2, "Tap to start", 2);
        return;
    }

    if (s_pstate == PSTATE_CALIBRATING) {
        draw_calibrating(g);
        return;
    }

    // PSTATE_PLAYING or PSTATE_GAMEOVER from here on.

    // Enemies - perspective-scaled, fixed frame (hit-detection accurate).
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!s_enemies[i].active) continue;
        int ex, ey;
        project_fixed(s_enemies[i].wx, s_enemies[i].wy, s_enemies[i].z, &ex, &ey);
        float sc = proj_scale(s_enemies[i].z);
        int hw = (int)(15.0f * sc);
        int hh = (int)(12.0f * sc);
        uint16_t c = (s_enemies[i].type == ENEMY_WEAVER) ? COLOR565(0xCC, 0x44, 0xFF) : COLOR_BAD;
        if (s_enemies[i].type == ENEMY_WEAVER) {
            // Diamond - visually distinct from the basic triangle enemy.
            g->fillTriangle(ex - hw, ey, ex, ey - hh, ex, ey + hh, c);
            g->fillTriangle(ex + hw, ey, ex, ey - hh, ex, ey + hh, c);
        } else {
            g->fillTriangle(ex - hw, ey - hh, ex + hw, ey - hh, ex, ey + hh, c);
        }
    }

    // Pickups - same fixed frame, small icon per type.
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!s_pickups[i].active) continue;
        int px, py;
        project_fixed(s_pickups[i].wx, s_pickups[i].wy, s_pickups[i].z, &px, &py);
        int r = 3 + (int)(9.0f * proj_scale(s_pickups[i].z));
        draw_pickup_icon(g, px, py, r, s_pickups[i].type);
    }

    // Bullets
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!s_bullets[i].active) continue;
        int bx, by;
        project_fixed(s_bullets[i].wx, s_bullets[i].wy, s_bullets[i].z, &bx, &by);
        int r = 1 + (int)(4.0f * proj_scale(s_bullets[i].z));
        g->fillCircle(bx, by, r, COLOR_ACCENT3);
    }

    // Explosion particles (screen-space, no projection needed)
    for (int i = 0; i < s_particle_count; i++) {
        int r = (16 - s_particles[i].ttl) / 2 + 3;
        uint16_t c = (s_particles[i].ttl > 7) ? COLOR_WARN : COLOR_ACCENT3;
        g->fillCircle((int)s_particles[i].x, (int)s_particles[i].y, r, c);
    }

    // Ship: blink while invulnerable
    int ship_x = VANISH_X + (int)s_aim_x;
    int ship_y = VANISH_Y + (int)s_aim_y;
    bool ship_visible = (millis() >= s_invuln_until_ms) || ((millis() / 120) % 2 == 0);
    draw_ship(g, ship_x, ship_y, s_pstate == PSTATE_PLAYING ? ship_visible : false);

    // HUD
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_score);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    g->setCursor(16, 14);
    g->print(buf);

    for (int i = 0; i < s_lives; i++) {
        g->fillCircle(LCD_WIDTH - 20 - i * 22, 20, 7, COLOR_BAD);
    }

    int status_y = HUD_TOP_MARGIN_PLN - 30;
    if (s_boosting) {
        ui_draw_centered_text(status_y, COLOR_ACCENT3, "BOOST", 2);
        status_y -= 24;
    }
    if (s_shield_active) {
        ui_draw_centered_text(status_y, COLOR565(0x66, 0xCC, 0xFF), "SHIELD", 2);
        status_y -= 24;
    }
    if (s_rapid_active) {
        ui_draw_centered_text(status_y, COLOR_WARN, "RAPID", 2);
        status_y -= 24;
    }
    if (s_jammed) {
        ui_draw_centered_text(status_y, COLOR565(0xCC, 0x66, 0xFF), "JAMMED", 2);
    }

    if (s_pstate == PSTATE_GAMEOVER) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 16, COLOR_TEXT, buf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 46, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

// ---- Touch: quick tap = shoot, hold = boost -------------------------------

static void plane_touch(int x, int y, bool pressed) {
    (void)x; (void)y; // movement comes from tilt, not touch position
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    if (s_pstate == PSTATE_TITLE) {
        if (tap_edge) enter_calibrating();
        return;
    }
    if (s_pstate == PSTATE_CALIBRATING) {
        return; // just hold still - no touch needed during the quick calibration
    }
    if (s_pstate == PSTATE_GAMEOVER) {
        if (tap_edge) {
            plane_create();
            enter_calibrating();
        }
        return;
    }

    // PSTATE_PLAYING
    if (pressed) {
        if (!s_touch_active) {
            s_touch_active = true;
            s_touch_start_ms = millis();
            s_boosting = false;
        } else if (!s_boosting && millis() - s_touch_start_ms >= BOOST_HOLD_MS) {
            s_boosting = true;
        }
    } else {
        if (s_touch_active) {
            uint32_t held = millis() - s_touch_start_ms;
            if (!s_boosting && held < BOOST_HOLD_MS) {
                plane_shoot();
            }
            s_touch_active = false;
            s_boosting = false;
        }
    }
}

// ---- Fixed-step physics ---------------------------------------------------

static void physics_step() {
    uint32_t now = millis();

    // --- Power-up/down expiry ---
    if (s_shield_active && now >= s_shield_until_ms) s_shield_active = false;
    if (s_rapid_active && now >= s_rapid_until_ms) s_rapid_active = false;
    if (s_jammed && now >= s_jammed_until_ms) s_jammed = false;

    // --- Tilt steering, relative to the calibrated center ---
    ImuSample s = imu_read();
    if (s.valid) {
        float tx, ty;
        imu_get_calibrated_tilt(s, tx, ty);
        s_aim_x = (tx - s_cal_x) * TILT_GAIN;
        s_aim_y = (ty - s_cal_y) * TILT_GAIN;
        if (s_aim_x < -AIM_RANGE_X) s_aim_x = -AIM_RANGE_X;
        if (s_aim_x > AIM_RANGE_X)  s_aim_x = AIM_RANGE_X;
        if (s_aim_y < -AIM_RANGE_Y) s_aim_y = -AIM_RANGE_Y;
        if (s_aim_y > AIM_RANGE_Y)  s_aim_y = AIM_RANGE_Y;
    }

    // Controller: d-pad nudges aim continuously (no "absolute tilt
    // position" for a digital d-pad to map to directly), button A
    // shoots (instant, on press - a controller has a dedicated button
    // so it doesn't need touch's tap-vs-hold disambiguation), button B
    // holds to boost.
    if (controller_connected()) {
        const float CTRL_AIM_SPEED = 220.0f; // px/sec
        float cdt = PHYS_STEP_MS / 1000.0f;
        if (controller_dpad(CTRL_LEFT)) s_aim_x -= CTRL_AIM_SPEED * cdt;
        if (controller_dpad(CTRL_RIGHT)) s_aim_x += CTRL_AIM_SPEED * cdt;
        if (controller_dpad(CTRL_UP)) s_aim_y -= CTRL_AIM_SPEED * cdt;
        if (controller_dpad(CTRL_DOWN)) s_aim_y += CTRL_AIM_SPEED * cdt;
        if (s_aim_x < -AIM_RANGE_X) s_aim_x = -AIM_RANGE_X;
        if (s_aim_x > AIM_RANGE_X)  s_aim_x = AIM_RANGE_X;
        if (s_aim_y < -AIM_RANGE_Y) s_aim_y = -AIM_RANGE_Y;
        if (s_aim_y > AIM_RANGE_Y)  s_aim_y = AIM_RANGE_Y;

        s_boosting = controller_button(CTRL_BTN_B);

        static bool s_ctrl_a_prev = false;
        bool a = controller_button(CTRL_BTN_A);
        if (a && !s_ctrl_a_prev) plane_shoot();
        s_ctrl_a_prev = a;
    }

    // Promote a held touch to boost even if plane_touch() hasn't fired
    // again yet (touch is only re-delivered by the OS on movement).
    if (s_touch_active && !s_boosting && now - s_touch_start_ms >= BOOST_HOLD_MS) {
        s_boosting = true;
    }

    float enemy_speed = s_boosting ? ENEMY_Z_SPEED * BOOST_Z_MULT : ENEMY_Z_SPEED;

    // --- Bullets: fly away from the camera ---
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!s_bullets[i].active) continue;
        s_bullets[i].z += BULLET_Z_SPEED;
        if (s_bullets[i].z > Z_FAR) s_bullets[i].active = false;
    }

    // --- Enemies: approach the camera; weavers also drift laterally ---
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!s_enemies[i].active) continue;
        s_enemies[i].z -= enemy_speed;
        if (s_enemies[i].type == ENEMY_WEAVER) {
            s_enemies[i].weave_phase += 0.15f;
            s_enemies[i].wx += sinf(s_enemies[i].weave_phase) * 1.6f;
            if (s_enemies[i].wx > WORLD_RANGE_X) s_enemies[i].wx = WORLD_RANGE_X;
            if (s_enemies[i].wx < -WORLD_RANGE_X) s_enemies[i].wx = -WORLD_RANGE_X;
        }
        if (s_enemies[i].z <= Z_NEAR) {
            // Arrived - dodge check against the ship's current aim,
            // both projected to the near plane in screen space.
            bool invuln = (now < s_invuln_until_ms) || s_shield_active;
            if (!invuln) {
                float ex_screen = s_enemies[i].wx / Z_NEAR;
                float ey_screen = s_enemies[i].wy / Z_NEAR;
                float dx = ex_screen - s_aim_x;
                float dy = ey_screen - s_aim_y;
                if (dx * dx + dy * dy < (float)(ARRIVAL_HIT_PX * ARRIVAL_HIT_PX)) {
                    add_particle(VANISH_X + s_aim_x, VANISH_Y + s_aim_y);
                    s_lives--;
                    s_invuln_until_ms = now + INVULN_MS;
                    if (s_lives <= 0) {
                        s_pstate = PSTATE_GAMEOVER;
                        game_sfx_gameover();
                    } else {
                        game_sfx_hit();
                    }
                }
            }
            s_enemies[i].active = false;
        }
    }

    // --- Pickups: approach the camera, collect on arrival if close ---
    for (int i = 0; i < MAX_PICKUPS; i++) {
        if (!s_pickups[i].active) continue;
        s_pickups[i].z -= PICKUP_Z_SPEED;
        if (s_pickups[i].z <= Z_NEAR) {
            float px_screen = s_pickups[i].wx / Z_NEAR;
            float py_screen = s_pickups[i].wy / Z_NEAR;
            float dx = px_screen - s_aim_x;
            float dy = py_screen - s_aim_y;
            if (dx * dx + dy * dy < (float)(ARRIVAL_HIT_PX * ARRIVAL_HIT_PX)) {
                add_particle(VANISH_X + s_aim_x, VANISH_Y + s_aim_y);
                switch (s_pickups[i].type) {
                    case PICKUP_SHIELD:
                        s_shield_active = true;
                        s_shield_until_ms = now + SHIELD_DURATION_MS;
                        game_sfx_score();
                        break;
                    case PICKUP_RAPID:
                        s_rapid_active = true;
                        s_rapid_until_ms = now + RAPID_DURATION_MS;
                        game_sfx_score();
                        break;
                    case PICKUP_LIFE:
                        if (s_lives < MAX_LIVES) s_lives++;
                        game_sfx_score();
                        break;
                    case PICKUP_JAM:
                        s_jammed = true;
                        s_jammed_until_ms = now + JAM_DURATION_MS;
                        game_sfx_hit();
                        break;
                }
            }
            s_pickups[i].active = false;
        }
    }

    // --- Stars: recycle once they pass the camera ---
    for (int i = 0; i < STAR_COUNT; i++) {
        s_stars[i].z -= (s_boosting ? 0.05f : 0.02f);
        if (s_stars[i].z <= Z_NEAR) spawn_star_at(i);
    }

    // --- Bullet vs enemy collisions: same depth, close laterally ---
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!s_enemies[i].active) continue;
        for (int j = 0; j < MAX_BULLETS; j++) {
            if (!s_bullets[j].active) continue;
            if (fabsf(s_bullets[j].z - s_enemies[i].z) > Z_HIT_TOL) continue;
            float dx = s_bullets[j].wx - s_enemies[i].wx;
            float dy = s_bullets[j].wy - s_enemies[i].wy;
            if (dx * dx + dy * dy < WORLD_HIT_RADIUS * WORLD_HIT_RADIUS) {
                float sc = proj_scale(s_enemies[i].z);
                add_particle(VANISH_X + s_enemies[i].wx * sc, VANISH_Y + s_enemies[i].wy * sc);
                s_enemies[i].active = false;
                s_bullets[j].active = false;
                s_score += (s_enemies[i].type == ENEMY_WEAVER) ? 20 : 10;
                game_sfx_explosion();
                break;
            }
        }
    }

    // --- Spawn enemies: faster with score, faster still while boosting ---
    uint32_t interval = 900 - (s_score > 400 ? 400 : (uint32_t)s_score);
    if (s_boosting) interval /= 2;
    if (now - s_last_enemy_spawn >= interval) {
        s_last_enemy_spawn = now;
        spawn_enemy();
    }

    // --- Spawn pickups: randomized interval, independent of enemies ---
    if (now >= s_next_pickup_spawn_ms) {
        spawn_pickup();
        s_next_pickup_spawn_ms = now + PICKUP_SPAWN_MIN_MS + (uint32_t)random(PICKUP_SPAWN_MAX_MS - PICKUP_SPAWN_MIN_MS);
    }

    // --- Particle decay ---
    for (int i = s_particle_count - 1; i >= 0; i--) {
        s_particles[i].ttl--;
        if (s_particles[i].ttl <= 0) {
            s_particles[i] = s_particles[s_particle_count - 1];
            s_particle_count--;
        }
    }

    // --- Score trickle while boosting ---
    if (s_boosting && now - s_last_boost_score_tick >= 100) {
        s_last_boost_score_tick = now;
        s_score += 1;
    }
}

static void plane_tick() {
    game_music_poll(); // keep looping regardless of game sub-state
    uint32_t now = millis();

    if (s_pstate == PSTATE_CALIBRATING) {
        ImuSample s = imu_read();
        if (s.valid) {
            float tx, ty;
            imu_get_calibrated_tilt(s, tx, ty);
            s_cal_sum_x += tx;
            s_cal_sum_y += ty;
            s_cal_samples++;
        }
        if (now - s_cal_start_ms >= CAL_HOLD_MS) {
            if (s_cal_samples > 0) {
                s_cal_x = s_cal_sum_x / s_cal_samples;
                s_cal_y = s_cal_sum_y / s_cal_samples;
            }
            s_pstate = PSTATE_PLAYING;
            s_last_phys_step = now; // avoid a burst of catch-up steps
        }
        return;
    }

    if (s_pstate != PSTATE_PLAYING) return;

    // Fixed-step simulation: run as many 16ms steps as have elapsed.
    // Capped at a few steps per call so a long stall (e.g. a screen
    // switch) can't cause a burst of simulation to "catch up" at once.
    int guard = 4;
    while (now - s_last_phys_step >= PHYS_STEP_MS && guard-- > 0) {
        s_last_phys_step += PHYS_STEP_MS;
        physics_step();
    }
}

Screen plane_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    plane_create, plane_draw, plane_touch, plane_tick, plane_destroy, nullptr,
    0, true, true // idle_frame_ms, suppress_idle, needs_tilt_calibration - this game steers by tilt
};
