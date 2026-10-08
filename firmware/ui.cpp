#include "ui.h"
#include "ui_font.h"
#include "board_pins.h"
#include "hal_touch.h"
#include "hal_imu.h"
#include "hal_power.h"
#include "hal_ble.h"
#include "hal_display.h"
#include "hal_sleep.h"
#include "hal_wifi.h"
#include "hal_ntp.h"
#include "hal_gps.h"
#include "hal_vibrate.h"
#include "icons.h"
#include "hal_nvs.h"
#include "app_settings_state.h"
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---- Canvas / GFX --------------------------------------------------------
// SwapCanvas: thin subclass that lets us point the canvas at any frame
// buffer from hal_display's pool - every frame is drawn into whichever
// buffer is free, then handed to the background panel sender (see
// hal_display.h) instead of being flushed synchronously here.
class SwapCanvas : public Arduino_Canvas {
public:
    using Arduino_Canvas::Arduino_Canvas;
    void setFramebuffer(uint16_t *buf) { _framebuffer = buf; }
};

static Arduino_GFX   *s_gfx      = nullptr;
static SwapCanvas    *s_canvas   = nullptr;
static uint16_t      *s_render   = nullptr;  // pool buffer the canvas currently draws into
static bool           s_pipeline = false;    // false = fallback to synchronous flush()

#define FB_PIXELS ((size_t)LCD_WIDTH * LCD_HEIGHT)

// ---- Screen transitions (slide on push/pop, finger-following back) -------
// One extra full frame in PSRAM, allocated on first use. It holds either
// the outgoing screen during a push/pop slide, or - right after a push -
// the screen *underneath* the current one, which is what slides in from
// the left while you drag the current screen away with an edge swipe.
static uint16_t *s_snap = nullptr;
static bool      s_snap_tried = false;
static Screen   *s_snap_below_of = nullptr; // s_snap shows the screen below this one

enum TransKind : uint8_t { TR_NONE, TR_PUSH, TR_POP, TR_DRAG, TR_DRAG_SETTLE };
static TransKind s_tr = TR_NONE;
static uint32_t  s_tr_start_ms = 0;
static float     s_tr_from = 0.0f;   // starting offset (px) for settle animations
static float     s_tr_to = 0.0f;     // target offset (px)
static bool      s_tr_commit = false; // DRAG_SETTLE: pop once it reaches the edge
static const uint32_t TR_SLIDE_MS  = 240;
static const uint32_t TR_SETTLE_MS = 180;

// Edge-swipe drag state
enum DragState : uint8_t { DRAG_IDLE, DRAG_PENDING, DRAG_ACTIVE };
static DragState s_drag = DRAG_IDLE;
static int       s_drag_x0 = 0, s_drag_y0 = 0;   // logical coords of the press
static int       s_drag_last_x = 0, s_drag_last_y = 0;
static uint32_t  s_drag_t0 = 0;
static float     s_drag_off = 0.0f;              // px the screen has been dragged right
static float     s_drag_vel = 0.0f;              // px/ms, smoothed
static uint32_t  s_drag_last_ms = 0;
static const int DRAG_EDGE_PX = 48;              // a press this close to the left edge may become a drag
static const int DRAG_START_PX = 14;             // horizontal travel that commits to dragging

// ---- Screen stack ---------------------------------------------------------
struct StackEntry { Screen *screen; };
static StackEntry s_stack[UI_MAX_STACK];
extern Screen calibration_screen; // for ui_push()'s tilt-calibration gate below
static int s_top = -1;

// ---- Frame rate -----------------------------------------------------------
static uint32_t s_last_flush_ms = 0;

// ---- Touch feedback -------------------------------------------------------
static bool     s_tf_active = false;
static uint16_t s_tf_x = 0;
static uint16_t s_tf_y = 0;
static uint32_t s_tf_until = 0;
static uint16_t s_last_touch_x = 0;
static uint16_t s_last_touch_y = 0;
// Tracks which screen actually received the "press" for the touch
// sequence currently in progress, and whether one is in progress at
// all - see ui_handle_touch()'s screen-change handling for why.
static bool s_touch_seq_active = false;
static Screen *s_touch_seq_screen = nullptr;

// ---- Toast queue ----------------------------------------------------------
struct ToastEntry {
    char     text[96];
    uint32_t until_ms;
};
static ToastEntry s_toast_queue[TOAST_QUEUE_SIZE];
static int s_toast_head = 0;
static int s_toast_tail = 0;
// Screen rect of whatever toast is currently drawn, updated each
// ui_draw_toast() call - lets ui_handle_touch() hit-test a tap for
// dismissal without duplicating the layout math.
static int s_toast_rect_x = 0, s_toast_rect_y = 0, s_toast_rect_w = 0, s_toast_rect_h = 0;

// ---- Confirmation modal ---------------------------------------------------
static char         s_confirm_title[48]  = "";
static char         s_confirm_body[96]   = "";
static char         s_confirm_yes[24]    = "Yes";
static char         s_confirm_no[24]     = "No";
static ConfirmCallback s_confirm_cb      = nullptr;
static bool         s_confirm_active     = false;

// ---- Auto-rotate (IMU) — continuous 360° rotation --------------------------
static int      s_rot_angle = 0;       // current rotation in degrees (0-359)
static int      s_rot_applied = 0;     // last angle actually sent to rotate_framebuffer
static uint32_t s_rot_last_ms = 0;
static const uint32_t AUTO_ROT_INTERVAL_MS = 50; // IMU at ~20 Hz
static float    s_rot_ax_smooth = 0;
static float    s_rot_ay_smooth = 0;
static const float SMOOTH_ALPHA = 0.35f;
static const int ROT_HYSTERESIS = 3;   // only re-rotate when angle changes by ≥3°

// 360-entry fixed-point sin/cos × 1024 for 1° resolution
static int16_t S_SIN360[360];
static int16_t S_COS360[360];
static bool    s_rot_tables_init = false;

static void init_rot_tables() {
    if (s_rot_tables_init) return;
    for (int i = 0; i < 360; i++) {
        float rad = (float)i * (float)M_PI / 180.0f;
        S_SIN360[i] = (int16_t)(sinf(rad) * 1024.0f + 0.5f);
        S_COS360[i] = (int16_t)(cosf(rad) * 1024.0f + 0.5f);
    }
    s_rot_tables_init = true;
}

// ---- Circle geometry ------------------------------------------------------
static const int S_CX = LCD_WIDTH / 2;
static const int S_CY = LCD_HEIGHT / 2;
static const int S_R  = LCD_WIDTH / 2;

static void init_circle_bounds();  // forward decl

// ---- Init -----------------------------------------------------------------
void ui_init(Arduino_GFX *display) {
    s_top = -1;
    s_canvas = new SwapCanvas(LCD_WIDTH, LCD_HEIGHT, display, 0, 0, 0);
    s_canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
    s_gfx = s_canvas;
    memset(s_toast_queue, 0, sizeof(s_toast_queue));
    init_rot_tables();
    init_circle_bounds();
    // The canvas's own framebuffer becomes buffer 0 of the display
    // pipeline's pool; from here on the canvas is re-pointed at a fresh
    // pool buffer every frame (see ui_update()).
    // With only one buffer (no spare PSRAM) the pipeline still works,
    // just without overlap: acquiring the next frame waits for the
    // previous one to finish sending.
    int pool = display_pipeline_start(s_canvas->getFramebuffer());
    s_pipeline = pool >= 1;
    if (s_pipeline) {
        s_render = display_fb_acquire(false);
        if (s_render) s_canvas->setFramebuffer(s_render);
        else s_pipeline = false;
    }
    if (!s_pipeline) {
        // Panel/pipeline never came up - keep the old synchronous flush.
        s_render = s_canvas->getFramebuffer();
    }
    DEBUG_PRINTF("[ui] frame pipeline %s (%d buffers)\n", s_pipeline ? "on" : "OFF", pool);
}

Arduino_GFX *ui_gfx() { return s_gfx; }
uint16_t *ui_framebuffer() { return s_render; }

bool ui_render_screen_below(uint16_t *dst) {
    if (!s_canvas || !dst || s_top < 1) return false;
    Screen *below = s_stack[s_top - 1].screen;
    if (!below || !below->on_draw) return false;
    uint16_t *saved = s_canvas->getFramebuffer();
    s_canvas->setFramebuffer(dst);
    s_canvas->fillScreen(COLOR_BG);
    below->on_draw();
    if (below->title && below->title[0]) ui_draw_header(below->title);
    s_canvas->setFramebuffer(saved);
    return true;
}

// ---- Display geometry -----------------------------------------------------
int ui_screen_radius() { return S_R; }
int ui_circle_margin() { return 16; }

bool ui_is_on_circle(int x, int y) {
    int32_t dx = (int32_t)x - S_CX;
    int32_t dy = (int32_t)y - S_CY;
    return (dx * dx + dy * dy) <= (int32_t)S_R * S_R;
}

// ---- Auto-rotate -----------------------------------------------------------
// Precomputed circle row bounds: for each row, the leftmost and rightmost
// pixel inside the circle. Avoids per-pixel circle test.
static int s_circ_left[LCD_HEIGHT];
static int s_circ_right[LCD_HEIGHT];

static void init_circle_bounds() {
    int half = LCD_WIDTH / 2;
    int r_sq = half * half;
    for (int dy = 0; dy < LCD_HEIGHT; dy++) {
        int dyc = dy - half;
        int rem = r_sq - dyc * dyc;
        if (rem < 0) { s_circ_left[dy] = LCD_WIDTH; s_circ_right[dy] = -1; continue; }
        int dx_max = (int)sqrtf((float)rem);
        int l = half - dx_max;
        int r = half + dx_max;
        if (l < 0) l = 0;
        if (r >= LCD_WIDTH) r = LCD_WIDTH - 1;
        s_circ_left[dy]  = l;
        s_circ_right[dy] = r;
    }
}

// Software framebuffer rotation using INVERSE mapping for PSRAM cache efficiency.
// Instead of: for each dst pixel, find src pixel (random PSRAM read)
// We do:      for each src pixel, write to dst pixel (sequential PSRAM read)
// After rotation, swap the canvas framebuffer pointer — no memcpy needed.
// Rotates the current render buffer into a second pool buffer, then makes
// that the render buffer (the unrotated one goes back to the pool).
static void rotate_framebuffer(int angle_deg) {
    if (angle_deg == 0 || !s_render) return;
    angle_deg = angle_deg % 360;
    if (angle_deg < 0) angle_deg += 360;

    uint32_t t_start = millis();
    uint16_t *src = s_render;
    uint16_t *dst = s_pipeline ? display_fb_acquire(true) : nullptr;
    if (!dst) return; // no spare buffer right now - show this frame unrotated
    const int w = LCD_WIDTH;
    const int h = LCD_HEIGHT;
    const int16_t sv = S_SIN360[angle_deg];
    const int16_t cv = S_COS360[angle_deg];
    const int32_t cx2 = w;                 // center × 2  (= 466 for 466px)

    // TRUE inverse mapping: iterate DESTINATION pixels (guarantees every
    // one gets written exactly once, so there can be no gaps) and
    // compute which SOURCE pixel each one's color comes FROM, via the
    // inverse rotation - the transpose of the forward rotation matrix
    // used below, i.e. same cv/sv values with the sign on the sin term
    // flipped between the two component equations.
    //
    // This replaces a genuinely different approach that used to iterate
    // SOURCE pixels and scatter each one forward into a computed
    // destination position. Forward/scatter mapping has a well-known
    // problem on a discrete pixel grid: at non-90-degree rotation
    // angles, integer rounding means some destination pixels receive
    // more than one source write while others receive none at all -
    // gaps. Since the destination buffer was never cleared first (it's
    // just whichever framebuffer wasn't current a moment ago), any
    // pixel that didn't get a fresh write kept showing whatever content
    // was already sitting in that buffer from an earlier frame at a
    // different rotation angle - exactly the reported "artifacts in
    // previous positions while rotating." Iterating the destination
    // instead eliminates the gap case entirely, by construction.
    for (int dy = 0; dy < h; dy++) {
        int left  = s_circ_left[dy];
        int right = s_circ_right[dy];
        if (left > right) continue;
        int32_t oy2 = dy * 2 + 1 - cx2;
        for (int dx = left; dx <= right; dx++) {
            int32_t ox2 = dx * 2 + 1 - cx2;
            int32_t raw_sx =  (int32_t)cv * ox2 + (int32_t)sv * oy2;
            int32_t raw_sy = -(int32_t)sv * ox2 + (int32_t)cv * oy2;
            int32_t sx2 = (raw_sx >= 0 ? (raw_sx >> 10) : -(((-raw_sx) >> 10))) + cx2;
            int32_t sy2 = (raw_sy >= 0 ? (raw_sy >> 10) : -(((-raw_sy) >> 10))) + cx2;
            int sx = (sx2 + 1) / 2;
            int sy = (sy2 + 1) / 2;
            // Round display: rotation preserves circle radius, so every
            // destination pixel inside the circle maps to a source
            // pixel also inside the framebuffer - this bounds check is
            // a safety net (write black rather than leave whatever
            // garbage was in a freshly allocated buffer), not expected
            // to actually trigger in normal operation.
            if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
                dst[(size_t)dy * w + dx] = src[(size_t)sy * w + sx];
            } else {
                dst[(size_t)dy * w + dx] = 0;
            }
        }
        // Yield every 10 rows (was 40) to give touch/BLE processing
        // more chances to run during this loop - the loop itself is a
        // real, necessary per-frame cost (see the comment at this
        // function's call site for why it can't just be skipped), so
        // this doesn't make the rotation faster, but it does stop it
        // from hogging the CPU in one long uninterrupted stretch, which
        // is what actually made touch feel unresponsive while rotated.
        if (dy % 10 == 0) yield(); // was (dy & 9) == 0, which is not "every 10 rows"
    }

    // The rotated buffer becomes this frame; the unrotated one is free again.
    display_fb_release(src);
    s_render = dst;
    s_canvas->setFramebuffer(dst);

    uint32_t t_loop_end = millis();
    static uint32_t s_last_rot_detail = 0;
    if (t_loop_end - s_last_rot_detail > 2000) {
        DEBUG_PRINTF("[rot] inverse_loop=%lums\n", (unsigned long)(t_loop_end - t_start));
        s_last_rot_detail = t_loop_end;
    }
}

static void ui_update_auto_rotate() {
    if (!g_app_settings.auto_rotate || !s_render) return;

    uint32_t now = millis();
    if (now - s_rot_last_ms < AUTO_ROT_INTERVAL_MS) return;
    s_rot_last_ms = now;

    ImuSample s = imu_read();
    if (!s.valid) {
        static uint32_t s_last_warn = 0;
        if (now - s_last_warn > 2000) {
            DEBUG_PRINTF("[rot] IMU not ready\n");
            s_last_warn = now;
        }
        return;
    }

    // Deliberately RAW accelerometer values here, NOT
    // imu_get_calibrated_tilt() - that calibration (app_calibration.cpp's
    // tilt steps) exists to make GAME steering feel intuitive to a
    // person, which is a different problem with different correctness
    // requirements than screen-orientation detection. Auto-rotate needs
    // a fixed, known relationship between gravity direction and the
    // physical display orientation - it doesn't care how a person
    // prefers a game to steer. Routing it through the same
    // user-adjustable calibration meant that calibrating tilt for games
    // (which can legitimately flip a sign to make steering feel right
    // for a given IMU mounting) silently also flipped auto-rotate's
    // sense of direction - exactly the "rotates on the opposite side
    // after calibration" report. Using the raw sample here makes
    // auto-rotate's behavior fixed and independent of whatever the user
    // has calibrated tilt controls to.
    //
    // The -s.ay (not +s.ay) matters and was wrong in an earlier pass at
    // this fix: it's not an arbitrary choice, it's this board's actual
    // fixed IMU-to-display mounting relationship - confirmed from
    // app_settings_state.h's own DEFAULT calibration values
    // (tilt_sign_y = -1, tilt_map_y_from = 1, i.e. "y comes from raw ay,
    // negated"), which is what auto-rotate used to get for free by
    // going through imu_get_calibrated_tilt() at default settings.
    // Dropping the calibration call without carrying over this specific
    // default sign is what caused the screen to land 90 degrees off
    // from where it should be.
    float tx = s.ax, ty = -s.ay;

    // Low-pass filter on gravity components
    s_rot_ax_smooth = s_rot_ax_smooth * (1.0f - SMOOTH_ALPHA) + tx * SMOOTH_ALPHA;
    s_rot_ay_smooth = s_rot_ay_smooth * (1.0f - SMOOTH_ALPHA) + ty * SMOOTH_ALPHA;

    float ax = s_rot_ax_smooth;
    float ay = s_rot_ay_smooth;

    // Deadzone — don't change rotation when nearly centered
    const float DEADZONE = 0.15f;
    if (fabsf(ax) < DEADZONE && fabsf(ay) < DEADZONE) return;

    // Compute display rotation angle from the raw (smoothed) gravity
    // vector - see the comment above tx/ty for why this is
    // deliberately not routed through any calibration.
    float watch_angle = atan2f(ax, ay) * 180.0f / (float)M_PI;
    if (watch_angle < 0.0f) watch_angle += 360.0f;
    float angle = 360.0f - watch_angle;
    if (angle >= 360.0f) angle -= 360.0f;

    int new_angle = (int)(angle + 0.5f) % 360;

    // Hysteresis: only update if the angle changed by more than ROT_HYSTERESIS degrees
    int diff = new_angle - s_rot_angle;
    if (diff > 180) diff -= 360;
    if (diff < -180) diff += 360;
    if (abs(diff) < ROT_HYSTERESIS) return;

    static uint32_t s_last_rot_log = 0;
    if (now - s_last_rot_log > 1000) {
        DEBUG_PRINTF("[rot] ax=%.2f ay=%.2f angle=%d -> %d\n", ax, ay, s_rot_angle, new_angle);
        s_last_rot_log = now;
    }
    s_rot_angle = new_angle;
}

// Clears any in-progress auto-rotate state (target angle, applied angle,
// smoothing filters). Used when the user toggles auto-rotate off, and
// by app_calibration.cpp when it suspends/restores auto-rotate around
// the tilt-calibration flow, so a stale rotation doesn't linger or
// snap oddly once calibration changes what the raw axes mean.
void ui_reset_auto_rotate_state() {
    s_rot_angle = 0;
    s_rot_applied = 0;
    s_rot_ax_smooth = 0;
    s_rot_ay_smooth = 0;
}

// ---- Screen helpers -------------------------------------------------------
static Screen *active_screen() {
    if (s_top < 0) return nullptr;
    return s_stack[s_top].screen;
}

// ---- Keep awake (quick panel "coffee" tile) ---------------------------------
// Runtime only - a reboot always goes back to normal sleep behaviour.
static bool s_keep_awake = false;
void ui_set_keep_awake(bool on) { s_keep_awake = on; }
bool ui_keep_awake() { return s_keep_awake; }

bool ui_screen_in_stack(const Screen *screen) {
    for (int i = 0; i <= s_top; i++) if (s_stack[i].screen == screen) return true;
    return false;
}

bool ui_screen_on_top(const Screen *screen) {
    return s_top >= 0 && s_stack[s_top].screen == screen;
}

bool ui_current_screen_suppresses_idle() {
    if (s_keep_awake) return true;   // keep-awake tile in the top panel
    Screen *scr = active_screen();
    return scr && scr->suppress_idle;
}


static void sync_gesture_mode() {
    Screen *scr = active_screen();
    touch_set_gesture_mode(scr ? (uint8_t)scr->gesture_mode : 0);
}

// ---- Transitions ------------------------------------------------------------
static bool s_suppress_tr = false;   // set while a drag-back commit pops the screen itself
static bool s_pending_commit = false;

static bool ensure_snap() {
    if (s_snap) return true;
    if (s_snap_tried) return false;
    s_snap_tried = true;
    s_snap = (uint16_t *)heap_caps_aligned_alloc(16, FB_PIXELS * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_snap) DEBUG_PRINTF("[ui] no PSRAM for transition snapshot - transitions off\n");
    return s_snap != nullptr;
}

static bool transitions_allowed(Screen *from, Screen *to) {
    if (!s_pipeline || s_suppress_tr) return false;
    // Snapshots are taken from the last frame sent to the panel; a rotated
    // frame or one with the top panel drawn over it can't be slid in
    // logical space.
    if (s_rot_angle != 0 || s_rot_applied != 0) return false;
    if (ui_top_panel_is_open() || s_confirm_active) return false;
    if (!from || !to || from == to) return false;
    if (from->no_transition || to->no_transition) return false;
    return true;
}

// Copies what's on the panel right now into s_snap.
static bool snapshot_last_frame() {
    const uint16_t *last = display_fb_last_submitted();
    if (!last || !ensure_snap()) return false;
    memcpy(s_snap, last, FB_PIXELS * 2);
    return true;
}

static void do_drag_commit() {
    Screen *scr = active_screen();
    int top_before = s_top;
    s_suppress_tr = true;
    if (scr && scr->on_gesture) scr->on_gesture(GESTURE_SWIPE_RIGHT);
    s_suppress_tr = false;
    if (s_top != top_before || active_screen() != scr) s_snap_below_of = nullptr;
}

// Jumps any running slide to its end state (a new press, or another
// push/pop, arrived before it finished).
static void finish_transition() {
    bool commit = (s_tr == TR_DRAG_SETTLE && s_tr_commit) || s_pending_commit;
    s_tr = TR_NONE;
    s_drag = DRAG_IDLE;
    s_pending_commit = false;
    if (commit) do_drag_commit();
}

static inline float ease_out_cubic(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// live = the frame just drawn (s_render), snap = s_snap.
// shift_right: live moved right by d px, snap's right edge fills the gap
//              on the left (push slide-in, and edge-drag back).
// otherwise:   live moved left by d px, snap's left edge fills the gap on
//              the right (pop: the old screen slides off to the right).
static void compose_shift(uint16_t *fb, const uint16_t *snap, int d, bool shift_right) {
    const int W = LCD_WIDTH;
    if (d <= 0) return;
    if (d > W) d = W;
    for (int y = 0; y < LCD_HEIGHT; y++) {
        uint16_t *row = fb + (size_t)y * W;
        const uint16_t *srow = snap + (size_t)y * W;
        if (shift_right) {
            if (d < W) memmove(row + d, row, (size_t)(W - d) * 2);
            memcpy(row, srow + (W - d), (size_t)d * 2);
        } else {
            if (d < W) memmove(row, row + d, (size_t)(W - d) * 2);
            memcpy(row + (W - d), srow, (size_t)d * 2);
        }
    }
}

static void compose_transition(uint32_t now) {
    if (s_tr == TR_NONE || !s_snap || !s_render) return;
    float d = 0.0f;
    bool shift_right = true;
    switch (s_tr) {
        case TR_PUSH:
        case TR_POP: {
            float t = (float)(now - s_tr_start_ms) / (float)TR_SLIDE_MS;
            if (t >= 1.0f) { s_tr = TR_NONE; return; }
            d = LCD_WIDTH * (1.0f - ease_out_cubic(t));
            shift_right = (s_tr == TR_PUSH);
            break;
        }
        case TR_DRAG:
            d = s_drag_off;
            break;
        case TR_DRAG_SETTLE: {
            float t = (float)(now - s_tr_start_ms) / (float)TR_SETTLE_MS;
            if (t >= 1.0f) {
                d = s_tr_to;
                s_tr = TR_NONE;
                if (s_tr_commit) s_pending_commit = true; // pops after this frame is out
            } else {
                d = s_tr_from + (s_tr_to - s_tr_from) * ease_out_cubic(t);
            }
            break;
        }
        default:
            return;
    }
    compose_shift(s_render, s_snap, (int)(d + 0.5f), shift_right);
}

static bool drag_back_eligible(Screen *scr) {
    return s_pipeline && scr && s_top >= 1 && s_tr == TR_NONE &&
           scr->gesture_mode == GESTURE_MODE_EDGE && scr->on_gesture && !scr->no_drag_back &&
           s_snap && s_snap_below_of == scr && s_rot_applied == 0 && !ui_top_panel_is_open();
}

// ---- Screen stack ---------------------------------------------------------
void ui_push(Screen *screen) {
    if (!screen) return;

    if (screen->needs_tilt_calibration && !g_app_settings.tilt_calibrated) {
        ui_show_confirm("Calibrate tilt sensor?",
                        "This needs the tilt sensor calibrated first.",
                        "Calibrate", "Cancel",
                        [](bool confirmed) {
                            if (confirmed) ui_push(&calibration_screen);
                            // Declined: do nothing further - the
                            // original screen/setting the user wanted
                            // simply doesn't activate. They can re-tap
                            // it after calibrating; not auto-resuming
                            // it here is deliberate, see the
                            // needs_tilt_calibration comment in ui.h.
                        });
        return;
    }

    if (s_tr != TR_NONE || s_drag != DRAG_IDLE) finish_transition();
    Screen *from = active_screen();
    bool animate = s_top < UI_MAX_STACK - 1 && transitions_allowed(from, screen) && snapshot_last_frame();

    if (s_top >= UI_MAX_STACK - 1) return;   // full: leave the top screen alive (callers retrying every loop must not destroy it)
    if (s_top >= 0 && s_stack[s_top].screen && s_stack[s_top].screen->on_destroy)
        s_stack[s_top].screen->on_destroy();
    if (s_top < UI_MAX_STACK - 1) {
        s_top++;
        s_stack[s_top].screen = screen;
        if (screen->on_create) screen->on_create();
        sync_gesture_mode();
        if (animate) {
            // s_snap now holds the outgoing screen: it slides out to the
            // left now, and later slides back in under an edge-drag.
            s_tr = TR_PUSH;
            s_tr_start_ms = millis();
            s_snap_below_of = screen;
        } else {
            s_snap_below_of = nullptr;
        }
    }
}

// Shared by pop and go-home: slide the current screen off to the right,
// revealing `to`.
static bool begin_pop_slide(Screen *to) {
    if (s_tr != TR_NONE || s_drag != DRAG_IDLE) finish_transition();
    if (s_top <= 0) return false;
    return transitions_allowed(active_screen(), to) && snapshot_last_frame();
}

void ui_pop_screen() {
    if (s_top <= 0) return;
    bool animate = begin_pop_slide(s_stack[s_top - 1].screen);
    if (s_top <= 0) return; // finishing a drag-back may already have popped
    if (s_stack[s_top].screen && s_stack[s_top].screen->on_destroy)
        s_stack[s_top].screen->on_destroy();
    s_stack[s_top].screen = nullptr;
    s_top--;
    sync_gesture_mode();
    s_snap_below_of = nullptr; // s_snap is the screen that just left, not the one below
    if (animate) {
        s_tr = TR_POP;
        s_tr_start_ms = millis();
    }
}

void ui_go_home() {
    bool animate = s_top > 0 && begin_pop_slide(s_stack[0].screen);
    while (s_top > 0) {
        if (s_stack[s_top].screen && s_stack[s_top].screen->on_destroy)
            s_stack[s_top].screen->on_destroy();
        s_stack[s_top].screen = nullptr;
        s_top--;
    }
    sync_gesture_mode();
    s_snap_below_of = nullptr;
    if (animate) {
        s_tr = TR_POP;
        s_tr_start_ms = millis();
    }
}

// ---- Touch ---------------------------------------------------------------
// Transform physical touch coords to logical coords based on the rotation
// angle that was LAST APPLIED to the framebuffer (s_rot_applied), not the
// target angle (s_rot_angle). This ensures touch always matches what's on screen.
static void transform_touch(int px, int py, int &lx, int &ly) {
    if (s_rot_applied == 0) { lx = px; ly = py; return; }
    int deg = s_rot_applied % 360;
    int16_t sv = S_SIN360[deg];
    int16_t cv = S_COS360[deg];
    int32_t cx2 = LCD_WIDTH;  // 2× center for 466px display
    int32_t ox2 = px * 2 + 1 - cx2;
    int32_t oy2 = py * 2 + 1 - cx2;
    int32_t raw_sx = (int32_t)cv * ox2 + (int32_t)sv * oy2;
    int32_t raw_sy = (int32_t)(-sv) * ox2 + (int32_t)cv * oy2;
    int32_t sx2 = (raw_sx >= 0 ? (raw_sx >> 10) : -(((-raw_sx) >> 10))) + cx2;
    int32_t sy2 = (raw_sy >= 0 ? (raw_sy >> 10) : -(((-raw_sy) >> 10))) + cx2;
    lx = (sx2 + 1) / 2;
    ly = (sy2 + 1) / 2;
    if (lx < 0) lx = 0; else if (lx >= LCD_WIDTH)  lx = LCD_WIDTH  - 1;
    if (ly < 0) ly = 0; else if (ly >= LCD_HEIGHT) ly = LCD_HEIGHT - 1;
}

// Routes one touch event to the active screen (the normal path).
static void dispatch_touch(int tx, int ty, bool pressed) {
    Screen *scr = active_screen();
    // If the previous dispatch (still part of this same physical
    // touch - the finger hasn't lifted) caused a screen change,
    // routing this event to the new screen would deliver a fresh
    // "pressed" at the same coordinates the old screen's tap just
    // fired at - if the new screen happens to have something tappable
    // in the same spot (very common, since many screens share similar
    // layouts), that reads as the same tap firing again on whatever
    // just appeared, without the finger ever lifting in between.
    //
    // Fix: remember which screen actually received this sequence's
    // press. Once the active screen no longer matches that, every
    // further event in the sequence - including its eventual release
    // - is swallowed rather than forwarded, since the new screen never
    // got the press its own release would otherwise correspond to. A
    // fresh press after an actual finger-lift is unaffected and
    // reaches whatever's active normally.
    if (pressed) {
        if (!s_touch_seq_active) {
            s_touch_seq_active = true;
            s_touch_seq_screen = scr;
        } else if (scr != s_touch_seq_screen) {
            return;
        }
    } else {
        if (!s_touch_seq_active) return;
        s_touch_seq_active = false;
        if (scr != s_touch_seq_screen) return;
    }
    if (scr && scr->on_touch) scr->on_touch(tx, ty, pressed);
}

// Edge-swipe back, following the finger (MUSE's tileview feel): the
// current screen moves right with the finger while a snapshot of the
// screen underneath slides in from the left. Release past a third of
// the width (or with a flick) completes it - the screen's own
// on_gesture(GESTURE_SWIPE_RIGHT) runs, exactly as for a classic edge
// swipe - otherwise it springs back.
static void handle_drag(int tx, int ty, bool pressed) {
    uint32_t now = millis();
    int dx = tx - s_drag_x0, dy = ty - s_drag_y0;

    if (s_drag == DRAG_PENDING) {
        if (pressed) {
            if (dx > DRAG_START_PX && dx > 2 * abs(dy)) {
                s_drag = DRAG_ACTIVE;
                s_tr = TR_DRAG;
                s_drag_off = (float)dx;
                s_drag_vel = 0.0f;
                s_drag_last_ms = now;
                return;
            }
            if (abs(dy) > DRAG_START_PX || dx < -DRAG_START_PX || now - s_drag_t0 > 220) {
                // Not a back-drag after all: deliver the held press, then this move.
                s_drag = DRAG_IDLE;
                dispatch_touch(s_drag_x0, s_drag_y0, true);
                dispatch_touch(tx, ty, true);
            }
            return;
        }
        // Lifted before it became a drag: an ordinary tap near the edge.
        s_drag = DRAG_IDLE;
        dispatch_touch(s_drag_x0, s_drag_y0, true);
        dispatch_touch(tx, ty, false);
        return;
    }

    // DRAG_ACTIVE
    if (pressed) {
        float off = dx > 0 ? (float)dx : 0.0f;
        uint32_t dt = now - s_drag_last_ms;
        if (dt > 0) {
            float v = (off - s_drag_off) / (float)dt;
            s_drag_vel = s_drag_vel * 0.6f + v * 0.4f;
            s_drag_last_ms = now;
        }
        s_drag_off = off;
        s_drag_last_x = tx;
        s_drag_last_y = ty;
        return;
    }

    // Released: the hal_touch swipe classifier saw this same motion -
    // drop its pending swipe so it doesn't pop a second screen.
    touch_cancel_swipes();
    bool commit = s_drag_off > LCD_WIDTH * 0.33f || s_drag_vel > 0.5f;
    s_drag = DRAG_IDLE;
    s_tr = TR_DRAG_SETTLE;
    s_tr_start_ms = now;
    s_tr_from = s_drag_off;
    s_tr_to = commit ? (float)LCD_WIDTH : 0.0f;
    s_tr_commit = commit;
}

void ui_handle_touch(int x, int y, bool pressed) {
    // Transform coordinates if canvas is rotated
    int tx_raw, ty_raw;
    transform_touch(x, y, tx_raw, ty_raw);

    if (pressed) {
        if (g_app_settings.touch_feedback) {
            s_tf_active = true;
            s_tf_x = x;       // physical x for feedback dot
            s_tf_y = y;       // physical y for feedback dot
            s_tf_until = millis() + 300;
        }
        s_last_touch_x = tx_raw;
        s_last_touch_y = ty_raw;
        if (g_app_settings.haptic_on_touch) vibrate_buzz();
    }
    int tx = pressed ? tx_raw : s_last_touch_x;
    int ty = pressed ? ty_raw : s_last_touch_y;

    // Confirmation modal takes priority over everything, including a toast.
    if (s_confirm_active) {
        ui_confirm_touch(tx, ty, pressed);
        return;
    }

    // Toast dismiss - checked next, ahead of the top panel and the
    // active screen, so a tap on a toast always dismisses it rather
    // than reaching whatever's underneath. Only check on the initial
    // press, not every move/release.
    if (pressed && ui_toast_touch(tx, ty)) {
        return;
    }

    // Top panel takes priority when open (uses physical coords, not rotated)
    if (ui_top_panel_is_open()) {
        ui_top_panel_touch(x, y, pressed);
        return;
    }

    Screen *scr = active_screen();

    // A new press while a slide is still animating jumps it to the end
    // first, so the tap lands on the screen that's actually arriving.
    if (pressed && !s_touch_seq_active && s_drag == DRAG_IDLE && s_tr != TR_NONE)
        finish_transition();

    if (s_drag != DRAG_IDLE) {
        handle_drag(tx, ty, pressed);
        return;
    }
    if (pressed && !s_touch_seq_active && tx < DRAG_EDGE_PX && drag_back_eligible(scr)) {
        // Might be the start of a back-drag: hold the press back from
        // the screen until we know (see handle_drag()).
        s_drag = DRAG_PENDING;
        s_drag_x0 = s_drag_last_x = tx;
        s_drag_y0 = s_drag_last_y = ty;
        s_drag_t0 = millis();
        return;
    }
    dispatch_touch(tx, ty, pressed);
}

// ---- Tick -----------------------------------------------------------------
void ui_tick() {
    Screen *scr = active_screen();
    if (scr && scr->on_tick) scr->on_tick();
}

// ---- Gesture polling ------------------------------------------------------
// Remap physical gesture direction to logical direction based on rotation angle
static Gesture remap_gesture(Gesture g) {
    if (s_rot_applied == 0 || g == GESTURE_NONE) return g;
    // Physical gesture angles in degrees: RIGHT=0, DOWN=90, LEFT=180, UP=270
    static const int phys_deg[5] = {0, 0, 180, 270, 90}; // NONE, RIGHT, LEFT, UP, DOWN
    int pd = phys_deg[g];
    int ld = (pd - s_rot_applied + 720) % 360;
    if (ld < 45 || ld >= 315) return GESTURE_SWIPE_RIGHT;
    if (ld >= 45  && ld < 135) return GESTURE_SWIPE_DOWN;
    if (ld >= 135 && ld < 225) return GESTURE_SWIPE_LEFT;
    return GESTURE_SWIPE_UP;
}

void ui_poll_gestures() {
    // Confirmation modal blocks gestures
    if (s_confirm_active) return;

    // Top panel: swipe-up closes it (not while a slider/tile is being
    // dragged - the panel cancels swipes itself then). Other swipes are
    // dropped so they can't reach the screen once the panel is gone.
    if (ui_top_panel_is_open()) {
        if (touch_swipe_up_detected() && !ui_top_panel_busy()) ui_top_panel_close();
        touch_cancel_swipes();
        return;
    }

    // Swipe-down from top zone on any non-free screen opens top panel
    Screen *scr = active_screen();
    if (scr && scr->gesture_mode != GESTURE_MODE_FREE) {
        if (touch_swipe_down_detected()) {
            // Root screens (watchface): swipe-down opens top panel
            // Non-root screens: also open top panel (consistent UX)
            if (s_top == 0) {
                // Root screen: swipe-down opens panel
                ui_top_panel_open();
                return;
            }
            // Non-root: swipe-down opens panel too
            ui_top_panel_open();
            return;
        }
    }

    // Route remaining gestures to active screen
    if (!scr || !scr->on_gesture) return;
    if (touch_swipe_right_detected()) scr->on_gesture(remap_gesture(GESTURE_SWIPE_RIGHT));
    if (touch_swipe_left_detected())  scr->on_gesture(remap_gesture(GESTURE_SWIPE_LEFT));
    if (touch_swipe_up_detected())    scr->on_gesture(remap_gesture(GESTURE_SWIPE_UP));
    // swipe_down already handled above
}

// ---- Main update ----------------------------------------------------------
#define IDLE_THROTTLE_AFTER_MS 2000 // no touch/button activity for this long -> idle_frame_ms kicks in

// Defined further below (near ui_draw_header) - forward-declared here
// since ui_update() calls it but is defined earlier in the file.
static void ui_draw_battery_indicator();

void ui_update() {
    uint32_t now = millis();

    // Per-screen frame rate - screens that opted into idle_frame_ms
    // (see ui.h) get paced at that slower rate once genuinely idle,
    // since the expensive part of a redraw is the full-framebuffer
    // flush to the display, not the drawing calls themselves. Any
    // touch or button activity resets this immediately back to the
    // normal rate (sleep_ms_since_activity() naturally drops to ~0),
    // so this never adds perceptible lag to actually interacting with
    // the screen - it only slows down redraws nobody's watching change.
    Screen *scr = active_screen();
    uint16_t frame_ms = scr ? scr->frame_ms : UI_FRAME_MS_DEFAULT;
    if (scr && scr->idle_frame_ms > 0 && sleep_ms_since_activity() >= IDLE_THROTTLE_AFTER_MS) {
        frame_ms = scr->idle_frame_ms;
    }
    // Slides and drags always run at 60 fps, whatever the screen asks
    // for - motion is where frame rate is actually visible.
    if (s_tr != TR_NONE || s_drag == DRAG_ACTIVE || ui_top_panel_animating() || ui_top_panel_busy()) {
        if (frame_ms == 0 || frame_ms > UI_FRAME_MS_SMOOTH) frame_ms = UI_FRAME_MS_SMOOTH;
    } else if (ui_top_panel_is_open()) {
        // The panel has its own clock/status to keep fresh, whatever the
        // screen underneath would idle down to.
        frame_ms = UI_FRAME_MS_DEFAULT;
    }
    if (frame_ms == 0) {
        // Uncapped — only throttle by hardware limits
        // Still add a 4ms minimum to prevent spinning
        if (now - s_last_flush_ms < 4) return;
    } else {
        if (now - s_last_flush_ms < frame_ms) return;
    }
    s_last_flush_ms = now;

    if (!scr || !s_gfx) return;

    if (s_pipeline && !s_render) {
        // The previous frame couldn't get a buffer - try again now.
        s_render = display_fb_acquire(false);
        if (!s_render) return;
        s_canvas->setFramebuffer(s_render);
    }

    // Per-frame game/screen logic. Screens that need real-time behaviour
    // (games) do their own internal millis()-based sub-stepping inside
    // on_tick — see game_dodger.cpp/game_plane.cpp for the pattern — so
    // on_tick must be driven by the render loop, not by the once-a-second
    // clock tick in the .ino. (That once-a-second tick is still used
    // separately for the watchface clock/battery/step readout.)
    if (scr->on_tick) scr->on_tick();
    scr = active_screen(); // on_tick may have pushed/popped
    if (!scr) return;

    // Update auto-rotate from IMU
    ui_update_auto_rotate();

    uint32_t t_draw0 = millis();

    // Clear to black
    s_gfx->fillScreen(COLOR_BG);

    // Draw screen - skipped while the top panel covers it completely.
    bool covered = ui_top_panel_covers_screen();
    if (!covered && scr->on_draw) scr->on_draw();

    // Header with back button (if screen has a title) - part of the
    // screen, so it slides with it.
    if (!covered && scr->title && scr->title[0] != '\0') {
        ui_draw_header(scr->title);
    }

    // Push/pop slide or edge-drag: shift this frame and fill the gap
    // from the snapshot of the other screen.
    compose_transition(now);

    // Always-visible battery readout, drawn on top of every screen's
    // own content (see ui_draw_battery_indicator() for why top-right).
    // After the slide composite, so it stays put while screens move.
    if (!scr->hide_status && !covered) ui_draw_battery_indicator();

    // Toast overlay
    ui_draw_toast();

    // Confirmation modal overlay
    ui_draw_confirm();

    // Apply software framebuffer rotation (after content, before overlays)
    // Must rotate every frame when non-zero — content is redrawn from scratch each frame.
    uint32_t t0 = millis();

    if (s_rot_angle != 0) {
        rotate_framebuffer(s_rot_angle);
    }
    s_rot_applied = s_rot_angle;

    uint32_t t1 = millis();

    // Top panel overlay (drawn in physical space, on top of rotated content)
    ui_top_panel_draw();

    // Touch feedback (drawn in physical space)
    ui_draw_touch_feedback();

    if (s_pipeline) {
        // Hand the frame to the panel sender (core 0) and immediately
        // carry on - the next frame gets drawn while this one is sent.
        // The canvas is re-pointed at a fresh buffer straight away, so
        // any drawing that happens between frames (e.g. from a touch
        // handler) can never land in a buffer that's being sent.
        display_fb_submit(s_render);
        s_render = display_fb_acquire(false);
        if (s_render) s_canvas->setFramebuffer(s_render);
    } else {
        s_canvas->flush();
        yield();
    }

    uint32_t t2 = millis();

    // A drag-back that has just finished sliding pops the screen now,
    // after its last frame (showing the screen underneath) went out.
    if (s_pending_commit) {
        s_pending_commit = false;
        do_drag_commit();
    }

    static uint32_t s_last_base_log = 0;
    if (t2 - s_last_base_log > 5000) {
        DEBUG_PRINTF("[perf] draw=%lums rot=%lums handoff=%lums angle=%d\n",
                     (unsigned long)(t0 - t_draw0), (unsigned long)(t1 - t0),
                     (unsigned long)(t2 - t1), s_rot_angle);
        s_last_base_log = t2;
    }
}

// ---- Drawing helpers ------------------------------------------------------
// Text goes through ui_font.h: smooth proportional fonts (with accented
// letters) sized to match the old built-in font's sizes, or the old font
// when Settings > Display > Smooth fonts is off.
static int text_width(const char *text, int size) {
    return ui_text_width(text, size);
}

void ui_draw_centered_text(int y, uint16_t color, const char *text, int size) {
    if (!s_gfx || !text) return;
    int w = text_width(text, size);
    ui_print((LCD_WIDTH - w) / 2, y, size, color, text);
}

// Same as ui_draw_centered_text but centered on an arbitrary x
// (rather than always the full screen width) - for text that needs
// to sit centered within one half of the screen, a card, a button,
// etc.
void ui_draw_centered_text_at(int x_center, int y, uint16_t color, const char *text, int size) {
    if (!s_gfx || !text) return;
    int w = text_width(text, size);
    ui_print(x_center - w / 2, y, size, color, text);
}

void ui_draw_list_row(int x, int y, int w, int h, const char *label,
                      const char *desc, bool selected) {
    if (!s_gfx) return;
    bool has_desc = desc && desc[0] != '\0';
    // Rounded card, not a full stadium pill - list rows read better
    // with a gentler curve than a circular-ended button/chip does,
    // same distinction Wear OS's own Card vs Chip/Button components
    // make.
    int radius = h > 40 ? 18 : h / 2;
    // Inset double-fill for the border rather than drawRoundRect -
    // see draw_switch()'s comment in app_settings.cpp for why
    // (drawFastHLine/drawFastVLine are documented elsewhere in this
    // project as broken on this display driver, and
    // Adafruit_GFX-derived drawRoundRect's outline typically
    // implements its straight edges through exactly those calls).
    // This helper is meant to be reusable, including in scrolling
    // list contexts that redraw every frame, so it sticks to the
    // confirmed-safe fill-only primitives from the start.
    uint16_t border_c = selected ? COLOR_ACCENT : COLOR_TEXT_DIM;
    uint16_t fill_c = selected ? COLOR_ACCENT : COLOR_PANEL;
    s_gfx->fillRoundRect(x, y, w, h, radius, border_c);
    if (w > 4 && h > 4) {
        int inner_r = radius > 2 ? radius - 2 : 0;
        s_gfx->fillRoundRect(x + 2, y + 2, w - 4, h - 4, inner_r, fill_c);
    }

    int pad = 16;
    ui_print(x + pad, y + (has_desc ? 10 : (h - 16) / 2), 2, COLOR_TEXT, label);

    if (has_desc) {
        ui_print(x + pad, y + h - 22, 1, selected ? COLOR_TEXT : COLOR_TEXT_DIM, desc);
    }
}

// ---- Round-screen helpers (see ui.h) ---------------------------------------
void ui_polar(float deg, float radius, int *x, int *y) {
    float a = deg * (float)M_PI / 180.0f;
    *x = (int)lroundf(S_CX + radius * sinf(a));
    *y = (int)lroundf(S_CY - radius * cosf(a));
}

float ui_angle_of(int x, int y) {
    float a = atan2f((float)(x - S_CX), (float)(S_CY - y)) * 180.0f / (float)M_PI;
    if (a < 0.0f) a += 360.0f;
    return a;
}

int ui_radius_of(int x, int y) {
    int dx = x - S_CX, dy = y - S_CY;
    return (int)sqrtf((float)(dx * dx + dy * dy));
}

bool ui_angle_in_arc(float deg, float start, float sweep) {
    if (sweep < 0.0f) { start += sweep; sweep = -sweep; }
    if (sweep >= 360.0f) return true;
    float d = fmodf(deg - start, 360.0f);
    if (d < 0.0f) d += 360.0f;
    return d <= sweep;
}

uint16_t ui_dim(uint16_t c, float f) {
    if (f >= 1.0f) return c;
    if (f <= 0.0f) return 0;
    int r = (int)(((c >> 11) & 0x1F) * f), g = (int)(((c >> 5) & 0x3F) * f), b = (int)((c & 0x1F) * f);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void ui_text_center(int cx, int cy, uint16_t color, const char *text, int size) {
    if (!s_gfx || !text) return;
    if (ui_fonts_smooth()) {
        ui_font_center(cx, cy, ui_font_for_size(size), color, text);
        return;
    }
    ui_print(cx - text_width(text, size) / 2, cy - 4 * size, size, color, text);
}

void ui_arc(int cx, int cy, int r_outer, int thickness, float start_deg, float sweep_deg,
            uint16_t color, bool round_caps) {
    if (!s_gfx || thickness <= 0 || sweep_deg == 0.0f) return;
    if (sweep_deg > 360.0f) sweep_deg = 360.0f;
    if (sweep_deg < -360.0f) sweep_deg = -360.0f;
    const float r_in = (float)(r_outer - thickness);
    const float ro = (float)r_outer;
    // ~3 degree segments: invisible facets even at the screen edge.
    int n = (int)ceilf(fabsf(sweep_deg) / 3.0f);
    if (n < 1) n = 1;
    const float k = (float)M_PI / 180.0f;
    float a = start_deg * k;
    float sa = sinf(a), ca = cosf(a);
    int ox0 = (int)lroundf(cx + ro * sa),   oy0 = (int)lroundf(cy - ro * ca);
    int ix0 = (int)lroundf(cx + r_in * sa), iy0 = (int)lroundf(cy - r_in * ca);
    for (int i = 1; i <= n; i++) {
        a = (start_deg + sweep_deg * (float)i / (float)n) * k;
        sa = sinf(a); ca = cosf(a);
        int ox1 = (int)lroundf(cx + ro * sa),   oy1 = (int)lroundf(cy - ro * ca);
        int ix1 = (int)lroundf(cx + r_in * sa), iy1 = (int)lroundf(cy - r_in * ca);
        s_gfx->fillTriangle(ox0, oy0, ox1, oy1, ix1, iy1, color);
        s_gfx->fillTriangle(ox0, oy0, ix1, iy1, ix0, iy0, color);
        ox0 = ox1; oy0 = oy1; ix0 = ix1; iy0 = iy1;
    }
    if (round_caps && fabsf(sweep_deg) < 360.0f && thickness >= 3) {
        float rm = ro - thickness / 2.0f;
        int cr = thickness / 2;
        int x, y;
        a = start_deg * k;
        x = (int)lroundf(cx + rm * sinf(a)); y = (int)lroundf(cy - rm * cosf(a));
        s_gfx->fillCircle(x, y, cr, color);
        a = (start_deg + sweep_deg) * k;
        x = (int)lroundf(cx + rm * sinf(a)); y = (int)lroundf(cy - rm * cosf(a));
        s_gfx->fillCircle(x, y, cr, color);
    }
}

void ui_edge_ring(float start_deg, float sweep_deg, uint16_t color, int thickness, int inset, bool round_caps) {
    ui_arc(S_CX, S_CY, S_R - inset, thickness, start_deg, sweep_deg, color, round_caps);
}

void ui_fill_circle_rect(int x, int y, int w, int h, int r, uint16_t color) {
    if (!s_gfx) return;
    // Simple approach: just fill the rounded rect
    // Circle clipping happens naturally since the display is round
    s_gfx->fillRoundRect(x, y, w, h, r, color);
}

TileRect ui_draw_header(const char *title) {
    static const int HEADER_Y = 46;
    TileRect back = {0, HEADER_Y - 10, 80, 40};

    s_gfx->setTextSize(3);
    s_gfx->setTextColor(COLOR_TEXT);
    ui_print(20, HEADER_Y, 3, COLOR_TEXT, "<");

    int tw = text_width(title, 2);
    ui_print((LCD_WIDTH - tw) / 2, HEADER_Y + 4, 2, COLOR_TEXT_DIM, title);

    return back;
}

// Small always-on-top battery readout, drawn on every screen regardless
// of what else is showing - top-right corner, clear of both the header
// (which only occupies the left/center) and any per-screen HUD content
// in the round display's outer band. Position verified against the
// actual circle geometry, not eyeballed - the top-right corner of a
// round display is one of the tightest-width areas on screen.
static void ui_draw_battery_indicator() {
    if (!s_gfx) return;
    int pct = power_get_battery_percent();
    if (pct < 0) return; // no battery reading available - stay silent rather than show 0%

    bool charging = power_is_charging();
    uint16_t color = charging ? COLOR_ACCENT2 : (pct > 20 ? COLOR_GOOD : COLOR_BAD);

    int icon_x = 348, icon_y = 64, icon_w = 20, icon_h = 12;
    s_gfx->drawRoundRect(icon_x, icon_y, icon_w, icon_h, 2, color);
    s_gfx->fillRect(icon_x + icon_w, icon_y + 3, 2, icon_h - 6, color); // nub
    int fill_w = (int)((icon_w - 4) * (pct / 100.0f));
    if (fill_w > 0) s_gfx->fillRect(icon_x + 2, icon_y + 2, fill_w, icon_h - 4, color);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    s_gfx->setTextSize(1);
    s_gfx->setTextColor(color);
    ui_print(icon_x + icon_w + 8, icon_y + 2, 1, color, buf);

    // WiFi indicator, to the left of the battery icon. Checked against
    // the round display's safe radius at this y (icon_y=64, worst-case
    // corner) - stays well inside it with icon_x=348 as the right edge.
    if (wifi_is_connected()) {
        int wx = icon_x - 34, wy_base = icon_y + icon_h;
        for (int i = 0; i < 3; i++) {
            int bh = 4 + i * 4;
            s_gfx->fillRect(wx + i * 8, wy_base - bh, 5, bh, COLOR_GOOD);
        }
    }
}

TileRect ui_draw_tile(int col, int row, uint16_t accent,
                       const char *icon_text, const char *label) {
    static const int TILE_SIZE = 150;
    static const int GAP = 18;
    static const int GRID_LEFT = (LCD_WIDTH - 2 * TILE_SIZE - GAP) / 2;
    static const int GRID_TOP = 100;

    int x = GRID_LEFT + col * (TILE_SIZE + GAP);
    int y = GRID_TOP + row * (TILE_SIZE + GAP);

    uint16_t dim_accent = COLOR565(
        (uint8_t)(((accent >> 11) & 0x1F) * 20 / 100 * 8),
        (uint8_t)(((accent >> 5) & 0x3F) * 20 / 100 * 4),
        (uint8_t)((accent & 0x1F) * 20 / 100 * 8));
    s_gfx->fillCircle(x + TILE_SIZE / 2, y + 30, 31, dim_accent);

    int iw = text_width(icon_text, 3);
    ui_print(x + (TILE_SIZE - iw) / 2, y + 16, 3, accent, icon_text);

    int lw = text_width(label, 2);
    if (lw > TILE_SIZE - 10) ui_print(x + 4, y + TILE_SIZE - 30, 2, COLOR_TEXT, label);
    else ui_print(x + (TILE_SIZE - lw) / 2, y + TILE_SIZE - 28, 2, COLOR_TEXT, label);

    return {x, y, TILE_SIZE, TILE_SIZE};
}

// ---- Toast ----------------------------------------------------------------
void ui_show_toast(const char *text, uint32_t duration_ms) {
    int next = (s_toast_head + 1) % TOAST_QUEUE_SIZE;
    if (next == s_toast_tail) return; // queue full, drop
    strncpy(s_toast_queue[s_toast_head].text, text,
            sizeof(s_toast_queue[0].text) - 1);
    s_toast_queue[s_toast_head].text[sizeof(s_toast_queue[0].text) - 1] = '\0';
    ui_utf8_trim(s_toast_queue[s_toast_head].text);
    s_toast_queue[s_toast_head].until_ms = millis() + duration_ms;
    s_toast_head = next;
}

bool ui_toast_active() {
    return s_toast_tail != s_toast_head;
}

void ui_draw_toast() {
    if (!s_gfx || s_toast_tail == s_toast_head) return;
    uint32_t now = millis();

    // Expire old toasts
    while (s_toast_tail != s_toast_head &&
           now > s_toast_queue[s_toast_tail].until_ms) {
        s_toast_tail = (s_toast_tail + 1) % TOAST_QUEUE_SIZE;
    }
    if (s_toast_tail == s_toast_head) return;

    // Draw the current toast at the top, centered: one line, or two
    // (word-wrapped, the second shortened with "..") for longer text.
    ToastEntry &t = s_toast_queue[s_toast_tail];
    const int size = 2, max_w = 320;
    char l1[96], l2[96];
    l1[0] = l2[0] = 0;
    {
        const char *p = t.text;
        int n = 0, last_space = -1;
        while (p[n] && n < (int)sizeof(l1) - 1) {
            l1[n] = p[n]; l1[n + 1] = 0;
            if (p[n] == ' ') last_space = n;
            if (text_width(l1, size) > max_w) {
                n = last_space > 0 ? last_space : n;
                break;
            }
            n++;
        }
        l1[n] = 0;
        p += n;
        while (*p == ' ') p++;
        strncpy(l2, p, sizeof(l2) - 1);
        l2[sizeof(l2) - 1] = 0;
        ui_fit(l2, size, max_w);
    }
    int tw = text_width(l1, size), tw2 = l2[0] ? text_width(l2, size) : 0;
    if (tw2 > tw) tw = tw2;
    int pw = tw + 36;
    int ph = l2[0] ? 64 : 38;
    int px = (LCD_WIDTH - pw) / 2;
    int py = 58;
    s_toast_rect_x = px; s_toast_rect_y = py; s_toast_rect_w = pw; s_toast_rect_h = ph;

    s_gfx->fillRoundRect(px, py, pw, ph, 19, COLOR_PANEL);
    s_gfx->drawRoundRect(px, py, pw, ph, 19, COLOR_TEXT_DIM);
    ui_print(px + 18, py + 11, size, COLOR_TEXT, l1);
    if (l2[0]) ui_print(px + 18, py + 37, size, ui_dim(COLOR_TEXT, 0.75f), l2);

    // Auto-advance to next toast
    if (now > t.until_ms) {
        s_toast_tail = (s_toast_tail + 1) % TOAST_QUEUE_SIZE;
    }
}

bool ui_toast_touch(int x, int y) {
    if (s_toast_tail == s_toast_head) return false; // none active
    if (x < s_toast_rect_x || x > s_toast_rect_x + s_toast_rect_w ||
        y < s_toast_rect_y || y > s_toast_rect_y + s_toast_rect_h) {
        return false;
    }
    // Dismiss the currently-shown toast (tail is the one being drawn).
    s_toast_tail = (s_toast_tail + 1) % TOAST_QUEUE_SIZE;
    return true;
}

// ---- Confirmation modal ---------------------------------------------------
void ui_show_confirm(const char *title, const char *body,
                     const char *yes_label, const char *no_label,
                     ConfirmCallback cb) {
    strncpy(s_confirm_title, title, sizeof(s_confirm_title) - 1);
    s_confirm_title[sizeof(s_confirm_title) - 1] = '\0';
    strncpy(s_confirm_body, body, sizeof(s_confirm_body) - 1);
    s_confirm_body[sizeof(s_confirm_body) - 1] = '\0';
    strncpy(s_confirm_yes, yes_label, sizeof(s_confirm_yes) - 1);
    s_confirm_yes[sizeof(s_confirm_yes) - 1] = '\0';
    strncpy(s_confirm_no, no_label, sizeof(s_confirm_no) - 1);
    s_confirm_no[sizeof(s_confirm_no) - 1] = '\0';
    s_confirm_cb = cb;
    s_confirm_active = true;
}

bool ui_confirm_active() { return s_confirm_active; }

void ui_confirm_touch(int x, int y, bool pressed) {
    if (!s_confirm_active || !pressed) return;

    int modal_w = 300;
    int modal_h = 200;
    int mx = (LCD_WIDTH - modal_w) / 2;
    int my = (LCD_HEIGHT - modal_h) / 2;

    // Yes button
    int btn_w = 120;
    int btn_h = 40;
    int btn_y = my + modal_h - 60;
    int yes_x = mx + 20;
    int no_x = mx + modal_w - btn_w - 20;

    if (y >= btn_y && y <= btn_y + btn_h) {
        if (x >= yes_x && x <= yes_x + btn_w) {
            s_confirm_active = false;
            if (s_confirm_cb) s_confirm_cb(true);
            return;
        }
        if (x >= no_x && x <= no_x + btn_w) {
            s_confirm_active = false;
            if (s_confirm_cb) s_confirm_cb(false);
            return;
        }
    }
}

void ui_draw_confirm() {
    if (!s_confirm_active || !s_gfx) return;

    int modal_w = 300;
    int modal_h = 200;
    int mx = (LCD_WIDTH - modal_w) / 2;
    int my = (LCD_HEIGHT - modal_h) / 2;

    // Dimmed background
    s_gfx->fillScreen(0x0000);

    // Modal panel - more rounded corners than before (16->24), softer
    // Wear OS Material 3 Expressive look.
    s_gfx->fillRoundRect(mx, my, modal_w, modal_h, 24, COLOR_PANEL);
    s_gfx->drawRoundRect(mx, my, modal_w, modal_h, 24, COLOR_TEXT_DIM);

    // Title
    int tw = text_width(s_confirm_title, 2);
    ui_print(mx + (modal_w - tw) / 2, my + 20, 2, COLOR_WARN, s_confirm_title);

    // Body - word-wrapped over up to 3 lines inside the panel
    {
        const int max_w = modal_w - 40;
        const char *p = s_confirm_body;
        int line_y = my + 56;
        for (int line = 0; line < 3 && *p; line++) {
            char buf[96];
            int n = 0, last_space = -1;
            while (p[n] && n < (int)sizeof(buf) - 1) {
                buf[n] = p[n];
                buf[n + 1] = 0;
                if (p[n] == ' ') last_space = n;
                if (text_width(buf, 2) > max_w) {
                    if (last_space > 0) n = last_space;
                    break;
                }
                n++;
            }
            buf[n] = 0;
            ui_print(mx + (modal_w - text_width(buf, 2)) / 2, line_y, 2, COLOR_TEXT, buf);
            p += n;
            while (*p == ' ') p++;
            line_y += 24;
        }
    }

    // Buttons - stadium-shaped (Wear OS Chip/Button style: corner
    // radius = half the height, a full pill), matching the shape
    // used across the rest of the UI now rather than the previous
    // small fixed corner radius.
    int btn_w = 120;
    int btn_h = 40;
    int btn_y = my + modal_h - 60;

    // Yes
    s_gfx->fillRoundRect(mx + 20, btn_y, btn_w, btn_h, btn_h / 2, COLOR_GOOD);
    int yw = text_width(s_confirm_yes, 2);
    ui_print(mx + 20 + (btn_w - yw) / 2, btn_y + 12, 2, COLOR_TEXT, s_confirm_yes);

    // No
    s_gfx->fillRoundRect(mx + modal_w - btn_w - 20, btn_y, btn_w, btn_h, btn_h / 2, COLOR_PANEL);
    s_gfx->drawRoundRect(mx + modal_w - btn_w - 20, btn_y, btn_w, btn_h, btn_h / 2, COLOR_TEXT_DIM);
    int nw = text_width(s_confirm_no, 2);
    ui_print(mx + modal_w - btn_w - 20 + (btn_w - nw) / 2, btn_y + 12, 2, COLOR_TEXT, s_confirm_no);
}

// ---- Top panel overlay ----------------------------------------------------
// Lives in ui_panel.cpp.

// ---- Touch feedback -------------------------------------------------------
void ui_draw_touch_feedback() {
    if (!s_tf_active || !s_gfx || millis() > s_tf_until) {
        s_tf_active = false;
        return;
    }
    s_gfx->drawCircle(s_tf_x, s_tf_y, 12, COLOR565(57, 214, 255));
    s_gfx->drawCircle(s_tf_x, s_tf_y, 11, COLOR565(57, 214, 255));
}
