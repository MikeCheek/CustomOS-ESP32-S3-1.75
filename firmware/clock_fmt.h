/*
 * clock_fmt.h - the time as Settings > Display > 24-hour clock wants it.
 *   24-hour: "09:05", "21:05"
 *   12-hour: "9:05" and "AM" / "PM" beside it (faces draw that smaller),
 *            or "9:05 PM" in running text
 */
#pragma once
#include <stdio.h>
#include "app_settings_state.h"

inline bool clock_24h() { return g_app_settings.clock_24h; }

// The digits only.
inline void clock_hm(char *buf, int n, int hour, int minute) {
    if (clock_24h()) { snprintf(buf, n, "%02d:%02d", hour, minute); return; }
    int h = hour % 12;
    snprintf(buf, n, "%d:%02d", h ? h : 12, minute);
}

// "" with the 24-hour clock.
inline const char *clock_ampm(int hour) {
    if (clock_24h()) return "";
    return hour < 12 ? "AM" : "PM";
}

// Digits and AM/PM together, for text.
inline void clock_hm_text(char *buf, int n, int hour, int minute) {
    char hm[8];
    clock_hm(hm, sizeof(hm), hour, minute);
    if (clock_24h()) snprintf(buf, n, "%s", hm);
    else snprintf(buf, n, "%s %s", hm, clock_ampm(hour));
}
