/*
 * app_sos.h
 * Emergency SOS (hold PWR, slide "Emergency SOS"): five seconds counting
 * down with loud beeps - tap Cancel to stop - then the phone is asked to
 * help: {"e":"sos"}. The companion app rings and texts the emergency
 * contact (Settings > Safety) with the phone's location, and answers
 * {"t":"sos","ok":1} (sent) or {"t":"sos","ok":0,"why":"..."}.
 */
#pragma once
#include "ui.h"

void sos_on_reply(bool ok, const char *why);   // phone link "sos"
extern Screen sos_screen;
