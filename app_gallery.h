/*
 * app_gallery.h
 * Gallery: every photo and video on the SD card in a scrolling grid of
 * round thumbnails, and a full-screen viewer that swipes between them.
 *
 *   Images: .jpg/.jpeg, .png, .bmp     Videos: .avi/.mov/.mp4 (MJPEG, see app_video.h)
 *
 * Viewer: swipe left/right = next/previous, double-tap = zoom in at that
 * spot (drag to pan, double-tap again to fit), tap = show/hide info and
 * controls (slideshow, back), swipe up or BOOT = back to the grid.
 */
#pragma once
#include "ui.h"

extern Screen gallery_screen;
extern Screen gallery_viewer_screen;

// From the Files/Media browser: open `path` in the viewer, with the other
// images/videos of the same folder one swipe away.
void gallery_open_file(const char *path);
