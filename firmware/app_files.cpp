#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "hal_sd.h"
#include <Arduino_GFX_Library.h>
#if FEATURE_SD_CARD
#include <SD.h>
#endif

// =========================================================================
//  Files screen (SD card browser)
// =========================================================================

static void files_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    // No manual header draw - the framework draws it automatically
    // from Screen.title.

#if FEATURE_SD_CARD
    if (!sd_is_mounted()) {
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "No SD card", 2);
        return;
    }

    String listing = sd_list_root(15);
    int y = 80;
    int idx = 0;
    int row_w = 340, row_x = (LCD_WIDTH - row_w) / 2, row_h = 44, row_gap = 8; // 340 fits safely at y=80, the first row's top (needs <=351 there)
    while (idx < (int)listing.length() && y + row_h < LCD_HEIGHT - 40) {
        int nl = listing.indexOf('\n', idx);
        if (nl < 0) nl = listing.length();
        String line = listing.substring(idx, nl);
        line.trim();

        // Split "filename  (1234 B)" into a label and a description
        // for the card - directories have no size suffix, so they
        // just get the whole line as the label with no description.
        String label = line, desc = "";
        int paren = line.lastIndexOf("  (");
        if (paren >= 0) {
            label = line.substring(0, paren);
            desc = line.substring(paren + 2);
        }
        ui_draw_list_row(row_x, y, row_w, row_h, label.c_str(), desc.length() ? desc.c_str() : nullptr);
        y += row_h + row_gap;
        idx = nl + 1;
    }

    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "Total: %llu MB", sd_card_size_mb());
    ui_draw_centered_text(LCD_HEIGHT - 30, COLOR_TEXT_DIM, sbuf, 1);
#else
    ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "SD not available", 2);
#endif
}

static void files_touch(int x, int y, bool pressed) {
    static bool s_prev_pressed = false;
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;
    if (x < 54 && y < 54) { ui_pop_screen(); return; }
}

static void files_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) ui_pop_screen();
}

Screen files_screen = {
    "Files", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, files_draw, files_touch, nullptr, nullptr, files_gesture
};
