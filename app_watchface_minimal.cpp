#include "config.h"
#include "complications.h"
#include "ui.h"
#include "ui_font.h"
#include "board_pins.h"
#include "app_menu.h"
#include "apps.h"
#include "hal_rtc.h"
#include "watchface_registry.h"
#include "app_pet.h"
#include <Arduino_GFX_Library.h>
#include <string.h>

// =========================================================================
//  Minimal Watchface (simple time display for registry)
// =========================================================================

static void watchface_minimal_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    pet_step_and_draw(g); // drawn first (and unconditionally) so it still
                           // animates even before RTC time is synced

    WatchTime t = rtc_now();
    if (!t.valid) return;

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.hour, t.minute);
    g->setTextSize(8);
    g->setTextColor(COLOR_TEXT);
    int tw = ui_text_width(buf, 8);
    ui_print((LCD_WIDTH - tw) / 2, LCD_HEIGHT / 2 - 32, 8, COLOR_TEXT, buf);

    char date_buf[32];
    snprintf(date_buf, sizeof(date_buf), "%02d/%02d/%04d", t.month, t.day, t.year);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    int dw = ui_text_width(date_buf, 2);
    ui_print((LCD_WIDTH - dw) / 2, LCD_HEIGHT / 2 + 40, 2, COLOR_TEXT_DIM, date_buf);

    comp_draw_slots(g, LCD_HEIGHT / 2 + 120, 38);
}

static void watchface_minimal_gesture(Gesture g) {
    switch (g) {
        case GESTURE_SWIPE_LEFT:
            ui_push(&menu_screen);
            break;
        case GESTURE_SWIPE_RIGHT:
            ui_push(&notifications_screen);
            break;
        case GESTURE_SWIPE_UP:
            watchface_cycle_next();
            break;
        default:
            break;
    }
}

static void watchface_minimal_touch(int x, int y, bool pressed) {
    pet_handle_touch(x, y, pressed);
}

Screen watchface_minimal_screen = {
    nullptr, GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, watchface_minimal_draw, watchface_minimal_touch,
    nullptr, nullptr, watchface_minimal_gesture,
    1000, // idle_frame_ms - a digital/analog clock face still needs to
          // visibly tick over once a second, just not redraw+flush at
          // 30fps while nothing's changed in between
};
