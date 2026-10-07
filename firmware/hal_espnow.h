/*
 * hal_espnow.h
 * ESP-NOW receiver for a dedicated joystick companion device (a
 * second ESP32 running the paired sketch in tools/joystick_esp32/) -
 * an alternative input path to the phone app's BLE controller, for
 * when you want a real physical joystick instead of a touchscreen
 * one. This module's only job is turning ESP-NOW packets into
 * controller_set_state() calls (see hal_controller.h) - every game
 * and the OS menu already consume controller input generically and
 * don't know or care which transport (BLE or ESP-NOW) it came from.
 *
 * Coexistence note: this project's hal_wifi.cpp already uses WIFI_STA
 * for periodic NTP sync (connect briefly, sync, disconnect - not a
 * persistent connection). ESP-NOW is pinned to a fixed channel here
 * so the watch and the joystick device always agree on it
 * deterministically, rather than the joystick having to guess
 * whatever channel the watch's WiFi happens to be on at the moment -
 * the tradeoff is that if NTP sync is actively connecting to a router
 * on a different channel at the exact same instant, ESP-NOW packets
 * could be briefly missed. That's a rare, short-lived window, and the
 * joystick's own reconnect logic (see the paired sketch) retries
 * automatically, so it self-recovers rather than needing anything
 * special here.
 */
#pragma once
#include <Arduino.h>

void espnow_enable();
void espnow_disable();
bool espnow_is_enabled();
bool espnow_joystick_paired(); // true if a packet arrived within the last 5 seconds
void espnow_get_joystick_mac(uint8_t mac_out[6]);
uint32_t espnow_last_packet_ms();
