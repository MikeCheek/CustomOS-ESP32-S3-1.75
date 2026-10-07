#include "game_dodger.h"
#include "config.h"
#include "board_pins.h"
#include "hal_imu.h"
#include "hal_controller.h"
#include "ui.h"
#include "game_audio.h"
#include <Arduino_GFX_Library.h>

#define PLAYER_W 30
#define PLAYER_H 30
#define OBS_W 24
#define OBS_H 24
#define MAX_OBS 12

// Safe horizontal band + player row - the same 300px-wide, y<=405
// bounds already verified against this display's actual circle radius
// for game_breakout.cpp's arena and the media browser's list. Without
// this, the player (fixed near the bottom, where the circle's chord
// is much narrower than the full screen width) could slide into
// territory that's outside the visible display entirely.
#define PLAY_LEFT   83
#define PLAY_RIGHT  383
#define PLAYER_Y    (375) // bottom edge lands at 405, the verified-safe y bound
// Obstacles that fall past the player row are already "dodged" -
// despawn them there instead of letting them fall another 60px into
// the narrowing chord near the very bottom of the circle, where a
// still-visible obstacle would start clipping outside it.
#define OBS_DESPAWN_Y (PLAYER_Y + PLAYER_H + 20)

static int s_player_x;
static int s_player_y;
static int s_obs_x[MAX_OBS], s_obs_y[MAX_OBS];
static int s_obs_count;
static bool s_dodger_alive;
static bool s_dodger_started;
static int s_dodger_score;
static uint32_t s_last_spawn;
static uint32_t s_last_fall;
static int s_fall_speed;

static void dodger_create() {
    s_player_x = LCD_WIDTH / 2 - PLAYER_W / 2;
    s_player_y = PLAYER_Y;
    s_obs_count = 0;
    s_dodger_alive = true;
    s_dodger_started = false;
    s_dodger_score = 0;
    s_last_spawn = millis();
    s_last_fall = millis();
    s_fall_speed = 3;
}

static void dodger_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    if (!s_dodger_started) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 30, COLOR_TEXT, "DODGER", 4);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 10, COLOR_TEXT_DIM, "Tilt or tap to move", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 40, COLOR_TEXT_DIM, "Tap to start", 2);
        return;
    }

    g->fillRoundRect(s_player_x, s_player_y, PLAYER_W, PLAYER_H, 6, COLOR_GOOD);

    for (int i = 0; i < s_obs_count; i++) {
        g->fillRoundRect(s_obs_x[i], s_obs_y[i], OBS_W, OBS_H, 4, COLOR_BAD);
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_dodger_score);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    g->setCursor(10, 5);
    g->print(buf);

    if (!s_dodger_alive) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 20, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

static void dodger_touch(int x, int y, bool pressed) {
    if (!pressed) return;
    if (!s_dodger_started) { s_dodger_started = true; return; }
    if (!s_dodger_alive) { dodger_create(); s_dodger_started = true; return; }
    s_player_x = x - PLAYER_W / 2;
    if (s_player_x < PLAY_LEFT) s_player_x = PLAY_LEFT;
    if (s_player_x > PLAY_RIGHT - PLAYER_W) s_player_x = PLAY_RIGHT - PLAYER_W;
}

static void dodger_tick() {
    if (!s_dodger_started || !s_dodger_alive) return;

    ImuSample s = imu_read();
    if (s.valid) {
        float tilt_x, tilt_y;
        imu_get_calibrated_tilt(s, tilt_x, tilt_y);
        s_player_x += (int)(tilt_x * 8);
        if (s_player_x < PLAY_LEFT) s_player_x = PLAY_LEFT;
        if (s_player_x > PLAY_RIGHT - PLAYER_W) s_player_x = PLAY_RIGHT - PLAYER_W;
    }
    if (controller_connected()) {
        const int CTRL_SPEED_PX = 6; // per tick, continuous while held
        if (controller_dpad(CTRL_LEFT)) s_player_x -= CTRL_SPEED_PX;
        if (controller_dpad(CTRL_RIGHT)) s_player_x += CTRL_SPEED_PX;
        if (s_player_x < PLAY_LEFT) s_player_x = PLAY_LEFT;
        if (s_player_x > PLAY_RIGHT - PLAYER_W) s_player_x = PLAY_RIGHT - PLAYER_W;
    }

    uint32_t now = millis();
    if (now - s_last_fall >= 16) {
        s_last_fall = now;
        for (int i = s_obs_count - 1; i >= 0; i--) {
            s_obs_y[i] += s_fall_speed;
            if (s_obs_y[i] > OBS_DESPAWN_Y) {
                s_obs_x[i] = s_obs_x[s_obs_count - 1];
                s_obs_y[i] = s_obs_y[s_obs_count - 1];
                s_obs_count--;
                s_dodger_score++;
                game_sfx_score();
                if (s_fall_speed < 8) s_fall_speed++;
            }
        }
    }

    if (now - s_last_spawn >= 800 && s_obs_count < MAX_OBS) {
        s_last_spawn = now;
        s_obs_x[s_obs_count] = PLAY_LEFT + random(PLAY_RIGHT - PLAY_LEFT - OBS_W);
        s_obs_y[s_obs_count] = -OBS_H;
        s_obs_count++;
    }

    for (int i = 0; i < s_obs_count; i++) {
        if (s_obs_x[i] + OBS_W > s_player_x && s_obs_x[i] < s_player_x + PLAYER_W &&
            s_obs_y[i] + OBS_H > s_player_y && s_obs_y[i] < s_player_y + PLAYER_H) {
            s_dodger_alive = false;
            game_sfx_gameover();
        }
    }
}

Screen dodger_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    dodger_create, dodger_draw, dodger_touch, dodger_tick, nullptr, nullptr,
    0, true, true // idle_frame_ms, suppress_idle, needs_tilt_calibration - this game steers by tilt
};
