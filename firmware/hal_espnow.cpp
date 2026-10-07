#include "hal_espnow.h"
#include "hal_controller.h"
#include "config.h"
#include "app_settings_state.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <cstring>

#define ESPNOW_CHANNEL 1

// Single-byte packet type prefix, same idea as hal_ble.cpp's file
// characteristic prefix-byte convention.
#define PKT_DISCOVER       0x01 // broadcast from the joystick, empty payload
#define PKT_DISCOVER_ACK   0x02 // unicast reply from the watch, empty payload
#define PKT_JOYSTICK_STATE 0x03 // unicast from the joystick: [1]=dpad bits [2]=button bits

static uint8_t s_joystick_mac[6] = { 0, 0, 0, 0, 0, 0 };
static bool s_ever_paired = false;
static uint32_t s_last_packet_ms = 0;

static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (len < 1 || !info) return;
    uint8_t type = data[0];
    const uint8_t *mac = info->src_addr;

    if (type == PKT_DISCOVER) {
        if (!esp_now_is_peer_exist(mac)) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, mac, 6);
            peer.channel = ESPNOW_CHANNEL;
            peer.encrypt = false;
            esp_now_add_peer(&peer);
        }
        uint8_t ack[1] = { PKT_DISCOVER_ACK };
        esp_now_send(mac, ack, 1);
        memcpy(s_joystick_mac, mac, 6);
        s_ever_paired = true;
        s_last_packet_ms = millis();
        DEBUG_PRINTF("[espnow] discover from %02X:%02X:%02X:%02X:%02X:%02X, acked\n",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }

    if (type == PKT_JOYSTICK_STATE && len >= 3) {
        controller_set_state(data[1], data[2]);
        memcpy(s_joystick_mac, mac, 6);
        s_ever_paired = true;
        s_last_packet_ms = millis();
        return;
    }
}

static bool s_enabled = false;

void espnow_enable() {
    if (s_enabled) return;
    // Shares the radio's STA mode with hal_wifi.cpp's periodic NTP
    // sync - safe to call unconditionally here since setting STA mode
    // again when WiFi already has it set is a no-op, not a conflict.
    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        DEBUG_PRINTF("[espnow] init failed\n");
        return;
    }
    esp_now_register_recv_cb(on_data_recv);
    s_enabled = true;
    DEBUG_PRINTF("[espnow] ready - watch MAC: %s, channel %d\n",
                 WiFi.macAddress().c_str(), ESPNOW_CHANNEL);
}

void espnow_disable() {
    if (!s_enabled) return;
    esp_now_deinit();
    s_enabled = false;
    // Only actually power the radio down if WiFi isn't also using it
    // right now - turning it off out from under an active WiFi/NTP
    // session would break that feature, since both share one radio.
    if (!g_app_settings.wifi_on) {
        WiFi.mode(WIFI_OFF);
    }
    DEBUG_PRINTF("[espnow] disabled\n");
}

bool espnow_is_enabled() {
    return s_enabled;
}

bool espnow_joystick_paired() {
    return s_ever_paired && (millis() - s_last_packet_ms < 5000);
}

void espnow_get_joystick_mac(uint8_t mac_out[6]) {
    memcpy(mac_out, s_joystick_mac, 6);
}

uint32_t espnow_last_packet_ms() {
    return s_last_packet_ms;
}
