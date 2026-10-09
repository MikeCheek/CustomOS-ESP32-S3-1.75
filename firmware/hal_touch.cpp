#include "hal_touch.h"
#include "board_pins.h"
#include "config.h"
#include "ui.h"
#include "app_settings_state.h"
#include <Wire.h>
#include <math.h>

static bool s_last_touched = false;
static uint16_t s_last_x = 0, s_last_y = 0;
static uint32_t s_last_valid_ms = 0;
static const uint32_t GRACE_MS = 100;

static uint32_t s_last_release_ms = 0;
// A press within this long of a release is held back: it's usually the
// controller glitching mid-gesture, not a new tap. Screens that need
// quick repeated taps (the T9 keypad) shorten it - see touch_set_fast_taps().
static const uint32_t RELEASE_COOLDOWN_MS = 240;
static const uint32_t FAST_TAP_COOLDOWN_MS = 40;
static uint32_t s_release_cooldown_ms = RELEASE_COOLDOWN_MS;

// Gesture mode — set by ui.cpp when the active screen changes
static uint8_t s_gesture_mode = 0; // 0=NONE, 1=EDGE, 2=FREE

#define SWIPE_MIN_DX        55
#define SWIPE_MIN_DY        55
#define SWIPE_MAX_CROSS     50
#define SWIPE_MAX_TIME_MS   500
#define SWIPE_DOWN_MIN_DY   50
#define SWIPE_DOWN_START_Y   100
#define SWIPE_EDGE_LEFT_X    70
#define SWIPE_EDGE_RIGHT_X   (LCD_WIDTH - 70)
#define SCREEN_CX (LCD_WIDTH / 2)
#define SCREEN_CY (LCD_HEIGHT / 2)
#define SCREEN_R  (LCD_WIDTH / 2)

static bool is_on_screen(uint16_t x, uint16_t y) {
    int32_t dx = (int32_t)x - SCREEN_CX;
    int32_t dy = (int32_t)y - SCREEN_CY;
    return (dx * dx + dy * dy) <= (int32_t)SCREEN_R * SCREEN_R;
}

static uint16_t s_swipe_start_x = 0, s_swipe_start_y = 0;
static uint32_t s_swipe_start_ms = 0;
static bool s_swipe_tracking = false;
static bool s_swipe_right_pending = false;
static bool s_swipe_down_pending = false;
static bool s_swipe_left_pending = false;
static bool s_swipe_up_pending = false;

// Minimum gap between two classified swipes. Without this, a touch
// controller glitch mid-gesture (a single dropped/garbage I2C frame
// reporting "no touch" for an instant while the finger is still down)
// can look like a release, get classified as a completed swipe from
// the partial motion so far, and then the finger landing back on the
// glass starts a fresh touch-down that finishes the same physical
// gesture as a SECOND swipe — i.e. one real swipe, two fired events.
#define SWIPE_COOLDOWN_MS 350
static uint32_t s_last_swipe_ms = 0;

static bool s_touch_pending = false;
static uint16_t s_pending_x = 0, s_pending_y = 0;
static uint32_t s_pending_ms = 0;
static const uint32_t PENDING_TIMEOUT_MS = 150;

static bool cst_read_regs(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(TP_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    uint8_t got = Wire.requestFrom((int)TP_I2C_ADDR, (int)len);
    if (got != len) return false;
    for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

void touch_init() {
    if (PIN_TP_RST >= 0) {
        pinMode(PIN_TP_RST, OUTPUT);
        digitalWrite(PIN_TP_RST, LOW);
        delay(10);
        digitalWrite(PIN_TP_RST, HIGH);
        delay(50);
    }
    if (PIN_TP_INT >= 0) {
        pinMode(PIN_TP_INT, INPUT);
    }
    DEBUG_PRINTF("[touch] CST9217 init at 0x%02X\n", TP_I2C_ADDR);
}

void touch_set_fast_taps(bool on) {
    s_release_cooldown_ms = on ? FAST_TAP_COOLDOWN_MS : RELEASE_COOLDOWN_MS;
}

void touch_set_gesture_mode(uint8_t mode) {
    s_gesture_mode = mode;
}

static void evaluate_swipe(uint32_t now) {
    if (!s_swipe_tracking) return;
    s_swipe_tracking = false;

    if (now - s_last_swipe_ms < SWIPE_COOLDOWN_MS) return;

    int32_t dx = (int32_t)s_last_x - (int32_t)s_swipe_start_x;
    int32_t dy = (int32_t)s_last_y - (int32_t)s_swipe_start_y;
    uint32_t dt = now - s_swipe_start_ms;

    if (dt >= SWIPE_MAX_TIME_MS) return;

    int32_t abs_dx = dx < 0 ? -dx : dx;
    int32_t abs_dy = dy < 0 ? -dy : dy;

    bool edge = (s_gesture_mode == 1); // GESTURE_EDGE
    bool on_screen = is_on_screen(s_swipe_start_x, s_swipe_start_y);

    if (dx > SWIPE_MIN_DX && abs_dy < SWIPE_MAX_CROSS) {
        if (!edge || (s_swipe_start_x < SWIPE_EDGE_LEFT_X && on_screen)) {
            s_swipe_right_pending = true;
            s_last_swipe_ms = now;
            return;
        }
    }
    if (dx < -SWIPE_MIN_DX && abs_dy < SWIPE_MAX_CROSS) {
        if (!edge || (s_swipe_start_x > SWIPE_EDGE_RIGHT_X && on_screen)) {
            s_swipe_left_pending = true;
            s_last_swipe_ms = now;
            return;
        }
    }
    if (dy < -SWIPE_MIN_DY && abs_dx < SWIPE_MAX_CROSS) {
        s_swipe_up_pending = true;
        s_last_swipe_ms = now;
        return;
    }
    if (dy > SWIPE_DOWN_MIN_DY && abs_dx < SWIPE_MAX_CROSS) {
        if (!edge || s_swipe_start_y < SWIPE_DOWN_START_Y) {
            s_swipe_down_pending = true;
            s_last_swipe_ms = now;
            return;
        }
    }
}

TouchPoint touch_read() {
    TouchPoint tp{false, 0, 0};

    uint8_t buf[7] = {0};
    bool ok = cst_read_regs(0x00, buf, sizeof(buf));
    uint32_t now = millis();

    bool frame_ok = ok && buf[6] == 0xAB;
    uint8_t touch_count = frame_ok ? (buf[0] & 0x0F) : 0;

    if (frame_ok && touch_count > 0) {
        uint16_t raw_x = ((uint16_t)buf[1] << 4) | (buf[3] >> 4);
        uint16_t raw_y = ((uint16_t)buf[2] << 4) | (buf[3] & 0x0F);

        // Sanity bound against a corrupt/noise I2C read only — the real
        // edge extents come from calibration below, not this constant,
        // so this is deliberately looser than [0, LCD_WIDTH).
        if (raw_x < 4096 && raw_y < 4096) {
            int xf = (int)lroundf(g_app_settings.touch_cal_ax * (float)raw_x + g_app_settings.touch_cal_bx);
            int yf = (int)lroundf(g_app_settings.touch_cal_ay * (float)raw_y + g_app_settings.touch_cal_by);
            if (xf < 0) xf = 0; else if (xf > LCD_WIDTH - 1) xf = LCD_WIDTH - 1;
            if (yf < 0) yf = 0; else if (yf > LCD_HEIGHT - 1) yf = LCD_HEIGHT - 1;
            uint16_t x = (uint16_t)xf;
            uint16_t y = (uint16_t)yf;

            if (s_last_touched) {
                s_last_touched = true;
                s_last_x = x;
                s_last_y = y;
                s_last_valid_ms = now;
                s_touch_pending = false;
                tp.touched = true;
                tp.x = x;
                tp.y = y;
                return tp;
            }

            if (s_touch_pending && (now - s_pending_ms) < PENDING_TIMEOUT_MS &&
                abs((int)x - (int)s_pending_x) < 30 && abs((int)y - (int)s_pending_y) < 30 &&
                (now - s_last_release_ms) >= s_release_cooldown_ms) {
                s_last_touched = true;
                s_last_x = x;
                s_last_y = y;
                s_last_valid_ms = now;
                s_touch_pending = false;
                s_swipe_start_x = x;
                s_swipe_start_y = y;
                s_swipe_start_ms = now;
                s_swipe_tracking = true;
                tp.touched = true;
                tp.x = x;
                tp.y = y;
                return tp;
            }

            s_touch_pending = true;
            s_pending_x = x;
            s_pending_y = y;
            s_pending_ms = now;
        }
    }

    if (frame_ok && touch_count == 0 && s_last_touched) {
        evaluate_swipe(now);
        s_last_release_ms = now;
        s_last_touched = false;
        s_touch_pending = false;
        return tp;
    }

    if (s_last_touched && (now - s_last_valid_ms) < GRACE_MS) {
        tp.touched = true;
        tp.x = s_last_x;
        tp.y = s_last_y;
        return tp;
    }

    if (s_last_touched) {
        evaluate_swipe(now);
        s_last_release_ms = now;
        s_last_touched = false;
    }
    return tp;
}

bool touch_is_currently_touched() { return s_last_touched; }

bool touch_read_raw(uint16_t &raw_x, uint16_t &raw_y) {
    uint8_t buf[7] = {0};
    if (!cst_read_regs(0x00, buf, sizeof(buf))) return false;
    if (buf[6] != 0xAB) return false;
    uint8_t touch_count = buf[0] & 0x0F;
    if (touch_count == 0) return false;
    raw_x = ((uint16_t)buf[1] << 4) | (buf[3] >> 4);
    raw_y = ((uint16_t)buf[2] << 4) | (buf[3] & 0x0F);
    return true;
}

bool touch_swipe_right_detected() {
    if (!s_swipe_right_pending) return false;
    s_swipe_right_pending = false;
    return true;
}
bool touch_swipe_down_detected() {
    if (!s_swipe_down_pending) return false;
    s_swipe_down_pending = false;
    return true;
}
bool touch_swipe_left_detected() {
    if (!s_swipe_left_pending) return false;
    s_swipe_left_pending = false;
    return true;
}
bool touch_swipe_up_detected() {
    if (!s_swipe_up_pending) return false;
    s_swipe_up_pending = false;
    return true;
}

void touch_cancel_swipes() {
    s_swipe_right_pending = false;
    s_swipe_down_pending = false;
    s_swipe_left_pending = false;
    s_swipe_up_pending = false;
    s_swipe_tracking = false;
}
