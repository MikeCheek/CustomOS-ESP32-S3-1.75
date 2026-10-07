/*
 * game_flappy.cpp
 * Flappy Bird, using whichever pet species the player picked in the
 * Pet app (see app_pet.h) as the bird sprite - now with level
 * progression and a boss encounter.
 *
 * Levels: every LEVEL_SCORE_STEP points, the gap narrows and pipes
 * speed up (both capped at a floor so it never becomes literally
 * impossible). Every BOSS_EVERY_N_LEVELS is a boss level: instead of
 * a stream of random pipes, three "boss gates" appear in sequence,
 * each with a gap that OSCILLATES vertically as it approaches (a sine
 * wave in time) rather than sitting still - the boss mechanic here is
 * "thread a moving target three times in a row", which is the genre's
 * own core skill (precision timing through a gap) turned into a
 * deliberate gauntlet, rather than bolting on a combat mechanic this
 * kind of game was never built for.
 *
 * Round-native by construction, not by clipping a rectangle: the bird
 * flies along the horizontal line through the display's center (the
 * circle's widest point), and each pipe/gate's height is computed
 * from the circle's actual boundary AT ITS CURRENT X POSITION - so
 * the playable "tunnel" narrows naturally near the left/right edges
 * of the screen, the same way it would flying through a round tube,
 * rather than using a fixed rectangular play area. The circle
 * boundary itself is the ceiling and floor.
 *
 * Physics runs in fixed 16ms steps (see game_plane.cpp for why).
 */
#include "game_flappy.h"
#include "config.h"
#include "board_pins.h"
#include "app_pet.h"
#include "game_audio.h"
#include "hal_controller.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

#define FB_CX (LCD_WIDTH / 2)
#define FB_CY (LCD_HEIGHT / 2)
#define FB_R  222.0f // arena radius, just inside the true bezel

#define BIRD_X (FB_CX - 90)
#define BIRD_RADIUS 13.0f
#define GRAVITY     480.0f  // px/s^2
#define FLAP_VY    -190.0f  // px/s impulse
#define MAX_FALL_VY 320.0f

#define PIPE_GAP_BASE   108.0f
#define PIPE_GAP_MIN     78.0f
#define PIPE_WIDTH       30
#define PIPE_SPEED_BASE 130.0f // px/s
#define PIPE_SPEED_MAX  210.0f
#define PIPE_SPAWN_MS  1500
#define MAX_PIPES         6

#define LEVEL_SCORE_STEP     8   // points per level
#define BOSS_EVERY_N_LEVELS  5
#define BOSS_GATES_REQUIRED  3
#define BOSS_GATE_GAP       120.0f
#define BOSS_OSCILLATE_AMPL  55.0f
#define BOSS_OSCILLATE_HZ     0.6f

#define PHYS_STEP_MS 16

enum FBState { FB_TITLE, FB_PLAYING, FB_LEVELUP, FB_BOSS_INTRO, FB_BOSS_CLEAR, FB_GAMEOVER };

struct Pipe { float x, gap_center; bool scored; bool active; };
struct BossGate { float x, gap_base; uint32_t spawn_ms; bool scored; bool active; };

static FBState s_state;
static float s_bird_y, s_bird_vy;
static float s_bird_tunnel_half_h; // precomputed once - BIRD_X is fixed
static Pipe s_pipes[MAX_PIPES];
static BossGate s_boss_gate;
static int s_boss_gates_passed;
static int s_score;
static int s_level;
static uint32_t s_last_spawn_ms;
static uint32_t s_last_phys_step;
static bool s_prev_pressed; // rising-edge - without this, holding the finger down re-armed FLAP_VY every frame, letting the bird hover instead of requiring discrete taps
static uint32_t s_state_enter_ms;

static float tunnel_half_h_at(float x) {
    float dx = x - FB_CX;
    float under = FB_R * FB_R - dx * dx;
    if (under <= 0.0f) return 0.0f;
    return sqrtf(under);
}

static bool is_boss_level(int level) { return level % BOSS_EVERY_N_LEVELS == 0; }

static float pipe_gap_for_level(int level) {
    float g = PIPE_GAP_BASE - (float)(level - 1) * 3.0f;
    return g < PIPE_GAP_MIN ? PIPE_GAP_MIN : g;
}
static float pipe_speed_for_level(int level) {
    float s = PIPE_SPEED_BASE + (float)(level - 1) * 6.0f;
    return s > PIPE_SPEED_MAX ? PIPE_SPEED_MAX : s;
}

static void enter_state(FBState st) {
    s_state = st;
    s_state_enter_ms = millis();
}

static void spawn_boss_gate() {
    s_boss_gate.active = true;
    s_boss_gate.scored = false;
    s_boss_gate.x = FB_CX + FB_R + 30.0f;
    float range = FB_R * 0.35f;
    s_boss_gate.gap_base = ((float)random(2001) / 1000.0f - 1.0f) * range;
    s_boss_gate.spawn_ms = millis();
}

static void flappy_reset() {
    s_bird_y = FB_CY;
    s_bird_vy = 0;
    for (int i = 0; i < MAX_PIPES; i++) s_pipes[i].active = false;
    s_boss_gate.active = false;
    s_boss_gates_passed = 0;
    s_score = 0;
    s_level = 1;
    s_last_spawn_ms = millis();
    s_last_phys_step = millis();
    s_bird_tunnel_half_h = tunnel_half_h_at(BIRD_X);
}

static void flappy_create() {
    enter_state(FB_TITLE);
    flappy_reset();
    s_prev_pressed = false;
}

static void flappy_destroy() {}

static void spawn_pipe() {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (s_pipes[i].active) continue;
        s_pipes[i].active = true;
        s_pipes[i].scored = false;
        s_pipes[i].x = FB_CX + FB_R + 30.0f;
        // Gap center within a range that stays clear of the tunnel
        // walls across most of the arena width, not just at spawn.
        float range = FB_R * 0.45f;
        s_pipes[i].gap_center = ((float)random(2001) / 1000.0f - 1.0f) * range;
        return;
    }
}

static void die() {
    enter_state(FB_GAMEOVER);
    game_sfx_gameover();
}

static void physics_step() {
    float dt = PHYS_STEP_MS / 1000.0f;
    uint32_t now = millis();

    s_bird_vy += GRAVITY * dt;
    if (s_bird_vy > MAX_FALL_VY) s_bird_vy = MAX_FALL_VY;
    s_bird_y += s_bird_vy * dt;

    float top = FB_CY - s_bird_tunnel_half_h + BIRD_RADIUS;
    float bottom = FB_CY + s_bird_tunnel_half_h - BIRD_RADIUS;
    if (s_bird_y < top || s_bird_y > bottom) { die(); return; }

    bool boss_level = is_boss_level(s_level);
    float pipe_speed = pipe_speed_for_level(s_level);

    if (!boss_level) {
        float gap = pipe_gap_for_level(s_level);
        for (int i = 0; i < MAX_PIPES; i++) {
            if (!s_pipes[i].active) continue;
            s_pipes[i].x -= pipe_speed * dt;
            if (s_pipes[i].x < FB_CX - FB_R - 30.0f) { s_pipes[i].active = false; continue; }

            if (!s_pipes[i].scored && s_pipes[i].x < BIRD_X) {
                s_pipes[i].scored = true;
                s_score++;
                game_sfx_score();
                if (s_score % LEVEL_SCORE_STEP == 0) {
                    s_level++;
                    if (is_boss_level(s_level)) {
                        enter_state(FB_BOSS_INTRO);
                    } else {
                        enter_state(FB_LEVELUP);
                    }
                    return;
                }
            }

            if (fabsf(s_pipes[i].x - BIRD_X) < PIPE_WIDTH / 2.0f + BIRD_RADIUS) {
                float gtop = s_pipes[i].gap_center - gap / 2.0f;
                float gbot = s_pipes[i].gap_center + gap / 2.0f;
                float rel = s_bird_y - FB_CY;
                if (rel - BIRD_RADIUS < gtop || rel + BIRD_RADIUS > gbot) { die(); return; }
            }
        }

        if (now - s_last_spawn_ms >= PIPE_SPAWN_MS) {
            s_last_spawn_ms = now;
            spawn_pipe();
        }
    } else {
        if (!s_boss_gate.active) { spawn_boss_gate(); return; }

        s_boss_gate.x -= (pipe_speed * 1.15f) * dt;
        float t = (float)(now - s_boss_gate.spawn_ms) / 1000.0f;
        float gap_center = s_boss_gate.gap_base + sinf(t * BOSS_OSCILLATE_HZ * 2.0f * (float)M_PI) * BOSS_OSCILLATE_AMPL;

        if (s_boss_gate.x < FB_CX - FB_R - 30.0f) {
            s_boss_gate.active = false; // slipped through unscored - shouldn't normally happen, just don't get stuck
            return;
        }

        if (!s_boss_gate.scored && s_boss_gate.x < BIRD_X) {
            s_boss_gate.scored = true;
            s_boss_gates_passed++;
            s_score++;
            game_sfx_score();
            if (s_boss_gates_passed >= BOSS_GATES_REQUIRED) {
                s_score += 25;
                s_level++;
                game_sfx_levelup();
                enter_state(FB_BOSS_CLEAR);
                return;
            }
            s_boss_gate.active = false; // next gate spawns on the following physics step
        }

        if (fabsf(s_boss_gate.x - BIRD_X) < PIPE_WIDTH / 2.0f + BIRD_RADIUS) {
            float gtop = gap_center - BOSS_GATE_GAP / 2.0f;
            float gbot = gap_center + BOSS_GATE_GAP / 2.0f;
            float rel = s_bird_y - FB_CY;
            if (rel - BIRD_RADIUS < gtop || rel + BIRD_RADIUS > gbot) { die(); return; }
        }
    }
}

static void draw_pipe_at(Arduino_GFX *g, float x, float gap_center, float gap, uint16_t color) {
    float half_h = tunnel_half_h_at(x);
    if (half_h <= 0.0f) return;
    int px = (int)(x - PIPE_WIDTH / 2.0f);
    float gtop = gap_center - gap / 2.0f;
    float gbot = gap_center + gap / 2.0f;
    int top_y = (int)(FB_CY - half_h);
    int top_h = (int)((FB_CY + gtop) - (FB_CY - half_h));
    if (top_h > 0) g->fillRect(px, top_y, PIPE_WIDTH, top_h, color);
    int bot_y = (int)(FB_CY + gbot);
    int bot_h = (int)((FB_CY + half_h) - (FB_CY + gbot));
    if (bot_h > 0) g->fillRect(px, bot_y, PIPE_WIDTH, bot_h, color);
}

static void flappy_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    g->drawCircle(FB_CX, FB_CY, (int)FB_R, COLOR_PANEL);

    if (s_state == FB_TITLE) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 90, COLOR_TEXT, "FLAPPY", 4);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_TEXT_DIM, "Tap to flap", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_TEXT_DIM, "Level up every 8 points", 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 10, COLOR_TEXT_DIM, "Boss gate every 5 levels", 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_ACCENT2, "Tap to start", 2);
        pet_draw_icon(g, FB_CX, FB_CY - 130);
        return;
    }

    bool boss_level = is_boss_level(s_level);
    float gap = pipe_gap_for_level(s_level);

    if (!boss_level) {
        for (int i = 0; i < MAX_PIPES; i++) {
            if (!s_pipes[i].active) continue;
            draw_pipe_at(g, s_pipes[i].x, s_pipes[i].gap_center, gap, COLOR_GOOD);
        }
    } else if (s_boss_gate.active) {
        float t = (float)(millis() - s_boss_gate.spawn_ms) / 1000.0f;
        float gap_center = s_boss_gate.gap_base + sinf(t * BOSS_OSCILLATE_HZ * 2.0f * (float)M_PI) * BOSS_OSCILLATE_AMPL;
        draw_pipe_at(g, s_boss_gate.x, gap_center, BOSS_GATE_GAP, COLOR_BAD);
    }

    pet_draw_icon(g, BIRD_X, (int)s_bird_y);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_score);
    g->setTextSize(3);
    g->setTextColor(COLOR_TEXT);
    int tw = (int)strlen(buf) * 18;
    g->setCursor(LCD_WIDTH / 2 - tw / 2, 40);
    g->print(buf);

    snprintf(buf, sizeof(buf), boss_level ? "BOSS - Level %d" : "Level %d", s_level);
    ui_draw_centered_text(80, COLOR_TEXT_DIM, buf, 1);
    if (boss_level) {
        snprintf(buf, sizeof(buf), "Gates: %d/%d", s_boss_gates_passed, BOSS_GATES_REQUIRED);
        ui_draw_centered_text(96, COLOR_BAD, buf, 1);
    }

    if (s_state == FB_LEVELUP) {
        char lbuf[16];
        snprintf(lbuf, sizeof(lbuf), "LEVEL %d", s_level);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, lbuf, 3);
    } else if (s_state == FB_BOSS_INTRO) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_BAD, "BOSS GATES", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 26, COLOR_TEXT_DIM, "Thread the moving gap", 2);
    } else if (s_state == FB_BOSS_CLEAR) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 10, COLOR_GOOD, "BOSS CLEARED!", 3);
    } else if (s_state == FB_GAMEOVER) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        snprintf(buf, sizeof(buf), "Score: %d", s_score);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 16, COLOR_TEXT, buf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 46, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

static void flappy_touch(int x, int y, bool pressed) {
    (void)x; (void)y;
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;
    if (s_state == FB_TITLE) {
        flappy_reset();
        enter_state(FB_PLAYING);
        s_bird_vy = FLAP_VY;
        return;
    }
    if (s_state == FB_GAMEOVER) {
        flappy_reset();
        enter_state(FB_PLAYING);
        s_bird_vy = FLAP_VY;
        return;
    }
    if (s_state == FB_PLAYING) {
        s_bird_vy = FLAP_VY;
    }
    // FB_LEVELUP/FB_BOSS_INTRO/FB_BOSS_CLEAR: transient banners, no tap needed
}

static void flappy_tick() {
    uint32_t now = millis();

    if (s_state == FB_LEVELUP) {
        if (now - s_state_enter_ms > 1100) enter_state(FB_PLAYING);
        return;
    }
    if (s_state == FB_BOSS_INTRO) {
        if (now - s_state_enter_ms > 1300) {
            s_boss_gates_passed = 0;
            s_boss_gate.active = false;
            enter_state(FB_PLAYING);
            s_last_phys_step = now;
        }
        return;
    }
    if (s_state == FB_BOSS_CLEAR) {
        if (now - s_state_enter_ms > 1300) enter_state(FB_PLAYING);
        return;
    }
    if (s_state != FB_PLAYING) return;

    if (controller_connected()) {
        static bool s_ctrl_a_prev = false;
        bool a = controller_button(CTRL_BTN_A);
        if (a && !s_ctrl_a_prev) s_bird_vy = FLAP_VY;
        s_ctrl_a_prev = a;
    }

    int guard = 4;
    while (now - s_last_phys_step >= PHYS_STEP_MS && guard-- > 0) {
        s_last_phys_step += PHYS_STEP_MS;
        physics_step();
        if (s_state != FB_PLAYING) break;
    }
}

Screen flappy_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    flappy_create, flappy_draw, flappy_touch, flappy_tick, flappy_destroy, nullptr,
    0, true // idle_frame_ms, suppress_idle - never dim or lock while a game is active
};
