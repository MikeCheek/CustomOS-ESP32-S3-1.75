/*
 * app_calibration.cpp
 * Guided calibration for the touch panel's usable edge area and for
 * which physical tilt direction maps to which IMU axis/sign.
 *
 * Touch step: shows a target near each of the 4 cardinal edges of the
 * round display in turn. Tap-and-hold the target; while held, raw
 * (untransformed) touch readings are averaged via touch_read_raw().
 * From the left/right raw x extremes and top/bottom raw y extremes, a
 * per-axis linear transform (screen = a*raw + b) is solved and written
 * to AppSettings, replacing hal_touch.cpp's old hardcoded flip.
 *
 * Tilt step: shows a big arrow for each of the 4 directions in turn.
 * Touch-and-hold anywhere on screen while physically holding the watch
 * tilted that way; raw IMU ax/ay are averaged over the hold. Comparing
 * the up/down sample pair and the left/right sample pair identifies
 * which raw axis (ax or ay) moves for which logical direction, and
 * its sign - written to AppSettings as tilt_map_x/y_from + tilt_sign_x/y,
 * which hal_imu.cpp's imu_get_calibrated_tilt() then applies for every
 * game and for ui.cpp's auto-rotate.
 *
 * Auto-rotate is force-disabled for the whole time this screen is open
 * (tilting the board on purpose to calibrate would otherwise spin the
 * framebuffer mid-calibration) and restored on exit via on_destroy -
 * which fires whether the user finishes normally or backs out early.
 * Once restored, auto-rotate immediately benefits from whatever tilt
 * calibration was completed, since it reads the same AppSettings
 * fields through the same imu_get_calibrated_tilt() call every frame.
 */
#include "app_calibration.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "ui_font.h"
#include "hal_touch.h"
#include "hal_imu.h"
#include "hal_audio.h"
#include "app_settings_state.h"
#include "hal_nvs.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>

enum CalStep {
    CAL_CHOOSE,
    CAL_INTRO,
    CAL_TOUCH_TOP, CAL_TOUCH_RIGHT, CAL_TOUCH_BOTTOM, CAL_TOUCH_LEFT,
    CAL_TILT_INTRO,
    CAL_TILT_UP, CAL_TILT_DOWN, CAL_TILT_LEFT, CAL_TILT_RIGHT,
    CAL_DONE,
    CAL_ERROR,
};

enum CalMode { CAL_MODE_TOUCH, CAL_MODE_TILT, CAL_MODE_BOTH };
static CalMode s_mode = CAL_MODE_BOTH;

#define HOLD_MS_TOUCH   400
#define HOLD_MS_TILT    900
#define TOUCH_TARGET_TOL 70   // px — how close a tap must land to the target

// Inset from the true circle edge for each touch target, tappable
// without needing to reach the very rim of the round glass.
#define TOUCH_INSET_H   60
#define TOUCH_INSET_V   85

static CalStep s_step;
static const char *s_error_msg;
static CalStep s_error_retry_step; // which step to resume at after CAL_ERROR

static bool     s_holding;
static uint32_t s_hold_start_ms;
static float    s_sum_a, s_sum_b;   // generic accumulator (raw x/y or ax/ay)
static int      s_sample_count;

static bool  s_prev_auto_rotate;
// Rising-edge tracking for the simple "tap anywhere to advance" steps
// (CAL_CHOOSE/CAL_INTRO/CAL_TILT_INTRO/CAL_DONE/CAL_ERROR) below -
// those used to act on raw `pressed` directly, which fires on every
// single frame the finger stays down, not just once. A tap that
// advanced from CAL_CHOOSE to CAL_INTRO would then have that same
// still-ongoing physical press immediately re-trigger CAL_INTRO's own
// "if (pressed) advance" too, skipping straight through to the next
// step in one touch. The hold-based steps further down don't need
// this - they already guard their own start with "if (!s_holding)".
static bool  s_prev_pressed;

// Raw touch samples per edge (only the axis that matters for that edge
// is used, but both are stored for simplicity).
static float s_raw_top_y, s_raw_bottom_y, s_raw_left_x, s_raw_right_x;
// Raw tilt samples per direction.
static float s_up_ax, s_up_ay, s_down_ax, s_down_ay;
static float s_left_ax, s_left_ay, s_right_ax, s_right_ay;

static int target_x(CalStep step) {
    if (step == CAL_TOUCH_LEFT)  return TOUCH_INSET_H;
    if (step == CAL_TOUCH_RIGHT) return LCD_WIDTH - 1 - TOUCH_INSET_H;
    return LCD_WIDTH / 2;
}
static int target_y(CalStep step) {
    if (step == CAL_TOUCH_TOP)    return TOUCH_INSET_V;
    if (step == CAL_TOUCH_BOTTOM) return LCD_HEIGHT - 1 - TOUCH_INSET_V;
    return LCD_HEIGHT / 2;
}

static void reset_hold() {
    s_holding = false;
    s_sum_a = s_sum_b = 0;
    s_sample_count = 0;
}

static void enter_step(CalStep step) {
    s_step = step;
    reset_hold();
}

static void calibration_create() {
    s_prev_auto_rotate = g_app_settings.auto_rotate;
    g_app_settings.auto_rotate = false;
    ui_reset_auto_rotate_state();
    s_prev_pressed = false; // don't let a stale true from a previous session swallow this session's first tap
    enter_step(CAL_CHOOSE);
}

static void calibration_destroy() {
    g_app_settings.auto_rotate = s_prev_auto_rotate;
    ui_reset_auto_rotate_state();
}

// ---- Solve touch transform from the 4 collected edge samples --------------

static void finish_touch_calibration() {
    float raw_dx = s_raw_right_x - s_raw_left_x;
    float raw_dy = s_raw_bottom_y - s_raw_top_y;
    if (fabsf(raw_dx) < 20.0f || fabsf(raw_dy) < 20.0f) {
        s_error_msg = "Touch samples too close together - try again, tapping firmly nearer each edge.";
        s_error_retry_step = CAL_TOUCH_TOP;
        enter_step(CAL_ERROR);
        return;
    }

    float ax = (float)(target_x(CAL_TOUCH_RIGHT) - target_x(CAL_TOUCH_LEFT)) / raw_dx;
    float bx = (float)target_x(CAL_TOUCH_LEFT) - ax * s_raw_left_x;
    float ay = (float)(target_y(CAL_TOUCH_BOTTOM) - target_y(CAL_TOUCH_TOP)) / raw_dy;
    float by = (float)target_y(CAL_TOUCH_TOP) - ay * s_raw_top_y;

    g_app_settings.touch_cal_ax = ax;
    g_app_settings.touch_cal_bx = bx;
    g_app_settings.touch_cal_ay = ay;
    g_app_settings.touch_cal_by = by;
    g_app_settings.touch_calibrated = true;
    nvs_save_settings(g_app_settings);

    enter_step(s_mode == CAL_MODE_TOUCH ? CAL_DONE : CAL_TILT_INTRO);
}

// ---- Solve tilt axis mapping from the 4 collected direction samples -------

static void finish_tilt_calibration() {
    float vdax = s_up_ax - s_down_ax, vday = s_up_ay - s_down_ay; // vertical pair
    float hdax = s_right_ax - s_left_ax, hday = s_right_ay - s_left_ay; // horizontal pair

    const float MIN_DELTA = 0.12f; // g — below this, the tilt wasn't registered
    bool vert_on_ay = fabsf(vday) >= fabsf(vdax);
    bool horiz_on_ax = fabsf(hdax) >= fabsf(hday);
    float vert_delta = vert_on_ay ? vday : vdax;
    float horiz_delta = horiz_on_ax ? hdax : hday;

    int8_t map_y = vert_on_ay ? 1 : 0;
    int8_t map_x = horiz_on_ax ? 0 : 1;
    if (map_x == map_y) map_x = 1 - map_y; // disambiguate a degenerate case

    if (fabsf(vert_delta) < MIN_DELTA || fabsf(horiz_delta) < MIN_DELTA) {
        s_error_msg = "Tilt wasn't detected clearly - hold a firmer tilt next time.";
        s_error_retry_step = CAL_TILT_UP;
        enter_step(CAL_ERROR);
        return;
    }

    g_app_settings.tilt_map_y_from = map_y;
    g_app_settings.tilt_sign_y = (vert_delta > 0) ? 1 : -1;
    g_app_settings.tilt_map_x_from = map_x;
    g_app_settings.tilt_sign_x = (horiz_delta > 0) ? 1 : -1;
    g_app_settings.tilt_calibrated = true;
    nvs_save_settings(g_app_settings);

    enter_step(CAL_DONE);
}

// ---- Draw helpers -----------------------------------------------------

static void draw_target(Arduino_GFX *g, int x, int y, float progress) {
    g->drawCircle(x, y, 26, COLOR_TEXT_DIM);
    g->drawCircle(x, y, 18, COLOR_ACCENT2);
    g->fillCircle(x, y, 5, COLOR_TEXT);
    if (progress > 0.0f) {
        int r = (int)(26.0f * progress);
        g->drawCircle(x, y, r, COLOR_GOOD);
        g->drawCircle(x, y, r > 0 ? r - 1 : 0, COLOR_GOOD);
    }
}

static void draw_progress_ring(Arduino_GFX *g, int cx, int cy, float progress) {
    // Cheap ring: draw arcs as short radial ticks around the circle.
    int r = 70;
    int total_ticks = 36;
    int lit = (int)(progress * total_ticks);
    for (int i = 0; i < total_ticks; i++) {
        float a = (float)i / total_ticks * 2.0f * (float)M_PI - (float)M_PI / 2.0f;
        int x0 = cx + (int)(cosf(a) * (r - 6));
        int y0 = cy + (int)(sinf(a) * (r - 6));
        int x1 = cx + (int)(cosf(a) * r);
        int y1 = cy + (int)(sinf(a) * r);
        g->drawLine(x0, y0, x1, y1, i < lit ? COLOR_GOOD : COLOR_PANEL);
    }
}

// dir: 0=up, 1=down, 2=left, 3=right
static void draw_arrow(Arduino_GFX *g, int cx, int cy, int dir, uint16_t color) {
    const int SHAFT = 46, HEAD = 30;
    switch (dir) {
        case 0: // up
            g->fillRoundRect(cx - 6, cy - SHAFT, 12, SHAFT, 4, color);
            g->fillTriangle(cx, cy - SHAFT - HEAD, cx - HEAD, cy - SHAFT + 6, cx + HEAD, cy - SHAFT + 6, color);
            break;
        case 1: // down
            g->fillRoundRect(cx - 6, cy, 12, SHAFT, 4, color);
            g->fillTriangle(cx, cy + SHAFT + HEAD, cx - HEAD, cy + SHAFT - 6, cx + HEAD, cy + SHAFT - 6, color);
            break;
        case 2: // left
            g->fillRoundRect(cx - SHAFT, cy - 6, SHAFT, 12, 4, color);
            g->fillTriangle(cx - SHAFT - HEAD, cy, cx - SHAFT + 6, cy - HEAD, cx - SHAFT + 6, cy + HEAD, color);
            break;
        case 3: // right
            g->fillRoundRect(cx, cy - 6, SHAFT, 12, 4, color);
            g->fillTriangle(cx + SHAFT + HEAD, cy, cx + SHAFT - 6, cy - HEAD, cx + SHAFT - 6, cy + HEAD, color);
            break;
    }
}

static void calibration_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    float progress = s_holding
        ? (float)(millis() - s_hold_start_ms) / (float)(s_step >= CAL_TILT_INTRO ? HOLD_MS_TILT : HOLD_MS_TOUCH)
        : 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    switch (s_step) {
    case CAL_CHOOSE: {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 130, COLOR_TEXT, "Calibrate", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 90, COLOR_TEXT_DIM, "What do you want", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 62, COLOR_TEXT_DIM, "to calibrate?", 2);
        static const char *labels[3] = { "Touch Screen", "Tilt Sensors", "Both" };
        int by = LCD_HEIGHT / 2 - 10, bh = 56, gap = 14;
        for (int i = 0; i < 3; i++) {
            int y0 = by + i * (bh + gap);
            g->fillRoundRect(LCD_WIDTH / 2 - 110, y0, 220, bh, 12, COLOR_PANEL);
            g->drawRoundRect(LCD_WIDTH / 2 - 110, y0, 220, bh, 12, COLOR_ACCENT2);
            g->setTextSize(2);
            g->setTextColor(COLOR_TEXT);
            int tw = ui_text_width(labels[i], 2);
            ui_print(LCD_WIDTH / 2 - tw / 2, y0 + bh / 2 - 8, 2, COLOR_TEXT, labels[i]);
        }
        break;
    }

    case CAL_INTRO:
        ui_draw_centered_text(LCD_HEIGHT / 2 - 60, COLOR_TEXT, "Calibration", 3);
        if (s_mode == CAL_MODE_TOUCH) {
            ui_draw_centered_text(LCD_HEIGHT / 2 - 15, COLOR_TEXT_DIM, "Touch screen", 2);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 15, COLOR_TEXT_DIM, "calibration only.", 2);
        } else {
            ui_draw_centered_text(LCD_HEIGHT / 2 - 15, COLOR_TEXT_DIM, "Touch edges, then", 2);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 15, COLOR_TEXT_DIM, "tilt directions.", 2);
        }
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_ACCENT2, "Tap to begin", 2);
        break;

    case CAL_TOUCH_TOP: case CAL_TOUCH_RIGHT: case CAL_TOUCH_BOTTOM: case CAL_TOUCH_LEFT: {
        ui_draw_centered_text(70, COLOR_TEXT, "Touch Calibration", 2);
        const char *label =
            s_step == CAL_TOUCH_TOP    ? "Tap + hold TOP target" :
            s_step == CAL_TOUCH_RIGHT  ? "Tap + hold RIGHT target" :
            s_step == CAL_TOUCH_BOTTOM ? "Tap + hold BOTTOM target" :
                                          "Tap + hold LEFT target";
        ui_draw_centered_text(LCD_HEIGHT - 50, COLOR_TEXT_DIM, label, 2);
        draw_target(g, target_x(s_step), target_y(s_step), progress);
        break;
    }

    case CAL_TILT_INTRO:
        ui_draw_centered_text(LCD_HEIGHT / 2 - 40, COLOR_TEXT, "Tilt Calibration", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2, COLOR_TEXT_DIM, "Follow each arrow, then", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 28, COLOR_TEXT_DIM, "touch+hold anywhere", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 70, COLOR_ACCENT2, "Tap to begin", 2);
        break;

    case CAL_TILT_UP: case CAL_TILT_DOWN: case CAL_TILT_LEFT: case CAL_TILT_RIGHT: {
        int dir = s_step == CAL_TILT_UP ? 0 : s_step == CAL_TILT_DOWN ? 1 :
                  s_step == CAL_TILT_LEFT ? 2 : 3;
        const char *dir_word =
            s_step == CAL_TILT_UP ? "up" : s_step == CAL_TILT_DOWN ? "down" :
            s_step == CAL_TILT_LEFT ? "left" : "right";
        char instr[80];
        ui_draw_centered_text(70, COLOR_TEXT, "Tilt Calibration", 2);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 148, COLOR_TEXT_DIM, "Put the device so you can", 1);
        snprintf(instr, sizeof(instr), "see the displayed arrow pointed %s,", dir_word);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 132, COLOR_TEXT_DIM, instr, 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 116, COLOR_TEXT_DIM, "then press and hold anywhere", 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 - 100, COLOR_TEXT_DIM, "until it's done", 1);
        draw_arrow(g, LCD_WIDTH / 2, LCD_HEIGHT / 2, dir, s_holding ? COLOR_GOOD : COLOR_ACCENT3);
        if (s_holding) draw_progress_ring(g, LCD_WIDTH / 2, LCD_HEIGHT / 2, progress);
        if (s_holding) ui_draw_centered_text(LCD_HEIGHT - 60, COLOR_TEXT_DIM, "Hold still...", 2);
        break;
    }

    case CAL_DONE: {
        ui_draw_centered_text(LCD_HEIGHT / 2 - 70, COLOR_GOOD, "Calibrated!", 3);
        char buf[48];
        if (s_mode != CAL_MODE_TILT) {
            snprintf(buf, sizeof(buf), "Touch: a=%.3f/%.3f", g_app_settings.touch_cal_ax, g_app_settings.touch_cal_ay);
            ui_draw_centered_text(LCD_HEIGHT / 2 - 20, COLOR_TEXT_DIM, buf, 1);
        }
        if (s_mode != CAL_MODE_TOUCH) {
            snprintf(buf, sizeof(buf), "Tilt X<-%s(%+d) Y<-%s(%+d)",
                     g_app_settings.tilt_map_x_from ? "ay" : "ax", g_app_settings.tilt_sign_x,
                     g_app_settings.tilt_map_y_from ? "ay" : "ax", g_app_settings.tilt_sign_y);
            ui_draw_centered_text(LCD_HEIGHT / 2 + 10, COLOR_TEXT_DIM, buf, 1);
        }
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_ACCENT2, "Tap to finish", 2);
        break;
    }

    case CAL_ERROR:
        ui_draw_centered_text(LCD_HEIGHT / 2 - 30, COLOR_BAD, "Calibration issue", 3);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 10, COLOR_TEXT_DIM, s_error_msg ? s_error_msg : "", 1);
        ui_draw_centered_text(LCD_HEIGHT / 2 + 60, COLOR_ACCENT2, "Tap to retry this step", 2);
        break;
    }
}

// ---- Touch handling ---------------------------------------------------

static void calibration_touch(int x, int y, bool pressed) {
    // See s_prev_pressed's declaration above for why this exists -
    // used only by the simple tap-to-advance cases just below; the
    // hold-based steps further down keep using raw `pressed`.
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;

    switch (s_step) {
    case CAL_CHOOSE: {
        if (!tap_edge) return;
        int by = LCD_HEIGHT / 2 - 10, bh = 56, gap = 14;
        for (int i = 0; i < 3; i++) {
            int y0 = by + i * (bh + gap);
            if (x >= LCD_WIDTH / 2 - 110 && x <= LCD_WIDTH / 2 + 110 && y >= y0 && y <= y0 + bh) {
                if (i == 0) { s_mode = CAL_MODE_TOUCH; enter_step(CAL_INTRO); }
                else if (i == 1) { s_mode = CAL_MODE_TILT; enter_step(CAL_TILT_INTRO); }
                else { s_mode = CAL_MODE_BOTH; enter_step(CAL_INTRO); }
                return;
            }
        }
        return;
    }
    case CAL_INTRO:
        if (tap_edge) enter_step(CAL_TOUCH_TOP);
        return;
    case CAL_TILT_INTRO:
        if (tap_edge) enter_step(CAL_TILT_UP);
        return;
    case CAL_DONE:
        if (tap_edge) ui_pop_screen();
        return;
    case CAL_ERROR:
        if (tap_edge) {
            // Retry: resume at the start of whichever phase failed.
            enter_step(s_error_retry_step);
        }
        return;
    default:
        break;
    }

    // Touch steps: only start a hold if the tap landed near the target.
    if (s_step == CAL_TOUCH_TOP || s_step == CAL_TOUCH_RIGHT ||
        s_step == CAL_TOUCH_BOTTOM || s_step == CAL_TOUCH_LEFT) {
        if (pressed) {
            if (!s_holding) {
                int dx = x - target_x(s_step), dy = y - target_y(s_step);
                if (dx * dx + dy * dy > TOUCH_TARGET_TOL * TOUCH_TARGET_TOL) return;
                s_holding = true;
                s_hold_start_ms = millis();
                s_sum_a = s_sum_b = 0;
                s_sample_count = 0;
            }
        } else {
            reset_hold(); // released early (or after capture) - either way, clear
        }
        return;
    }

    // Tilt steps: any touch anywhere starts/stops the hold.
    if (pressed) {
        if (!s_holding) {
            s_holding = true;
            s_hold_start_ms = millis();
            s_sum_a = s_sum_b = 0;
            s_sample_count = 0;
        }
    } else {
        reset_hold();
    }
}

// ---- Tick: accumulate samples while holding, advance on completion -------

static void advance_touch_step() {
    float avg_a = s_sample_count > 0 ? s_sum_a / s_sample_count : 0;
    float avg_b = s_sample_count > 0 ? s_sum_b / s_sample_count : 0;
    if (s_step == CAL_TOUCH_TOP)    s_raw_top_y = avg_b;
    if (s_step == CAL_TOUCH_BOTTOM) s_raw_bottom_y = avg_b;
    if (s_step == CAL_TOUCH_LEFT)   s_raw_left_x = avg_a;
    if (s_step == CAL_TOUCH_RIGHT)  s_raw_right_x = avg_a;

    if (s_step == CAL_TOUCH_TOP) enter_step(CAL_TOUCH_RIGHT);
    else if (s_step == CAL_TOUCH_RIGHT) enter_step(CAL_TOUCH_BOTTOM);
    else if (s_step == CAL_TOUCH_BOTTOM) enter_step(CAL_TOUCH_LEFT);
    else finish_touch_calibration();
}

static void advance_tilt_step() {
    // Non-blocking (audio_play_sfx, not audio_beep) so this doesn't
    // stall the progress-ring animation or the next step's draw calls -
    // a short confirmation chirp per completed step.
    audio_play_sfx(1200, 80);

    float avg_ax = s_sample_count > 0 ? s_sum_a / s_sample_count : 0;
    float avg_ay = s_sample_count > 0 ? s_sum_b / s_sample_count : 0;
    if (s_step == CAL_TILT_UP)    { s_up_ax = avg_ax;    s_up_ay = avg_ay; }
    if (s_step == CAL_TILT_DOWN)  { s_down_ax = avg_ax;  s_down_ay = avg_ay; }
    if (s_step == CAL_TILT_LEFT)  { s_left_ax = avg_ax;  s_left_ay = avg_ay; }
    if (s_step == CAL_TILT_RIGHT) { s_right_ax = avg_ax; s_right_ay = avg_ay; }

    if (s_step == CAL_TILT_UP) enter_step(CAL_TILT_DOWN);
    else if (s_step == CAL_TILT_DOWN) enter_step(CAL_TILT_LEFT);
    else if (s_step == CAL_TILT_LEFT) enter_step(CAL_TILT_RIGHT);
    else finish_tilt_calibration();
}

static void calibration_tick() {
    if (!s_holding) return;

    bool is_touch_phase = (s_step == CAL_TOUCH_TOP || s_step == CAL_TOUCH_RIGHT ||
                            s_step == CAL_TOUCH_BOTTOM || s_step == CAL_TOUCH_LEFT);
    bool is_tilt_phase = (s_step == CAL_TILT_UP || s_step == CAL_TILT_DOWN ||
                           s_step == CAL_TILT_LEFT || s_step == CAL_TILT_RIGHT);

    if (is_touch_phase) {
        uint16_t rx, ry;
        if (touch_read_raw(rx, ry)) {
            s_sum_a += rx;
            s_sum_b += ry;
            s_sample_count++;
        }
        if (millis() - s_hold_start_ms >= HOLD_MS_TOUCH) {
            advance_touch_step();
        }
    } else if (is_tilt_phase) {
        ImuSample s = imu_read();
        if (s.valid) {
            s_sum_a += s.ax;
            s_sum_b += s.ay;
            s_sample_count++;
        }
        if (millis() - s_hold_start_ms >= HOLD_MS_TILT) {
            advance_tilt_step();
        }
    }
}

static void calibration_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_LEFT || g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_UP) {
        ui_pop_screen();
    }
}

Screen calibration_screen = {
    "Calibrate", GESTURE_MODE_FREE,
    UI_FRAME_MS_DEFAULT,
    calibration_create, calibration_draw, calibration_touch, calibration_tick,
    calibration_destroy, calibration_gesture,
};
