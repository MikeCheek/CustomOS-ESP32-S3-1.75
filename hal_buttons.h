/*
 * hal_buttons.h
 * The two physical side buttons on this board:
 *   - BOOT: a real ESP32 GPIO (see board_pins.h), read directly here
 *     with simple debounce + long-press timing.
 *   - PWR: not a GPIO on this board - read via the AXP2101 PMU's own
 *     PEK short/long-press IRQ status (hal_power.cpp). This file just
 *     polls that and folds it into the same short/long-press API as
 *     BOOT so callers don't need to care which button is which kind of
 *     hardware.
 *
 * Both buttons also detect a double-press (two short presses within
 * DOUBLE_PRESS_WINDOW_MS of each other, see hal_buttons.cpp) - since
 * that means a single short-press can't be reported the instant it's
 * released (a second one might still follow), a lone short-press is
 * only emitted once the double-press window has passed without a
 * follow-up, so it lands ~350ms after release rather than instantly.
 * Long-press is unaffected - it's already resolved before release.
 *
 * Call buttons_update() once per loop() iteration; it's non-blocking.
 */

#pragma once

enum ButtonId {
    BTN_BOOT = 0,
    BTN_PWR = 1,
};

enum ButtonEvent {
    BTN_EVENT_NONE = 0,
    BTN_EVENT_SHORT_PRESS,
    BTN_EVENT_DOUBLE_PRESS,
    BTN_EVENT_LONG_PRESS,
};

void buttons_init();

// Non-blocking; call every loop() iteration.
void buttons_update();

// Returns (and consumes) any button event detected since the last call.
// Poll both buttons each loop if you care about both.
ButtonEvent buttons_get_event(ButtonId id);
