/*
 * hal_ntp.h
 * On-demand NTP time sync. Connects to WiFi, syncs time from an NTP
 * server, writes the result to the RTC, then disconnects.
 */

#pragma once
#include <stdint.h>

enum NtpStatus { NTP_IDLE, NTP_CONNECTING, NTP_SYNCING, NTP_DONE, NTP_FAILED };

// Call from setup() after wifi_init(). No-op if FEATURE_WIFI is 0.
void ntp_init();

// Starts an async sync. Non-blocking: use ntp_get_status() to poll.
// On success, the RTC is updated with the received time.
void ntp_sync();

// Returns the current sync state. Call from loop() to drive the
// async state machine.
NtpStatus ntp_get_status();

// Call from loop() while NTP_CONNECTING or NTP_SYNCING.
// Handles the async state machine and timeout.
void ntp_update();
