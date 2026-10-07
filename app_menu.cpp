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

enum MenuCategory { CAT_GAMES, CAT_TOOLS, CAT_TESTS, CAT_COUNT };
static const uint16_t CATEGORY_COLOR[CAT_COUNT] = { COLOR_GOOD, COLOR_ACCENT, COLOR_ACCENT2 };
static const char *CATEGORY_NAME[CAT_COUNT] = { "Games", "Tools", "Tests" };

struct MenuEntry {
    const char *label;
    MenuCategory category;
    void (*icon_fn)(Arduino_GFX *g, int cx, int cy, float br);
    Screen *screen;
};

extern Screen settings_screen;
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

static const MenuEntry MENU_ENTRIES[] = {
    {"Settings",      CAT_TOOLS, icon_settings,      &settings_screen},
    {"Phone",         CAT_TOOLS, icon_phone,         &phone_screen},
    {"Calendar",      CAT_TOOLS, icon_calendar,      &calendar_screen},
    {"Navigation",    CAT_TOOLS, icon_navigation,    &navigation_screen},
    {"Notifications", CAT_TOOLS, icon_notifications, &notifications_screen},
    {"BLE Status",    CAT_TOOLS, icon_ble,           &ble_status_screen},
    {"Battery",       CAT_TOOLS, icon_battery,       &battery_screen},
    {"Joystick Link",  CAT_TOOLS, icon_espnow,        &espnow_status_screen},
    {"3D Printer",     CAT_TOOLS, icon_bambu,         &bambu_printer_screen},
    {"Gallery",       CAT_TOOLS, icon_gallery,       &gallery_screen},
    {"Music",         CAT_TOOLS, icon_music,         &music_screen},
    {"Media",         CAT_TOOLS, icon_media,         &media_screen},
    {"Contacts",      CAT_TOOLS, icon_contacts,      &contacts_screen},
    {"Files",         CAT_TOOLS, icon_files,         &files_screen},
    {"Recorder",      CAT_TOOLS, icon_recorder,      &recorder_screen},
    {"Calibrate",     CAT_TOOLS, icon_calibration,   &calibration_screen},
    {"Pet",           CAT_TOOLS, icon_pet,           &pet_screen},
    {"Tests",         CAT_TESTS, icon_tests,         &test_menu_screen},
    {"Sprite Test",   CAT_TESTS, icon_sprite_test,   &sprite_test_screen},
    {"Snake",         CAT_GAMES, icon_snake,         &snake_screen},
    {"Reaction",      CAT_GAMES, icon_reaction,      &reaction_screen},
    {"Dodger",        CAT_GAMES, icon_dodger,        &dodger_screen},
    {"Simon",         CAT_GAMES, icon_simon,         &simon_screen},
    {"Plane",         CAT_GAMES, icon_plane,         &plane_screen},
    {"Breakout",      CAT_GAMES, icon_breakout,      &breakout_screen},
    {"Blackjack",     CAT_GAMES, icon_blackjack,     &blackjack_screen},
    {"Crystal Cavern",CAT_GAMES, icon_cavern,        &cavern_screen},
    {"Flappy",        CAT_GAMES, icon_flappy,        &flappy_screen},
    {"Fruit Slice",   CAT_GAMES, icon_fruitninja,    &fruitninja_screen},
    {"Runner",        CAT_GAMES, icon_runner,        &runner_screen},
    {"Maze Raider",   CAT_GAMES, icon_maze,          &maze_screen},
    {"Ninja Dungeon", CAT_GAMES, icon_ninjadungeon,  &ninjadungeon_screen},
};
static const int MENU_COUNT = sizeof(MENU_ENTRIES) / sizeof(MENU_ENTRIES[0]);

// ---- Hex-packed "world" layout, Apple Watch style ------------------------
//
// Icons sit at fixed positions in an infinite 2D world; the screen just
// shows whatever's under a pannable window (s_pan_x/y = world coordinate
// currently at screen center). Positions are laid out in concentric hex
// rings around the origin (icon 0 at the center, the next 6 forming a
// ring around it, the next 12 forming a ring around that, and so on) -
// the same ring-spiral construction used for hex-grid tile maps, applied
// here to precisely circle-pack a cluster the way watchOS's app grid
// does, rather than left to a physics simulation this hardware has no
// business running for a menu screen.
#define ICON_RADIUS    62
#define ICON_SPACING   144 // center-to-center; > 2*ICON_RADIUS leaves a gap

// Each category gets its own hex-spiral cluster centered at a
// distinct offset, rather than one continuous spiral with categories
// just consecutive within it - genuinely separate "sections" you pan
// between, not a single blended field. Order matches the MenuCategory
// enum (CAT_GAMES, CAT_TOOLS, CAT_TESTS). Tools sits at the shared
// origin (most-used, and where the very first icon naturally lands);
// Games and Tests are offset far enough apart that even the largest
// cluster's footprint (currently Games, up to 2 hex rings) doesn't
// overlap its neighbors.
// Each category gets its own hex-spiral cluster centered at a
// distinct offset, rather than one continuous spiral with categories
// just consecutive within it - genuinely separate "sections" you pan
// between, not a single blended field. Computed at runtime in
// menu_create() from each category's ACTUAL item count (not a guessed
// constant), so the clusters sit exactly as close as they safely can
// without overlapping, and stay correct if a category grows. Order
// matches the MenuCategory enum (CAT_GAMES, CAT_TOOLS, CAT_TESTS).
static float CAT_OFFSET_X[CAT_COUNT];
static float CAT_OFFSET_Y[CAT_COUNT];

// How many hex rings are needed to hold `count` items - ring 0 holds
// 1 (the center), each ring k>0 adds 6k more. Used to size each
// category's actual footprint instead of assuming a worst case.
static int rings_needed(int count) {
    if (count <= 1) return 0;
    int held = 1, ring = 0;
    while (held < count) {
        ring++;
        held += 6 * ring;
    }
    return ring;
}

struct HexCoord { int q, r; };

static void gen_hex_positions(HexCoord *out, int count) {
    // Standard axial hex neighbor directions, walked in order to trace
    // each ring (see redblobgames' hex-grid guide for the reference
    // algorithm this follows).
    static const int dq[6] = { 1, 1, 0, -1, -1, 0 };
    static const int dr[6] = { 0, -1, -1, 0, 1, 1 };

    int n = 0;
    if (n < count) { out[n].q = 0; out[n].r = 0; n++; }

    for (int radius = 1; n < count; radius++) {
        int q = -radius, r = radius; // start of this ring: direction[4] * radius
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

static float s_icon_wx[MENU_COUNT];
static float s_icon_wy[MENU_COUNT];

static float s_pan_x = 0, s_pan_y = 0;
static float s_vel_x = 0, s_vel_y = 0; // momentum coast after a flick, decays in menu_tick()

static bool  s_dragging = false;
static int   s_drag_start_x = 0, s_drag_start_y = 0;
static float s_drag_start_pan_x = 0, s_drag_start_pan_y = 0;
static int   s_last_x = 0, s_last_y = 0;
static bool  s_moved_significantly = false;
static int   s_nearest_idx = -1; // whichever icon is currently focused - read by controller button-select
static bool  s_ctrl_btn_prev = false; // edge detection for controller select button, same pattern as touch

static void menu_create() {
    int cat_count[CAT_COUNT] = { 0, 0, 0 };
    for (int i = 0; i < MENU_COUNT; i++) cat_count[MENU_ENTRIES[i].category]++;

    float cat_radius[CAT_COUNT];
    for (int cat = 0; cat < CAT_COUNT; cat++) {
        cat_radius[cat] = rings_needed(cat_count[cat]) * ICON_SPACING;
    }

    // Tools anchors the shared origin; Games and Tests sit at two
    // genuine hex-neighbor directions from it, 120 degrees apart - the
    // same step geometry gen_hex_positions() uses for icons, just
    // applied one level up so the GROUPS themselves relate to each
    // other in a hexagonal pattern too, not just each group's own
    // internal packing. Each placed exactly as close as its own
    // footprint plus Tools' footprint safely allows (computed from the
    // real item counts above, not a guessed distance), with a small
    // fixed gap on top so the boundary reads as a deliberate seam
    // rather than icons touching edge-to-edge.
    const float GAP = ICON_SPACING * 0.15f;
    CAT_OFFSET_X[CAT_TOOLS] = 0.0f;
    CAT_OFFSET_Y[CAT_TOOLS] = 0.0f;

    float games_dist = cat_radius[CAT_TOOLS] + cat_radius[CAT_GAMES] + GAP;
    CAT_OFFSET_X[CAT_GAMES] = games_dist;         // hex direction (1, 0)
    CAT_OFFSET_Y[CAT_GAMES] = 0.0f;

    float tests_dist = cat_radius[CAT_TOOLS] + cat_radius[CAT_TESTS] + GAP;
    CAT_OFFSET_X[CAT_TESTS] = tests_dist * -0.5f;       // hex direction (0, -1),
    CAT_OFFSET_Y[CAT_TESTS] = tests_dist * -0.8660254f; // 120 degrees from Games'

    // Each category gets its own hex-spiral (starting fresh at its own
    // ring 0), then shifted by that category's offset computed above -
    // this is what actually makes them separate visual "sections"
    // rather than just consecutive positions within one shared spiral.
    HexCoord coords[MENU_COUNT];
    for (int cat = 0; cat < CAT_COUNT; cat++) {
        int cat_indices[MENU_COUNT];
        int n = 0;
        for (int i = 0; i < MENU_COUNT; i++) {
            if (MENU_ENTRIES[i].category == cat) cat_indices[n++] = i;
        }
        if (n == 0) continue;

        gen_hex_positions(coords, n);
        for (int k = 0; k < n; k++) {
            int entry_i = cat_indices[k];
            // Standard axial->pixel conversion for a pointy-top hex
            // layout - moving along q shifts a full ICON_SPACING;
            // moving along r shifts half that horizontally and
            // sqrt(3)/2 of it vertically, which is what makes adjacent
            // circles touch evenly.
            s_icon_wx[entry_i] = CAT_OFFSET_X[cat] + ICON_SPACING * ((float)coords[k].q + (float)coords[k].r / 2.0f);
            s_icon_wy[entry_i] = CAT_OFFSET_Y[cat] + ICON_SPACING * ((float)coords[k].r * 0.8660254f);
        }
    }

    s_pan_x = 0;
    s_pan_y = 0;
    s_vel_x = 0;
    s_vel_y = 0;
    s_dragging = false;
}

static int hit_test(int x, int y) {
    int cx0 = LCD_WIDTH / 2, cy0 = LCD_HEIGHT / 2;
    for (int i = 0; i < MENU_COUNT; i++) {
        int sx = cx0 + (int)(s_icon_wx[i] - s_pan_x);
        int sy = cy0 + (int)(s_icon_wy[i] - s_pan_y);
        int dx = x - sx, dy = y - sy;
        if (dx * dx + dy * dy <= ICON_RADIUS * ICON_RADIUS) return i;
    }
    return -1;
}

static void menu_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    int cx0 = LCD_WIDTH / 2, cy0 = LCD_HEIGHT / 2;

    // Find whichever icon is nearest screen center first, so its focus
    // highlight can be drawn correctly in the same pass as everything
    // else below (Apple Watch subtly magnifies the centered icon as
    // you pan - this is that, minus the physics). Stored in the
    // persistent s_nearest_idx (not just a local) so the controller's
    // button-select in menu_tick() can act on "whichever icon is
    // currently focused" without recomputing this same search.
    s_nearest_idx = -1;
    float nearest_d2 = 1e9f;
    for (int i = 0; i < MENU_COUNT; i++) {
        float sx = s_icon_wx[i] - s_pan_x;
        float sy = s_icon_wy[i] - s_pan_y;
        float d2 = sx * sx + sy * sy;
        if (d2 < nearest_d2) { nearest_d2 = d2; s_nearest_idx = i; }
    }

    for (int i = 0; i < MENU_COUNT; i++) {
        int sx = cx0 + (int)(s_icon_wx[i] - s_pan_x);
        int sy = cy0 + (int)(s_icon_wy[i] - s_pan_y);

        bool focused = (i == s_nearest_idx);
        int r = focused ? (ICON_RADIUS + 8) : ICON_RADIUS;
        float br = focused ? 1.0f : 0.4f; // dimmed until it's the focused (centered) icon

        if (sx + r < 0 || sx - r > LCD_WIDTH || sy + r < 0 || sy - r > LCD_HEIGHT) continue;

        g->fillCircle(sx, sy, r, COLOR_PANEL);
        uint16_t ring_c = blend565(CATEGORY_COLOR[MENU_ENTRIES[i].category], COLOR_PANEL, br);
        g->drawCircle(sx, sy, r, ring_c);
        if (focused) g->drawCircle(sx, sy, r - 1, ring_c);

        if (MENU_ENTRIES[i].icon_fn) {
            MENU_ENTRIES[i].icon_fn(g, sx, sy, br);
        }
    }

    // No per-icon labels (a clean field of icons is the point) - just a
    // caption for whichever one is currently centered.
    if (s_nearest_idx >= 0) {
        ui_draw_centered_text(LCD_HEIGHT - 44, COLOR_TEXT, MENU_ENTRIES[s_nearest_idx].label, 2);
    }

    // Persistent per-cluster direction indicators - a small arrow at
    // the display's edge, in that category's own color, for every
    // OTHER cluster that isn't currently on screen (skipped once
    // you're close enough to actually see it, so this doesn't clutter
    // the view with an arrow pointing at something already visible).
    // Distinct from a single "you're lost, here's the way back"
    // pointer - this is constant orientation ("games are that way,
    // tests are that way") regardless of which cluster you're
    // currently browsing.
    for (int cat = 0; cat < CAT_COUNT; cat++) {
        float ddx = s_pan_x - CAT_OFFSET_X[cat], ddy = s_pan_y - CAT_OFFSET_Y[cat];
        float dist = sqrtf(ddx * ddx + ddy * ddy);
        if (dist <= 300.0f) continue; // close enough that this cluster is already in view

        float angle = atan2f(CAT_OFFSET_Y[cat] - s_pan_y, CAT_OFFSET_X[cat] - s_pan_x);
        float ca = cosf(angle), sa = sinf(angle);
        uint16_t c = CATEGORY_COLOR[cat];
        int ptr_r = 195; // inset from the true bezel edge
        int px = cx0 + (int)(ca * ptr_r), py = cy0 + (int)(sa * ptr_r);

        // Small triangle pointing along `angle`.
        float perp = angle + (float)M_PI / 2.0f;
        int tip_x = px + (int)(ca * 14), tip_y = py + (int)(sa * 14);
        int base1_x = px + (int)(cosf(perp) * 8), base1_y = py + (int)(sinf(perp) * 8);
        int base2_x = px - (int)(cosf(perp) * 8), base2_y = py - (int)(sinf(perp) * 8);
        g->fillTriangle(tip_x, tip_y, base1_x, base1_y, base2_x, base2_y, c);

        // Label just inside the arrow, toward screen center so it
        // stays within a safely readable area rather than right at
        // the edge - verified against the actual circle geometry at
        // every angle used here, not eyeballed.
        int label_x = cx0 + (int)(ca * (ptr_r - 40)), label_y = cy0 + (int)(sa * (ptr_r - 40));
        ui_draw_centered_text_at(label_x, label_y - 4, c, CATEGORY_NAME[cat], 1);
    }
}

static void menu_touch(int x, int y, bool pressed) {
    if (pressed) {
        if (!s_dragging) {
            s_dragging = true;
            s_drag_start_x = x;
            s_drag_start_y = y;
            s_drag_start_pan_x = s_pan_x;
            s_drag_start_pan_y = s_pan_y;
            s_last_x = x;
            s_last_y = y;
            // If the world was still coasting from a previous flick,
            // touching down is catching/stopping it, not tapping an
            // icon - treat it as already "moved significantly" from
            // the very start so a quick catch-and-release can never
            // be misread as a tap and launch whatever happens to be
            // under the finger at that instant. A genuinely stationary
            // world still starts this false, same as before.
            s_moved_significantly = (fabsf(s_vel_x) > 0.3f || fabsf(s_vel_y) > 0.3f);
            s_vel_x = 0;
            s_vel_y = 0;
        } else {
            int dx = x - s_drag_start_x, dy = y - s_drag_start_y;
            s_pan_x = s_drag_start_pan_x - dx;
            s_pan_y = s_drag_start_pan_y - dy;
            if (abs(dx) > 8 || abs(dy) > 8) s_moved_significantly = true;
            // Per-sample screen delta becomes the coast velocity if this
            // turns out to be the last sample before release.
            s_vel_x = -(float)(x - s_last_x);
            s_vel_y = -(float)(y - s_last_y);
            s_last_x = x;
            s_last_y = y;
        }
    } else {
        if (s_dragging) {
            s_dragging = false;
            if (s_moved_significantly) {
                // Cancel any swipe-to-exit gesture this drag might also
                // have triggered, same as the old grid's scroll did -
                // keep s_vel_x/y as-is so menu_tick() coasts it out.
                touch_cancel_swipes();
            } else {
                s_vel_x = 0;
                s_vel_y = 0;
                int idx = hit_test(x, y);
                if (idx >= 0 && MENU_ENTRIES[idx].screen) {
                    ui_push(MENU_ENTRIES[idx].screen);
                }
            }
        }
    }
}

static void menu_tick() {
    if (s_dragging) return;
    if (fabsf(s_vel_x) > 0.3f || fabsf(s_vel_y) > 0.3f) {
        s_pan_x += s_vel_x;
        s_pan_y += s_vel_y;
        s_vel_x *= 0.90f; // friction - coasts to a stop over ~20-30 frames
        s_vel_y *= 0.90f;
    } else {
        s_vel_x = 0;
        s_vel_y = 0;
    }

    // Controller d-pad pans the world the same way a drag would;
    // button A selects whichever icon is currently focused (centered),
    // the same action a tap on it triggers. Only active once a
    // controller packet has actually arrived, so a watch with no
    // controller connected behaves exactly as before.
    if (controller_connected()) {
        float cdx, cdy;
        controller_get_direction(&cdx, &cdy);
        if (cdx != 0.0f || cdy != 0.0f) {
            const float CTRL_PAN_SPEED = 6.0f;
            s_pan_x += cdx * CTRL_PAN_SPEED;
            s_pan_y += cdy * CTRL_PAN_SPEED;
            // Real input (not just "a controller happens to be
            // connected") - counts as activity the same way touch
            // does, so idle_frame_ms throttling (see ui.cpp) doesn't
            // kick in and make panning choppy while actively steering
            // via a physical or phone-app joystick with no touch
            // involved at all.
            sleep_register_activity();
        }
        bool btn = controller_button(CTRL_BTN_A);
        if (btn && !s_ctrl_btn_prev && s_nearest_idx >= 0 && MENU_ENTRIES[s_nearest_idx].screen) {
            sleep_register_activity();
            ui_push(MENU_ENTRIES[s_nearest_idx].screen);
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
    "", // no header chrome - an edge swipe (GESTURE_MODE_EDGE) still exits,
        // same as watchOS's app grid has no title bar either
    GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    menu_create,
    menu_draw,
    menu_touch,
    menu_tick,
    nullptr,
    menu_gesture,
    150, // idle_frame_ms - once idle 2s+ (no touch, no active controller
         // input), drop from 30fps to ~6-7fps; the icon field is static
         // between pans so nothing looks different, it's just redrawn
         // (and flushed to the display) far less often
};
