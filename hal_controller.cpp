#include "hal_controller.h"

static uint8_t s_dpad = 0;
static uint8_t s_buttons = 0;
static uint32_t s_last_packet_ms = 0;

void controller_set_state(uint8_t dpad, uint8_t buttons) {
    s_dpad = dpad;
    s_buttons = buttons;
    s_last_packet_ms = millis();
}

bool controller_connected() {
    return s_last_packet_ms != 0 && millis() - s_last_packet_ms < 3000;
}

bool controller_dpad(uint8_t bit) { return (s_dpad & bit) != 0; }
bool controller_button(uint8_t bit) { return (s_buttons & bit) != 0; }

void controller_get_direction(float *dx, float *dy) {
    float x = 0, y = 0;
    if (s_dpad & CTRL_LEFT) x -= 1.0f;
    if (s_dpad & CTRL_RIGHT) x += 1.0f;
    if (s_dpad & CTRL_UP) y -= 1.0f;
    if (s_dpad & CTRL_DOWN) y += 1.0f;
    float len = sqrtf(x * x + y * y);
    if (len > 0.01f) { x /= len; y /= len; }
    *dx = x;
    *dy = y;
}
