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
 *   "sp":1 in the result: the answer is being spoken (a "say:" file
 *   follows); "fl":1 too: once it has been said, the watch listens a few
 *   seconds more for a follow-up - no wake word, no tap - and sends it as
 *   {"e":"ask",...,"fu":1}, so the phone answers it in context. Nothing
 *   said in 4 s: back to the answer.
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
                         int32_t seconds, const char *err, bool spoken = false, bool follow = false);

// Something the watch asked the phone to say (a message read aloud, the
// briefing, the Today card): its "say:" file is played even outside the
// assistant, for the next minute.
void assistant_expect_speech();
bool assistant_speech_expected();
// A message by voice ("tell Marco I'm late"): the result has "do":"send" and
// the answer asks for the OK - the Send button sends {"e":"asksend"} (or
// "yes" as a follow-up); the phone answers {"t":"sent","ok":1,"to":".."}.
void assistant_on_sent(bool ok, const char *to);

// The answers kept for the history (swipe up on an answer), up to 6.
int assistant_history_count();

// loop(): rings the timer when it's due, awake or asleep.
void assistant_timer_update();
bool assistant_timer_active();
uint32_t assistant_timer_left_s();
void assistant_timer_set(uint32_t seconds);   // 0 = cancel
// The Timer app's controls (app_clock.cpp): pause keeps what's left.
void assistant_timer_pause(bool pause);
bool assistant_timer_paused();
uint32_t assistant_timer_length_s();
uint32_t assistant_timer_left_ms();

// A question already recorded (16-bit mono, what was said after the wake
// word that wasn't a command): saved as _ask.wav and sent to the phone,
// with the assistant showing the answer. False without the phone link or
// the SD card - then nothing happens.
bool assistant_ask_recorded(const int16_t *pcm, int samples, int rate);
