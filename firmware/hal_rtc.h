/*
 * hal_rtc.h
 * PCF85063 real-time clock, battery-backed via AXP2101. Uses SensorLib
 * (the same library Waveshare's PCF85063 demo for this board uses).
 */

#pragma once
#include <stdint.h>

struct WatchTime {
    int year, month, day, hour, minute, second, weekday; // weekday: 0=Sunday
    bool valid;
};

bool rtc_init();
WatchTime rtc_now();
void rtc_set(int year, int month, int day, int hour, int minute, int second);
