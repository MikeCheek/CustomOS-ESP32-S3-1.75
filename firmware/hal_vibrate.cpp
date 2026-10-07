#include "hal_vibrate.h"
#include "board_pins.h"
#include "config.h"

#if FEATURE_VIBRATE && PIN_VIBRATE >= 0
#include <Arduino.h>

static bool s_vibrating = false;
static uint32_t s_vibrate_off_ms = 0;

void vibrate_init() {
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_VIBRATE, 0);
    ledcWrite(PIN_VIBRATE, 0);
    DEBUG_PRINTF("[vibrate] init on GPIO %d\n", PIN_VIBRATE);
}

void vibrate_ms(uint16_t duration_ms) {
    ledcWrite(PIN_VIBRATE, 180);
    s_vibrating = true;
    s_vibrate_off_ms = millis() + duration_ms;
}

void vibrate_buzz() {
    vibrate_ms(50);
}

// Call from loop() to turn off vibration when time expires.
void vibrate_update() {
    if (s_vibrating && millis() >= s_vibrate_off_ms) {
        ledcWrite(PIN_VIBRATE, 0);
        s_vibrating = false;
    }
}

#else

void vibrate_init() {}
void vibrate_ms(uint16_t) {}
void vibrate_buzz() {}
void vibrate_update() {}

#endif
