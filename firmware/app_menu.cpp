#include "app_menu.h"
#include "board_pins.h"
#include "config.h"
#include "ui.h"
#include "hal_controller.h"
#include "hal_sleep.h"
#include "app_quicksettings.h"
#include "hal_touch.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "ui_font.h"
#include <string.h>
#include <esp_heap_caps.h>
#include "app_settings_state.h"
#include "app_clock.h"
#include "app_weather.h"
#include "app_reminders.h"
#include "app_workout.h"
#include "app_life.h"
#include "app_tips.h"

enum MenuCategory { CAT_GAMES, CAT_TOOLS, CAT_TESTS, CAT_COUNT };

struct MenuEntry {
    const char *label;
    MenuCategory category;
    void (*icon_fn)(Arduino_GFX *g, int cx, int cy, float br);
    Screen *screen;
    bool dev = false;   // only with Settings > System > Developer mode
    uint16_t disc = 0;  // the icon's disc colour (0: taken from its drawing)
    int8_t folder = -1; // >= 0: this is a folder holding that category (games, developer tools)
};

extern Screen settings_screen;
extern Screen assistant_screen;
extern Screen sleep_screen;
extern Screen draw_screen;
extern Screen activity_screen;
extern Screen notifications_screen;
extern Screen ble_status_screen;
extern Screen battery_screen;
extern Screen espnow_status_screen;
extern Screen media_screen;
extern Screen gallery_screen;
extern Screen music_screen;
extern Screen contacts_screen;
extern Screen files_screen;
extern Screen test_menu_screen;
extern Screen calibration_screen;
extern Screen snake_screen;
extern Screen reaction_screen;
extern Screen dodger_screen;
extern Screen simon_screen;
extern Screen plane_screen;
extern Screen recorder_screen;
extern Screen breakout_screen;
extern Screen pet_screen;
extern Screen flappy_screen;
extern Screen piano_screen;
extern Screen fruitninja_screen;
extern Screen runner_screen;
extern Screen maze_screen;
extern Screen sprite_test_screen;
extern Screen ninjadungeon_screen;
extern Screen blackjack_screen;
extern Screen cavern_screen;
extern Screen bambu_printer_screen;
extern Screen phone_screen;
extern Screen calendar_screen;
extern Screen navigation_screen;

// ---- Custom icon drawing functions ----
//
// Every icon takes a `br` (brightness, 0..1) so it can be drawn
// dimmed/"low opacity" until focused. There's no real alpha
// compositing available on this display path, so "opacity" is faked
// by blending each color toward the tile's own panel background
// (blend565) rather than toward black - that's what makes a dimmed
// icon look like a washed-out version of itself sitting on its tile,
// instead of just looking dark.
static uint16_t blend565(uint16_t fg, uint16_t bg, float alpha) {
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    uint8_t fr = (fg >> 11) & 0x1F, fgg = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    uint8_t br_ = (bg >> 11) & 0x1F, bgg = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    uint8_t r = (uint8_t)(fr * alpha + br_ * (1.0f - alpha));
    uint8_t g_ = (uint8_t)(fgg * alpha + bgg * (1.0f - alpha));
    uint8_t b = (uint8_t)(fb * alpha + bb * (1.0f - alpha));
    return (uint16_t)((r << 11) | (g_ << 5) | b);
}
#define IC(color) blend565((uint16_t)(color), COLOR_PANEL, br)

static void icon_settings(Arduino_GFX *g, int cx, int cy, float br) {
    // Gear: circle with teeth
    g->fillCircle(cx, cy, 20, IC(COLOR_ACCENT));
    g->fillCircle(cx, cy, 12, IC(COLOR_BG));
    for (int a = 0; a < 360; a += 45) {
        float r = a * 3.14159f / 180.0f;
        int tx = cx + (int)(cosf(r) * 18);
        int ty = cy + (int)(sinf(r) * 18);
        g->fillCircle(tx, ty, 5, IC(COLOR_ACCENT));
    }
}

static void icon_snake(Arduino_GFX *g, int cx, int cy, float br) {
    // Snake body
    uint16_t c = IC(COLOR_GOOD);
    int sx[] = {-12, -4, 4, 12};
    int sy[] = {6, 6, 6, 0};
    for (int i = 0; i < 4; i++) {
        g->fillRoundRect(cx + sx[i] - 4, cy + sy[i] - 4, 8, 8, 2, c);
    }
    g->fillCircle(cx + 14, cy - 2, 3, IC(COLOR_TEXT)); // eye
}

static void icon_reaction(Arduino_GFX *g, int cx, int cy, float br) {
    // Lightning bolt
    g->fillTriangle(cx - 2, cy - 18, cx + 10, cy - 4, cx + 2, cy - 4, IC(COLOR_ACCENT3));
    g->fillTriangle(cx + 2, cy - 4, cx - 10, cy + 18, cx - 2, cy + 4, IC(COLOR_ACCENT3));
}

static void icon_dodger(Arduino_GFX *g, int cx, int cy, float br) {
    // Shield shape
    g->fillTriangle(cx, cy - 18, cx + 16, cy - 6, cx - 16, cy - 6, IC(COLOR_BAD));
    g->fillRoundRect(cx - 16, cy - 6, 32, 20, 0, IC(COLOR_BAD));
    g->fillTriangle(cx, cy + 18, cx + 16, cy + 4, cx - 16, cy + 4, IC(COLOR_BAD));
}

static void icon_simon(Arduino_GFX *g, int cx, int cy, float br) {
    // Diamond
    g->fillTriangle(cx, cy - 16, cx + 14, cy, cx - 14, cy, IC(COLOR_ACCENT));
    g->fillTriangle(cx, cy + 16, cx + 14, cy, cx - 14, cy, IC(COLOR_ACCENT2));
}

static void icon_piano(Arduino_GFX *g, int cx, int cy, float br) {
    // Piano keys: four white keys, three black ones on top
    for (int k = 0; k < 4; k++) g->fillRoundRect(cx - 21 + k * 11, cy - 16, 10, 32, 2, IC(COLOR_TEXT));
    for (int k = 0; k < 3; k++) g->fillRect(cx - 14 + k * 11, cy - 16, 6, 19, IC(COLOR_BG));
    g->fillCircle(cx + 15, cy - 12, 4, IC(COLOR_ACCENT));
}

static void icon_plane(Arduino_GFX *g, int cx, int cy, float br) {
    // Paper-plane silhouette: nose + swept wings
    g->fillTriangle(cx, cy - 18, cx - 14, cy + 14, cx + 14, cy + 14, IC(COLOR_GOOD));
    g->fillTriangle(cx - 22, cy + 12, cx, cy - 2, cx - 12, cy + 16, IC(COLOR_GOOD));
    g->fillTriangle(cx + 22, cy + 12, cx, cy - 2, cx + 12, cy + 16, IC(COLOR_GOOD));
}

static void icon_breakout(Arduino_GFX *g, int cx, int cy, float br) {
    // A row of colored bricks, a paddle, and a ball
    g->fillRoundRect(cx - 22, cy - 18, 14, 8, 2, IC(COLOR_GOOD));
    g->fillRoundRect(cx - 6, cy - 18, 14, 8, 2, IC(COLOR_WARN));
    g->fillRoundRect(cx + 10, cy - 18, 14, 8, 2, IC(COLOR_BAD));
    g->fillCircle(cx - 4, cy + 4, 5, IC(COLOR_TEXT));
    g->fillRoundRect(cx - 16, cy + 16, 32, 7, 3, IC(COLOR_ACCENT));
}

static void icon_blackjack(Arduino_GFX *g, int cx, int cy, float br) {
    // Two overlapping playing cards, a small red diamond pip on each
    uint16_t card_c = IC(COLOR565(0xF0, 0xF0, 0xE8));
    uint16_t pip_c = IC(COLOR565(0xE0, 0x30, 0x30));
    g->fillRoundRect(cx - 16, cy - 14, 20, 28, 3, IC(COLOR_TEXT_DIM));
    g->fillRoundRect(cx - 4, cy - 10, 20, 28, 3, card_c);
    g->fillTriangle(cx + 6, cy - 2, cx + 2, cy + 4, cx + 10, cy + 4, pip_c);
    g->fillTriangle(cx + 6, cy + 10, cx + 2, cy + 4, cx + 10, cy + 4, pip_c);
}

static void icon_cavern(Arduino_GFX *g, int cx, int cy, float br) {
    // A small cluster of colored crystals - a circle, a square and a triangle
    g->fillCircle(cx - 12, cy + 6, 9, IC(COLOR565(0x50, 0x90, 0xFF)));
    g->fillRect(cx + 2, cy - 2, 16, 16, IC(COLOR565(0xFF, 0xD8, 0x30)));
    g->fillTriangle(cx - 2, cy - 16, cx - 12, cy - 2, cx + 8, cy - 2, IC(COLOR565(0xB0, 0x60, 0xF0)));
}

static void icon_pet(Arduino_GFX *g, int cx, int cy, float br) {
    // A small friendly face - round head, two ears, two eyes
    uint16_t body = IC(COLOR565(0xFF, 0xB8, 0x6B));
    g->fillCircle(cx, cy, 16, body);
    g->fillTriangle(cx - 12, cy - 10, cx - 4, cy - 20, cx - 2, cy - 6, body);
    g->fillTriangle(cx + 12, cy - 10, cx + 4, cy - 20, cx + 2, cy - 6, body);
    g->fillCircle(cx - 6, cy - 1, 2, IC(COLOR_TEXT));
    g->fillCircle(cx + 6, cy - 1, 2, IC(COLOR_TEXT));
    g->fillTriangle(cx - 3, cy + 6, cx + 3, cy + 6, cx, cy + 10, IC(COLOR565(0x66, 0x33, 0x11)));
}

static void icon_flappy(Arduino_GFX *g, int cx, int cy, float br) {
    // A little bird between two green pipe gaps
    uint16_t pipe = IC(COLOR_GOOD);
    g->fillRoundRect(cx - 24, cy - 22, 10, 16, 2, pipe);
    g->fillRoundRect(cx - 24, cy + 8, 10, 16, 2, pipe);
    g->fillRoundRect(cx + 14, cy - 14, 10, 16, 2, pipe);
    g->fillRoundRect(cx + 14, cy + 12, 10, 8, 2, pipe);
    g->fillCircle(cx, cy, 10, IC(COLOR_WARN));
    g->fillTriangle(cx + 8, cy, cx + 16, cy - 3, cx + 16, cy + 3, IC(COLOR_BAD));
}

static void icon_fruitninja(Arduino_GFX *g, int cx, int cy, float br) {
    // Two sliced fruit halves with a diagonal slice line
    g->fillCircle(cx - 8, cy - 4, 11, IC(COLOR_BAD));
    g->fillCircle(cx + 9, cy + 6, 9, IC(COLOR_WARN));
    uint16_t line = IC(COLOR_TEXT);
    g->drawLine(cx - 22, cy + 16, cx + 22, cy - 16, line);
    g->drawLine(cx - 21, cy + 17, cx + 23, cy - 15, line);
}

static void icon_runner(Arduino_GFX *g, int cx, int cy, float br) {
    // A small runner figure over 3 converging lane lines
    uint16_t lane = IC(COLOR_PANEL);
    g->drawLine(cx - 20, cy + 18, cx - 4, cy - 20, lane);
    g->drawLine(cx, cy + 18, cx, cy - 20, lane);
    g->drawLine(cx + 20, cy + 18, cx + 4, cy - 20, lane);
    uint16_t body = IC(COLOR_GOOD);
    g->fillRoundRect(cx - 7, cy - 6, 14, 14, 5, body);
    g->fillCircle(cx, cy - 12, 6, body);
}

static void icon_maze(Arduino_GFX *g, int cx, int cy, float br) {
    // Converging corridor walls with a small enemy silhouette ahead
    g->fillTriangle(cx - 22, cy + 18, cx - 6, cy + 18, cx - 2, cy - 4, IC(COLOR_ACCENT));
    g->fillTriangle(cx + 22, cy + 18, cx + 6, cy + 18, cx + 2, cy - 4, IC(COLOR_ACCENT));
    g->fillRoundRect(cx - 5, cy - 2, 10, 14, 3, IC(COLOR_BAD));
}

static void icon_recorder(Arduino_GFX *g, int cx, int cy, float br) {
    // Microphone
    uint16_t c = IC(COLOR_BAD);
    g->fillRoundRect(cx - 6, cy - 16, 12, 22, 6, c);
    g->drawRoundRect(cx - 12, cy + 4, 24, 12, 6, c);
    g->fillRoundRect(cx - 1, cy + 16, 2, 8, 1, c);
    g->fillRoundRect(cx - 8, cy + 22, 16, 3, 1, c);
}

static void icon_notifications(Arduino_GFX *g, int cx, int cy, float br) {
    // Bell
    uint16_t c = IC(COLOR_ACCENT2);
    g->fillRoundRect(cx - 12, cy - 14, 24, 20, 10, c);
    g->fillCircle(cx, cy + 10, 4, c);
    g->fillCircle(cx, cy + 10, 2, IC(COLOR_BG));
}

static void icon_ble(Arduino_GFX *g, int cx, int cy, float br) {
    // Bluetooth rune
    uint16_t c = IC(COLOR_ACCENT);
    g->fillRoundRect(cx - 3, cy - 16, 6, 32, 2, c);
    g->fillTriangle(cx - 10, cy + 4, cx, cy - 10, cx, cy + 2, c);
    g->fillTriangle(cx + 10, cy - 4, cx, cy + 10, cx, cy - 2, c);
    g->fillCircle(cx, cy - 14, 3, c);
    g->fillCircle(cx, cy + 14, 3, c);
}

static void icon_battery(Arduino_GFX *g, int cx, int cy, float br) {
    uint16_t c = IC(COLOR_GOOD);
    g->drawRoundRect(cx - 16, cy - 10, 32, 20, 4, c);
    g->fillRect(cx + 16, cy - 4, 4, 8, c);
    g->fillRect(cx - 12, cy - 6, 20, 12, c);
}


static void icon_espnow(Arduino_GFX *g, int cx, int cy, float br) {
    // A small joystick base + stick, with radiating "wireless" arcs
    uint16_t c = IC(COLOR_ACCENT2);
    g->fillRoundRect(cx - 14, cy + 6, 28, 10, 4, c);
    g->fillRoundRect(cx - 3, cy - 10, 6, 18, 2, c);
    g->fillCircle(cx, cy - 12, 6, c);
    g->drawCircle(cx, cy - 12, 16, IC(COLOR_TEXT_DIM));
    g->drawCircle(cx, cy - 12, 24, IC(COLOR_TEXT_DIM));
}

static void icon_bambu(Arduino_GFX *g, int cx, int cy, float br) {
    // A simple gantry-style 3D printer frame with a small printed
    // object on the bed - fillRect/fillTriangle only, no drawRect
    // outline (see this project's established note elsewhere on why).
    uint16_t frame_c = IC(COLOR_TEXT_DIM);
    g->fillRect(cx - 16, cy - 18, 32, 6, frame_c);   // top bar
    g->fillRect(cx - 16, cy - 18, 5, 30, frame_c);   // left rail
    g->fillRect(cx + 11, cy - 18, 5, 30, frame_c);   // right rail
    g->fillRect(cx - 16, cy + 10, 32, 6, frame_c);   // bed
    g->fillTriangle(cx, cy - 2, cx - 8, cy + 8, cx + 8, cy + 8, IC(COLOR_ACCENT2)); // printed object
}

static void icon_media(Arduino_GFX *g, int cx, int cy, float br) {
    // Play note
    uint16_t c = IC(COLOR_ACCENT2);
    g->fillCircle(cx - 4, cy + 10, 6, c);
    g->fillRoundRect(cx + 2, cy - 14, 4, 28, 2, c);
    g->fillTriangle(cx + 6, cy - 14, cx + 18, cy - 10, cx + 6, cy - 6, c);
}

static void icon_gallery(Arduino_GFX *g, int cx, int cy, float br) {
    // Photo: frame, sun and two hills
    uint16_t c = IC(COLOR_GOOD);
    g->fillRoundRect(cx - 18, cy - 14, 36, 28, 5, c);
    g->fillRoundRect(cx - 15, cy - 11, 30, 22, 3, IC(COLOR_BG));
    g->fillTriangle(cx - 13, cy + 9, cx - 3, cy - 4, cx + 7, cy + 9, c);
    g->fillTriangle(cx + 1, cy + 9, cx + 7, cy + 1, cx + 13, cy + 9, c);
    g->fillCircle(cx + 8, cy - 5, 3, IC(COLOR_WARN));
}

static void icon_music(Arduino_GFX *g, int cx, int cy, float br) {
    // Two beamed notes
    uint16_t c = IC(COLOR_ACCENT);
    g->fillCircle(cx - 9, cy + 11, 6, c);
    g->fillCircle(cx + 9, cy + 8, 6, c);
    g->fillRect(cx - 5, cy - 12, 4, 23, c);
    g->fillRect(cx + 13, cy - 15, 4, 23, c);
    g->fillTriangle(cx - 5, cy - 12, cx + 17, cy - 15, cx + 17, cy - 9, c);
    g->fillTriangle(cx - 5, cy - 12, cx + 17, cy - 9, cx - 5, cy - 6, c);
}

static void icon_assistant(Arduino_GFX *g, int cx, int cy, float br) {
    // A speech bubble with a sound wave
    uint16_t c = IC(COLOR_ACCENT);
    g->fillRoundRect(cx - 20, cy - 16, 40, 28, 12, c);
    g->fillTriangle(cx - 10, cy + 10, cx - 2, cy + 10, cx - 14, cy + 20, c);
    uint16_t w = IC(COLOR_BG);
    static const int8_t H[5] = { 4, 10, 16, 10, 4 };
    for (int k = 0; k < 5; k++) g->fillRoundRect(cx - 12 + k * 6, cy - 2 - H[k] / 2, 3, H[k], 1, w);
}

static void icon_sleep(Arduino_GFX *g, int cx, int cy, float br) {
    // a crescent moon and a small star
    g->fillCircle(cx - 2, cy, 18, IC(COLOR_ACCENT2));
    g->fillCircle(cx + 7, cy - 6, 15, IC(COLOR_PANEL));
    g->fillCircle(cx + 14, cy + 10, 3, IC(COLOR_TEXT));
}

static void icon_draw(Arduino_GFX *g, int cx, int cy, float br) {
    // a hand-drawn "A" stroke with the pen dot at its end
    static const int8_t P[][2] = {{-16, 16}, {-8, -2}, {0, -16}, {8, -2}, {16, 16}};
    for (int i = 0; i + 1 < 5; i++)
        for (int k = 0; k <= 6; k++)
            g->fillCircle(cx + P[i][0] + (P[i + 1][0] - P[i][0]) * k / 6, cy + P[i][1] + (P[i + 1][1] - P[i][1]) * k / 6, 3,
                          IC(COLOR_ACCENT2));
    g->fillCircle(cx + 16, cy + 16, 5, IC(COLOR_TEXT));
}

static void icon_activity(Arduino_GFX *g, int cx, int cy, float br) {
    // a running figure: head, body, arms and legs as dots
    uint16_t c = IC(COLOR_ACCENT3);
    g->fillCircle(cx + 4, cy - 16, 5, c);
    static const int8_t P[][2] = {{2, -9}, {0, -3}, {-2, 3}, {-8, 10}, {-14, 16}, {4, 8}, {10, 16}, {-8, -6}, {10, -4}};
    for (auto &p : P) g->fillCircle(cx + p[0], cy + p[1], 3, c);
}

static void icon_phone(Arduino_GFX *g, int cx, int cy, float br) {
    // Smartphone with a link wave
    uint16_t c = IC(COLOR_ACCENT2);
    g->fillRoundRect(cx - 11, cy - 20, 22, 40, 5, c);
    g->fillRoundRect(cx - 8, cy - 15, 16, 28, 2, IC(COLOR_PANEL));
    g->fillCircle(cx, cy + 16, 2, IC(COLOR_PANEL));
    uint16_t w = IC(COLOR_GOOD);
    g->fillRect(cx + 15, cy - 6, 3, 12, w);
    g->fillRect(cx + 21, cy - 10, 3, 20, w);
}

static void icon_calendar(Arduino_GFX *g, int cx, int cy, float br) {
    // Calendar page with a red header and two rings
    g->fillRoundRect(cx - 18, cy - 16, 36, 34, 6, IC(COLOR_TEXT));
    g->fillRoundRect(cx - 18, cy - 16, 36, 12, 5, IC(COLOR_BAD));
    g->fillRect(cx - 18, cy - 9, 36, 5, IC(COLOR_BAD));
    g->fillRoundRect(cx - 11, cy - 21, 5, 10, 2, IC(COLOR_TEXT_DIM));
    g->fillRoundRect(cx + 6, cy - 21, 5, 10, 2, IC(COLOR_TEXT_DIM));
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 3; c++)
            g->fillRect(cx - 12 + c * 9, cy + 1 + r * 8, 6, 5, IC(COLOR_PANEL));
}

static void icon_navigation(Arduino_GFX *g, int cx, int cy, float br) {
    // Turn arrow on a green sign
    g->fillRoundRect(cx - 20, cy - 20, 40, 40, 10, IC(COLOR_GOOD));
    uint16_t c = IC(COLOR_BG);
    g->fillRect(cx - 7, cy - 2, 7, 16, c);
    g->fillRect(cx - 7, cy - 2, 14, 7, c);
    g->fillTriangle(cx + 15, cy + 1, cx + 5, cy - 9, cx + 5, cy + 11, c);
}

static void icon_contacts(Arduino_GFX *g, int cx, int cy, float br) {
    // Person
    uint16_t c = IC(COLOR_ACCENT);
    g->fillCircle(cx, cy - 8, 9, c);
    g->fillRoundRect(cx - 14, cy + 4, 28, 16, 8, c);
}

static void icon_files(Arduino_GFX *g, int cx, int cy, float br) {
    // Folder
    uint16_t c = IC(COLOR_ACCENT2);
    g->fillRoundRect(cx - 16, cy - 6, 32, 22, 4, c);
    g->fillRoundRect(cx - 16, cy - 12, 16, 10, 4, c);
    g->fillRoundRect(cx - 12, cy, 24, 12, 2, IC(COLOR_BG));
}

static void icon_tests(Arduino_GFX *g, int cx, int cy, float br) {
    // Wrench
    uint16_t c = IC(COLOR_ACCENT2);
    g->fillRoundRect(cx - 3, cy - 16, 6, 28, 2, c);
    g->fillCircle(cx, cy + 10, 8, c);
    g->fillCircle(cx, cy + 10, 4, IC(COLOR_BG));
}

static void icon_calibration(Arduino_GFX *g, int cx, int cy, float br) {
    // Crosshair/target
    uint16_t c = IC(COLOR_ACCENT2);
    g->drawCircle(cx, cy, 18, c);
    g->drawCircle(cx, cy, 10, c);
    g->fillCircle(cx, cy, 3, IC(COLOR_TEXT));
    g->fillRoundRect(cx - 2, cy - 26, 4, 10, 1, c);
    g->fillRoundRect(cx - 2, cy + 16, 4, 10, 1, c);
    g->fillRoundRect(cx - 26, cy - 2, 10, 4, 1, c);
    g->fillRoundRect(cx + 16, cy - 2, 10, 4, 1, c);
}

static void icon_sprite_test(Arduino_GFX *g, int cx, int cy, float br) {
    // A little placeholder figure over a checkerboard - a stand-in for
    // "testing bitmap sprites", not a real game icon
    uint16_t c1 = IC(COLOR_PANEL), c2 = IC(COLOR_TEXT_DIM);
    g->fillRect(cx - 16, cy - 16, 8, 8, c1);
    g->fillRect(cx - 8, cy - 16, 8, 8, c2);
    g->fillRect(cx - 16, cy - 8, 8, 8, c2);
    g->fillRect(cx - 8, cy - 8, 8, 8, c1);
    g->fillCircle(cx + 8, cy - 4, 5, IC(COLOR_WARN));
    g->fillRoundRect(cx + 3, cy + 2, 10, 10, 3, IC(COLOR_GOOD));
}

static void icon_ninjadungeon(Arduino_GFX *g, int cx, int cy, float br) {
    // A hooded ninja silhouette over a stone-block dungeon wall
    uint16_t wall = IC(COLOR565(0x55, 0x55, 0x60));
    g->fillRect(cx - 22, cy - 20, 44, 14, wall);
    g->fillRect(cx - 22, cy + 6, 44, 14, wall);
    uint16_t mortar = IC(COLOR565(0x22, 0x22, 0x28));
    g->fillRect(cx - 8, cy - 20, 1, 14, mortar); // fillRect, not drawFastVLine - broken on CO5300
    g->fillRect(cx + 10, cy - 6, 1, 14, mortar);
    uint16_t body = IC(COLOR565(0x2E, 0x6B, 0x3A));
    g->fillCircle(cx, cy - 2, 11, body);
    g->fillRect(cx - 3, cy - 6, 6, 3, IC(COLOR_TEXT));
}

static void icon_timer(Arduino_GFX *g, int cx, int cy, float br) {
    // a stopwatch-style dial with a quarter swept
    uint16_t c = IC(COLOR_WARN);
    g->fillCircle(cx, cy + 3, 19, c);
    g->fillCircle(cx, cy + 3, 15, IC(COLOR_PANEL));
    g->fillTriangle(cx, cy + 3, cx, cy - 12, cx + 15, cy + 3, c);
    g->fillRoundRect(cx - 5, cy - 22, 10, 5, 2, c);
}

static void icon_stopwatch(Arduino_GFX *g, int cx, int cy, float br) {
    uint16_t c = IC(COLOR_WARN);
    g->fillCircle(cx, cy + 3, 19, c);
    g->fillCircle(cx, cy + 3, 15, IC(COLOR_PANEL));
    g->drawLine(cx, cy + 3, cx + 8, cy - 7, c);
    g->drawLine(cx + 1, cy + 3, cx + 9, cy - 7, c);
    g->fillCircle(cx, cy + 3, 3, c);
    g->fillRoundRect(cx - 4, cy - 22, 8, 6, 2, c);
    g->fillRoundRect(cx + 13, cy - 17, 6, 6, 2, c);
}

static void icon_alarm(Arduino_GFX *g, int cx, int cy, float br) {
    // an alarm clock: two bells on top, legs
    uint16_t c = IC(COLOR_WARN);
    g->fillCircle(cx - 14, cy - 15, 7, c);
    g->fillCircle(cx + 14, cy - 15, 7, c);
    g->fillCircle(cx, cy + 2, 18, c);
    g->fillCircle(cx, cy + 2, 14, IC(COLOR_PANEL));
    g->drawLine(cx, cy + 2, cx, cy - 8, c);
    g->drawLine(cx, cy + 2, cx + 7, cy + 6, c);
    g->drawLine(cx - 12, cy + 17, cx - 16, cy + 22, c);
    g->drawLine(cx + 12, cy + 17, cx + 16, cy + 22, c);
}

static void icon_worldclock(Arduino_GFX *g, int cx, int cy, float br) {
    // a globe: meridians and the equator
    uint16_t c = IC(COLOR_ACCENT2);
    g->fillCircle(cx, cy, 20, c);
    g->fillCircle(cx, cy, 17, IC(COLOR_PANEL));
    g->drawFastHLine(cx - 17, cy, 34, c);
    g->drawFastHLine(cx - 14, cy - 9, 28, c);
    g->drawFastHLine(cx - 14, cy + 9, 28, c);
    g->drawFastVLine(cx, cy - 17, 34, c);
    g->drawEllipse(cx, cy, 8, 17, c);
}

static void icon_weather(Arduino_GFX *g, int cx, int cy, float br) {
    // the sun behind a cloud
    g->fillCircle(cx - 6, cy - 7, 11, IC(COLOR_WARN));
    uint16_t c = IC(COLOR_TEXT);
    g->fillCircle(cx - 8, cy + 6, 8, c);
    g->fillCircle(cx + 3, cy + 1, 10, c);
    g->fillCircle(cx + 12, cy + 7, 7, c);
    g->fillRoundRect(cx - 15, cy + 6, 33, 9, 4, c);
    // the disc colour comes from the saturated parts: make it a sky blue
    g->fillCircle(cx + 17, cy - 15, 2, IC(COLOR_ACCENT2));
    g->fillCircle(cx - 19, cy + 17, 2, IC(COLOR_ACCENT2));
}

static void icon_reminders(Arduino_GFX *g, int cx, int cy, float br) {
    // three rows with a dot each
    uint16_t c = IC(COLOR_ACCENT3);
    for (int i = 0; i < 3; i++) {
        int y = cy - 12 + i * 12;
        g->fillCircle(cx - 12, y, 4, c);
        g->fillRoundRect(cx - 4, y - 2, 20, 4, 2, IC(COLOR_TEXT));
    }
}

static void icon_workout(Arduino_GFX *g, int cx, int cy, float br) {
    // a runner in mid-stride
    uint16_t c = IC(0x9F45);
    g->fillCircle(cx + 5, cy - 16, 5, c);
    g->fillTriangle(cx + 4, cy - 10, cx - 2, cy + 4, cx + 6, cy + 2, c);
    g->drawLine(cx - 1, cy + 3, cx - 10, cy + 18, c);
    g->drawLine(cx, cy + 3, cx - 9, cy + 18, c);
    g->drawLine(cx + 2, cy + 3, cx + 12, cy + 10, c);
    g->drawLine(cx + 12, cy + 10, cx + 9, cy + 19, c);
    g->drawLine(cx + 3, cy - 7, cx + 14, cy - 2, c);
    g->drawLine(cx + 3, cy - 7, cx - 8, cy - 4, c);
}

static void icon_life(Arduino_GFX *g, int cx, int cy, float br) {
    // a heart with a leaf: living well
    uint16_t c = IC(COLOR_TEXT);
    g->fillCircle(cx - 8, cy - 5, 10, c);
    g->fillCircle(cx + 8, cy - 5, 10, c);
    g->fillTriangle(cx - 18, cy - 1, cx + 18, cy - 1, cx, cy + 19, c);
    g->fillCircle(cx + 15, cy - 18, 3, IC(0x2CCA));   // keeps the disc green
}

static void icon_tips(Arduino_GFX *g, int cx, int cy, float br) {
    // a lit bulb
    uint16_t c = IC(COLOR_TEXT);
    g->fillCircle(cx, cy - 5, 14, c);
    g->fillRoundRect(cx - 7, cy + 6, 14, 10, 3, c);
    g->fillRoundRect(cx - 5, cy + 17, 10, 4, 2, c);
    g->fillCircle(cx + 15, cy - 18, 2, IC(COLOR_WARN));   // a yellow disc
}

// In honeycomb order: the first one sits in the middle, the next six
// around it, and so on - so the apps used most come first, games after.
static const MenuEntry MENU_ENTRIES[] = {
    {"Assistant",     CAT_TOOLS, icon_assistant,     &assistant_screen},
    {"Music",         CAT_TOOLS, icon_music,         &music_screen},
    {"Calendar",      CAT_TOOLS, icon_calendar,      &calendar_screen},
    {"Activity",      CAT_TOOLS, icon_activity,      &activity_screen},
    {"Life",          CAT_TOOLS, icon_life,          &life_screen, false, 0x2CCA},
    {"Workout",       CAT_TOOLS, icon_workout,       &workout_screen},
    {"Timer",         CAT_TOOLS, icon_timer,         &timer_screen, false, 0xFC60},
    {"Weather",       CAT_TOOLS, icon_weather,       &weather_screen, false, 0x2A9F},
    {"Alarms",        CAT_TOOLS, icon_alarm,         &alarms_screen, false, 0xFC60},
    {"Notifications", CAT_TOOLS, icon_notifications, &notifications_screen},
    {"Reminders",     CAT_TOOLS, icon_reminders,     &reminders_screen},
    {"Games",         CAT_TOOLS, nullptr,            nullptr, false, 0, CAT_GAMES},
    {"Recorder",      CAT_TOOLS, icon_recorder,      &recorder_screen},
    {"Stopwatch",     CAT_TOOLS, icon_stopwatch,     &stopwatch_screen, false, 0xFC60},
    {"World Clock",   CAT_TOOLS, icon_worldclock,    &worldclock_screen, false, 0x3A7F},
    {"Settings",      CAT_TOOLS, icon_settings,      &settings_screen},
    {"Tips",          CAT_TOOLS, icon_tips,          &tips_screen, false, 0xFD20},
    {"Sleep",         CAT_TOOLS, icon_sleep,         &sleep_screen},
    {"Phone",         CAT_TOOLS, icon_phone,         &phone_screen},
    {"Navigation",    CAT_TOOLS, icon_navigation,    &navigation_screen},
    {"Gallery",       CAT_TOOLS, icon_gallery,       &gallery_screen},
    {"Files",         CAT_TOOLS, icon_media,         &media_screen},   // plays, shows and opens what's on the card
    {"Contacts",      CAT_TOOLS, icon_contacts,      &contacts_screen},
    {"Draw",          CAT_TOOLS, icon_draw,          &draw_screen},
    {"Battery",       CAT_TOOLS, icon_battery,       &battery_screen},
    {"Pet",           CAT_TOOLS, icon_pet,           &pet_screen},
    {"3D Printer",    CAT_TOOLS, icon_bambu,         &bambu_printer_screen},
    {"Developer",     CAT_TOOLS, nullptr,            nullptr, true,  0, CAT_TESTS},
    {"Piano Tiles",   CAT_GAMES, icon_piano,         &piano_screen},
    {"Flappy",        CAT_GAMES, icon_flappy,        &flappy_screen},
    {"Fruit Slice",   CAT_GAMES, icon_fruitninja,    &fruitninja_screen},
    {"Snake",         CAT_GAMES, icon_snake,         &snake_screen},
    {"Breakout",      CAT_GAMES, icon_breakout,      &breakout_screen},
    {"Runner",        CAT_GAMES, icon_runner,        &runner_screen},
    {"Blackjack",     CAT_GAMES, icon_blackjack,     &blackjack_screen},
    {"Simon",         CAT_GAMES, icon_simon,         &simon_screen},
    {"Reaction",      CAT_GAMES, icon_reaction,      &reaction_screen},
    {"Dodger",        CAT_GAMES, icon_dodger,        &dodger_screen},
    {"Plane",         CAT_GAMES, icon_plane,         &plane_screen},
    {"Crystal Cavern",CAT_GAMES, icon_cavern,        &cavern_screen},
    {"Maze Raider",   CAT_GAMES, icon_maze,          &maze_screen},
    {"Ninja Dungeon", CAT_GAMES, icon_ninjadungeon,  &ninjadungeon_screen},
    // developer mode
    {"Tests",         CAT_TESTS, icon_tests,         &test_menu_screen,      true},
    {"Calibrate",     CAT_TESTS, icon_calibration,   &calibration_screen,    true},
    {"BLE Status",    CAT_TESTS, icon_ble,           &ble_status_screen,     true},
    {"Joystick Link", CAT_TESTS, icon_espnow,        &espnow_status_screen,  true},
    {"SD browser",    CAT_TESTS, icon_files,         &files_screen,          true},
    {"Sprite Test",   CAT_TESTS, icon_sprite_test,   &sprite_test_screen,    true},
};
static const int MENU_COUNT = sizeof(MENU_ENTRIES) / sizeof(MENU_ENTRIES[0]);

// =========================================================================
//  Icons: drawn once into a small image each, in one style
// =========================================================================
//
// Every icon is drawn by its function above into a 128x128 image, then
// restyled the same way: a disc in the icon's own colour (lighter at the
// top) with the drawing in white on it - the look of watchOS's app icons,
// whatever colours each drawing used. The images can then be drawn at any
// size: smaller toward the edge of the honeycomb, growing when an app
// opens, small in the list.
static const int SPR = 128, SPR_R = 62;
static const uint16_t KEY = 0x0821;              // "nothing here" (outside the disc)
static uint16_t *s_spr[MENU_COUNT];

static void rgb(uint16_t c, int &r, int &g, int &b) { r = (c >> 11) << 3; g = ((c >> 5) & 0x3F) << 2; b = (c & 0x1F) << 3; }

static void make_sprite(int i) {
    static Arduino_Canvas *canvas = nullptr;
    if (!canvas) {
        canvas = new Arduino_Canvas(SPR, SPR, nullptr);
        if (!canvas->begin(GFX_SKIP_OUTPUT_BEGIN)) { delete canvas; canvas = nullptr; return; }
    }
    uint16_t *px = canvas->getFramebuffer();
    canvas->fillScreen(KEY);
    canvas->fillCircle(SPR / 2, SPR / 2, SPR_R, COLOR_PANEL);
    if (MENU_ENTRIES[i].icon_fn) MENU_ENTRIES[i].icon_fn(canvas, SPR / 2, SPR / 2, 1.0f);

    // The drawing's own colour (its most saturated pixels) becomes the disc's.
    long sr = 0, sg = 0, sb = 0, sw = 0;
    for (int k = 0; k < SPR * SPR; k++) {
        uint16_t c = px[k];
        if (c == KEY || c == COLOR_PANEL) continue;
        int r, g, b;
        rgb(c, r, g, b);
        int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        int sat = mx - mn;
        if (sat < 40) continue;   // white / grey parts don't say what colour it is
        sr += r * sat; sg += g * sat; sb += b * sat; sw += sat;
    }
    int tr = 0x6E, tg = 0x5C, tb = 0xE6;   // no colour of its own: the accent violet
    if (sw > 0) { tr = sr / sw; tg = sg / sw; tb = sb / sw; }
    if (MENU_ENTRIES[i].disc) rgb(MENU_ENTRIES[i].disc, tr, tg, tb);
    // a bit richer and never too dark, so white reads on it
    int mx = tr > tg ? (tr > tb ? tr : tb) : (tg > tb ? tg : tb);
    if (mx < 150 && mx > 0) { tr = tr * 150 / mx; tg = tg * 150 / mx; tb = tb * 150 / mx; }

    uint16_t *out = (uint16_t *)heap_caps_malloc(SPR * SPR * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return;
    for (int y = 0; y < SPR; y++) {
        // disc: lighter at the top, deeper at the bottom
        float k = 1.08f - 0.38f * y / SPR;
        int br = (int)(tr * k), bg = (int)(tg * k), bb = (int)(tb * k);
        if (br > 255) br = 255;
        if (bg > 255) bg = 255;
        if (bb > 255) bb = 255;
        uint16_t disc = COLOR565(br, bg, bb);
        for (int x = 0; x < SPR; x++) {
            uint16_t c = px[y * SPR + x];
            if (c == KEY) { out[y * SPR + x] = KEY; continue; }
            if (c == COLOR_PANEL) { out[y * SPR + x] = disc; continue; }
            int r, g, b;
            rgb(c, r, g, b);
            int luma = (r * 3 + g * 6 + b) / 10;
            // the drawing in white, its darker parts a shade of the disc (keeps detail)
            float a = luma > 90 ? 1.0f : 0.25f + 0.75f * luma / 90.0f;
            out[y * SPR + x] = COLOR565((int)(255 * a + br * (1 - a)), (int)(255 * a + bg * (1 - a)),
                                        (int)(255 * a + bb * (1 - a)));
        }
    }
    s_spr[i] = out;
}

// Draws icon i with its centre at (cx, cy), r pixels across the middle
// (nearest-neighbour scaling, straight into the frame).
static void blit_icon(uint16_t *fb, int i, int cx, int cy, int r);

// A folder: a frosted disc with its first four apps, small, in a 2x2 grid.
static void blit_folder(uint16_t *fb, int i, int cx, int cy, int r) {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    g->fillCircle(cx, cy, r, 0x3186);
    int shown = 0, cat = MENU_ENTRIES[i].folder;
    int mr = r * 30 / 100, off = r * 36 / 100;
    for (int k = 0; k < MENU_COUNT && shown < 4; k++) {
        const MenuEntry &e = MENU_ENTRIES[k];
        if (e.folder >= 0 || e.category != cat) continue;
        int mx = cx + (shown % 2 ? off : -off), my = cy + (shown / 2 ? off : -off);
        blit_icon(fb, k, mx, my, mr);
        shown++;
    }
}

static void blit_icon(uint16_t *fb, int i, int cx, int cy, int r) {
    if (r < 3) return;
    if (MENU_ENTRIES[i].folder >= 0) { blit_folder(fb, i, cx, cy, r); return; }
    if (!s_spr[i]) make_sprite(i);
    const uint16_t *sp = s_spr[i];
    if (!sp || !fb) return;
    int d = 2 * r;
    int x0 = cx - r, y0 = cy - r;
    // the sprite's disc is SPR_R of SPR/2: map r -> SPR_R
    float step = (float)SPR_R / r;
    for (int y = 0; y < d; y++) {
        int sy = y0 + y;
        if (sy < 0 || sy >= LCD_HEIGHT) continue;
        int v = (int)(SPR / 2 + (y - r) * step);
        if (v < 0 || v >= SPR) continue;
        const uint16_t *srow = sp + v * SPR;
        uint16_t *drow = fb + sy * LCD_WIDTH;
        for (int x = 0; x < d; x++) {
            int sx = x0 + x;
            if (sx < 0 || sx >= LCD_WIDTH) continue;
            int u = (int)(SPR / 2 + (x - r) * step);
            if (u < 0 || u >= SPR) continue;
            uint16_t c = srow[u];
            if (c != KEY) drow[sx] = c;
        }
    }
}

void menu_draw_app_icon(const Screen *screen, int cx, int cy, int r) {
    for (int i = 0; i < MENU_COUNT; i++)
        if (MENU_ENTRIES[i].screen == screen) { blit_icon(ui_framebuffer(), i, cx, cy, r); return; }
}

// =========================================================================
//  The honeycomb (watchOS style) - and the list
// =========================================================================
//
// One cluster of all the apps (developer ones only in developer mode),
// packed in hex rings from the middle. Icons are full size near the
// middle and shrink toward the edge (a fisheye), so a glance shows ~20
// of them; panning moves the whole field, with momentum and a rubber
// band at the edge of the cluster. Tapping one zooms it up to fill the
// screen as the app opens.
#define ICON_RADIUS    50
#define ICON_SPACING   112   // centre to centre; > 2*ICON_RADIUS leaves a gap
static const float FISH_R0 = 105.0f;   // full size inside this
static const float FISH_FALL = 230.0f; // then this far to shrink to nothing

struct HexCoord { int q, r; };

static void gen_hex_positions(HexCoord *out, int count) {
    // axial hex directions, walked ring by ring (redblobgames' hex guide)
    static const int dq[6] = { 1, 1, 0, -1, -1, 0 };
    static const int dr[6] = { 0, -1, -1, 0, 1, 1 };
    int n = 0;
    if (n < count) { out[n].q = 0; out[n].r = 0; n++; }
    for (int radius = 1; n < count; radius++) {
        int q = -radius, r = radius;
        for (int side = 0; side < 6 && n < count; side++) {
            for (int step = 0; step < radius && n < count; step++) {
                out[n].q = q;
                out[n].r = r;
                n++;
                q += dq[side];
                r += dr[side];
            }
        }
    }
}

static int   s_vis[MENU_COUNT];          // the entries shown, in order
static int   s_vis_n = 0;
static float s_icon_wx[MENU_COUNT], s_icon_wy[MENU_COUNT];   // by position in s_vis
static float s_bound = 0;                // the cluster's radius: the rubber band starts there

static float s_pan_x = 0, s_pan_y = 0;
static float s_vel_x = 0, s_vel_y = 0;
static bool  s_dragging = false;
static int   s_drag_start_x = 0, s_drag_start_y = 0;
static float s_drag_start_pan_x = 0, s_drag_start_pan_y = 0;
static int   s_last_x = 0, s_last_y = 0;
static bool  s_moved_significantly = false;
static int   s_nearest_idx = -1;         // position in s_vis
static bool  s_ctrl_btn_prev = false;

// opening an app: the icon grows from where it was to fill the screen
static int      s_launch = -1;           // position in s_vis
static uint32_t s_launch_ms = 0;
static int      s_launch_x = 0, s_launch_y = 0, s_launch_r = 0;
static const uint32_t LAUNCH_MS = 200;
// coming back: the app shrinks into its icon (the launch in reverse)
static int      s_return = -1;           // position in s_vis
static uint32_t s_return_ms = 0;
static int      s_opened = -1;           // the app opened from here, still open
static const uint32_t RETURN_MS = 220;

// the list (Settings > Display > Apps as a list)
static int   s_order[MENU_COUNT];        // s_vis positions, A-Z
static float s_list_y = 0, s_list_vel = 0;
static const int LIST_ROW = 72, LIST_TOP = 96;

static bool listing() { return g_app_settings.menu_list; }

// Which view the field shows: the top level (-1) or a folder's category.
static int s_folder = -1, s_next_folder = -1;
static float s_top_pan_x = 0, s_top_pan_y = 0;   // where the top level was, for coming back
extern Screen menu_folder_screen;

static bool visible_in_view(const MenuEntry &e) {
    bool dev_ok = !e.dev || g_app_settings.developer_mode;
    if (!dev_ok) return false;
    if (listing()) return e.folder < 0;   // the list: every app, A-Z, no folders
    if (s_folder < 0) return e.folder >= 0 || (e.category != CAT_GAMES && e.category != CAT_TESTS);
    return e.folder < 0 && e.category == s_folder;
}

static void build_field() {
    s_vis_n = 0;
    for (int i = 0; i < MENU_COUNT; i++)
        if (visible_in_view(MENU_ENTRIES[i])) s_vis[s_vis_n++] = i;
    HexCoord coords[MENU_COUNT];
    gen_hex_positions(coords, s_vis_n);
    s_bound = 0;
    for (int k = 0; k < s_vis_n; k++) {
        // axial -> pixel, pointy-top hexes
        s_icon_wx[k] = ICON_SPACING * ((float)coords[k].q + (float)coords[k].r / 2.0f);
        s_icon_wy[k] = ICON_SPACING * ((float)coords[k].r * 0.8660254f);
        float d = sqrtf(s_icon_wx[k] * s_icon_wx[k] + s_icon_wy[k] * s_icon_wy[k]);
        if (d > s_bound) s_bound = d;
    }
    s_bound = s_bound > ICON_SPACING ? s_bound - ICON_SPACING * 0.6f : 0;   // the outer ring may reach the middle
    // A-Z for the list
    for (int k = 0; k < s_vis_n; k++) s_order[k] = k;
    for (int a = 1; a < s_vis_n; a++)
        for (int b = a; b > 0 && strcasecmp(MENU_ENTRIES[s_vis[s_order[b - 1]]].label, MENU_ENTRIES[s_vis[s_order[b]]].label) > 0; b--) {
            int t = s_order[b]; s_order[b] = s_order[b - 1]; s_order[b - 1] = t;
        }
    s_pan_x = s_pan_y = s_vel_x = s_vel_y = 0;
    s_list_y = s_list_vel = 0;
    s_dragging = false;
    s_launch = -1;
}

static void menu_create() {
    s_folder = -1;
    s_opened = s_return = -1;
    ui_zoom_back_to(&menu_screen);
    ui_zoom_back_to(&menu_folder_screen);
    build_field();
}

static int s_top_opened = -1;   // the folder icon, to close into when the folder goes

static void folder_create() {
    s_top_opened = s_opened;
    s_opened = s_return = -1;
    s_top_pan_x = s_pan_x;
    s_top_pan_y = s_pan_y;
    s_folder = s_next_folder;
    build_field();
}

static void folder_destroy() {
    // back to the top level, where it was - and the folder closes into its icon
    s_folder = -1;
    build_field();
    s_pan_x = s_top_pan_x;
    s_pan_y = s_top_pan_y;
    s_opened = s_top_opened;   // menu_tick zooms the folder shut into its icon
    s_return = -1;
}

// Where position k is drawn: fisheye from the middle. False when off screen.
static bool place(int k, int &sx, int &sy, int &r) {
    float ox = s_icon_wx[k] - s_pan_x, oy = s_icon_wy[k] - s_pan_y;
    float d = sqrtf(ox * ox + oy * oy);
    float scale = 1.0f, dd = d;
    if (d > FISH_R0) {
        scale = 1.0f - (d - FISH_R0) / FISH_FALL;
        dd = FISH_R0 + (d - FISH_R0) * 0.78f;   // pulled in a little, like watchOS
    }
    if (scale < 0.18f) return false;
    float f = d > 0.5f ? dd / d : 1.0f;
    sx = LCD_WIDTH / 2 + (int)(ox * f);
    sy = LCD_HEIGHT / 2 + (int)(oy * f);
    r = (int)(ICON_RADIUS * scale);
    int ex = sx - LCD_WIDTH / 2, ey = sy - LCD_HEIGHT / 2;
    float out = sqrtf((float)ex * ex + (float)ey * ey) - r;
    return out < LCD_WIDTH / 2;
}

static int hit_test(int x, int y) {
    if (listing()) {
        int row = (int)((y - LIST_TOP + s_list_y) / LIST_ROW);
        return row >= 0 && row < s_vis_n ? s_order[row] : -1;
    }
    int best = -1, best_d = 1 << 30;
    for (int k = 0; k < s_vis_n; k++) {
        int sx, sy, r;
        if (!place(k, sx, sy, r)) continue;
        int dx = x - sx, dy = y - sy, d2 = dx * dx + dy * dy;
        if (d2 <= r * r && d2 < best_d) { best = k; best_d = d2; }
    }
    return best;
}

static void launch(int k, int sx, int sy, int r) {
    if (k < 0 || (!MENU_ENTRIES[s_vis[k]].screen && MENU_ENTRIES[s_vis[k]].folder < 0)) return;
    sleep_register_activity();
    s_launch = k;
    s_launch_ms = millis();
    s_launch_x = sx; s_launch_y = sy; s_launch_r = r;
}

static void draw_list(Arduino_GFX *g, uint16_t *fb) {
    const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2, R = LCD_WIDTH / 2;
    for (int n = 0; n < s_vis_n; n++) {
        int y = LIST_TOP + n * LIST_ROW - (int)s_list_y + LIST_ROW / 2;   // row centre
        if (y < -LIST_ROW || y > LCD_HEIGHT + LIST_ROW) continue;
        int dy = y - CY;
        if (abs(dy) >= R - 20) continue;
        // follow the round edge, like watchOS's list
        int half = (int)sqrtf((float)R * R - (float)dy * dy);
        int ix = CX - half + 58;
        if (ix < 70) ix = 70;
        int k = s_order[n];
        blit_icon(fb, s_vis[k], ix, y, 26);
        ui_print(ix + 40, y - 9, 2, COLOR_TEXT, MENU_ENTRIES[s_vis[k]].label);
    }
}

static void draw_honeycomb(uint16_t *fb) {
    s_nearest_idx = -1;
    float nearest = 1e9f;
    for (int k = 0; k < s_vis_n; k++) {
        float ox = s_icon_wx[k] - s_pan_x, oy = s_icon_wy[k] - s_pan_y;
        float d2 = ox * ox + oy * oy;
        if (d2 < nearest) { nearest = d2; s_nearest_idx = k; }
        int sx, sy, r;
        if (!place(k, sx, sy, r)) continue;
        if (k == s_launch || k == s_return) continue;   // drawn on top, growing or shrinking
        blit_icon(fb, s_vis[k], sx, sy, r);
    }
}

static void menu_draw() {
    Arduino_GFX *g = ui_gfx();
    uint16_t *fb = ui_framebuffer();
    if (!g || !fb) return;
    g->fillScreen(COLOR_BG);
    if (listing()) draw_list(g, fb);
    else draw_honeycomb(fb);

    if (s_launch >= 0) {
        // ease out: fast first, settling
        float t = (millis() - s_launch_ms) / (float)LAUNCH_MS;
        if (t > 1) t = 1;
        float e = 1 - (1 - t) * (1 - t);
        int cx = s_launch_x + (int)((LCD_WIDTH / 2 - s_launch_x) * e);
        int cy = s_launch_y + (int)((LCD_HEIGHT / 2 - s_launch_y) * e);
        int r = s_launch_r + (int)((LCD_WIDTH * 0.75f - s_launch_r) * e);
        blit_icon(fb, s_vis[s_launch], cx, cy, r);
        return;
    }
    if (s_return >= 0 && s_return < s_vis_n) {
        int sx, sy, r;
        if (place(s_return, sx, sy, r)) {
            float t = (millis() - s_return_ms) / (float)RETURN_MS;
            if (t > 1) t = 1;
            float e = t * t * (3 - 2 * t);   // smoothstep: settles into the slot
            int cx = LCD_WIDTH / 2 + (int)((sx - LCD_WIDTH / 2) * e);
            int cy = LCD_HEIGHT / 2 + (int)((sy - LCD_HEIGHT / 2) * e);
            int rr = (int)(LCD_WIDTH * 0.75f + (r - LCD_WIDTH * 0.75f) * e);
            blit_icon(fb, s_vis[s_return], cx, cy, rr);
        }
    }
    if (listing()) return;
    // which app is in the middle: its name in a pill at the bottom, following the pan
    if (s_nearest_idx >= 0) {
        const char *name = MENU_ENTRIES[s_vis[s_nearest_idx]].label;
        int tw = ui_text_width(name, 2);
        int w = tw + 40, h = 40, x = LCD_WIDTH / 2 - w / 2, y = LCD_HEIGHT - 64;
        g->fillRoundRect(x - 2, y - 2, w + 4, h + 4, h / 2 + 2, COLOR_BG);
        g->fillRoundRect(x, y, w, h, h / 2, 0x2945);
        ui_text_center(LCD_WIDTH / 2, y + h / 2, COLOR_TEXT, name, 2);
    }
    if (s_folder >= 0) {
        const char *title = s_folder == CAT_GAMES ? "Games" : "Developer";
        int tw = ui_text_width(title, 2);
        g->fillRoundRect(LCD_WIDTH / 2 - tw / 2 - 22, 22, tw + 44, 36, 18, COLOR_BG);
        ui_text_center(LCD_WIDTH / 2, 40, COLOR_TEXT_DIM, title, 2);
    }
}

// Past the edge: the drag moves a third as far (the rubber band).
static void band(float &px, float &py) {
    float d = sqrtf(px * px + py * py);
    if (d > s_bound && d > 0) {
        float over = d - s_bound;
        float nd = s_bound + over * 0.35f;
        px *= nd / d;
        py *= nd / d;
    }
}

static void menu_touch(int x, int y, bool pressed) {
    if (s_launch >= 0) return;
    if (pressed) {
        if (!s_dragging) {
            s_dragging = true;
            s_drag_start_x = x;
            s_drag_start_y = y;
            s_drag_start_pan_x = listing() ? 0 : s_pan_x;
            s_drag_start_pan_y = listing() ? s_list_y : s_pan_y;
            s_last_x = x;
            s_last_y = y;
            // catching a coasting field is not a tap
            s_moved_significantly = fabsf(s_vel_x) > 0.3f || fabsf(s_vel_y) > 0.3f || fabsf(s_list_vel) > 0.3f;
            s_vel_x = s_vel_y = s_list_vel = 0;
        } else {
            int dx = x - s_drag_start_x, dy = y - s_drag_start_y;
            if (abs(dx) > 8 || abs(dy) > 8) s_moved_significantly = true;
            if (listing()) {
                float maxy = (float)(s_vis_n * LIST_ROW - (LCD_HEIGHT - LIST_TOP - 60));
                if (maxy < 0) maxy = 0;
                float ny = s_drag_start_pan_y - dy;
                if (ny < 0) ny *= 0.35f;                               // rubber band
                else if (ny > maxy) ny = maxy + (ny - maxy) * 0.35f;
                s_list_y = ny;
                s_list_vel = -(float)(y - s_last_y);
            } else {
                float px = s_drag_start_pan_x - dx, py = s_drag_start_pan_y - dy;
                band(px, py);
                s_pan_x = px;
                s_pan_y = py;
                s_vel_x = -(float)(x - s_last_x);
                s_vel_y = -(float)(y - s_last_y);
            }
            s_last_x = x;
            s_last_y = y;
        }
    } else if (s_dragging) {
        s_dragging = false;
        if (s_moved_significantly) {
            touch_cancel_swipes();   // a pan, not a swipe away
        } else {
            s_vel_x = s_vel_y = s_list_vel = 0;
            int k = hit_test(x, y);
            if (k < 0) return;
            if (listing()) {
                sleep_register_activity();
                ui_push(MENU_ENTRIES[s_vis[k]].screen);
            } else {
                int sx, sy, r;
                if (place(k, sx, sy, r)) launch(k, sx, sy, r);
            }
        }
    }
}

static void menu_tick() {
    // back from the app opened here: play it closing into its icon
    if (s_opened >= 0) {
        if (!listing() && !ui_last_pop_was_drag()) {   // a drag back already showed the menu
            s_return = s_opened;
            s_return_ms = millis();
        }
        s_opened = -1;
    }
    if (s_return >= 0 && millis() - s_return_ms >= RETURN_MS) s_return = -1;
    if (s_launch >= 0) {
        if (millis() - s_launch_ms >= LAUNCH_MS) {
            const MenuEntry &e = MENU_ENTRIES[s_vis[s_launch]];
            s_opened = s_launch;
            s_launch = -1;
            if (e.folder >= 0) {   // a folder opens as its own honeycomb
                s_next_folder = e.folder;
                ui_push_instant(&menu_folder_screen);
            } else {
                ui_push_instant(e.screen);   // it zoomed in already: no slide
            }
        }
        return;
    }
    if (s_dragging) return;
    if (listing()) {
        float maxy = (float)(s_vis_n * LIST_ROW - (LCD_HEIGHT - LIST_TOP - 60));
        if (maxy < 0) maxy = 0;
        if (fabsf(s_list_vel) > 0.3f) { s_list_y += s_list_vel; s_list_vel *= 0.92f; }
        else s_list_vel = 0;
        // spring back from past the ends
        if (s_list_y < 0) { s_list_y *= 0.7f; s_list_vel *= 0.5f; if (s_list_y > -0.5f) s_list_y = 0; }
        if (s_list_y > maxy) { s_list_y = maxy + (s_list_y - maxy) * 0.7f; s_list_vel *= 0.5f; if (s_list_y - maxy < 0.5f) s_list_y = maxy; }
        return;
    }
    if (fabsf(s_vel_x) > 0.3f || fabsf(s_vel_y) > 0.3f) {
        s_pan_x += s_vel_x;
        s_pan_y += s_vel_y;
        s_vel_x *= 0.90f;
        s_vel_y *= 0.90f;
    } else {
        s_vel_x = s_vel_y = 0;
    }
    // spring back inside the cluster
    float d = sqrtf(s_pan_x * s_pan_x + s_pan_y * s_pan_y);
    if (d > s_bound && d > 0) {
        float nd = s_bound + (d - s_bound) * 0.72f;
        if (nd - s_bound < 0.5f) nd = s_bound;
        s_pan_x *= nd / d;
        s_pan_y *= nd / d;
        s_vel_x *= 0.6f;
        s_vel_y *= 0.6f;
    }

    // a game controller pans and picks, as a finger would
    if (controller_connected()) {
        float cdx, cdy;
        controller_get_direction(&cdx, &cdy);
        if (cdx != 0.0f || cdy != 0.0f) {
            s_pan_x += cdx * 6.0f;
            s_pan_y += cdy * 6.0f;
            sleep_register_activity();
        }
        bool btn = controller_button(CTRL_BTN_A);
        if (btn && !s_ctrl_btn_prev && s_nearest_idx >= 0) {
            int sx, sy, r;
            if (place(s_nearest_idx, sx, sy, r)) launch(s_nearest_idx, sx, sy, r);
        }
        s_ctrl_btn_prev = btn;
    }
}

static void menu_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) {
        ui_pop_screen();
    }
}

Screen menu_screen = {
    "", // no header - watchOS's app grid has none either; an edge swipe still leaves
    GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    menu_create,
    menu_draw,
    menu_touch,
    menu_tick,
    nullptr,
    menu_gesture,
    150,   // idle: the field is still between pans
    false, false, false, false,
    true,  // hide_status: the honeycomb uses the whole round screen
};

// A folder (Games, Developer): the same honeycomb with just its apps.
// Swipe or BOOT goes back to the top level, where it was.
Screen menu_folder_screen = {
    "",
    GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    folder_create,
    menu_draw,
    menu_touch,
    menu_tick,
    folder_destroy,
    menu_gesture,
    150,
    false, false, false, false,
    true,
};

void menu_open_folder(int category) {
    s_next_folder = category;
    ui_push_instant(&menu_folder_screen);
}
