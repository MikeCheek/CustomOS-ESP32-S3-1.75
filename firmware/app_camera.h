/*
 * app_camera.h
 * Camera remote for the phone, the way directions work: when the app's
 * camera (Camera remote) opens on the phone, the watch shows the remote by
 * itself - a live view, the shutter, the self-timer (3 / 10 s, counted
 * down on the watch), flash, front/back, zoom, photo or video - and closes
 * it when the camera does. When another app opens the camera (Android),
 * the watch offers to open the app's one. From the menu it asks the phone
 * to open it.
 *
 *   phone -> watch  {"t":"cam","on":1,"tm":3,"fl":"auto","fr":0,"vm":0,"z":10,"rec":0}   (any of them)
 *                   {"t":"cam","on":1,"ext":1}   another app's camera   {"t":"cam","on":0}
 *                   {"t":"cam","cd":3}   the self-timer        {"t":"cam","ok":1}   a photo was taken
 *                   live view: image frames of kind 'p' (phone_images.h), ~2 a second
 *   watch -> phone  {"e":"cam","a":"shot"|"flip"|"mode"|"timer"|"flash"|"zoom-"|"zoom+"|"open"}
 */
#pragma once
#include "ui.h"
#include <ArduinoJson.h>

extern Screen camera_screen;
void camera_on_link(JsonDocument &doc);   // phone link "cam"
