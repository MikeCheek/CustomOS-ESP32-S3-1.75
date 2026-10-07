/*
 * hal_expander.h
 * TCA9554 8-bit I2C GPIO expander. Used on this board to drive a few
 * reset/enable lines that would otherwise cost a native ESP32 GPIO.
 * Which logical pin drives what is set in board_pins.h
 * (EXPANDER_PIN_*).
 */

#pragma once
#include <stdint.h>

void expander_init();
void expander_set_pin(uint8_t pin, bool level);
void expander_pulse_reset(uint8_t pin, uint16_t low_ms = 10, uint16_t settle_ms = 50);
