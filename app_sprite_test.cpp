/*
 * app_sprite_test.cpp
 * Phase 1 of the "can this hardware run a real sprite-based dungeon
 * game" testing plan - NOT using any real Ninja Adventure assets
 * (haven't been able to get them into this environment yet, see the
 * chat for why), just a small procedurally-generated placeholder
 * sprite (a rough humanoid silhouette) used purely to measure real
 * blit performance with Arduino_GFX's actual transparent-bitmap
 * primitive on this exact display/bus.
 *
 * What this measures, and why it's the right first question to
 * answer before writing a single line of game logic: every game in
 * this project so far draws flat-color shapes (fillCircle, fillRect,
 * etc.), which this display's driver handles cheaply. Blitting many
 * small RGB565 bitmaps with per-pixel transparency is a genuinely
 * different, heavier workload, and nothing in this codebase has ever
 * measured it. If a dungeon scene needs a moving player, a few
 * enemies, and NPCs on screen at once, we need to know NOW how many
 * simultaneous sprites this MCU can actually blit per frame - not
 * assume it'll be fine because the vector-drawn games were.
 *
 * Reports:
 *  - Live FPS while continuously blitting a configurable number of
 *    moving, animated (2-frame) sprites with color-key transparency.
 *  - The blit-only time for the last frame's batch, in microseconds,
 *    so the cost can be read off directly rather than inferred from
 *    FPS alone (which also includes the double-buffer flush).
 *  - Tap to step through sprite counts (10 / 30 / 60 / 100) to see
 *    how the cost scales - the actual number that matters for
 *    deciding how busy a real dungeon room's rendering could be.
 */
#include "app_sprite_test.h"
#include "config.h"
#include "board_pins.h"
#include <Arduino_GFX_Library.h>
#include <math.h>

#define SPR_W 16
#define SPR_H 16
#define TRANS_KEY ((uint16_t)0xF81F) // magenta - chosen as unlikely to appear in real sprite art either

// Two frames (idle / step) so the "animated" part of the cost is
// measured too, not just a single static blit repeated.
static uint16_t *s_frame[2] = { nullptr, nullptr };

static const int COUNT_OPTIONS[4] = { 10, 30, 60, 100 };
static int s_count_idx = 1; // start at 30

#define MAX_SPRITES 100
struct TestSprite { float x, y, vx, vy; };
static TestSprite s_sprites[MAX_SPRITES];

static uint32_t s_last_tick_ms;
static int s_frame_count;
static float s_fps;
static uint32_t s_last_blit_us;
static int s_anim_frame;
static uint32_t s_last_anim_ms;

static void generate_frame(uint16_t *buf, bool step_pose) {
    for (int y = 0; y < SPR_H; y++) {
        for (int x = 0; x < SPR_W; x++) {
            uint16_t c = TRANS_KEY;
            int dx = x - 8, dy = y - 4;
            if (dx * dx + dy * dy <= 9) {
                c = (uint16_t)COLOR565(0xE0, 0xC0, 0x90); // head
            } else if (y >= 7 && y <= 11 && x >= 4 && x <= 11) {
                c = (uint16_t)COLOR565(0x30, 0x70, 0x30); // body/tunic
            } else if (y >= 12 && y <= 15) {
                // legs - offset in the "step" pose so the animation is
                // visibly (and computationally) a real two-frame cycle
                int off = step_pose ? 1 : 0;
                if (x == 5 - off || x == 6 - off || x == 9 + off || x == 10 + off) {
                    c = (uint16_t)COLOR565(0x20, 0x20, 0x20);
                }
            }
            buf[y * SPR_W + x] = c;
        }
    }
}

static void reset_sprites() {
    int count = COUNT_OPTIONS[s_count_idx];
    for (int i = 0; i < count; i++) {
        s_sprites[i].x = 90.0f + (float)random(280);
        s_sprites[i].y = 100.0f + (float)random(260);
        float ang = (float)random(628) / 100.0f;
        s_sprites[i].vx = cosf(ang) * 40.0f;
        s_sprites[i].vy = sinf(ang) * 40.0f;
    }
}

static void sprite_test_create() {
    if (!s_frame[0]) s_frame[0] = (uint16_t *)ps_malloc(SPR_W * SPR_H * sizeof(uint16_t));
    if (!s_frame[1]) s_frame[1] = (uint16_t *)ps_malloc(SPR_W * SPR_H * sizeof(uint16_t));
    if (s_frame[0]) generate_frame(s_frame[0], false);
    if (s_frame[1]) generate_frame(s_frame[1], true);
    reset_sprites();
    s_last_tick_ms = millis();
    s_frame_count = 0;
    s_fps = 0.0f;
    s_last_blit_us = 0;
    s_anim_frame = 0;
    s_last_anim_ms = millis();
}

static void sprite_test_destroy() {}

static void sprite_test_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g || !s_frame[0] || !s_frame[1]) return;

    int count = COUNT_OPTIONS[s_count_idx];

    uint32_t t0 = micros();
    for (int i = 0; i < count; i++) {
        int px = (int)s_sprites[i].x - SPR_W / 2;
        int py = (int)s_sprites[i].y - SPR_H / 2;
        g->draw16bitRGBBitmapWithTranColor(px, py, s_frame[s_anim_frame], TRANS_KEY, SPR_W, SPR_H);
    }
    s_last_blit_us = micros() - t0;

    char buf[32];
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT);
    snprintf(buf, sizeof(buf), "Sprites: %d", count);
    g->setCursor(140, 40);
    g->print(buf);

    snprintf(buf, sizeof(buf), "FPS: %.1f", s_fps);
    g->setCursor(140, 65);
    g->print(buf);

    snprintf(buf, sizeof(buf), "Blit: %lu us", (unsigned long)s_last_blit_us);
    g->setCursor(140, 90);
    g->print(buf);

    ui_draw_centered_text(LCD_HEIGHT - 50, COLOR_TEXT_DIM, "Tap to change sprite count", 1);
}

static void sprite_test_touch(int x, int y, bool pressed) {
    (void)x; (void)y;
    static bool s_prev_pressed = false;
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;
    s_count_idx = (s_count_idx + 1) % 4;
    reset_sprites();
}

static void sprite_test_tick() {
    uint32_t now = millis();
    float dt = (float)(now - s_last_tick_ms) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    s_last_tick_ms = now;

    int count = COUNT_OPTIONS[s_count_idx];
    for (int i = 0; i < count; i++) {
        s_sprites[i].x += s_sprites[i].vx * dt;
        s_sprites[i].y += s_sprites[i].vy * dt;
        if (s_sprites[i].x < 90 || s_sprites[i].x > 370) s_sprites[i].vx = -s_sprites[i].vx;
        if (s_sprites[i].y < 100 || s_sprites[i].y > 360) s_sprites[i].vy = -s_sprites[i].vy;
    }

    if (now - s_last_anim_ms >= 300) {
        s_last_anim_ms = now;
        s_anim_frame = 1 - s_anim_frame;
    }

    s_frame_count++;
    static uint32_t s_fps_window_start = 0;
    if (s_fps_window_start == 0) s_fps_window_start = now;
    if (now - s_fps_window_start >= 1000) {
        s_fps = (float)s_frame_count * 1000.0f / (float)(now - s_fps_window_start);
        s_frame_count = 0;
        s_fps_window_start = now;
    }
}

Screen sprite_test_screen = {
    "Sprite Test", GESTURE_MODE_EDGE,
    UI_FRAME_MS_GAME,
    sprite_test_create, sprite_test_draw, sprite_test_touch, sprite_test_tick, sprite_test_destroy, nullptr
};
