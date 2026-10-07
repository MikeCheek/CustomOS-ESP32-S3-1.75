/*
 * hal_wifi_xfer.h
 * Fast recording transfers over Wi-Fi (about 20-50x Bluetooth).
 *
 * The companion app asks over the phone link ({"t":"wifi","on":1}). The
 * watch joins its saved Wi-Fi network, starts a small HTTP server on port
 * 8080 that only answers requests carrying a one-time key, and tells the app
 * where it is ({"e":"wifi","st":"ready","ip":"...","port":8080,"k":"..."}).
 * The phone must be on the same network. The server stops - and the radio
 * goes back to how it was - when the app says so, or after 2 idle minutes.
 *
 *   GET  /list?k=KEY            [{"name":"rec_001.wav","size":123}, ...]
 *   GET  /f?k=KEY&n=NAME        the file from /Recordings
 *   POST /up  (headers X-Key, X-Name)  body = file -> /Recordings (wav/mp3); {"name":"saved as"}
 *   POST /put (headers X-Key, X-Name)  body = any file -> card root (like a Bluetooth file transfer)
 * (Every request may also pass the key/name as X-Key / X-Name headers.)
 */
#pragma once
#include <stdint.h>

void wifi_xfer_request(bool on);   // phone link "wifi" (phone_link.cpp)
void wifi_xfer_update();           // main loop, awake or asleep
bool wifi_xfer_active();           // server up or starting
// Recordings were added over Wi-Fi since the last call (refresh the index).
bool wifi_xfer_take_changes();
// Files received with POST /put since the last call.
int wifi_xfer_take_files_received();
// The transfer server has this SD path open right now.
bool wifi_xfer_file_busy(const char *path);
