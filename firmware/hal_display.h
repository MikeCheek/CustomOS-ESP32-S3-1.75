/*
 * hal_display.h
 * CO5300 AMOLED via Arduino_GFX (no LVGL), driven by a background
 * "panel sender" task.
 *
 * Frame pipeline (the trick borrowed from Meta's MUSE firmware, adapted
 * to this project's own renderer):
 *
 *   loop() / core 1                      lcd_send task / core 0
 *   ------------------------------       ------------------------------
 *   draw frame N+1 into a free buffer    diff frame N against what the
 *   display_fb_submit(N+1)  ------->     panel already shows, send only
 *   carry on (touch, logic, ...)         the changed rectangles
 *
 * Drawing and sending overlap instead of alternating, and a frame where
 * only the clock digits changed sends a few thousand pixels instead of
 * all 217k. Every access to the panel (pixels, brightness, sleep) goes
 * through that one task, so nothing else may call into display_gfx()
 * directly once display_init() has returned.
 *
 * Buffers are full 466x466 RGB565 frames in PSRAM, in a small pool:
 * one being drawn, one queued, one being sent, one = "what the panel
 * shows" (the diff reference). A newer submitted frame replaces a
 * queued one that hasn't started sending yet - the panel always gets
 * the latest frame, never a backlog.
 */

#pragma once
#include <cstdint>
#include <cstddef>

struct Arduino_GFX;

void display_init();
bool display_is_ok();
void display_set_brightness(uint8_t level);   // applied by the sender between frames
uint8_t display_get_brightness();

// AMOLED hardware sleep/wake - blocks until the sender has done it.
void display_sleep();
void display_wakeup();

// The raw panel driver. Only ui.cpp uses this, and only as the
// Arduino_Canvas "output" pointer (it never flushes through it) - see
// the note above.
Arduino_GFX *display_gfx();

// ---- Frame pipeline ------------------------------------------------------
// Adopt `first` (an existing full-frame buffer, e.g. the canvas's own) as
// pool buffer 0 and allocate the rest. Returns the number of buffers in
// the pool (>= 2 means the pipeline is running).
int display_pipeline_start(uint16_t *first);

// A buffer nobody else is using, for the caller to draw into.
// allow_drop: if no buffer is free, reclaim the queued-but-not-yet-sent
// frame (it's about to be superseded anyway) instead of waiting.
uint16_t *display_fb_acquire(bool allow_drop);
// Give back a buffer acquired but not submitted.
void display_fb_release(uint16_t *fb);
// Hand a finished frame to the sender. The caller must not touch it again.
void display_fb_submit(uint16_t *fb);
// The most recently submitted frame (read-only) - for screen-transition
// snapshots. Valid until the caller's own next display_fb_acquire().
const uint16_t *display_fb_last_submitted();
// Next frame is sent in full instead of diffed (after wake, etc.).
void display_force_full_refresh();

struct DisplayStats {
    uint32_t frames_sent;
    uint32_t frames_dropped;    // superseded while queued
    uint32_t frames_skipped;    // identical to what's shown - nothing sent
    uint32_t px_sent;           // pixels actually pushed over QSPI
    uint32_t send_ms_total;
};
DisplayStats display_get_stats();
