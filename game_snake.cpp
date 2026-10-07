#include "game_snake.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "hal_controller.h"
#include <Arduino_GFX_Library.h>

#define GRID 15
#define COLS (LCD_WIDTH / GRID)
#define ROWS (LCD_HEIGHT / GRID)
#define MAX_LEN 200

// Play radius: the rectangular COLS x ROWS grid includes corner cells
// that are well outside the visible circle (a corner is ~329px from
// center on this 233px-radius display - completely off the physical
// screen). Movement and food spawns are constrained to cells whose
// center falls within this radius instead, so nothing ever happens in
// invisible territory. Small margin below LCD_WIDTH/2 so a segment's
// full GRID-sized box doesn't clip the bezel either.
#define PLAY_R (LCD_WIDTH / 2.0f - GRID * 0.7f)

static bool cell_in_circle(int gx, int gy) {
    float cx = gx * GRID + GRID / 2.0f;
    float cy = gy * GRID + GRID / 2.0f;
    float dx = cx - LCD_WIDTH / 2.0f;
    float dy = cy - LCD_HEIGHT / 2.0f;
    return (dx * dx + dy * dy) <= PLAY_R * PLAY_R;
}

static int s_snake_x[MAX_LEN], s_snake_y[MAX_LEN];
static int s_len, s_dir;
static int s_food_x, s_food_y;
static bool s_alive;
static bool s_started;
static uint32_t s_last_move;
static int s_speed_ms;
static int s_score;

static void place_food() {
    for (int i = 0; i < 1000; i++) {
        int fx = random(COLS);
        int fy = random(ROWS);
        if (!cell_in_circle(fx, fy)) continue; // stay on the visible play area
        bool on_snake = false;
        for (int j = 0; j < s_len; j++) {
            if (s_snake_x[j] == fx && s_snake_y[j] == fy) { on_snake = true; break; }
        }
        if (!on_snake) { s_food_x = fx; s_food_y = fy; return; }
    }
}

static void snake_create() {
    s_len = 4;
    s_dir = 1;
    s_alive = true;
    s_started = false;
    s_score = 0;
    s_speed_ms = 150;
    s_last_move = millis();
    for (int i = 0; i < s_len; i++) { s_snake_x[i] = COLS / 2 - i; s_snake_y[i] = ROWS / 2; }
    place_food();
}

static void snake_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    // Play boundary - makes the actual (circular) death zone visible
    // instead of an invisible edge the player only discovers by dying.
    g->drawCircle(LCD_WIDTH / 2, LCD_HEIGHT / 2, (int)PLAY_R, COLOR_PANEL);

    if (!s_started) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 30, COLOR_TEXT, "SNAKE", 4);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 10, COLOR_TEXT_DIM, "Tap to start", 2);
        return;
    }

    for (int i = 0; i < s_len; i++) {
        g->fillRoundRect(s_snake_x[i] * GRID + 1, s_snake_y[i] * GRID + 1, GRID - 2, GRID - 2, 3,
                         i == 0 ? COLOR_GOOD : COLOR_TEXT);
    }
    g->fillRoundRect(s_food_x * GRID + 2, s_food_y * GRID + 2, GRID - 4, GRID - 4, 4, COLOR_BAD);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_score);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    g->setCursor(10, 5);
    g->print(buf);

    if (!s_alive) {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_BAD, "GAME OVER", 3);
        char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "Score: %d", s_score);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 20, COLOR_TEXT, sbuf, 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 50, COLOR_TEXT_DIM, "Tap to retry", 2);
    }
}

static void snake_touch(int x, int y, bool pressed) {
    if (!pressed) return;
    if (!s_started) { s_started = true; return; }
    if (!s_alive) { snake_create(); s_started = true; return; }

    int head_cx = s_snake_x[0] * GRID + GRID / 2;
    int head_cy = s_snake_y[0] * GRID + GRID / 2;
    int dx = x - head_cx;
    int dy = y - head_cy;
    if (abs(dx) > abs(dy)) {
        if (dx > 0 && s_dir != -1) s_dir = 1;
        else if (dx < 0 && s_dir != 1) s_dir = -1;
    } else {
        if (dy > 0 && s_dir != -ROWS) s_dir = ROWS;
        else if (dy < 0 && s_dir != ROWS) s_dir = -ROWS;
    }
}

static void snake_tick() {
    if (!s_started || !s_alive) return;
    if (controller_connected()) {
        if (controller_dpad(CTRL_RIGHT) && s_dir != -1) s_dir = 1;
        else if (controller_dpad(CTRL_LEFT) && s_dir != 1) s_dir = -1;
        else if (controller_dpad(CTRL_DOWN) && s_dir != -ROWS) s_dir = ROWS;
        else if (controller_dpad(CTRL_UP) && s_dir != ROWS) s_dir = -ROWS;
    }
    uint32_t now = millis();
    if (now - s_last_move < (uint32_t)s_speed_ms) return;
    s_last_move = now;

    int nx = s_snake_x[0] + (s_dir == 1 ? 1 : s_dir == -1 ? -1 : 0);
    int ny = s_snake_y[0] + (s_dir == ROWS ? 1 : s_dir == -ROWS ? -1 : 0);

    if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS || !cell_in_circle(nx, ny)) { s_alive = false; return; }
    for (int i = 0; i < s_len - 1; i++) {
        if (s_snake_x[i] == nx && s_snake_y[i] == ny) { s_alive = false; return; }
    }

    if (nx == s_food_x && ny == s_food_y) {
        if (s_len < MAX_LEN) s_len++;
        s_score += 10;
        if (s_speed_ms > 60) s_speed_ms -= 3;
        place_food();
    }

    for (int i = s_len - 1; i > 0; i--) { s_snake_x[i] = s_snake_x[i - 1]; s_snake_y[i] = s_snake_y[i - 1]; }
    s_snake_x[0] = nx;
    s_snake_y[0] = ny;
}

Screen snake_screen = {
nullptr, GESTURE_MODE_FREE,
    UI_FRAME_MS_GAME,
    snake_create, snake_draw, snake_touch, snake_tick, nullptr, nullptr,
    0, true // idle_frame_ms, suppress_idle - never dim or lock while a game is active
};
