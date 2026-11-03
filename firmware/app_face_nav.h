/*
 * app_face_nav.h
 * Getting around from the watch face, the watchOS way:
 *
 *   swipe up         the Smart Stack - cards for what matters now (a timer
 *                    running, music playing, the next event, weather,
 *                    activity, messages, battery); tap one to open its app
 *   touch and hold   the face switcher - every face as a small preview,
 *                    swipe between them, tap one to wear it, "Edit" for
 *                    its complications
 *   BOOT (on a face) Control Center (the top panel); elsewhere it's still back
 */
#pragma once
#include "ui.h"

extern Screen smart_stack_screen;
extern Screen face_switch_screen;

// True when s is one of the watch faces (watchface_registry).
bool face_nav_is_face(const Screen *s);

// ui.cpp calls these for touches on a face, and every frame: a still hold
// of ~0.6 s opens the face switcher. True when the touch was taken.
void face_nav_touch(Screen *face, int x, int y, bool pressed);
void face_nav_poll();
