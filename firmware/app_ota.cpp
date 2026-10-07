#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_ota.h"
#include "diag.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>

// =========================================================================
//  Firmware update progress (pushed by hal_ota.cpp when the phone starts
//  sending an update). Stays on screen - and keeps the watch awake - until
//  it restarts into the new firmware, or the update fails.
// =========================================================================

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static bool s_cancel_down = false;
static bool s_open = false;

bool ota_screen_is_open() { return s_open; }

static void ota_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    OtaState st = ota_state();
    uint32_t total = ota_total();
    float frac = total ? (float)ota_written() / (float)total : 0.0f;

    ui_edge_ring(0, 360, COLOR_PANEL, 12, 6, false);
    uint16_t c = st == OTA_FAILED ? COLOR_BAD : st == OTA_DONE ? COLOR_GOOD : COLOR_ACCENT;
    if (st == OTA_DONE) frac = 1.0f;
    if (frac > 0.002f) ui_edge_ring(0, 360.0f * frac, c, 12, 6, true);

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100.0f + 0.5f));
    if (st == OTA_RECEIVING) {
        ui_text_center(CX, CY - 92, COLOR_TEXT_DIM, "Updating firmware", 2);
        ui_text_center(CX, CY - 10, COLOR_TEXT, pct, 6);
        char sz[32];
        snprintf(sz, sizeof(sz), "%lu / %lu KB", (unsigned long)(ota_written() / 1024), (unsigned long)(total / 1024));
        ui_text_center(CX, CY + 52, COLOR_TEXT_DIM, sz, 1);
        ui_text_center(CX, CY + 78, COLOR_TEXT_DIM, "Keep the phone close", 1);
        g->fillRoundRect(CX - 70, CY + 112, 140, 48, 24, s_cancel_down ? ui_dim(COLOR_PANEL, 0.6f) : COLOR_PANEL);
        ui_text_center(CX, CY + 136, COLOR_TEXT, "Cancel", 2);
    } else if (st == OTA_DONE) {
        ui_text_center(CX, CY - 30, COLOR_GOOD, "Update installed", 3);
        ui_text_center(CX, CY + 20, COLOR_TEXT_DIM, "Restarting...", 2);
    } else if (st == OTA_FAILED) {
        ui_text_center(CX, CY - 60, COLOR_BAD, "Update failed", 3);
        char why[48];
        snprintf(why, sizeof(why), "%s", ota_error());
        ui_fit(why, 1, 320);
        ui_text_center(CX, CY - 10, COLOR_TEXT, why, 1);
        char fw[40];
        snprintf(fw, sizeof(fw), "Still on firmware %s", diag_fw_version());
        ui_text_center(CX, CY + 22, COLOR_TEXT_DIM, fw, 1);
        ui_text_center(CX, CY + 80, COLOR_TEXT_DIM, "Tap to close", 1);
    } else {
        ui_text_center(CX, CY, COLOR_TEXT_DIM, "Waiting for the phone...", 2);
    }
}

static void ota_touch(int x, int y, bool pressed) {
    OtaState st = ota_state();
    if (st == OTA_RECEIVING) {
        bool on = x > CX - 80 && x < CX + 80 && y > CY + 104 && y < CY + 168;
        if (pressed) { s_cancel_down = on; return; }
        if (s_cancel_down && on) {
            ui_show_confirm("Cancel update?", "The watch keeps its current firmware.", "Stop", "Continue",
                            [](bool yes) { if (yes) ota_cancel(); });
        }
        s_cancel_down = false;
        return;
    }
    if (!pressed && (st == OTA_FAILED || st == OTA_IDLE)) {
        ota_cancel();   // FAILED -> IDLE
        s_open = false;
        ui_pop_screen();
    }
}

static void ota_create() { s_cancel_down = false; s_open = true; }
static void ota_destroy() { s_open = false; }   // popped, or covered by another screen

Screen ota_screen = {
    "Update", GESTURE_MODE_NONE,
    UI_FRAME_MS_DEFAULT,
    ota_create, ota_draw, ota_touch, nullptr, ota_destroy, nullptr,
    0,
    true,    // suppress_idle - stay awake while flashing
    false,
    true,    // no_transition
    true,    // no_drag_back
    true,    // hide_status
};
