/*
 * app_calendar.h
 * The phone's agenda on the watch: an agenda screen, and event reminders
 * that work even when the phone is out of range (the agenda is kept on the
 * SD card).
 *
 * The companion app sends the next two days of events over the phone link
 * (see COMPANION_PROTOCOL.md, "cal"). Times are LOCAL epoch seconds - the
 * phone's local date/time written as if it were UTC - so the watch can
 * compare them with its RTC (which runs on local time) directly.
 */
#pragma once
#include <stdint.h>
#include <ArduinoJson.h>
#include "ui.h"

#define CAL_MAX_EVENTS 24

struct CalEvent {
    uint32_t id;
    uint32_t start, end;     // local epoch seconds
    bool     all_day;
    uint16_t color;          // RGB565 calendar color
    char     title[48];
    char     location[40];
};

// Phone link: {"t":"cal", ...} messages (phone_link.cpp).
void calendar_on_link(JsonDocument &doc);

// Main loop, awake or asleep: reminders (rate-limited internally).
void calendar_update();

int  calendar_count();
bool calendar_get(int i, CalEvent &out);
// The next event that hasn't ended (all-day events last), false if none.
bool calendar_next(CalEvent &out);
// "09:30", "Tomorrow 09:30", "All day" ... for an event start.
void calendar_format_when(const CalEvent &e, char *out, int n);
uint32_t calendar_now();     // local epoch seconds from the RTC

extern Screen calendar_screen;
extern Screen calendar_reminder_screen;
