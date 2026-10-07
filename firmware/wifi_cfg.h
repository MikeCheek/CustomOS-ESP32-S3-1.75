/*
 * wifi_cfg.h
 * The watch's Wi-Fi set up and checked from the phone app (phone link
 * message "wcfg", see COMPANION_PROTOCOL.md "Wi-Fi setup from the app").
 *
 *   {"t":"wcfg","op":"status"}                    -> current state
 *   {"t":"wcfg","op":"scan"}                      -> networks the watch sees
 *   {"t":"wcfg","op":"set","s":"ssid","p":"pw"}   -> save + try to connect
 *   {"t":"wcfg","op":"test"}                      -> try the saved network
 *   {"t":"wcfg","op":"forget"}                    -> delete the saved network
 *
 * Replies are {"e":"wcfg","op":...}. Scans and connection tests run in the
 * main loop (wifi_cfg_update) so the link handler never blocks; afterwards
 * the radio is put back the way it was (off if Wi-Fi is off in Settings).
 */
#pragma once
#include <ArduinoJson.h>

void wifi_cfg_handle(JsonDocument &doc);   // phone_link.cpp
void wifi_cfg_update();                    // main loop
bool wifi_cfg_busy();                      // a scan / test is running
