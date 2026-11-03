/*
 * app_assistant.h
 * Voice assistant: ask out loud, the phone understands and acts.
 *
 *   watch: records /Recordings/_ask.wav (mono, stops by itself when you
 *          stop talking), then  {"e":"ask","n":<seq>,"f":"_ask.wav"}
 *          (or, after the wake word, the words that weren't a command:
 *          assistant_ask_recorded())
 *   phone: downloads it, transcribes it (Whisper), works out what was
 *          asked (its own rules first, then the local LLM) and does it -
 *          music, reminders and alarms in the calendar, weather, the
 *          agenda, Do Not Disturb, finding the phone, questions about
 *          memos, or just an answer
 *   phone -> watch  {"t":"ask","n":..,"st":"Thinking"}          progress
 *                   {"t":"ask","n":..,"q":"heard","a":"answer",
 *                    "do":"timer","s":300}                       the result
 *                   {"t":"ask","n":..,"err":"why"}
 *
 * "do" is for what happens on the watch itself: "timer" with "s" seconds
 * (0 = cancel). The timer keeps running when the screen is closed and
 * rings on its own screen. Opened from the app menu or by double-pressing
 * the BOOT button.
 */
#pragma once
#include <stdint.h>
#include "ui.h"

extern Screen assistant_screen;

// phone_link.cpp
void assistant_on_status(uint32_t seq, const char *status);
void assistant_on_result(uint32_t seq, const char *question, const char *answer, const char *action,
                         int32_t seconds, const char *err);

// loop(): rings the timer when it's due, awake or asleep.
void assistant_timer_update();
bool assistant_timer_active();
uint32_t assistant_timer_left_s();
void assistant_timer_set(uint32_t seconds);   // 0 = cancel

// A question already recorded (16-bit mono, what was said after the wake
// word that wasn't a command): saved as _ask.wav and sent to the phone,
// with the assistant showing the answer. False without the phone link or
// the SD card - then nothing happens.
bool assistant_ask_recorded(const int16_t *pcm, int samples, int rate);
