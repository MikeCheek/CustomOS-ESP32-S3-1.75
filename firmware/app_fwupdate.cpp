#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_fwupdate.h"
#include "diag.h"
#include <Arduino_GFX_Library.h>
#include <stdio.h>

// =========================================================================
//  Settings > Software update: checks GitHub for a newer firmware on
//  demand and installs it (hal_fwupdate.h). Also opened by the automatic
//  check's "Update available" pop-up to show the download. Leaving the
//  screen doesn't stop a download; the watch restarts when it's done.
// =========================================================================

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;
static const int BTN_W = 220, BTN_H = 56, BTN_Y = CY + 96;
static bool s_open = false, s_back = false, s_btn_down = false;

bool fwup_screen_is_open() { return s_open; }

// Label of the button for the current state, or nullptr for none.
static const char *button_label(FwupState st) {
    switch (st) {
        case FWUP_IDLE:        return "Check now";
        case FWUP_UP_TO_DATE:  return "Check again";
        case FWUP_AVAILABLE:   return "Install";
        case FWUP_DOWNLOADING: return "Cancel";
        case FWUP_FAILED:      return "Try again";
        default:               return nullptr;
    }
}

static void fwup_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    FwupState st = fwup_state();
    char line[64];

    if (st == FWUP_DOWNLOADING || st == FWUP_DONE) {
        uint32_t total = fwup_total();
        float frac = st == FWUP_DONE ? 1.0f : total ? (float)fwup_written() / (float)total : 0.0f;
        ui_edge_ring(0, 360, COLOR_PANEL, 12, 6, false);
        if (frac > 0.002f) ui_edge_ring(0, 360.0f * frac, st == FWUP_DONE ? COLOR_GOOD : COLOR_ACCENT, 12, 6, true);
    }

    snprintf(line, sizeof(line), "Installed: %s", diag_fw_version());
    ui_text_center(CX, CY - 112, COLOR_TEXT_DIM, line, 1);

    switch (st) {
        case FWUP_IDLE:
            ui_text_center(CX, CY - 30, COLOR_TEXT, "Check for a newer", 2);
            ui_text_center(CX, CY + 2, COLOR_TEXT, "firmware on GitHub", 2);
            ui_text_center(CX, CY + 50, COLOR_TEXT_DIM, "Also checked daily on Wi-Fi", 1);
            break;
        case FWUP_CONNECTING:
            ui_text_center(CX, CY - 10, COLOR_TEXT, "Connecting to Wi-Fi...", 2);
            break;
        case FWUP_CHECKING:
            ui_text_center(CX, CY - 10, COLOR_TEXT, "Checking...", 2);
            break;
        case FWUP_UP_TO_DATE:
            ui_text_center(CX, CY - 20, COLOR_GOOD, "You're up to date", 2);
            if (fwup_latest_version()[0]) {
                snprintf(line, sizeof(line), "Latest release: %s", fwup_latest_version());
                ui_text_center(CX, CY + 20, COLOR_TEXT_DIM, line, 1);
            } else {
                ui_text_center(CX, CY + 20, COLOR_TEXT_DIM, "No releases published yet", 1);
            }
            break;
        case FWUP_AVAILABLE:
            ui_text_center(CX, CY - 40, COLOR_TEXT_DIM, "New version", 1);
            ui_text_center(CX, CY - 8, COLOR_TEXT, fwup_latest_version(), 4);
            if (fwup_total()) {
                snprintf(line, sizeof(line), "%lu KB download", (unsigned long)(fwup_total() / 1024));
                ui_text_center(CX, CY + 40, COLOR_TEXT_DIM, line, 1);
            }
            break;
        case FWUP_DOWNLOADING: {
            uint32_t total = fwup_total();
            int pct = total ? (int)((uint64_t)fwup_written() * 100 / total) : 0;
            snprintf(line, sizeof(line), "%d%%", pct);
            ui_text_center(CX, CY - 24, COLOR_TEXT, line, 5);
            snprintf(line, sizeof(line), "Installing %s - %lu / %lu KB", fwup_latest_version(),
                     (unsigned long)(fwup_written() / 1024), (unsigned long)(total / 1024));
            ui_fit(line, 1, 320);
            ui_text_center(CX, CY + 36, COLOR_TEXT_DIM, line, 1);
            break;
        }
        case FWUP_DONE:
            ui_text_center(CX, CY - 20, COLOR_GOOD, "Update installed", 3);
            ui_text_center(CX, CY + 24, COLOR_TEXT_DIM, "Restarting...", 2);
            break;
        case FWUP_FAILED:
            ui_text_center(CX, CY - 40, COLOR_BAD, fwup_failed_installing() ? "Update failed" : "Couldn't check", 2);
            snprintf(line, sizeof(line), "%s", fwup_error());
            ui_fit(line, 1, 320);
            ui_text_center(CX, CY, COLOR_TEXT, line, 1);
            if (fwup_failed_installing()) {
                snprintf(line, sizeof(line), "Still on firmware %s", diag_fw_version());
                ui_text_center(CX, CY + 28, COLOR_TEXT_DIM, line, 1);
            }
            break;
    }

    const char *btn = button_label(st);
    if (btn) {
        uint16_t c = st == FWUP_AVAILABLE ? COLOR_ACCENT : COLOR_PANEL;
        g->fillRoundRect(CX - BTN_W / 2, BTN_Y, BTN_W, BTN_H, BTN_H / 2, s_btn_down ? ui_dim(c, 0.6f) : c);
        ui_text_center(CX, BTN_Y + BTN_H / 2, COLOR_TEXT, btn, 2);
    }
}

static void on_button(FwupState st) {
    switch (st) {
        case FWUP_IDLE:
        case FWUP_UP_TO_DATE:
            fwup_check();
            break;
        case FWUP_AVAILABLE:
            fwup_install();
            break;
        case FWUP_DOWNLOADING:
            ui_show_confirm("Cancel update?", "The watch keeps its current firmware.", "Stop", "Continue",
                            [](bool yes) { if (yes) fwup_cancel(); });
            break;
        case FWUP_FAILED:
            fwup_dismiss();
            fwup_check();
            break;
        default:
            break;
    }
}

static void fwup_touch(int x, int y, bool pressed) {
    if (pressed && !s_back) s_back = (x < 54 && y < 54);
    if (s_back) { if (!pressed) { s_back = false; ui_pop_screen(); } return; }
    FwupState st = fwup_state();
    bool on = button_label(st) && x > CX - BTN_W / 2 - 10 && x < CX + BTN_W / 2 + 10 && y > BTN_Y - 8 && y < BTN_Y + BTN_H + 8;
    if (pressed) { s_btn_down = on; return; }
    if (s_btn_down && on) on_button(st);
    s_btn_down = false;
}

static void fwup_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

static void fwup_create() { s_open = true; s_back = s_btn_down = false; }
static void fwup_destroy() {
    s_open = false;
    fwup_dismiss();   // a shown result is done with; a running check/download carries on
}

Screen fwup_screen = {
    "Software update", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    fwup_create, fwup_draw, fwup_touch, nullptr, fwup_destroy, fwup_gesture,
    0,
    true,    // suppress_idle - stay awake while checking / installing
    false,
    false,
    false,
    true,    // hide_status - the progress ring uses the edge
};
