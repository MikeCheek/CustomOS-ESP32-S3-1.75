/*
 * face_look.h
 * The watch's look, as the app's Faces tab sets it: which face is worn,
 * the main face's accent colour and parts (seconds arc, eyes, date, pet),
 * the slots, and display options (brightness, always-on, weather effects,
 * 24-hour clock, text size, app menu as a list).
 *
 *   phone -> watch  {"t":"look","wf":1,"acc":3,"sec":1,"eyes":1,"date":1,"pet":0,
 *                    "wx":1,"aod":0,"h24":1,"ts":0,"ml":0,"br":200}   (any of them)
 *   watch -> phone  {"e":"look","wf":1,"faces":["Eyes","Minimal","Custom","Photo"],
 *                    "acc":3,...,"comp":["steps","rings","battery"]}
 * The watch sends its look on "hi" and whenever it changes there (the face
 * switcher, Settings), so the app always shows what the watch shows.
 */
#pragma once
#include <stdint.h>
#include <ArduinoJson.h>

#define FACE_ACCENT_COUNT 8
uint16_t face_accent();                 // the main face's colour now

void look_on_link(JsonDocument &doc);   // phone link "look"
void look_send();                       // {"e":"look",...} now
void look_update();                     // loop(): sends it again after a change on the watch
