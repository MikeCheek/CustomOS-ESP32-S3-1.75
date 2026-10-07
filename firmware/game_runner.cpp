/*
 * game_runner.cpp
 * Endless 3-lane runner (subway-surfer style): swipe left/right to
 * change lanes, up to jump, down to duck, dodge obstacles, collect
 * coins - now with level progression and a boss chase.
 *
 * Levels: every LEVEL_SCORE_STEP points, speed and obstacle density
 * increase and a brief "LEVEL UP" banner shows. Every
 * BOSS_EVERY_N_LEVELS is a boss chase: a looming shape appears behind
 * the player, obstacle spawn rate roughly doubles, and it lasts a
 * fixed duration - surviving the whole chase clears the boss for a
 * score bonus. This genre's own core skill is dodging and surviving,
 * not attacking something, so "survive an intensified gauntlet for a
 * fixed stretch" is the boss mechanic here, rather than grafting on
 * a combat system the format was never built for. A hit during the
 * chase is still a normal game over, same as any other obstacle hit.
 *
 * Reuses the exact perspective projection technique game_plane.cpp
 * already established (a vanishing point, world positions divided by
 * depth) rather than inventing a new one - lanes are fixed world-x
 * offsets that naturally converge toward the vanishing point as depth
 * decreases, which is both the classic "converging road" look for
 * this genre AND already round-display-safe by construction, the same
 * way it was for Plane's tunnel.
 *
 * Physics runs in fixed 16ms steps (see game_plane.cpp for why).
 */
#include "game_runner.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "hal_controller.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#define VANISH_X (LCD_WIDTH / 2)
#define VANISH_Y 120
#define Z_FAR  2.6f
#define Z_NEAR 0.42f
#define LANE_WORLD_X 62.0f // fixed world-x offset per lane; /z gives the converging look

#define PHYS_STEP_MS 16
#define JUMP_MS 480
#define DUCK_MS 480
#define SWIPE_MIN_PX 28

#define MAX_OBSTACLES 6
#define MAX_COINS     6

#define LEVEL_SCORE_STEP     150
#define BOSS_EVERY_N_LEVELS    3
#define BOSS_CHASE_MS       12000

enum RState { R_TITLE, R_PLAYING, R_LEVELUP, R_BOSS_INTRO, R_BOSS_CHASE, R_BOSS_CLEAR, R_GAMEOVER };
enum PoseState { POSE_RUN, POSE_JUMP, POSE_DUCK };
enum ObsType { OBS_LOW, OBS_HIGH, OBS_FULL }; // LOW: jump it, HIGH: duck it, FULL: must be a different lane

struct Obstacle { int lane; float z; ObsType type; bool active; };
struct Coin { int lane; float z; bool active; };

static RState s_state;
static int s_lane;          // -1, 0, 1
static PoseState s_pose;
static uint32_t s_pose_until_ms;

static Obstacle s_obstacles[MAX_OBSTACLES];
static Coin s_coins[MAX_COINS];
static int s_score;
static int s_level;
static uint32_t s_last_obstacle_ms, s_last_coin_ms;
static uint32_t s_last_phys_step;
static uint32_t s_run_start_ms;
static uint32_t s_state_enter_ms;
static uint32_t s_chase_start_ms;
static int s_bonus_score; // from boss clears - added on top of the time+coin score

static bool s_touch_down;
// Rising-edge tracking for the title/gameover "tap to (re)start"
// transition only - the in-game touch tracking below already guards
// itself correctly via s_touch_down and needs continuous `pressed`.
static bool s_prev_pressed;
static int s_touch_start_x, s_touch_start_y;
static bool s_swipe_handled;

static inline float proj_scale(float z) { return 1.0f / z; }
static bool is_boss_level(int level) { return level % BOSS_EVERY_N_LEVELS == 0; }

static void enter_state(RState st) {
    s_state = st;
    s_state_enter_ms = millis();
}

static void runner_reset() {
    s_lane = 0;
    s_pose = POSE_RUN;
    s_pose_until_ms = 0;
    for (int i = 0; i < MAX_OBSTACLES; i++) s_obstacles[i].active = false;
    for (int i = 0; i < MAX_COINS; i++) s_coins[i].active = false;
    s_score = 0;
    s_level = 1;
    s_bonus_score = 0;
    uint32_t now = millis();
    s_last_obstacle_ms = now;
    s_last_coin_ms = now;
    s_last_phys_step = now;
    s_run_start_ms = now;
    s_touch_down = false;
    s_swipe_handled = false;
}

static void runner_create() {
    enter_state(R_TITLE);
    runner_reset();
    s_prev_pressed = false;
}

static void runner_destroy() {}

static void spawn_obstacle() {
    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (s_obstacles[i].active) continue;
        s_obstacles[i].active = true;
        s_obstacles[i].lane = random(3) - 1;
        s_obstacles[i].z = Z_FAR;
        int r = random(100);
        s_obstacles[i].type = (r < 40) ? OBS_LOW : (r < 75) ? OBS_HIGH : OBS_FULL;
        return;
    }
}

static void spawn_coin() {
    for (int i = 0; i < MAX_COINS; i++) {
        if (s_coins[i].active) continue;
        s_coins[i].active = true;
        s_coins[i].lane = random(3) - 1;
        s_coins[i].z = Z_FAR;
        return;
    }
}

static void do_jump() {
    if (s_pose != POSE_RUN) return;
    s_pose = POSE_JUMP;
    s_pose_until_ms = millis() + JUMP_MS;
}
static void do_duck() {
    if (s_pose != POSE_RUN) return;
    s_pose = POSE_DUCK;
    s_pose_until_ms = millis() + DUCK_MS;
}
static void change_lane(int dir) {
    int nl = s_lane + dir;
    if (nl < -1) nl = -1;
    if (nl > 1) nl = 1;
    s_lane = nl;
}

static void advance_level_if_due() {
    if (s_score / LEVEL_SCORE_STEP + 1 <= s_level) return;
    s_level++;
    if (is_boss_level(s_level)) {
        enter_state(R_BOSS_INTRO);
    } else {
        enter_state(R_LEVELUP);
    }
}

static void die() {
    enter_state(R_GAMEOVER);
    game_sfx_gameover();
}

static void physics_step() {
    uint32_t now = millis();
    if (s_pose != POSE_RUN && now >= s_pose_until_ms) s_pose = POSE_RUN;

    bool chasing = (s_state == R_BOSS_CHASE);

    float elapsed_s = (float)(now - s_run_start_ms) / 1000.0f;
    float speed = 0.014f + elapsed_s * 0.0006f + (float)(s_level - 1) * 0.0015f;
    if (speed > 0.05f) speed = 0.05f;
    if (chasing) speed *= 1.35f;

    for (int i = 0; i < MAX_OBSTACLES; i++) {
        if (!s_obstacles[i].active) continue;
        s_obstacles[i].z -= speed;
        if (s_obstacles[i].z <= Z_NEAR) {
            bool same_lane = (s_obstacles[i].lane == s_lane);
            bool safe = !same_lane;
            if (same_lane) {
                if (s_obstacles[i].type == OBS_LOW) safe = (s_pose == POSE_JUMP);
                else if (s_obstacles[i].type == OBS_HIGH) safe = (s_pose == POSE_DUCK);
                else safe = false; // OBS_FULL: only dodgeable by lane change
            }
            if (!safe) { die(); return; }
            s_obstacles[i].active = false;
            s_score += 5;
        }
    }

    for (int i = 0; i < MAX_COINS; i++) {
        if (!s_coins[i].active) continue;
        s_coins[i].z -= speed;
        if (s_coins[i].z <= Z_NEAR) {
            if (s_coins[i].lane == s_lane) {
                s_score += 15;
                game_sfx_score();
            }
            s_coins[i].active = false;
        }
    }

    uint32_t obs_interval = 900 - (uint32_t)(elapsed_s * 8.0f) - (uint32_t)(s_level * 20);
    if (chasing) obs_interval /= 2;
    if (obs_interval < 260) obs_interval = 260;
    if (now - s_last_obstacle_ms >= obs_interval) {
        s_last_obstacle_ms = now;
        spawn_obstacle();
    }
    if (now - s_last_coin_ms >= 700) {
        s_last_coin_ms = now;
        if (random(100) < 60) spawn_coin();
    }

    if (!chasing) advance_level_if_due();
    if (s_state != R_PLAYING && s_state != R_BOSS_CHASE) return;

    if (chasing && now - s_chase_start_ms >= BOSS_CHASE_MS) {
        s_bonus_score += 200;
        game_sfx_levelup();
        enter_state(R_BOSS_CLEAR);
    }
}

static void project(float world_x, float world_y, float z, int *sx, int *sy) {
    float sc = proj_scale(z);
    *sx = VANISH_X + (int)(world_x * sc);
    *sy = VANISH_Y + (int)(world_y * sc);
}

static void draw_lanes(Arduino_GFX *g) {
    for (int lane = -1; lane <= 1; lane++) {
        int x0, y0, x1, y1;
        project(lane * LANE_WORLD_X, 90.0f, Z_NEAR, &x0, &y0);
        project(lane * LANE_WORLD_X, 90.0f, Z_FAR, &x1, &y1);
        g->drawLine(x0, y0, x1, y1, COLOR_PANEL);
    }
    g->drawLine(0, VANISH_Y, LCD_WIDTH, VANISH_Y, COLOR_PANEL);
}

static void draw_boss_shadow(Arduino_GFX *g, uint32_t now) {
    // A looming presence right behind the vanishing point - not a
    // real approaching entity to collide with, just tension: it
    // pulses and grows subtly as the chase progresses.
    float t = (float)(now - s_chase_start_ms) / (float)BOSS_CHASE_MS;
    if (t > 1.0f) t = 1.0f;
    float pulse = 0.85f + 0.15f * sinf((float)now / 140.0f);
    int r = (int)((26.0f + t * 20.0f) * pulse);
    uint16_t c = COLOR565(0x55, 0x11, 0x22);
    g->fillCircle(VANISH_X, VANISH_Y - 6, r, c);
    g->fillCircle(VANISH_X - r / 2, VANISH_Y - 10, r / 3, COLOR_BAD);
    g->fillCircle(VANISH_X + r / 2, VANISH_Y - 10, r / 3, COLOR_BAD);
}

static void draw_player(Arduino_GFX *g) {
    int px, py;
    project(s_lane * LANE_WORLD_X, 90.0f, Z_NEAR, &px, &py);
    uint16_t c = COLOR_GOOD;
    if (s_pose == POSE_JUMP) {
        float t = 1.0f - (float)(s_pose_until_ms - millis()) / (float)JUMP_MS;
        if (t < 0) t = 0; if (t > 1) t = 1;
        int hop = (int)(sinf(t * (float)M_PI) * 34.0f);
        py -= hop;
        g->fillRoundRect(px - 14, py - 26, 28, 26, 8, c);
    } else if (s_pose == POSE_DUCK) {
        g->fillRoundRect(px - 15, py - 14, 30, 16, 6, c);
    } else {
        g->fillRoundRect(px - 14, py - 30, 28, 30, 8, c);
    }
    g->fillCircle(px, py - (s_pose == POSE_DUCK ? 16 : 32), 8, c);
}

static void draw_obstacle(Arduino_GFX *g, const Obstacle &o) {
    float sc = proj_scale(o.z);
    int cx, cy;
    project(o.lane * LANE_WORLD_X, 90.0f, o.z, &cx, &cy);
    int w = (int)(30.0f * sc), h = (int)(30.0f * sc);
    if (w < 2) w = 2;
    if (h < 2) h = 2;
    uint16_t c = o.type == OBS_FULL ? COLOR_BAD : o.type == OBS_LOW ? COLOR_WARN : COLOR_ACCENT3;
    if (o.type == OBS_LOW) {
        g->fillRect(cx - w / 2, cy - h, w, h / 2, c); // low barrier - jump it
    } else if (o.type == OBS_HIGH) {
        g->fillRect(cx - w / 2, cy - h * 2, w, h / 2, c); // overhead - duck it
    } else {
        g->fillRect(cx - w / 2, cy - h * 2, w, h * 2, c); // full lane block
    }
}

static void draw_coin(Arduino_GFX *g, const Coin &c) {
    float sc = proj_scale(c.z);
    int cx, cy;
    project(c.lane * LANE_WORLD_X, 90.0f, c.z, &cx, &cy);
    int r = (int)(8.0f * sc);
    if (r < 1) r = 1;
    g->fillCircle(cx, cy - (int)(20.0f * sc), r, COLOR_WARN);
}

static void runner_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    draw_lanes(g);

    if (s_state == R_TITLE) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 90, COLOR_TEXT, "RUNNER", 4);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_TEXT_DIM, "Swipe: change lane", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 12, COLOR_TEXT_DIM, "up/down: jump, duck", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 14, COLOR_TEXT_DIM, "Every 3rd level: boss chase", 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_ACCENT2, "Tap to start", 2);
        return;
    }

    if (s_state == R_BOSS_CHASE) draw_boss_shadow(g, millis());

    for (int i = MAX_OBSTACLES - 1; i >= 0; i--) if (s_obstacles[i].active) draw_obstacle(g, s_obstacles[i]);
    for (int i = MAX_COINS - 1; i >= 0; i--) if (s_coins[i].active) draw_coin(g, s_coins[i]);
    draw_player(g);

    float elapsed_s = (float)(millis() - s_run_start_ms) / 1000.0f;
    int total_score = s_score + s_bonus_score + (int)(elapsed_s * 10.0f);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", total_score);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    g->setCursor(16, 14);
    g->print(buf);

    snprintf(buf, sizeof(buf), "Level %d", s_level);
    ui_draw_centered_text(38, COLOR_TEXT_DIM, buf, 1);

    if (s_state == R_BOSS_CHASE) {
        uint32_t remaining = BOSS_CHASE_MS - (millis() - s_chase_start_ms);
        snprintf(buf, sizeof(buf), "Survive: %lus", (unsigned long)(remaining / 1000 + 1));
        ui_draw_centered_text(56, COLOR_BAD, buf, 1);
    }

    if (s_state == R_LEVELUP) {
        char lbuf[16];
        snprintf(lbuf, sizeof(lbuf), "LEVEL %d", s_level);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, lbuf, 3);
    } else if (s_state == R_BOSS_INTRO) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_BAD, "BOSS CHASE", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 26, COLOR_TEXT_DIM, "Survive 12 seconds!", 2);
    } else if (s_state == R_BOSS_CLEAR) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, "OUTRAN IT!", 3);
    } else if (s_state == R_GAMEOVER) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        snprintf(buf, sizeof(buf), "Score: %d", total_score);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 16, COLOR_TEXT, buf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 46, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

static void runner_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    if (s_state == R_TITLE || s_state == R_GAMEOVER) {
        if (tap_edge) {
            runner_reset();
            enter_state(R_PLAYING);
        }
        return;
    }
    if (s_state != R_PLAYING && s_state != R_BOSS_CHASE) return; // banners are transient
    if (pressed) {
        if (!s_touch_down) {
            s_touch_down = true;
            s_touch_start_x = x;
            s_touch_start_y = y;
            s_swipe_handled = false;
        } else if (!s_swipe_handled) {
            int dx = x - s_touch_start_x, dy = y - s_touch_start_y;
            if (abs(dx) > abs(dy) && abs(dx) > SWIPE_MIN_PX) {
                change_lane(dx > 0 ? 1 : -1);
                s_swipe_handled = true;
            } else if (abs(dy) > abs(dx) && abs(dy) > SWIPE_MIN_PX) {
                if (dy < 0) do_jump(); else do_duck();
                s_swipe_handled = true;
            }
        }
    } else {
        s_touch_down = false;
    }
}

static void runner_tick() {
    uint32_t now = millis();

    if (s_state == R_LEVELUP) {
        if (now - s_state_enter_ms > 1100) enter_state(R_PLAYING);
        return;
    }
    if (s_state == R_BOSS_INTRO) {
        if (now - s_state_enter_ms > 1300) {
            s_chase_start_ms = now;
            enter_state(R_BOSS_CHASE);
        }
        return;
    }
    if (s_state == R_BOSS_CLEAR) {
        if (now - s_state_enter_ms > 1300) {
            s_level++; // move off the boss level so normal play resumes
            enter_state(R_PLAYING);
        }
        return;
    }
    if (s_state != R_PLAYING && s_state != R_BOSS_CHASE) return;

    if (controller_connected()) {
        static bool s_prev_left = false, s_prev_right = false, s_prev_up = false, s_prev_down = false;
        bool l = controller_dpad(CTRL_LEFT), r = controller_dpad(CTRL_RIGHT);
        bool u = controller_dpad(CTRL_UP), d = controller_dpad(CTRL_DOWN);
        if (l && !s_prev_left) change_lane(-1);
        if (r && !s_prev_right) change_lane(1);
        if (u && !s_prev_up) do_jump();
        if (d && !s_prev_down) do_duck();
        s_prev_left = l; s_prev_right = r; s_prev_up = u; s_prev_down = d;
    }

    int guard = 4;
    while (now - s_last_phys_step >= PHYS_STEP_MS && guard-- > 0) {
        s_last_phys_step += PHYS_STEP_MS;
        physics_step();
        if (s_state != R_PLAYING && s_state != R_BOSS_CHASE) break;
    }
}

Screen runner_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    runner_create, runner_draw, runner_touch, runner_tick, runner_destroy, nullptr,
    0, true // idle_frame_ms, suppress_idle - never dim or lock while a game is active
};
