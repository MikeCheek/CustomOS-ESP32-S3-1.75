/*
 * config.h
 * Global feature flags and UI constants.
 */

#pragma once
#include <Arduino.h>

// ---- Feature flags -------------------------------------------------------
#define FEATURE_GPS        1
#define FEATURE_SD_CARD     1
#define FEATURE_USB_OTG_MODES 0  // Settings > USB Mode always offers
                                  // Charging only, Firmware & Debug and
                                  // reboot-to-flash. 1 adds File storage
                                  // (SD as a USB drive) and Media remote,
                                  // which start TinyUSB at runtime - build
                                  // with Tools > USB Mode > "Hardware CDC
                                  // and JTAG" (the default). Costs ~32 KB
                                  // of internal RAM (TinyUSB's static
                                  // buffers); set 0 if WiFi+BLE together
                                  // run short of heap. See hal_usb.h.
                                  // 0 since fw 2.8.1: with Bluetooth
                                  // connected those 32 KB were what
                                  // Wi-Fi needed to start at all.
#define FEATURE_AUDIO       1
#define FEATURE_IO_EXPANDER 1
#define FEATURE_NVS         1
#define FEATURE_VIBRATE     0
#define FEATURE_WIFI        1
#define FEATURE_BLE         1

// ---- Serial / debug ------------------------------------------------------
#define SERIAL_BAUD         115200
#define DEBUG_PRINTF(...)   Serial.printf(__VA_ARGS__)

// ---- Display -------------------------------------------------------------
// QSPI clock for the CO5300, passed to gfx->begin() in hal_display.cpp.
// 40 MHz is what Espressif's esp_lcd_co5300 driver and Waveshare's BSP
// (and Meta's MUSE firmware on this exact board) use. The two
// ESP32QSPI_* defines that used to be here never took effect: the
// Arduino_GFX library is compiled as its own translation unit and only
// sees its own defaults (40 MHz, 1024 px per transfer).
#define DISPLAY_QSPI_HZ     40000000

// ---- Watch behaviour -----------------------------------------------------
#define SCREEN_TIMEOUT_MS      15000
#define DEFAULT_BRIGHTNESS     200
#define DIM_BRIGHTNESS         40

// ---- Palette -------------------------------------------------------------
#define COLOR565(r, g, b) \
    ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | ((b) >> 3)))
// Colors are plain uint16_t RGB565.
#define COLOR_BG        ((uint16_t)0x0000)
#define COLOR_PANEL     ((uint16_t)0x2104)
#define COLOR_ACCENT    ((uint16_t)0x7B1F)
#define COLOR_ACCENT2   ((uint16_t)0x39BF)
#define COLOR_ACCENT3   ((uint16_t)0xFE14)
#define COLOR_TEXT      ((uint16_t)0xFFFF)
#define COLOR_TEXT_DIM  ((uint16_t)0x7BEF)
#define COLOR_WARN      ((uint16_t)0xFF60)
#define COLOR_BAD       ((uint16_t)0xF84D)
#define COLOR_GOOD      ((uint16_t)0x2FC7)
