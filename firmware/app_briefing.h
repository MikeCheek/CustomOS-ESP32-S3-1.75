/*
 * app_briefing.h
 * The morning briefing: the first time you look at the watch each morning
 * (5:00-11:00, after the night or the alarm), one card - good morning,
 * the weather with today's high and low, last night's sleep, the first
 * event, what the Life plan has next, and the battery. A tap or a swipe
 * closes it. Settings > Wake & quiet > Morning briefing. With a voice on the
 * phone (and "Read the morning briefing aloud" on), it's also said.
 */
#pragma once
#include "ui.h"

extern Screen briefing_screen;
void briefing_update();   // loop(): shows it once a morning, when a face is on screen
