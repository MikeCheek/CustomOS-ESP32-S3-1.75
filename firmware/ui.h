/*
 * ui.h
 * Screen manager + drawing helpers for round AMOLED watch.
 *
 * Features:
 *  - Per-screen frame rate (games get 60fps, UI gets 30fps)
 *  - Round display clipping awareness
 *  - Top panel overlay (swipe-down status/quick-settings)
 *  - Confirmation modal
 *  - Queued toasts
 *  - Global swipe-left-to-go-back
 *  - Touch feedback
 */

#pragma once
#include <Arduino.h>
#include <cstdint>
#include "config.h"

struct Arduino_GFX;
struct Arduino_Canvas;

// ---- Gesture types --------------------------------------------------------
enum Gesture : uint8_t {
    GESTURE_NONE = 0,
    GESTURE_SWIPE_RIGHT,
    GESTURE_SWIPE_LEFT,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN,
};

// ---- Gesture mode per screen ----------------------------------------------
enum GestureMode : uint8_t {
    GESTURE_MODE_NONE = 0,
    GESTURE_MODE_EDGE,
    GESTURE_MODE_FREE,
};

// ---- Screen interface ----------------------------------------------------
typedef void (*ScreenCreateFn)();
typedef void (*ScreenDrawFn)();
typedef void (*ScreenTouchFn)(int x, int y, bool pressed);
typedef void (*ScreenTickFn)();
typedef void (*ScreenDestroyFn)();
typedef void (*ScreenGestureFn)(Gesture g);

struct Screen {
    const char      *title;
    GestureMode      gesture_mode;
    uint8_t          frame_ms;     // 0 = uncapped (games), 33 = 30fps (UI), 16 = 60fps
    ScreenCreateFn   on_create;
    ScreenDrawFn     on_draw;
    ScreenTouchFn    on_touch;
    ScreenTickFn     on_tick;
    ScreenDestroyFn  on_destroy;
    ScreenGestureFn  on_gesture;
    // Added at the END with a default, not inserted earlier in the
    // struct - every existing Screen definition in this project uses
    // positional brace-init ({ "title", MODE, 33, create, draw, ... }),
    // so a field inserted in the middle would silently shift every
    // later field in every one of those definitions. 0 = no idle
    // throttling (default, matches old behavior exactly for every
    // screen that doesn't opt in). Non-zero = once
    // sleep_ms_since_activity() exceeds IDLE_THROTTLE_AFTER_MS (see
    // ui.cpp), ui_update() paces this screen at idle_frame_ms instead
    // of frame_ms - for screens that are mostly static when nothing's
    // being touched (the home watchface, the app menu) so the
    // expensive full-framebuffer flush to the display happens far less
    // often during genuinely idle stretches.
    uint16_t         idle_frame_ms = 0;
    // Same safe-append reasoning as idle_frame_ms above. true = while
    // this screen is active, suppress both auto-dim and the sleep
    // timeout regardless of touch/button inactivity - for games where
    // long touch-free stretches are normal gameplay (watching a
    // sequence play out, an idle moment between waves), not the user
    // having put the watch down. Checked via
    // ui_current_screen_suppresses_idle() in AmoledSmartWatchOS.ino
    // and hal_sleep.cpp, both of which otherwise only know about
    // touch/button activity, not what's actually happening on screen.
    bool             suppress_idle = false;
    // Same safe-append reasoning as the fields above. true = this
    // screen needs the tilt sensor calibrated before it makes sense to
    // use (a tilt-controlled game, mainly) - checked centrally in
    // ui_push() rather than at each individual call site, so it
    // catches every path that could open this screen, not just the
    // app menu's tap-to-launch. If tilt isn't calibrated yet, ui_push()
    // shows a confirm popup instead of opening the screen directly;
    // confirming opens the calibration app, declining just does
    // nothing further (this screen doesn't open either way until the
    // user re-taps it after calibrating - see ui_push()'s own comment
    // for why that's the deliberate, simpler choice here).
    bool             needs_tilt_calibration = false;
    // Same safe-append reasoning. true = never slide this screen in or
    // out (ui_push/ui_pop_screen animate between ordinary screens) - for
    // overlays with their own entrance animation, e.g. the lock/boot
    // particle animation, charging, power-off, find-me, onboarding.
    bool             no_transition = false;
    // true = no finger-following edge-drag back on this screen, for
    // screens whose GESTURE_SWIPE_RIGHT does something other than
    // "leave" (go up a folder, back a step inside the screen). The
    // classic edge swipe still reaches on_gesture as before.
    bool             no_drag_back = false;
    // true = don't draw the global battery/WiFi readout (top-right) over
    // this screen - for screens that use the screen edge themselves
    // (progress/indicator rings drawn with ui_arc()/ui_edge_ring()).
    bool             hide_status = false;
};

// ---- Screen manager ------------------------------------------------------
#define UI_MAX_STACK 8

void ui_init(Arduino_GFX *display);
void ui_push(Screen *screen);
void ui_pop_screen();
void ui_go_home();
void ui_update();
// True if the currently active screen has suppress_idle set (see the
// Screen struct above) - for games where long touch-free stretches
// are normal gameplay, not idleness.
bool ui_current_screen_suppresses_idle();
// Is the screen anywhere on the stack / the top one? (A screen's own
// create/destroy can't tell: being covered also calls on_destroy.)
bool ui_screen_in_stack(const Screen *screen);
bool ui_screen_on_top(const Screen *screen);
void ui_handle_touch(int x, int y, bool pressed);
void ui_tick();
void ui_poll_gestures();
Arduino_GFX *ui_gfx();

// Clears in-progress auto-rotate state (angle, smoothing filters).
// Call after toggling auto_rotate off, or when suspending/restoring it
// around a tilt-calibration flow — see app_calibration.cpp.
void ui_reset_auto_rotate_state();

// ---- Display geometry (round) --------------------------------------------
int ui_screen_radius();
bool ui_is_on_circle(int x, int y);
int ui_circle_margin();

// ---- Drawing helpers ------------------------------------------------------
void ui_draw_centered_text(int y, uint16_t color, const char *text, int size = 2);
void ui_draw_centered_text_at(int x_center, int y, uint16_t color, const char *text, int size = 2);
// A single list row as a rounded card: primary label, optional dimmer
// description line below it (pass nullptr/"" for none). Wear OS's
// Chip component shape (label + secondary label, on a rounded
// container) - for any screen showing a list of named items with
// optional detail, rather than each screen inventing its own row
// styling. selected draws the row filled/highlighted (COLOR_ACCENT)
// instead of just outlined, for the currently-focused/active item in
// a list, if the screen has such a concept.
void ui_draw_list_row(int x, int y, int w, int h, const char *label,
                      const char *desc = nullptr, bool selected = false);
struct TileRect { int x, y, w, h; };
TileRect ui_draw_tile(int col, int row, uint16_t accent,
                       const char *icon_text, const char *label);
TileRect ui_draw_header(const char *title);

// ---- Round-screen helpers ---------------------------------------------------
// Angles are in degrees CLOCKWISE FROM 12 O'CLOCK (0 = top, 90 = right,
// 180 = bottom, 270 = left) - the natural convention for a watch face.
// Negative sweeps go counter-clockwise.
//
// ui_arc() draws a thick arc as a strip of small triangles - fast enough
// to redraw every frame, unlike Arduino_GFX::fillArc(), which tests
// every pixel of the arc's bounding box (~217k tests for a screen-edge
// ring). Optional round end caps.
void ui_arc(int cx, int cy, int r_outer, int thickness, float start_deg, float sweep_deg,
            uint16_t color, bool round_caps = true);
// An arc hugging the round screen's edge (outer radius = screen radius - inset).
void ui_edge_ring(float start_deg, float sweep_deg, uint16_t color, int thickness = 8,
                  int inset = 4, bool round_caps = true);
// Point at (angle, radius) from the screen centre.
void ui_polar(float deg, float radius, int *x, int *y);
// Touch position -> angle (clock degrees, 0..360) / distance from centre.
float ui_angle_of(int x, int y);
int   ui_radius_of(int x, int y);
// Is clock-angle `deg` inside the arc starting at `start` sweeping `sweep`?
bool  ui_angle_in_arc(float deg, float start, float sweep);
// RGB565 colour scaled toward black (f = 1 unchanged, 0 = black).
uint16_t ui_dim(uint16_t c, float f);
// Text centred on (cx, cy) - both axes, unlike ui_draw_centered_text_at().
void ui_text_center(int cx, int cy, uint16_t color, const char *text, int size);

// Rounded rect that respects circle boundary (corners outside circle are not drawn)
void ui_fill_circle_rect(int x, int y, int w, int h, int r, uint16_t color);

// ---- Toast (queued, top-mounted) -----------------------------------------
#define TOAST_QUEUE_SIZE 4
void ui_show_toast(const char *text, uint32_t duration_ms = 2500);
void ui_draw_toast();
bool ui_toast_active();
// Hit-tests (x,y) against the currently-drawn toast and dismisses it if
// it hits. Returns true if a toast was dismissed (caller should treat
// the touch as consumed). Called internally by ui_handle_touch.
bool ui_toast_touch(int x, int y);

// ---- Confirmation modal ---------------------------------------------------
typedef void (*ConfirmCallback)(bool confirmed);
void ui_show_confirm(const char *title, const char *body,
                     const char *yes_label, const char *no_label,
                     ConfirmCallback cb);
void ui_draw_confirm();
void ui_confirm_touch(int x, int y, bool pressed);
bool ui_confirm_active();

// ---- Top panel overlay ----------------------------------------------------
void ui_top_panel_toggle();
void ui_top_panel_open();
void ui_top_panel_close();
bool ui_top_panel_is_open();
void ui_top_panel_draw();
void ui_top_panel_touch(int x, int y, bool pressed);
// (ui_panel.cpp) is_open() is true from the moment it starts opening until
// it has finished sliding away; covers_screen() only while fully down.
bool ui_top_panel_animating();
bool ui_top_panel_covers_screen();
bool ui_top_panel_busy();          // a slider drag or tile press in progress

// ---- Keep awake -------------------------------------------------------------
// Runtime override (top panel tile): no auto-dim, no auto-sleep while set.
void ui_set_keep_awake(bool on);
bool ui_keep_awake();

// ---- Touch feedback -------------------------------------------------------
void ui_draw_touch_feedback();

// ---- Kinetic vertical scrolling --------------------------------------------
// One shared scroller for every list (settings rows, network list...), so
// they all feel like MUSE's/LVGL's lists: content tracks the finger 1:1,
// keeps coasting after a flick and slows down with friction, rubber-bands
// past either end, and settles on a whole row instead of stopping half
// way through one.
//
//   static UiScroll s_scroll;
//   create:  ui_scroll_reset(&s_scroll, content_h - visible_h, ROW_PITCH);
//   touch:   if (ui_scroll_touch(&s_scroll, y, pressed)) { ...tap at y... }
//   draw:    int off = ui_scroll_offset(&s_scroll);  // content y - off = screen y
//   tick:    ui_scroll_tick(&s_scroll);  (or call it at the top of draw)
struct UiScroll {
    float    pos = 0;          // px scrolled (0 = top)
    float    vel = 0;          // px/ms, + = content moving up
    int      max = 0;          // largest resting pos
    int      snap = 0;         // row pitch to settle on, 0 = free
    bool     dragging = false;
    bool     moved = false;    // this touch moved enough to not be a tap
    bool     caught = false;   // this touch stopped a coasting list - not a tap either
    int      start_y = 0;
    float    start_pos = 0;
    int      last_y = 0;
    uint32_t last_ms = 0;
    uint32_t tick_ms = 0;
};
void ui_scroll_reset(UiScroll *s, int max_scroll, int snap_pitch);
// Changes the scroll range without resetting position (content grew).
void ui_scroll_set_max(UiScroll *s, int max_scroll);
// Feed every touch event. Returns true on the release of a tap (finger
// didn't travel and the list wasn't coasting) - hit-test at that y.
bool ui_scroll_touch(UiScroll *s, int y, bool pressed);
// Advance momentum/snap/spring. Call once per frame.
void ui_scroll_tick(UiScroll *s);
int  ui_scroll_offset(const UiScroll *s);
bool ui_scroll_is_moving(const UiScroll *s);

// ---- Default frame rates --------------------------------------------------
#define UI_FRAME_MS_DEFAULT  33   // 30fps for normal UI
#define UI_FRAME_MS_GAME      0   // uncapped for games
#define UI_FRAME_MS_SMOOTH   16   // 60fps for animations
