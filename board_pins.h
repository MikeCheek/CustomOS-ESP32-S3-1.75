/*
 * board_pins.h
 * ---------------------------------------------------------------------
 * ALL physical pin numbers for the ESP32-S3-Touch-AMOLED-1.75-G board
 * live in this ONE file. Nothing else in the project should hard-code
 * a GPIO number - everything includes this header instead.
 * ---------------------------------------------------------------------
 */

#pragma once

// ---------------------------------------------------------------------
// Display (CO5300 driver, QSPI) - Arduino_ESP32QSPI bus
// ---------------------------------------------------------------------
#define PIN_LCD_CS       12
#define PIN_LCD_SCLK     38
#define PIN_LCD_SDIO0    4
#define PIN_LCD_SDIO1    5
#define PIN_LCD_SDIO2    6
#define PIN_LCD_SDIO3    7
#define PIN_LCD_RST      39
#define PIN_LCD_TE       -1   // tearing-effect pin, optional (leave -1 if unused)

#define LCD_WIDTH        466
#define LCD_HEIGHT       466
#define LCD_ROTATION     0

// ---------------------------------------------------------------------
// Shared I2C bus (touch, IMU, RTC, PMU, IO expander all hang off this)
// ---------------------------------------------------------------------
#define PIN_IIC_SDA      15
#define PIN_IIC_SCL      14
#define IIC_CLOCK_HZ     400000

// ---------------------------------------------------------------------
// Touch controller (CST9217, I2C, addr 0x5A typical for CST92xx family)
// ---------------------------------------------------------------------
#define PIN_TP_INT       11
#define PIN_TP_RST       40
#define TP_I2C_ADDR      0x5A 

// ---------------------------------------------------------------------
// TCA9554 GPIO expander (drives some resets / backlight enable / etc.)
// Note: Since LCD_RST and TP_RST are direct GPIOs on this board revision,
// the expander might not be used for these, but kept for legacy/compatibility.
// ---------------------------------------------------------------------
#define EXPANDER_I2C_ADDR 0x20 
#define EXPANDER_PIN_LCD_RST   0  
#define EXPANDER_PIN_TP_RST    1  
#define EXPANDER_PIN_SPK_EN    2  
// Confirmed from the board's actual schematic (LC76G module pin 9,
// RESET, nets to GPS_RST which ties directly to EXIO7 on the TCA9554)
// - not present in earlier pin-map documentation here, which only
// covered the UART pins. Holding this low keeps the GNSS chip's RF
// frontend and tracking engine inactive, a real power-draw reduction
// beyond just not talking to it over UART.
#define EXPANDER_PIN_GPS_RST   7

// ---------------------------------------------------------------------
// QMI8658 IMU - shares the main I2C bus
// ---------------------------------------------------------------------
#define IMU_I2C_ADDR     0x6B 
#define PIN_IMU_INT1     -1   

// ---------------------------------------------------------------------
// PCF85063 RTC - shares the main I2C bus
// ---------------------------------------------------------------------
#define RTC_I2C_ADDR     0x51

// ---------------------------------------------------------------------
// AXP2101 PMU - shares the main I2C bus
// ---------------------------------------------------------------------
#define XPOWERS_CHIP_AXP2101
#define PMU_I2C_ADDR     0x34
#define PIN_PMU_IRQ      -1   

// ---------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------
// BOOT is a real ESP32 GPIO (confirmed against Waveshare's own docs for
// this board family: "under normal working conditions, GPIO0 can detect
// the high/low level of the [BOOT] button... click, double-click,
// long-press"). Active-low (pressed = LOW), needs INPUT_PULLUP.
#define PIN_BTN_BOOT     0

// PWR is NOT a raw ESP32 GPIO on boards in this family that use the
// AXP2101 (this one does - see hal_power.cpp) - it's wired into the PMU
// chip itself and read over I2C via its PEK (power-key) short/long-press
// IRQ status, using the same XPowersLib instance hal_power.cpp already
// talks to the AXP2101 with. See hal_buttons.cpp/hal_power.cpp - there
// is deliberately no PIN_BTN_PWR GPIO number here, because one doesn't
// exist for this button on this board.

// ---------------------------------------------------------------------
// microSD card (Mapped from SDMMC to SPI)
// ---------------------------------------------------------------------
#define PIN_SD_CS        41
#define PIN_SD_SCLK      2    // SDMMC_CLK
#define PIN_SD_MOSI      1    // SDMMC_CMD
#define PIN_SD_MISO      3    // SDMMC_DATA

// ---------------------------------------------------------------------
// GPS (LC76G module, -G variant only) - typically UART
// ---------------------------------------------------------------------
#define PIN_GPS_RX       17   // ESP32 RX <- GPS TX
#define PIN_GPS_TX       18   // ESP32 TX -> GPS RX
#define GPS_UART_NUM     1
#define GPS_BAUD         9600

// ---------------------------------------------------------------------
// Audio - ES8311 (speaker codec) + ES7210 (mic ADC), I2S
// ---------------------------------------------------------------------
#define PIN_I2S_MCLK     42   // I2S master clock (shared ES8311+ES7210). Was
                               // wired to GPIO16 - WRONG. Per the official
                               // repo's HARDWARE_REFERENCE.md (schematic-
                               // verified GPIO table): "Use GPIO42 for audio
                               // MCLK. GPIO16 is an expansion GPIO in the
                               // maintained BSP and board schematic" - GPIO16
                               // carries no MCLK signal at all on this board.
#define PIN_I2S_BCLK     9
#define PIN_I2S_WS       45
#define PIN_I2S_DOUT     8    // ESP32 -> ES8311 DSDIN: playback/speaker data.
                               // Named from the ESP32's own perspective, same
                               // as HARDWARE_REFERENCE.md's "I2S_DOUT" - see
                               // the i2s_pin_config_t note in hal_audio.cpp,
                               // this used to be paired with the wrong macro.
#define PIN_I2S_DIN      10   // ES7210 -> ESP32: mic capture data.
#define PIN_AUDIO_PA     46   // Speaker Amp Enable
#define ES8311_I2C_ADDR  0x18 // Speaker codec (output)
#define ES7210_I2C_ADDR  0x40 // Mic ADC (input) - DIFFERENT chip, different address.
                               // hal_audio.cpp previously sent the mic's
                               // init sequence to ES8311_I2C_ADDR by
                               // mistake, which is why the mic never
                               // actually got configured.

// ---------------------------------------------------------------------
// Vibration motor (if present)
// VERIFY: not on official Waveshare BOM for this board. If your
// variant has a motor on a different GPIO, update this number.
// ---------------------------------------------------------------------
#define PIN_VIBRATE      -1   // -1 = not connected