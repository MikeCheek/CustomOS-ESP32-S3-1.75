/*
 * app_fall.h
 * Fall detection (Settings > Wake & quiet > Fall detection, off by
 * default): the accelerometer's FIFO goes through FallDetector
 * (fall_detect.h) - every sample, awake or asleep. A fall wakes the watch
 * on "Did you fall?": 30 s of buzzing and beeping to answer "I'm OK",
 * then it asks the phone for help ({"e":"fall"}: the app rings, shows an
 * alert and can text an emergency contact with the location).
 */
#pragma once
#include "ui.h"

void fall_feed(float ax, float ay, float az);   // each FIFO sample (motion_update, app_activity.h)
bool fall_alert_active();           // keeps the loop awake while asking
extern Screen fall_alert_screen;
