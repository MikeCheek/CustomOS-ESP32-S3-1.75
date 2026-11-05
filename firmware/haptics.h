/*
 * haptics.h
 * One feel per kind of event - Apple's "taptic language", so you know what
 * happened without looking: a message taps twice, a turn to the right is
 * four even taps and to the left three pairs, success rises, failure falls,
 * a workout's start and stop are mirror images...
 *
 * This board has no vibration motor (PIN_VIBRATE -1), so each pattern is
 * also a short tone sequence on the speaker ("Notification sounds", Settings >
 * Sound); with a motor wired (FEATURE_VIBRATE) it buzzes the same rhythm.
 * Quiet in Do Not Disturb except the urgent ones (alarm, SOS).
 */
#pragma once
#include <stdint.h>

enum HapticKind : uint8_t {
    HAP_NOTIFY,       // a message
    HAP_SUCCESS,      // a goal reached, sent, saved
    HAP_FAILURE,      // didn't work
    HAP_START,        // a workout / stopwatch started
    HAP_STOP,         // ... paused or ended
    HAP_TURN_LEFT,    // directions
    HAP_TURN_RIGHT,
    HAP_ARRIVED,
    HAP_REMINDER,     // stand up, an event soon
    HAP_CLICK,        // a detent
    HAP_URGENT,       // SOS countdown, alarms
    HAP_COUNT
};

void haptic(HapticKind k);
void haptics_update();   // loop(): plays the steps on time
