#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_touch.h"
#include "hal_imu.h"
#include "hal_power.h"
#include "hal_audio.h"
#include <Arduino_GFX_Library.h>
#include <string.h>

// =========================================================================
//  Test Menu screen
// =========================================================================

enum TestId { TEST_NONE, TEST_DISPLAY, TEST_TOUCH, TEST_SPEAKER, TEST_IMU, TEST_BATTERY };
static TestId s_active_test = TEST_NONE;

static void tests_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    // No manual header/back-button draw anywhere in this function - the
    // framework already draws one automatically every frame for any
    // Screen with a non-null title (see ui_render() in ui.cpp), so the
    // corner back arrow shows up in both the test list and an active
    // test without this screen needing to draw its own copy of it.

    if (s_active_test != TEST_NONE) {
        if (s_active_test == TEST_DISPLAY) {
            g->fillScreen(COLOR_ACCENT);
            ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT, "Display OK", 4);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 50, COLOR_TEXT_DIM, "Tap back", 2);
        }
        else if (s_active_test == TEST_TOUCH) {
            TouchPoint tp = touch_read();
            g->fillScreen(COLOR_BG);
            ui_draw_centered_text(60, COLOR_TEXT, "Touch Test", 3);
            if (tp.touched) {
                g->fillCircle(tp.x, tp.y, 20, COLOR_GOOD);
                char buf[32];
                snprintf(buf, sizeof(buf), "X:%d Y:%d", tp.x, tp.y);
                ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT, buf, 2);
            } else {
                ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "Touch screen", 2);
            }
        }
        else if (s_active_test == TEST_SPEAKER) {
            g->fillScreen(COLOR_BG);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_TEXT, "Speaker Test", 3);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 20, COLOR_TEXT_DIM, "Tap to beep", 2);
        }
        else if (s_active_test == TEST_IMU) {
            ImuSample s = imu_read();
            g->fillScreen(COLOR_BG);
            ui_draw_centered_text(60, COLOR_TEXT, "IMU Test", 3);
            char buf[64];
            snprintf(buf, sizeof(buf), "AX:%.2f AY:%.2f", s.ax, s.ay);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_TEXT, buf, 2);
            snprintf(buf, sizeof(buf), "AZ:%.2f", s.az);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 10, COLOR_TEXT, buf, 2);
            snprintf(buf, sizeof(buf), "Steps: %u", imu_get_step_count());
            ui_draw_centered_text(LCD_HEIGHT / 2 + 50, COLOR_TEXT_DIM, buf, 2);
        }
        else if (s_active_test == TEST_BATTERY) {
            PowerStatus p = power_read();
            g->fillScreen(COLOR_BG);
            ui_draw_centered_text(60, COLOR_TEXT, "Battery Test", 3);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d%%", p.battery_percent);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_GOOD, buf, 6);
            snprintf(buf, sizeof(buf), "%.2fV", p.battery_voltage_v);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 40, COLOR_TEXT_DIM, buf, 2);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 70, COLOR_TEXT_DIM,
                                  p.is_charging ? "Charging" : "Not charging", 2);
        }
        return;
    }

    struct TestEntry { const char *label; TestId id; uint16_t color; };
    TestEntry tests[] = {
        {"Display",  TEST_DISPLAY,  COLOR_ACCENT},
        {"Touch",    TEST_TOUCH,    COLOR_ACCENT2},
        {"Speaker",  TEST_SPEAKER,  COLOR_ACCENT3},
        {"IMU",      TEST_IMU,      COLOR_GOOD},
        {"Battery",  TEST_BATTERY,  COLOR_WARN},
    };
    int count = 5;
    int btn_w = 180;
    int btn_h = 50;
    int gap = 16;
    int cols = 2;
    int grid_w = cols * btn_w + (cols - 1) * gap;
    int start_x = (LCD_WIDTH - grid_w) / 2;
    int start_y = 100;

    for (int i = 0; i < count; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx = start_x + col * (btn_w + gap);
        int by = start_y + row * (btn_h + gap);
        g->fillRoundRect(bx, by, btn_w, btn_h, 12, tests[i].color);
        g->setTextSize(2);
        g->setTextColor(COLOR_TEXT);
        int lw = ui_text_width(tests[i].label, 2);
        ui_print(bx + (btn_w - lw) / 2, by + 16, 2, COLOR_TEXT, tests[i].label);
    }
}

static void tests_touch(int x, int y, bool pressed) {
    static bool s_prev_pressed = false;
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;

    // If in a test, back button or tap actions
    if (s_active_test != TEST_NONE) {
        if (x < 54 && y < 54) { s_active_test = TEST_NONE; return; }
        if (s_active_test == TEST_SPEAKER) {
            audio_beep(880, 200);
        }
        return;
    }

    if (x < 54 && y < 54) { ui_pop_screen(); return; }

    int btn_w = 180, btn_h = 50, gap = 16, cols = 2;
    int grid_w = cols * btn_w + (cols - 1) * gap;
    int start_x = (LCD_WIDTH - grid_w) / 2;
    int start_y = 100;
    TestId ids[] = {TEST_DISPLAY, TEST_TOUCH, TEST_SPEAKER, TEST_IMU, TEST_BATTERY};

    for (int i = 0; i < 5; i++) {
        int col = i % cols;
        int row = i / cols;
        int bx = start_x + col * (btn_w + gap);
        int by = start_y + row * (btn_h + gap);
        if (x >= bx && x <= bx + btn_w && y >= by && y <= by + btn_h) {
            s_active_test = ids[i];
            return;
        }
    }
}

static void tests_gesture(Gesture g) {
    if (s_active_test != TEST_NONE) { s_active_test = TEST_NONE; return; }
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) ui_pop_screen();
}

Screen test_menu_screen = {
    "Tests", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, tests_draw, tests_touch, nullptr, nullptr, tests_gesture,
    0, false, false, false,
    true, // no_drag_back - swipe right first closes a running test inside this screen
};
