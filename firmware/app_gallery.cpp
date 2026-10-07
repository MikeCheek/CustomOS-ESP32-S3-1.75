#include "app_gallery.h"
#include "app_video.h"
#include "media_decode.h"
#include "media_library.h"
#include "config.h"
#include "board_pins.h"
#include "hal_sd.h"
#include "hal_touch.h"
#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

#define PS_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define PS_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

// =============================================================================
//  Media lists (media_library.h)
// =============================================================================

static MediaList s_all;          // Gallery: the whole card
static MediaList s_folder;       // opened from the file browser
static MediaList *s_list = &s_all;
static MediaScanner s_scanner;

static const int GALLERY_MAX_ITEMS = 3000;

static int gallery_filter(const char *name) {
    if (media_is_image(name)) return 0;
    if (media_is_video(name)) return 1;
    return -1;
}

static bool s_scanned = false;
static uint32_t s_scan_done_ms = 0;

static void scan_begin() {
    s_scanned = false;
    s_scanner.begin(&s_all, gallery_filter, GALLERY_MAX_ITEMS);
}

static void scan_step(uint32_t budget_ms) {
    if (s_scanner.step(budget_ms) && !s_scanned) {
        s_all.reverse();          // FAT order is roughly oldest-first; show newest first
        s_scanned = true;
        s_scan_done_ms = millis();
    }
}

// =============================================================================
//  Thumbnail cache
// =============================================================================

static const int THUMB = 120, TR = THUMB / 2;
static const int TCACHE = 60;
struct Thumb { int item = -1; uint16_t *px = nullptr; int w = 0, h = 0; uint32_t used = 0; };
static Thumb s_thumbs[TCACHE];
static uint8_t *s_tstate = nullptr;   // per item: 0 unknown, 1 loaded, 2 failed
static int s_tstate_n = 0;

static void thumbs_reset() {
    for (auto &t : s_thumbs) { free(t.px); t = Thumb(); }
    free(s_tstate); s_tstate = nullptr; s_tstate_n = 0;
}

static Thumb *thumb_find(int item) {
    for (auto &t : s_thumbs) if (t.item == item) { t.used = millis(); return &t; }
    return nullptr;
}

static void thumb_store(int item, DecodedImage &img) {
    Thumb *slot = nullptr;
    for (auto &t : s_thumbs) if (t.item < 0) { slot = &t; break; }
    if (!slot) {
        slot = &s_thumbs[0];
        for (auto &t : s_thumbs) if (t.used < slot->used) slot = &t;
        if (slot->item >= 0 && slot->item < s_tstate_n) s_tstate[slot->item] = 0;
        free(slot->px);
    }
    slot->item = item; slot->px = img.px; slot->w = img.w; slot->h = img.h; slot->used = millis();
    img.px = nullptr;
}

// =============================================================================
//  Small glyphs
// =============================================================================

static void glyph_image(Arduino_GFX *g, int cx, int cy, uint16_t c) {
    g->fillTriangle(cx - 18, cy + 12, cx - 4, cy - 8, cx + 10, cy + 12, c);
    g->fillTriangle(cx - 2, cy + 12, cx + 8, cy - 2, cx + 18, cy + 12, c);
    g->fillCircle(cx + 10, cy - 12, 5, c);
}
static void glyph_play(Arduino_GFX *g, int cx, int cy, int r, uint16_t c) {
    g->fillTriangle(cx - r / 2, cy - r * 3 / 5, cx - r / 2, cy + r * 3 / 5, cx + r * 3 / 5, cy, c);
}
static void spinner(int cx, int cy, int r, uint16_t c) {
    float a = (millis() % 1000) * 0.36f;
    ui_arc(cx, cy, r, 5, a, 90, c);
}

// Thumbnail clipped to a circle.
static void draw_round_thumb(Arduino_GFX *g, const Thumb *t, int cx, int cy) {
    int ox = (t->w - THUMB) / 2, oy = (t->h - THUMB) / 2;   // centre smaller thumbs
    for (int dy = -TR; dy < TR; dy++) {
        int half = (int)sqrtf((float)(TR * TR - (dy + 0.5f) * (dy + 0.5f)));
        int sy = dy + TR + oy;
        if (sy < 0 || sy >= t->h) continue;
        int x0 = TR - half + ox, x1 = TR + half + ox;
        if (x0 < 0) x0 = 0;
        if (x1 > t->w) x1 = t->w;
        if (x1 <= x0) continue;
        g->draw16bitRGBBitmap(cx - TR + (x0 - ox), cy + dy, t->px + (size_t)sy * t->w + x0, x1 - x0, 1);
    }
}

// =============================================================================
//  Grid screen
// =============================================================================

static const int COLS = 3, PITCH_X = 128, PITCH_Y = 130, GRID_TOP = 150;
static UiScroll s_scroll;
static bool s_back_armed = false;
static int  s_press_item = -1;
static bool s_prev_pressed = false;

static void viewer_open(MediaList *list, int idx);

static int grid_rows() { return (s_all.n + COLS - 1) / COLS; }
static int grid_content_h() { return GRID_TOP + (grid_rows() - 1) * PITCH_Y + TR + 120; }
static void item_center(int i, int off, int *x, int *y) {
    *x = CX + (i % COLS - 1) * PITCH_X;
    *y = GRID_TOP + (i / COLS) * PITCH_Y + off;
}

static void gallery_create() {
    s_back_armed = false;
    s_press_item = -1;
    s_prev_pressed = false;
    // Rescan when the card may have changed (first visit, or a while ago).
    if (!s_scanned || millis() - s_scan_done_ms > 60000 || s_all.n == 0) {
        thumbs_reset();
        scan_begin();
    }
    ui_scroll_reset(&s_scroll, 0, PITCH_Y);
}

static void gallery_destroy() {}

static void gallery_tick() {
    ui_scroll_tick(&s_scroll);
    if (s_scanner.scanning() || !s_scanned) { scan_step(12); return; }
    if (!s_scanned) return;
    ui_scroll_set_max(&s_scroll, grid_content_h() - LCD_HEIGHT);
    if (s_tstate_n != s_all.n) {
        free(s_tstate);
        s_tstate = (uint8_t *)calloc(s_all.n ? s_all.n : 1, 1);
        s_tstate_n = s_all.n;
    }
    if (!s_tstate || ui_scroll_is_moving(&s_scroll)) return;   // decode once the list settles
    // One thumbnail per frame: first visible (and one row ahead) without one.
    int off = -ui_scroll_offset(&s_scroll);
    int first = ((-off + 0) - GRID_TOP - TR) / PITCH_Y;
    if (first < 0) first = 0;
    int last = (LCD_HEIGHT - off - GRID_TOP + TR) / PITCH_Y + 1;
    for (int i = first * COLS; i < (last + 1) * COLS && i < s_all.n; i++) {
        if (s_tstate[i] == 2 || thumb_find(i)) continue;
        DecodedImage img;
        bool ok = s_all.tag[i] ? video_poster(s_all.path(i), THUMB, THUMB, true, &img)
                                 : img_decode_file(s_all.path(i), THUMB, THUMB, true, &img);
        if (ok) { thumb_store(i, img); s_tstate[i] = 1; }
        else s_tstate[i] = 2;
        break;
    }
}

static void gallery_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (!sd_is_mounted()) {
        ui_text_center(CX, CY - 10, COLOR_TEXT, "No SD card", 2);
        ui_text_center(CX, CY + 18, COLOR_TEXT_DIM, "Insert a card with photos or videos", 1);
        return;
    }
    if (s_scanner.scanning() || !s_scanned) {
        spinner(CX, CY - 20, 26, COLOR_ACCENT);
        char b[32]; snprintf(b, sizeof(b), "Scanning... %d", s_all.n);
        ui_text_center(CX, CY + 30, COLOR_TEXT_DIM, b, 2);
        return;
    }
    if (s_all.n == 0) {
        glyph_image(g, CX, CY - 40, COLOR_TEXT_DIM);
        ui_text_center(CX, CY + 4, COLOR_TEXT, "No photos or videos", 2);
        ui_text_center(CX, CY + 32, COLOR_TEXT_DIM, ".jpg .png .bmp images and", 1);
        ui_text_center(CX, CY + 46, COLOR_TEXT_DIM, "MJPEG .avi videos, anywhere on the card", 1);
        return;
    }
    int off = -ui_scroll_offset(&s_scroll);
    char b[24];
    snprintf(b, sizeof(b), "%d item%s", s_all.n, s_all.n == 1 ? "" : "s");
    ui_text_center(CX, 72 + off, COLOR_TEXT_DIM, b, 1);
    for (int i = 0; i < s_all.n; i++) {
        int x, y;
        item_center(i, off, &x, &y);
        if (y + TR < 0) continue;
        if (y - TR > LCD_HEIGHT) break;
        bool pressed = i == s_press_item;
        if (pressed) g->fillCircle(x, y, TR + 5, COLOR_ACCENT);
        Thumb *t = thumb_find(i);
        if (t) draw_round_thumb(g, t, x, y);
        else {
            g->fillCircle(x, y, TR, COLOR_PANEL);
            if (s_tstate && i < s_tstate_n && s_tstate[i] == 2) ui_text_center(x, y, COLOR_TEXT_DIM, "?", 3);
            else if (s_all.tag[i]) glyph_play(g, x, y, 22, COLOR_TEXT_DIM);
            else glyph_image(g, x, y, COLOR_TEXT_DIM);
        }
        if (s_all.tag[i]) {     // video badge
            int bx = x + 38, by = y + 38;
            g->fillCircle(bx, by, 15, COLOR_BG);
            g->fillCircle(bx, by, 12, COLOR_ACCENT);
            glyph_play(g, bx + 1, by, 12, COLOR_TEXT);
        }
    }
}

static void gallery_touch(int x, int y, bool pressed) {
    if (pressed && !s_scroll.dragging) s_back_armed = (x < 54 && y < 54);
    if (s_back_armed) { if (!pressed) { s_back_armed = false; ui_pop_screen(); } return; }
    if (!s_scanned || s_all.n == 0) return;
    bool edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    int off = -ui_scroll_offset(&s_scroll);
    if (edge) {
        s_press_item = -1;
        for (int i = 0; i < s_all.n; i++) {
            int cx, cy;
            item_center(i, off, &cx, &cy);
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= (TR + 4) * (TR + 4)) { s_press_item = i; break; }
        }
    }
    bool tap = ui_scroll_touch(&s_scroll, y, pressed);
    if (s_scroll.moved) s_press_item = -1;
    if (!pressed) {
        int it = s_press_item;
        s_press_item = -1;
        if (tap && it >= 0) viewer_open(&s_all, it);
    }
}

static void gallery_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT || g == GESTURE_SWIPE_LEFT) ui_pop_screen();
}

Screen gallery_screen = {
    "Gallery", GESTURE_MODE_EDGE,
    UI_FRAME_MS_SMOOTH,
    gallery_create, gallery_draw, gallery_touch, gallery_tick, gallery_destroy, gallery_gesture,
    0,
};

// =============================================================================
//  Viewer (pager)
// =============================================================================

static int  s_idx = 0;
static DecodedImage s_img;
static bool s_img_hi = false;          // decoded at 2x for zooming
static bool s_need_load = false, s_need_hi = false;
static int  s_load_frames = 0;
static char s_verr[48] = "";
static float s_zoom = 1.0f;
static float s_panx = 0, s_pany = 0;   // screen px offset of the image centre
static int  s_slide = 0;               // horizontal drag offset at zoom 1
static bool s_v_prev = false, s_dragging = false, s_moved = false;
static int  s_tx0, s_ty0, s_tlx, s_tly;
static uint32_t s_t_down = 0, s_last_tap = 0, s_pending_tap = 0;
static int  s_tap_x, s_tap_y;
static bool s_overlay = false;
static uint32_t s_overlay_until = 0;
static bool s_slideshow = false;
static uint32_t s_slide_next = 0;
static bool s_btn_play = false;
static const uint32_t SLIDE_MS = 4000;

static void viewer_load(int idx) {
    img_free(&s_img);
    s_img_hi = false;
    s_idx = idx;
    s_zoom = 1; s_panx = s_pany = 0; s_slide = 0;
    s_need_load = true; s_need_hi = false;
    s_load_frames = 0;
    s_verr[0] = 0;
}

static void viewer_open(MediaList *list, int idx) {
    s_list = list;
    s_idx = idx;
    ui_push(&gallery_viewer_screen);
}

static void viewer_create() {
    s_v_prev = s_dragging = false;
    s_overlay = true; s_overlay_until = millis() + 2500;
    s_slideshow = false;
    gallery_viewer_screen.suppress_idle = false;
    viewer_load(s_idx);
}

static void viewer_destroy() {
    img_free(&s_img);
    s_slideshow = false;
    gallery_viewer_screen.suppress_idle = false;
}

static bool cur_is_video() { return s_list->n > 0 && s_list->tag[s_idx]; }

// Display size of the image at zoom 1 (fit inside the 466 square; small
// images are enlarged up to 3x).
static void fit_size(float *w, float *h) {
    float iw = (float)(s_img_hi ? s_img.w / 2.0f : s_img.w), ih = (float)(s_img_hi ? s_img.h / 2.0f : s_img.h);
    if (iw < 1 || ih < 1) { *w = *h = 0; return; }
    float s = fminf(LCD_WIDTH / iw, LCD_HEIGHT / ih);
    if (s > 3) s = 3;
    *w = iw * s; *h = ih * s;
}

static void clamp_pan() {
    float fw, fh; fit_size(&fw, &fh);
    float dw = fw * s_zoom, dh = fh * s_zoom;
    float mx = dw > LCD_WIDTH ? (dw - LCD_WIDTH) / 2 : 0, my = dh > LCD_HEIGHT ? (dh - LCD_HEIGHT) / 2 : 0;
    if (s_panx > mx) s_panx = mx;
    if (s_panx < -mx) s_panx = -mx;
    if (s_pany > my) s_pany = my;
    if (s_pany < -my) s_pany = -my;
}

static void go(int dir) {
    if (s_list->n == 0) return;
    int n = s_idx + dir;
    if (n < 0 || n >= s_list->n) {
        if (!s_slideshow) return;
        n = (n + s_list->n) % s_list->n;     // slideshow loops
    }
    viewer_load(n);
}

static void viewer_tick() {
    uint32_t now = millis();
    // Actual decoding happens a couple of frames after a load request so
    // the spinner/slide is on screen while it runs.
    if (s_need_load && ++s_load_frames >= 2) {
        s_need_load = false;
        const char *p = s_list->path(s_idx);
        bool ok = cur_is_video() ? video_poster(p, LCD_WIDTH, LCD_HEIGHT, false, &s_img)
                                 : img_decode_file(p, LCD_WIDTH, LCD_HEIGHT, false, &s_img, s_verr, sizeof(s_verr));
        if (!ok && !s_verr[0]) strcpy(s_verr, cur_is_video() ? "Can't read video" : "Can't decode image");
        s_slide_next = now + SLIDE_MS;
    }
    if (s_need_hi && !s_need_load) {
        s_need_hi = false;
        DecodedImage hi;
        if (img_decode_file(s_list->path(s_idx), LCD_WIDTH * 2, LCD_HEIGHT * 2, false, &hi) && hi.w > s_img.w) {
            img_free(&s_img);
            s_img = hi;
            s_img_hi = true;
        } else img_free(&hi);
    }
    // single tap (no second tap followed): toggle the overlay
    if (s_pending_tap && now - s_pending_tap > 300) {
        s_pending_tap = 0;
        s_overlay = !s_overlay;
        s_overlay_until = now + 4000;
    }
    if (s_overlay && !s_slideshow && now > s_overlay_until && !cur_is_video()) s_overlay = false;
    if (s_slideshow && !s_need_load && !s_dragging && now >= s_slide_next) go(+1);
}

static void draw_image(Arduino_GFX *g) {
    if (!s_img.px) return;
    float fw, fh; fit_size(&fw, &fh);
    float dw = fw * s_zoom, dh = fh * s_zoom;
    float x0 = CX - dw / 2 + s_panx + s_slide, y0 = CY - dh / 2 + s_pany;
    int sx0 = (int)fmaxf(0, x0), sx1 = (int)fminf(LCD_WIDTH, x0 + dw);
    int sy0 = (int)fmaxf(0, y0), sy1 = (int)fminf(LCD_HEIGHT, y0 + dh);
    if (sx1 <= sx0 || sy1 <= sy0) return;
    static uint16_t row[LCD_WIDTH];
    static int16_t xmap[LCD_WIDTH];
    float kx = s_img.w / dw, ky = s_img.h / dh;
    for (int x = sx0; x < sx1; x++) {
        int sx = (int)((x - x0) * kx); if (sx >= s_img.w) sx = s_img.w - 1; if (sx < 0) sx = 0;
        xmap[x - sx0] = (int16_t)sx;
    }
    int w = sx1 - sx0;
    for (int y = sy0; y < sy1; y++) {
        int sy = (int)((y - y0) * ky); if (sy >= s_img.h) sy = s_img.h - 1; if (sy < 0) sy = 0;
        const uint16_t *src = s_img.px + (size_t)sy * s_img.w;
        for (int i = 0; i < w; i++) row[i] = src[xmap[i]];
        g->draw16bitRGBBitmap(sx0, y, row, w, 1);
    }
}

static void viewer_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_list->n == 0) { ui_text_center(CX, CY, COLOR_TEXT_DIM, "Nothing to show", 2); return; }
    if (s_need_load) {
        spinner(CX + s_slide, CY, 24, COLOR_ACCENT);
    } else if (!s_img.px) {
        ui_text_center(CX, CY - 12, COLOR_BAD, s_verr, 2);
        ui_text_center(CX, CY + 16, COLOR_TEXT_DIM, s_list->name(s_idx), 1);
    } else {
        draw_image(g);
    }
    bool video = cur_is_video();
    if (video && !s_need_load) {      // big play button over the poster
        g->fillCircle(CX + s_slide, CY, 50, s_btn_play ? COLOR_ACCENT : COLOR_BG);
        g->fillCircle(CX + s_slide, CY, 46, s_btn_play ? COLOR_ACCENT : COLOR_PANEL);
        glyph_play(g, CX + 4 + s_slide, CY, 40, COLOR_TEXT);
    }
    if (!s_overlay && !video) return;

    // position in the list: dim ring + bright segment
    int n = s_list->n;
    float seg = n > 0 ? fmaxf(360.0f / n, 8) : 360;
    float a = n > 1 ? (360.0f - seg) * s_idx / (n - 1) : 0;
    ui_edge_ring(0, 360, ui_dim(COLOR_TEXT, 0.18f), 5, 3, false);
    ui_edge_ring(a, seg, COLOR_ACCENT, 5, 3);

    char name[21];                    // ~250 px at size 2: fits the top of the circle
    const char *nm = s_list->name(s_idx);
    strncpy(name, nm, sizeof(name) - 1); name[sizeof(name) - 1] = 0;
    if (strlen(nm) > sizeof(name) - 1) strcpy(name + sizeof(name) - 4, "...");
    g->fillRoundRect(CX - 135, 28, 270, 50, 25, COLOR_BG);
    ui_text_center(CX, 44, COLOR_TEXT, name, 2);
    char info[48];
    if (s_img.px && !video) snprintf(info, sizeof(info), "%d / %d   %dx%d%s", s_idx + 1, n, s_img.src_w, s_img.src_h, s_zoom > 1.01f ? "  zoom" : "");
    else snprintf(info, sizeof(info), "%d / %d%s", s_idx + 1, n, video ? "   video" : "");
    ui_text_center(CX, 66, COLOR_TEXT_DIM, info, 1);

    if (!video) {
        // slideshow button
        int by = LCD_HEIGHT - 70;
        g->fillCircle(CX, by, 30, s_slideshow ? COLOR_ACCENT : COLOR_PANEL);
        if (s_slideshow) { g->fillRect(CX - 10, by - 12, 7, 24, COLOR_TEXT); g->fillRect(CX + 3, by - 12, 7, 24, COLOR_TEXT); }
        else glyph_play(g, CX + 3, by, 26, COLOR_TEXT);
        ui_text_center(CX, by + 44, COLOR_TEXT_DIM, s_slideshow ? "Slideshow on" : "Slideshow", 1);
    }
    // prev / next hints
    if (s_idx > 0) g->fillTriangle(22, CY, 36, CY - 14, 36, CY + 14, COLOR_TEXT_DIM);
    if (s_idx < n - 1) g->fillTriangle(LCD_WIDTH - 22, CY, LCD_WIDTH - 36, CY - 14, LCD_WIDTH - 36, CY + 14, COLOR_TEXT_DIM);
}

static void zoom_at(int tx, int ty, float z) {
    float fw, fh; fit_size(&fw, &fh);
    if (fw <= 0) return;
    // keep the point under the finger where it is
    float px = (tx - CX - s_panx) / (fw * s_zoom), py = (ty - CY - s_pany) / (fh * s_zoom);
    s_zoom = z;
    s_panx = tx - CX - px * fw * s_zoom;
    s_pany = ty - CY - py * fh * s_zoom;
    clamp_pan();
}

static void viewer_touch(int x, int y, bool pressed) {
    bool edge = pressed && !s_v_prev;
    s_v_prev = pressed;
    uint32_t now = millis();
    bool video = cur_is_video();
    if (edge) {
        s_dragging = true; s_moved = false;
        s_tx0 = s_tlx = x; s_ty0 = s_tly = y;
        s_t_down = now;
        s_btn_play = video && (x - CX) * (x - CX) + (y - CY) * (y - CY) <= 56 * 56;
        return;
    }
    if (pressed && s_dragging) {
        int dx = x - s_tx0, dy = y - s_ty0;
        if (abs(dx) > 12 || abs(dy) > 12) { s_moved = true; s_btn_play = false; }
        if (s_moved) {
            if (s_zoom > 1.01f) { s_panx += x - s_tlx; s_pany += y - s_tly; clamp_pan(); }
            else if (abs(dx) > abs(dy)) {
                bool at_end = (dx > 0 && s_idx == 0) || (dx < 0 && s_idx == s_list->n - 1);
                s_slide = at_end ? dx / 3 : dx;        // rubber band at the ends
            }
            touch_cancel_swipes();
        }
        s_tlx = x; s_tly = y;
        return;
    }
    if (pressed) return;
    // release
    s_dragging = false;
    int dx = s_tlx - s_tx0, dy = s_tly - s_ty0;
    if (s_btn_play) {
        s_btn_play = false;
        s_slideshow = false; gallery_viewer_screen.suppress_idle = false;
        video_open(s_list->path(s_idx));
        return;
    }
    if (s_moved) {
        touch_cancel_swipes();
        if (s_zoom <= 1.01f) {
            if (dx < -80 && abs(dx) > abs(dy) && s_idx < s_list->n - 1) { go(+1); return; }
            if (dx > 80 && abs(dx) > abs(dy) && s_idx > 0) { go(-1); return; }
            if (dy < -110 && abs(dy) > abs(dx) * 2) { ui_pop_screen(); return; }   // swipe up: back
        }
        s_slide = 0;
        return;
    }
    // a tap
    if (s_overlay && !video) {
        int by = LCD_HEIGHT - 70;
        if ((x - CX) * (x - CX) + (y - by) * (y - by) <= 36 * 36) {
            s_slideshow = !s_slideshow;
            gallery_viewer_screen.suppress_idle = s_slideshow;
            s_slide_next = now + SLIDE_MS;
            s_overlay_until = now + 2000;
            return;
        }
    }
    if (x < 50 && s_overlay) { go(-1); return; }
    if (x > LCD_WIDTH - 50 && s_overlay) { go(+1); return; }
    if (!video && now - s_last_tap < 300) {        // double tap: zoom
        s_pending_tap = 0;
        s_last_tap = 0;
        if (s_zoom > 1.01f) { s_zoom = 1; s_panx = s_pany = 0; }
        else {
            zoom_at(x, y, 2.5f);
            if (!s_img_hi && s_img.src_w > s_img.w * 1.3f) s_need_hi = true;   // sharper source for zoom
        }
        return;
    }
    s_last_tap = now;
    s_pending_tap = now;
    s_tap_x = x; s_tap_y = y;
}

static void viewer_gesture(Gesture g) {
    // Gestures are handled by the drag logic above (GESTURE_MODE_NONE);
    // swipe down still opens the top panel.
    (void)g;
}

Screen gallery_viewer_screen = {
    "", GESTURE_MODE_NONE,
    UI_FRAME_MS_SMOOTH,
    viewer_create, viewer_draw, viewer_touch, viewer_tick, viewer_destroy, viewer_gesture,
    250,
    false,   // suppress_idle: switched on while a slideshow runs
    false, false,
    true,    // no_drag_back: horizontal drags change the picture
    true,    // hide_status
};

// =============================================================================

void gallery_open_file(const char *path) {
    s_folder.clear();
    char dir[160];
    strncpy(dir, path, sizeof(dir) - 1); dir[sizeof(dir) - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    int sel = 0;
    File d = SD.open(dir[0] ? dir : "/");
    if (d) {
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            if (!e.isDirectory()) {
                const char *nm = e.name();
                const char *b = strrchr(nm, '/'); b = b ? b + 1 : nm;
                char full[160];
                snprintf(full, sizeof(full), "%s/%s", dir, b);
                if (media_is_image(b) || media_is_video(b)) {
                    if (!strcmp(full, path)) sel = s_folder.n;
                    s_folder.add(full, media_is_video(b));
                }
            }
            e.close();
            if (s_folder.n >= GALLERY_MAX_ITEMS) break;
        }
        d.close();
    }
    if (s_folder.n == 0) s_folder.add(path, media_is_video(path));
    viewer_open(&s_folder, sel);
}
