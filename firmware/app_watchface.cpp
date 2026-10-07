#include "app_watchface.h"
#include "complications.h"
#include "app_menu.h"
#include "watchface_registry.h"
#include "config.h"
#include "ui_font.h"
#include "hal_rtc.h"
#include "hal_power.h"
#include "hal_imu.h"
#include "hal_display.h"
#include "board_pins.h"
#include "app_pet.h"
#include <Arduino_GFX_Library.h>

#if FEATURE_BLE
#include "hal_ble.h"
#endif
#include "app_quicksettings.h"

// ---- Eye geometry --------------------------------------------------------
static const int32_t EYE_W = 26;
static const int32_t EYE_H = 34;
static const int32_t PUPIL_D = 12;
static const int32_t GAZE_MAX = 6;
static const int32_t EYES_Y = LCD_HEIGHT / 2 - 108;

static int32_t s_eye_h = EYE_H;
static int32_t s_gaze_dx = 0;
static int32_t s_gaze_dy = 0;
static int32_t s_glance_ticks = 0;

// Eye animation state
static bool     s_anim_active = false;
static uint32_t s_anim_start_ms = 0;
static uint16_t s_anim_duration = 0;
static int32_t  s_anim_from = 0;
static int32_t  s_anim_to = 0;

// Touch tracking for eye gaze
static bool     s_finger_down = false;
static uint16_t s_finger_x = 0;
static uint16_t s_finger_y = 0;

static const char *WEEKDAYS[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *MONTHS[] = {"","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

// ---- Eye animation helpers -----------------------------------------------
static void animate_eyes_to(int32_t target, uint16_t ms) {
    s_anim_active = true;
    s_anim_start_ms = millis();
    s_anim_duration = ms;
    s_anim_from = s_eye_h;
    s_anim_to = target;
}

static void eye_blink() {
    animate_eyes_to(4, 70);
    // Schedule reopen after blink close
    // We'll handle this in tick by checking elapsed time
}

static void eye_surprised() {
    animate_eyes_to(EYE_H + 6, 60);
}

static void update_eye_animation() {
    if (!s_anim_active) return;
    uint32_t elapsed = millis() - s_anim_start_ms;
    if (elapsed >= s_anim_duration) {
        s_eye_h = s_anim_to;
        s_anim_active = false;
        // If we just blinked closed, reopen
        if (s_eye_h == 4) {
            animate_eyes_to(EYE_H, 90);
        }
        // If we just widened, relax back after a delay
        else if (s_eye_h == EYE_H + 6) {
            // Will be handled by next tick
        }
        return;
    }
    // Linear interpolation
    float t = (float)elapsed / (float)s_anim_duration;
    s_eye_h = s_anim_from + (int32_t)((s_anim_to - s_anim_from) * t);
}

// ---- Drawing -------------------------------------------------------------
static void draw_eyes(Arduino_GFX *g) {
    int32_t cx = LCD_WIDTH / 2;
    int32_t cy = EYES_Y;
    int32_t gap = 8;
    int32_t pair_w = EYE_W * 2 + gap;

    for (int i = 0; i < 2; i++) {
        int32_t eye_x = cx - pair_w / 2 + i * (EYE_W + gap);
        int32_t eye_y = cy - s_eye_h / 2;

        // White sclera (rounded rect)
        g->fillRoundRect(eye_x, eye_y, EYE_W, s_eye_h, 10, COLOR_TEXT);

        // Pupil (centered within the eye, offset by gaze)
        int32_t px = eye_x + EYE_W / 2 - PUPIL_D / 2 + s_gaze_dx;
        int32_t py = eye_y + s_eye_h / 2 - PUPIL_D / 2 + s_gaze_dy;
        g->fillCircle(px + PUPIL_D / 2, py + PUPIL_D / 2, PUPIL_D / 2, COLOR_ACCENT);
    }
}

static void draw_seconds_arc(Arduino_GFX *g, uint8_t second) {
    // Draw a filled arc segment for each second
    // Simple approach: draw a thin arc from 0 to current second angle
    int cx = LCD_WIDTH / 2;
    int cy = LCD_HEIGHT / 2;
    int r = LCD_WIDTH / 2 - 8;
    int thickness = 4;

    if (second == 0) return;

    int end_angle = (int)second * 6; // 360/60 = 6 degrees per second

    // Draw arc as a series of small line segments
    for (int a = 0; a < end_angle; a += 2) {
        float rad = (float)(a - 90) * 3.14159f / 180.0f;
        float rad2 = (float)(a + 2 - 90) * 3.14159f / 180.0f;
        int x1 = cx + (int)(cosf(rad) * (r - thickness));
        int y1 = cy + (int)(sinf(rad) * (r - thickness));
        int x2 = cx + (int)(cosf(rad) * (r + thickness));
        int y2 = cy + (int)(sinf(rad) * (r + thickness));
        int x3 = cx + (int)(cosf(rad2) * (r + thickness));
        int y3 = cy + (int)(sinf(rad2) * (r + thickness));
        int x4 = cx + (int)(cosf(rad2) * (r - thickness));
        int y4 = cy + (int)(sinf(rad2) * (r - thickness));
        g->fillTriangle(x1, y1, x2, y2, x3, y3, COLOR_ACCENT);
        g->fillTriangle(x1, y1, x3, y3, x4, y4, COLOR_ACCENT);
    }
}

static void draw_time(Arduino_GFX *g) {
    WatchTime t = rtc_now();
    if (!t.valid) return;

    // Time - large centered text
    char time_buf[8];
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d", t.hour, t.minute);

    g->setTextSize(6);
    g->setTextColor(COLOR_TEXT);
    int tw = ui_text_width(time_buf, 6);
    ui_print((LCD_WIDTH - tw) / 2, LCD_HEIGHT / 2 - 30, 6, COLOR_TEXT, time_buf);

    // Date - small centered below time
    char date_buf[32];
    snprintf(date_buf, sizeof(date_buf), "%s %d %s %d",
             WEEKDAYS[t.weekday % 7], t.day, MONTHS[t.month], t.year);
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    int dw = ui_text_width(date_buf, 2);
    ui_print((LCD_WIDTH - dw) / 2, LCD_HEIGHT / 2 + 20, 2, COLOR_TEXT_DIM, date_buf);

    // Seconds arc
    draw_seconds_arc(g, t.second);
}

static void draw_battery(Arduino_GFX *g) {
    int pct = power_get_battery_percent();
    char buf[32];
    if (pct >= 0) {
        snprintf(buf, sizeof(buf), "BAT %d%%", pct);
    } else {
        snprintf(buf, sizeof(buf), "BAT --");
    }
#if FEATURE_BLE
    if (ble_is_connected()) {
        strncat(buf, "  BLE", sizeof(buf) - strlen(buf) - 1);
    }
#endif
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    int bw = ui_text_width(buf, 2);
    ui_print((LCD_WIDTH - bw) / 2, LCD_HEIGHT - 55, 2, COLOR_TEXT_DIM, buf);
}

static void draw_steps(Arduino_GFX *g) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%u steps", imu_get_step_count());
    g->setTextSize(2);
    g->setTextColor(COLOR_TEXT_DIM);
    int sw = ui_text_width(buf, 2);
    ui_print((LCD_WIDTH - sw) / 2, LCD_HEIGHT / 2 + 44, 2, COLOR_TEXT_DIM, buf);
}

// ---- Screen callbacks -----------------------------------------------------
static void watchface_create() {
    s_eye_h = EYE_H;
    s_gaze_dx = 0;
    s_gaze_dy = 0;
    s_glance_ticks = 0;
    s_finger_down = false;
    app_watchface_tick();
}

static void watchface_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    draw_seconds_arc(g, rtc_now().second);
    draw_eyes(g);
    draw_time(g);
    draw_battery(g);
    bool steps_in_slot = false;
    for (int i = 0; i < COMP_SLOTS; i++) steps_in_slot |= comp_slot(i) == COMP_STEPS;
    if (!steps_in_slot) draw_steps(g);
    comp_draw_slots(g, LCD_HEIGHT / 2 + 122, 36);
    pet_step_and_draw(g);
}

static void watchface_touch(int x, int y, bool pressed) {
    if (pet_handle_touch(x, y, pressed)) return; // dragging the pet takes priority

    if (pressed) {
        if (!s_finger_down) {
            eye_surprised();
        }
        s_finger_down = true;
        s_finger_x = x;
        s_finger_y = y;
    } else {
        s_finger_down = false;
        s_gaze_dx = 0;
        s_gaze_dy = 0;
    }
}

static void watchface_tick() {
    // Eye animation
    update_eye_animation();

    // Finger tracking for eye gaze
    if (s_finger_down) {
        int32_t center_x = LCD_WIDTH / 2;
        int32_t center_y = EYES_Y;
        int32_t dx = ((int32_t)s_finger_x - center_x) / 10;
        int32_t dy = ((int32_t)s_finger_y - center_y) / 10;
        if (dx > GAZE_MAX) dx = GAZE_MAX;
        if (dx < -GAZE_MAX) dx = -GAZE_MAX;
        if (dy > GAZE_MAX) dy = GAZE_MAX;
        if (dy < -GAZE_MAX) dy = -GAZE_MAX;
        s_gaze_dx = dx;
        s_gaze_dy = dy;
    }

    // Random idle glances
    if (!s_finger_down && s_glance_ticks > 0) {
        s_glance_ticks--;
        if (s_glance_ticks == 0) {
            s_gaze_dx = 0;
            s_gaze_dy = 0;
        }
    } else if (!s_finger_down && !s_anim_active) {
        int r = random(10);
        if (r == 0) {
            eye_blink();
        } else if (r == 1) {
            s_gaze_dx = (int32_t)random(-GAZE_MAX, GAZE_MAX + 1);
            s_gaze_dy = 0;
            s_glance_ticks = 2;
        }
    }

    // Update battery and steps on each second tick
    imu_step_counter_update(imu_read());
}

static void watchface_destroy() {
    s_finger_down = false;
}

// ---- Gesture handling ----------------------------------------------------
extern Screen menu_screen;
extern Screen notifications_screen;

static void watchface_gesture(Gesture g) {
    switch (g) {
        case GESTURE_SWIPE_RIGHT:
            ui_push(&notifications_screen);
            break;
        case GESTURE_SWIPE_LEFT:
            ui_push(&menu_screen);
            break;
        case GESTURE_SWIPE_UP:
            // Cycle to next watchface
            {
                int next = watchface_current() + 1;
                if (watchface_get(next)) {
                    watchface_set_current(next);
                    ui_go_home();
                    const WatchfaceEntry *wf = watchface_get(watchface_current());
                    if (wf && wf->screen) ui_push(wf->screen);
                }
            }
            break;
        default:
            break;
    }
}

Screen watchface_screen = {
    nullptr,                // title (root screen, no back button)
    GESTURE_MODE_EDGE,      // gesture_mode
    UI_FRAME_MS_SMOOTH,     // 60fps for eye animations
    watchface_create,
    watchface_draw,
    watchface_touch,
    watchface_tick,
    watchface_destroy,
    watchface_gesture,
};

void app_watchface_tick() {
    watchface_tick();
}
