/*
 * app_espnow_status.cpp
 * Shows ESP-NOW joystick pairing status: whether a joystick companion
 * device (a second ESP32 running tools/joystick_esp32/) is currently
 * paired, its MAC address, and how long since its last packet -
 * matching this project's existing "BLE Status" app's purpose, for
 * the ESP-NOW transport instead.
 */
#include "app_espnow_status.h"
#include "config.h"
#include "board_pins.h"
#include "hal_espnow.h"
#include <Arduino_GFX_Library.h>
#include <WiFi.h>

static void espnow_status_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    (void)g;

    ui_draw_centered_text(70, COLOR_TEXT, "Joystick Link", 2);

    bool paired = espnow_joystick_paired();
    ui_draw_centered_text(140, paired ? COLOR_GOOD : COLOR_WARN,
                           paired ? "Connected" : "Searching...", 2);

    char buf[32];
    if (paired) {
        uint8_t mac[6];
        espnow_get_joystick_mac(mac);
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ui_draw_centered_text(184, COLOR_TEXT_DIM, buf, 1);

        uint32_t age_s = (millis() - espnow_last_packet_ms()) / 1000;
        snprintf(buf, sizeof(buf), "Last packet: %lus ago", (unsigned long)age_s);
        ui_draw_centered_text(202, COLOR_TEXT_DIM, buf, 1);
    } else {
        ui_draw_centered_text(184, COLOR_TEXT_DIM, "Power on the joystick", 1);
        ui_draw_centered_text(200, COLOR_TEXT_DIM, "companion device nearby", 1);
    }

    snprintf(buf, sizeof(buf), "Watch: %s", WiFi.macAddress().c_str());
    ui_draw_centered_text(LCD_HEIGHT - 60, COLOR_TEXT_DIM, buf, 1);
}

Screen espnow_status_screen = {
    nullptr, GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, espnow_status_draw, nullptr, nullptr, nullptr, nullptr
};
