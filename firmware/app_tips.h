/*
 * app_tips.h
 * Tips: every gesture and button the watch uses, one page each with a small
 * animated watch doing it - swipe left / up / right / down, touch and hold
 * the face, the edge swipe back, the side and power buttons, the wrist
 * flick. Swipe or tap to page; shown once after this update (tips_seen),
 * then in the app menu and Settings > System.
 */
#pragma once
#include "ui.h"

void tips_show_if_new();   // setup(), after the face is up
extern Screen tips_screen;
