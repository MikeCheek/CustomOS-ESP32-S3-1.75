#include "hal_expander.h"
#include "board_pins.h"
#include "config.h"
#include <Wire.h>

// TCA9554 registers
#define TCA9554_REG_INPUT   0x00
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_POL     0x02
#define TCA9554_REG_CONFIG  0x03 // 1 = input, 0 = output (POR default = all inputs)

static uint8_t s_output_state = 0x00;

static bool write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(EXPANDER_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

void expander_init() {
    // Configure the pins we know about as outputs, default HIGH (most
    // reset lines on this kind of board are active-low, so idling high
    // keeps the downstream chip out of reset until we explicitly pulse
    // it). Any expander pins you haven't wired up yet stay as inputs.
    uint8_t config_mask = 0xFF; // start all-input
    config_mask &= ~(1 << EXPANDER_PIN_LCD_RST);
    config_mask &= ~(1 << EXPANDER_PIN_TP_RST);
    config_mask &= ~(1 << EXPANDER_PIN_SPK_EN);
    config_mask &= ~(1 << EXPANDER_PIN_GPS_RST);

    s_output_state = (1 << EXPANDER_PIN_LCD_RST) | (1 << EXPANDER_PIN_TP_RST);
    // SPK_EN and GPS_RST both left low (off/held-in-reset) at boot -
    // enabled explicitly when audio plays / GPS is turned on.

    write_reg(TCA9554_REG_OUTPUT, s_output_state);
    write_reg(TCA9554_REG_CONFIG, config_mask);

    DEBUG_PRINTF("[expander] TCA9554 init at 0x%02X\n", EXPANDER_I2C_ADDR);
}

void expander_set_pin(uint8_t pin, bool level) {
    if (pin >= 8) return;
    if (level) s_output_state |= (1 << pin);
    else s_output_state &= ~(1 << pin);
    write_reg(TCA9554_REG_OUTPUT, s_output_state);
}

void expander_pulse_reset(uint8_t pin, uint16_t low_ms, uint16_t settle_ms) {
    expander_set_pin(pin, false); // active-low reset
    delay(low_ms);
    expander_set_pin(pin, true);
    delay(settle_ms);
}
