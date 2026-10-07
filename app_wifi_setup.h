/*
 * app_wifi_setup.h
 * WiFi network setup screen: scans for nearby networks, lets the user
 * tap one (or enter a hidden SSID manually), type its password on an
 * on-screen keyboard, and connects - the piece that was missing for
 * wifi_set_credentials() to ever actually get called from the UI.
 * Reached from Settings > WiFi Setup (a ROW_SUBSCREEN row, same
 * pattern as Brightness/Volume/Battery Mode).
 */

#pragma once
#include "ui.h"

extern Screen wifi_setup_screen;
