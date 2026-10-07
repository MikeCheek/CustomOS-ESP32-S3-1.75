#include "hal_buttons.h"
#include "hal_power.h"
#include "board_pins.h"
#include "config.h"
#include <Arduino.h>

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t LONG_PRESS_MS = 600;
// Minimum gap between two EMITTED events (of either kind), on top of
// the raw-signal debounce above. The raw debounce filters contact
// bounce on the electrical signal itself; this catches the case where
// bounce is long/noisy enough to still produce two legitimate-looking
// transitions in quick succession, which would otherwise read as two
// separate presses.
static const uint32_t EVENT_COOLDOWN_MS = 180;
// How long to wait after a short-press release before deciding "no
// second press is coming, emit it as a single short-press" - also the
// window a second short-press has to land in to be read as a
// double-press instead. This is why a lone short-press now shows up
// ~350ms after release rather than instantly: there's no way to know
// it's NOT the first half of a double-press until this window passes.
static const uint32_t DOUBLE_PRESS_WINDOW_MS = 350;

// --- BOOT (real GPIO, active-low) --------------------------------------
static bool s_boot_raw_down = false;      // debounced physical state
static bool s_boot_last_raw = false;      // last raw pin read, for debounce
static uint32_t s_boot_last_change_ms = 0;
static uint32_t s_boot_down_since_ms = 0;
static bool s_boot_long_fired = false;
static ButtonEvent s_boot_event = BTN_EVENT_NONE;
static uint32_t s_boot_last_event_ms = 0;
static uint32_t s_boot_pending_short_ms = 0; // 0 = no single-press awaiting the double-press window

// --- PWR (via AXP2101 PEK IRQ) ------------------------------------------
static ButtonEvent s_pwr_event = BTN_EVENT_NONE;
static uint32_t s_pwr_last_event_ms = 0;
static uint32_t s_pwr_pending_short_ms = 0;

void buttons_init() {
    pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
    s_boot_last_raw = digitalRead(PIN_BTN_BOOT) == LOW;
    s_boot_raw_down = s_boot_last_raw;
}

// Called at the moment a genuine short-press release is detected -
// resolves it against any pending short-press from a moment ago
// (double-press) or starts a new pending window (might become a lone
// single-press once DOUBLE_PRESS_WINDOW_MS passes with nothing else).
static void register_short_press(uint32_t *pending_ms, ButtonEvent *event_out,
                                  uint32_t *last_event_ms, uint32_t now) {
    if (*pending_ms != 0 && (now - *pending_ms) <= DOUBLE_PRESS_WINDOW_MS) {
        *pending_ms = 0;
        if ((now - *last_event_ms) >= EVENT_COOLDOWN_MS) {
            *event_out = BTN_EVENT_DOUBLE_PRESS;
            *last_event_ms = now;
        }
    } else {
        *pending_ms = now;
    }
}

// Called every buttons_update() - resolves a pending short-press into
// a real BTN_EVENT_SHORT_PRESS once the double-press window has
// passed with no follow-up press.
static void resolve_pending_short(uint32_t *pending_ms, ButtonEvent *event_out,
                                   uint32_t *last_event_ms, uint32_t now) {
    if (*pending_ms != 0 && (now - *pending_ms) > DOUBLE_PRESS_WINDOW_MS) {
        *pending_ms = 0;
        if ((now - *last_event_ms) >= EVENT_COOLDOWN_MS) {
            *event_out = BTN_EVENT_SHORT_PRESS;
            *last_event_ms = now;
        }
    }
}

void buttons_update() {
    uint32_t now = millis();

    // --- BOOT: simple debounce + long-press-while-held detection ---
    bool raw_down = digitalRead(PIN_BTN_BOOT) == LOW; // active-low

    if (raw_down != s_boot_last_raw) {
        s_boot_last_raw = raw_down;
        s_boot_last_change_ms = now;
    }

    if ((now - s_boot_last_change_ms) >= DEBOUNCE_MS && raw_down != s_boot_raw_down) {
        s_boot_raw_down = raw_down;
        if (s_boot_raw_down) {
            s_boot_down_since_ms = now;
            s_boot_long_fired = false;
        } else {
            // Released - if we didn't already fire a long-press while
            // held, this was a short press (pending double-press
            // resolution, see register_short_press()).
            if (!s_boot_long_fired) {
                register_short_press(&s_boot_pending_short_ms, &s_boot_event, &s_boot_last_event_ms, now);
            }
        }
    }

    if (s_boot_raw_down && !s_boot_long_fired && (now - s_boot_down_since_ms) >= LONG_PRESS_MS) {
        s_boot_long_fired = true;
        s_boot_pending_short_ms = 0; // this press turned into a long-press, not a short one
        if ((now - s_boot_last_event_ms) >= EVENT_COOLDOWN_MS) {
            s_boot_event = BTN_EVENT_LONG_PRESS;
            s_boot_last_event_ms = now;
        }
    }

    resolve_pending_short(&s_boot_pending_short_ms, &s_boot_event, &s_boot_last_event_ms, now);

    // --- PWR: fold the PMU's own short/long IRQ status into the same
    // event model ---
    power_poll_pek_button();
    if (power_pek_long_press_pending()) {
        s_pwr_pending_short_ms = 0; // same reasoning as BOOT above
        if ((now - s_pwr_last_event_ms) >= EVENT_COOLDOWN_MS) {
            s_pwr_event = BTN_EVENT_LONG_PRESS;
            s_pwr_last_event_ms = now;
        }
    } else if (power_pek_short_press_pending()) {
        register_short_press(&s_pwr_pending_short_ms, &s_pwr_event, &s_pwr_last_event_ms, now);
    }

    resolve_pending_short(&s_pwr_pending_short_ms, &s_pwr_event, &s_pwr_last_event_ms, now);
}

ButtonEvent buttons_get_event(ButtonId id) {
    if (id == BTN_BOOT) {
        ButtonEvent e = s_boot_event;
        s_boot_event = BTN_EVENT_NONE;
        return e;
    } else {
        ButtonEvent e = s_pwr_event;
        s_pwr_event = BTN_EVENT_NONE;
        return e;
    }
}
