/*
 * game_fruitninja.cpp
 * Swipe to slice fruit launched up from below, avoid slicing bombs -
 * now with level progression and a boss encounter.
 *
 * Levels: every LEVEL_SCORE_STEP points, spawn interval shortens and
 * launch speed increases (both capped). Every BOSS_EVERY_N_LEVELS is a
 * boss level: a single oversized "Boss Melon" launches instead of the
 * normal fruit stream, with HP that takes several separate slices to
 * bring down (the same slice-detection code just doesn't remove it on
 * the first hit) - turning "slice things" into "land several hits on
 * one big target before it falls" without needing a mechanic this
 * genre doesn't otherwise have. No penalty if it falls unfinished -
 * it's a missed bonus opportunity, not a failure state.
 *
 * Round-display note: unlike game_flappy.cpp, this doesn't need a
 * bespoke circular geometry - fruit are launched on ballistic arcs
 * (gravity pulls them back down) entirely within the same verified-
 * safe rectangular bounds already established for the breakout arena
 * and the media file browser (300px wide, y 95..405).
 *
 * Physics runs in fixed 16ms steps (see game_plane.cpp for why).
 */
#include "game_fruitninja.h"
#include "config.h"
#include "board_pins.h"
#include "game_audio.h"
#include "hal_controller.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#define PLAY_LEFT   83
#define PLAY_RIGHT  383
#define PLAY_TOP     95
#define PLAY_BOTTOM 405

#define GRAVITY          480.0f
#define SPAWN_MIN_MS_BASE 550
#define SPAWN_MAX_MS_BASE 1100
#define SPAWN_MS_FLOOR     280
#define LAUNCH_VY_BASE     320.0f
#define LAUNCH_VY_MAX      420.0f
#define MAX_FRUITS   10
#define MAX_PARTICLES 24
#define START_LIVES  3

#define LEVEL_SCORE_STEP    100
#define BOSS_EVERY_N_LEVELS   3
#define BOSS_HP               5
#define BOSS_RADIUS         36.0f

#define PHYS_STEP_MS 16

enum FNState { FN_TITLE, FN_PLAYING, FN_LEVELUP, FN_BOSS_INTRO, FN_BOSS_CLEAR, FN_GAMEOVER };
enum FruitType { FRUIT_APPLE, FRUIT_ORANGE, FRUIT_MELON, FRUIT_BOMB, FRUIT_BOSS };

struct Fruit { float x, y, vx, vy, r; FruitType type; int hp; bool active; bool sliced; };
struct Particle { float x, y, vx, vy; uint16_t color; int ttl; };

static FNState s_state;
static Fruit s_fruits[MAX_FRUITS];
static Particle s_particles[MAX_PARTICLES];
static int s_particle_count;
static int s_score, s_lives, s_level;
static uint32_t s_next_spawn_ms;
static uint32_t s_last_phys_step;
static uint32_t s_state_enter_ms;
static bool s_boss_active_this_level;

static bool s_touch_down;
// Rising-edge tracking for the title/gameover "tap to (re)start"
// transition only - the in-game slice tracking below already guards
// itself correctly via s_touch_down and needs continuous `pressed`.
static bool s_prev_pressed;
static float s_last_tx, s_last_ty;

static bool is_boss_level(int level) { return level % BOSS_EVERY_N_LEVELS == 0; }

static void add_particle(float x, float y, uint16_t color) {
    if (s_particle_count >= MAX_PARTICLES) return;
    Particle &p = s_particles[s_particle_count++];
    p.x = x; p.y = y;
    p.vx = ((float)random(200) - 100.0f);
    p.vy = ((float)random(200) - 160.0f);
    p.color = color;
    p.ttl = 16;
}

static void enter_state(FNState st) {
    s_state = st;
    s_state_enter_ms = millis();
}

static void fruitninja_reset() {
    for (int i = 0; i < MAX_FRUITS; i++) s_fruits[i].active = false;
    s_particle_count = 0;
    s_score = 0;
    s_lives = START_LIVES;
    s_level = 1;
    s_boss_active_this_level = false;
    s_next_spawn_ms = millis() + 600;
    s_last_phys_step = millis();
    s_touch_down = false;
}

static void fruitninja_create() {
    enter_state(FN_TITLE);
    fruitninja_reset();
    s_prev_pressed = false;
}

static void fruitninja_destroy() {}

static uint16_t fruit_color(FruitType t) {
    switch (t) {
        case FRUIT_APPLE:  return COLOR_BAD;
        case FRUIT_ORANGE: return COLOR_WARN;
        case FRUIT_MELON:  return COLOR_GOOD;
        case FRUIT_BOMB:   return COLOR565(0x33, 0x33, 0x33);
        case FRUIT_BOSS:   return (uint16_t)COLOR565(0xFF, 0xD7, 0x00);
    }
    return COLOR_TEXT;
}

static void spawn_fruit() {
    float speed_mult = 1.0f + (float)(s_level - 1) * 0.05f;
    for (int i = 0; i < MAX_FRUITS; i++) {
        if (s_fruits[i].active) continue;
        s_fruits[i].active = true;
        s_fruits[i].sliced = false;
        s_fruits[i].x = PLAY_LEFT + 30 + (float)random((int)(PLAY_RIGHT - PLAY_LEFT - 60));
        s_fruits[i].y = PLAY_BOTTOM;
        s_fruits[i].vx = (float)random(120) - 60.0f;
        float vy = LAUNCH_VY_BASE + (float)random(90);
        if (vy > LAUNCH_VY_MAX) vy = LAUNCH_VY_MAX;
        s_fruits[i].vy = -vy * speed_mult;
        s_fruits[i].r = 20.0f;
        s_fruits[i].hp = 1;
        int r = random(100);
        s_fruits[i].type = (r < 12) ? FRUIT_BOMB : (r < 45) ? FRUIT_APPLE : (r < 78) ? FRUIT_ORANGE : FRUIT_MELON;
        return;
    }
}

static void spawn_boss_fruit() {
    for (int i = 0; i < MAX_FRUITS; i++) {
        if (s_fruits[i].active) continue;
        s_fruits[i].active = true;
        s_fruits[i].sliced = false;
        s_fruits[i].x = (PLAY_LEFT + PLAY_RIGHT) / 2.0f;
        s_fruits[i].y = PLAY_BOTTOM;
        s_fruits[i].vx = (float)random(60) - 30.0f;
        s_fruits[i].vy = -260.0f; // slower, longer hangtime - more chances to land hits
        s_fruits[i].r = BOSS_RADIUS;
        s_fruits[i].hp = BOSS_HP;
        s_fruits[i].type = FRUIT_BOSS;
        s_boss_active_this_level = true;
        return;
    }
}

static bool segment_hits_circle(float x1, float y1, float x2, float y2, float cx, float cy, float r) {
    float dx = x2 - x1, dy = y2 - y1;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0001f ? ((cx - x1) * dx + (cy - y1) * dy) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float px = x1 + t * dx, py = y1 + t * dy;
    float ddx = cx - px, ddy = cy - py;
    return (ddx * ddx + ddy * ddy) <= r * r;
}

static void advance_level_if_due() {
    if (s_score / LEVEL_SCORE_STEP + 1 <= s_level) return;
    s_level++;
    if (is_boss_level(s_level)) {
        enter_state(FN_BOSS_INTRO);
    } else {
        enter_state(FN_LEVELUP);
    }
}

static void slice_check(float x1, float y1, float x2, float y2) {
    for (int i = 0; i < MAX_FRUITS; i++) {
        if (!s_fruits[i].active || s_fruits[i].sliced) continue;
        if (!segment_hits_circle(x1, y1, x2, y2, s_fruits[i].x, s_fruits[i].y, s_fruits[i].r)) continue;

        if (s_fruits[i].type == FRUIT_BOMB) {
            s_fruits[i].sliced = true;
            s_fruits[i].active = false;
            add_particle(s_fruits[i].x, s_fruits[i].y, COLOR_BAD);
            add_particle(s_fruits[i].x, s_fruits[i].y, COLOR_WARN);
            game_sfx_explosion();
            enter_state(FN_GAMEOVER);
            return;
        }

        if (s_fruits[i].type == FRUIT_BOSS) {
            s_fruits[i].hp--;
            add_particle(s_fruits[i].x, s_fruits[i].y, fruit_color(FRUIT_BOSS));
            game_sfx_hit();
            if (s_fruits[i].hp <= 0) {
                s_fruits[i].sliced = true;
                s_fruits[i].active = false;
                for (int p = 0; p < 6; p++) add_particle(s_fruits[i].x, s_fruits[i].y, fruit_color(FRUIT_BOSS));
                s_score += 150;
                game_sfx_levelup();
                s_boss_active_this_level = false;
                s_level++; // move off the boss level so normal spawning resumes afterward
                enter_state(FN_BOSS_CLEAR);
                return;
            }
            continue; // still has HP left - not removed, keep falling
        }

        s_fruits[i].sliced = true;
        s_fruits[i].active = false;
        uint16_t c = fruit_color(s_fruits[i].type);
        for (int p = 0; p < 4; p++) add_particle(s_fruits[i].x, s_fruits[i].y, c);
        s_score += 10;
        game_sfx_score();
        advance_level_if_due();
        if (s_state != FN_PLAYING) return;
    }
}

static void physics_step() {
    float dt = PHYS_STEP_MS / 1000.0f;

    for (int i = 0; i < MAX_FRUITS; i++) {
        if (!s_fruits[i].active) continue;
        s_fruits[i].vy += GRAVITY * dt;
        s_fruits[i].x += s_fruits[i].vx * dt;
        s_fruits[i].y += s_fruits[i].vy * dt;
        if (s_fruits[i].x < PLAY_LEFT) { s_fruits[i].x = PLAY_LEFT; s_fruits[i].vx = fabsf(s_fruits[i].vx); }
        if (s_fruits[i].x > PLAY_RIGHT) { s_fruits[i].x = PLAY_RIGHT; s_fruits[i].vx = -fabsf(s_fruits[i].vx); }
        if (s_fruits[i].y > PLAY_BOTTOM + 40.0f) {
            bool was_boss = (s_fruits[i].type == FRUIT_BOSS);
            s_fruits[i].active = false;
            if (was_boss) {
                s_boss_active_this_level = false;
                s_level++; // boss encounter over either way, no penalty for an unfinished boss
                enter_state(FN_PLAYING);
            } else if (s_fruits[i].type != FRUIT_BOMB) {
                s_lives--;
                if (s_lives <= 0) {
                    enter_state(FN_GAMEOVER);
                    game_sfx_gameover();
                    return;
                }
            }
        }
    }

    for (int i = s_particle_count - 1; i >= 0; i--) {
        s_particles[i].x += s_particles[i].vx * dt;
        s_particles[i].y += s_particles[i].vy * dt;
        s_particles[i].vy += GRAVITY * 0.5f * dt;
        s_particles[i].ttl--;
        if (s_particles[i].ttl <= 0) {
            s_particles[i] = s_particles[s_particle_count - 1];
            s_particle_count--;
        }
    }

    if (!is_boss_level(s_level) && millis() >= s_next_spawn_ms) {
        spawn_fruit();
        uint32_t spawn_min = SPAWN_MIN_MS_BASE > (uint32_t)(s_level * 20) ? SPAWN_MIN_MS_BASE - s_level * 20 : SPAWN_MS_FLOOR;
        uint32_t spawn_max = SPAWN_MAX_MS_BASE > (uint32_t)(s_level * 30) ? SPAWN_MAX_MS_BASE - s_level * 30 : SPAWN_MS_FLOOR + 200;
        if (spawn_min < SPAWN_MS_FLOOR) spawn_min = SPAWN_MS_FLOOR;
        s_next_spawn_ms = millis() + spawn_min + (uint32_t)random(spawn_max - spawn_min);
    }
}

static void draw_fruit(Arduino_GFX *g, const Fruit &f) {
    int x = (int)f.x, y = (int)f.y, r = (int)f.r;
    if (f.type == FRUIT_BOMB) {
        g->fillCircle(x, y, r, fruit_color(FRUIT_BOMB));
        g->drawLine(x, y - r, x + 4, y - r - 8, COLOR_WARN);
        g->fillCircle(x + 4, y - r - 8, 3, COLOR_BAD);
    } else if (f.type == FRUIT_BOSS) {
        g->fillCircle(x, y, r, fruit_color(FRUIT_BOSS));
        g->drawCircle(x, y, r, COLOR_TEXT);
        g->fillRoundRect(x - 3, y - r - 8, 6, 10, 2, COLOR565(0x55, 0x33, 0x11));
        // HP pips above the boss fruit
        int total_w = BOSS_HP * 10 - 2;
        int px0 = x - total_w / 2;
        for (int i = 0; i < BOSS_HP; i++) {
            g->fillCircle(px0 + i * 10, y - r - 20, 3, i < f.hp ? COLOR_BAD : COLOR_PANEL);
        }
    } else {
        g->fillCircle(x, y, r, fruit_color(f.type));
        g->fillRoundRect(x - 2, y - r - 6, 4, 8, 2, COLOR565(0x55, 0x33, 0x11));
    }
}

static void fruitninja_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    g->drawRoundRect(PLAY_LEFT - 6, PLAY_TOP - 6, (PLAY_RIGHT - PLAY_LEFT) + 12,
                      (PLAY_BOTTOM - PLAY_TOP) + 12, 10, COLOR_PANEL);

    if (s_state == FN_TITLE) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 90, COLOR_TEXT, "FRUIT SLICE", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_TEXT_DIM, "Swipe to slice fruit", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 12, COLOR_TEXT_DIM, "Avoid the bombs!", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 14, COLOR_TEXT_DIM, "Every 3rd level: boss melon", 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_ACCENT2, "Tap to start", 2);
        return;
    }

    for (int i = 0; i < MAX_FRUITS; i++) {
        if (!s_fruits[i].active) continue;
        draw_fruit(g, s_fruits[i]);
    }
    for (int i = 0; i < s_particle_count; i++) {
        int r = s_particles[i].ttl / 3 + 2;
        g->fillCircle((int)s_particles[i].x, (int)s_particles[i].y, r, s_particles[i].color);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_score);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    g->setCursor(16, 14);
    g->print(buf);
    for (int i = 0; i < s_lives; i++) g->fillCircle(LCD_WIDTH - 20 - i * 22, 20, 7, COLOR_BAD);
    snprintf(buf, sizeof(buf), is_boss_level(s_level) ? "BOSS - Level %d" : "Level %d", s_level);
    ui_draw_centered_text(52, COLOR_TEXT_DIM, buf, 1);

    if (s_state == FN_LEVELUP) {
        char lbuf[16];
        snprintf(lbuf, sizeof(lbuf), "LEVEL %d", s_level);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, lbuf, 3);
    } else if (s_state == FN_BOSS_INTRO) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_BAD, "BOSS MELON", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 26, COLOR_TEXT_DIM, "Slice it 5 times!", 2);
    } else if (s_state == FN_BOSS_CLEAR) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, "BOSS SLICED!", 3);
    } else if (s_state == FN_GAMEOVER) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 16, COLOR_TEXT, buf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 46, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

static void fruitninja_touch(int x, int y, bool pressed) {
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    if (s_state == FN_TITLE || s_state == FN_GAMEOVER) {
        if (tap_edge) {
            fruitninja_reset();
            enter_state(FN_PLAYING);
        }
        return;
    }
    if (s_state != FN_PLAYING) return; // banners are transient, no touch needed
    if (pressed) {
        if (s_touch_down) {
            slice_check(s_last_tx, s_last_ty, (float)x, (float)y);
        }
        s_last_tx = (float)x;
        s_last_ty = (float)y;
        s_touch_down = true;
    } else {
        s_touch_down = false;
    }
}

static void fruitninja_tick() {
    uint32_t now = millis();

    if (s_state == FN_LEVELUP) {
        if (now - s_state_enter_ms > 1100) enter_state(FN_PLAYING);
        return;
    }
    if (s_state == FN_BOSS_INTRO) {
        if (now - s_state_enter_ms > 1300) {
            enter_state(FN_PLAYING);
            spawn_boss_fruit();
        }
        return;
    }
    if (s_state == FN_BOSS_CLEAR) {
        if (now - s_state_enter_ms > 1300) enter_state(FN_PLAYING);
        return;
    }
    if (s_state != FN_PLAYING) return;

    if (controller_connected()) {
        // Genuine design decision, not a forced fit: a d-pad has no
        // continuous swipe path the way touch does, so this maps
        // direction -> a fixed slice line through the play area's
        // center, and button A triggers it. Last-held direction
        // persists (defaults to horizontal) so you can aim before
        // slicing rather than needing to hold the d-pad and press A
        // in the same instant.
        static float s_ctrl_slice_dx = 1.0f, s_ctrl_slice_dy = 0.0f;
        if (controller_dpad(CTRL_LEFT)) { s_ctrl_slice_dx = -1.0f; s_ctrl_slice_dy = 0.0f; }
        else if (controller_dpad(CTRL_RIGHT)) { s_ctrl_slice_dx = 1.0f; s_ctrl_slice_dy = 0.0f; }
        else if (controller_dpad(CTRL_UP)) { s_ctrl_slice_dx = 0.0f; s_ctrl_slice_dy = -1.0f; }
        else if (controller_dpad(CTRL_DOWN)) { s_ctrl_slice_dx = 0.0f; s_ctrl_slice_dy = 1.0f; }

        static bool s_ctrl_a_prev = false;
        bool a = controller_button(CTRL_BTN_A);
        if (a && !s_ctrl_a_prev) {
            float cx = (PLAY_LEFT + PLAY_RIGHT) / 2.0f, cy = (PLAY_TOP + PLAY_BOTTOM) / 2.0f;
            float half_len = 140.0f;
            slice_check(cx - s_ctrl_slice_dx * half_len, cy - s_ctrl_slice_dy * half_len,
                        cx + s_ctrl_slice_dx * half_len, cy + s_ctrl_slice_dy * half_len);
        }
        s_ctrl_a_prev = a;
    }

    int guard = 4;
    while (now - s_last_phys_step >= PHYS_STEP_MS && guard-- > 0) {
        s_last_phys_step += PHYS_STEP_MS;
        physics_step();
        if (s_state != FN_PLAYING) break;
    }
}

Screen fruitninja_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    fruitninja_create, fruitninja_draw, fruitninja_touch, fruitninja_tick, fruitninja_destroy, nullptr,
    0, true // idle_frame_ms, suppress_idle - never dim or lock while a game is active
};
