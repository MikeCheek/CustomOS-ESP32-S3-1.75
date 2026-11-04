/*
 * app_reminders.h
 * Reminders: the open to-dos the companion app keeps (from voice memos),
 * ticked off on the watch. The phone sends the list in pages:
 *   {"t":"todos","r":1,"l":[{"k":"<key>","x":"Call Marco"},...]}
 *   {"t":"todos","l":[...],"done":1}       (the last page; replaces the list)
 * and the watch answers a tick with {"e":"todo","k":"<key>","d":1}.
 * The list is kept in NVS, so it's there without the phone.
 */
#pragma once
#include "ui.h"
#include <ArduinoJson.h>

void reminders_init();
void reminders_on_link(JsonDocument &doc);
int  reminders_count();
const char *reminders_first();   // "" when none

extern Screen reminders_screen;
