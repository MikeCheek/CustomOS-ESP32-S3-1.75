/*
 * phone_images.cpp
 * App icons and album art from the phone - see phone_images.h.
 *
 * The BLE task only copies frames into a staging buffer. A finished
 * image moves to a "ready" slot, and the main loop decodes it (JPEGDEC,
 * media_decode.cpp) into the cache. One image arrives at a time: the app
 * sends them in order, one per request.
 */
#include "phone_images.h"
#include "config.h"
#include "hal_ble.h"
#include "media_decode.h"
#include "fx3d.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

#define IMG_MAX_BYTES   (24 * 1024)   // JPEG; the app sends ~1-8 KB
#define ICON_SLOTS      40
#define ART_SLOTS       2
#define RETRY_MS        20000         // ask again if nothing came

struct Slot {
    uint32_t hash = 0;
    uint32_t used = 0;     // LRU stamp
    DecodedImage img;
};

static Slot s_icons[ICON_SLOTS];
static Slot s_art[ART_SLOTS];
static uint32_t s_stamp = 1;

// Recently requested hashes, so a screen redrawing 30 times a second
// asks once.
struct Req { uint32_t hash; uint32_t ms; };
static Req s_reqs[8];
static int s_req_next = 0;

// BLE task -> main loop
static uint8_t *s_stage = nullptr, *s_ready = nullptr;
static volatile uint32_t s_stage_hash = 0, s_stage_total = 0, s_stage_len = 0;
static volatile char s_stage_kind = 0;
static volatile bool s_ready_full = false;
static volatile uint32_t s_ready_hash = 0, s_ready_len = 0;
static volatile char s_ready_kind = 0;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void phone_images_on_frame(const uint8_t *d, int len) {
    if (len < 14) return;
    char kind = (char)d[1];
    uint32_t hash = rd32(d + 2), total = rd32(d + 6), off = rd32(d + 10);
    const uint8_t *data = d + 14;
    int n = len - 14;
    if ((kind != 'i' && kind != 'a') || !hash || total == 0 || total > IMG_MAX_BYTES) return;
    if (!s_stage) s_stage = (uint8_t *)heap_caps_malloc(IMG_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ready) s_ready = (uint8_t *)heap_caps_malloc(IMG_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stage || !s_ready) return;
    if (off == 0) {
        s_stage_hash = hash;
        s_stage_kind = kind;
        s_stage_total = total;
        s_stage_len = 0;
    }
    // Frames arrive in order on one connection; anything else is a
    // half-sent image being replaced - drop it.
    if (hash != s_stage_hash || off != s_stage_len || off + n > s_stage_total) return;
    memcpy(s_stage + off, data, n);
    s_stage_len = off + n;
    if (s_stage_len < s_stage_total) return;
    if (s_ready_full) { s_stage_hash = 0; return; }   // main loop busy: dropped, asked again later
    uint8_t *t = s_ready;
    s_ready = s_stage;
    s_stage = t;
    s_ready_hash = s_stage_hash;
    s_ready_kind = s_stage_kind;
    s_ready_len = s_stage_len;
    s_stage_hash = 0;
    __sync_synchronize();
    s_ready_full = true;
}

static Slot *find(Slot *slots, int n, uint32_t hash) {
    for (int i = 0; i < n; i++)
        if (slots[i].hash == hash && slots[i].img.px) return &slots[i];
    return nullptr;
}

static Slot *victim(Slot *slots, int n) {
    Slot *v = &slots[0];
    for (int i = 0; i < n; i++) {
        if (!slots[i].img.px) return &slots[i];
        if (slots[i].used < v->used) v = &slots[i];
    }
    return v;
}

void phone_images_update() {
    if (!s_ready_full) return;
    bool icon = s_ready_kind == 'i';
    int max = icon ? PHONE_ICON_SIZE : PHONE_ART_SIZE;
    DecodedImage img;
    if (img_decode_jpeg_mem(s_ready, s_ready_len, max, max, true, &img)) {
        Slot *slots = icon ? s_icons : s_art;
        int n = icon ? ICON_SLOTS : ART_SLOTS;
        Slot *s = find(slots, n, s_ready_hash);
        if (!s) s = victim(slots, n);
        img_free(&s->img);
        s->img = img;
        s->hash = s_ready_hash;
        s->used = s_stamp++;
        DEBUG_PRINTF("[img] %s %08lx %dx%d (%lu bytes)\n", icon ? "icon" : "art", (unsigned long)s->hash,
                     img.w, img.h, (unsigned long)s_ready_len);
    } else {
        DEBUG_PRINTF("[img] couldn't decode %08lx (%lu bytes)\n", (unsigned long)s_ready_hash,
                     (unsigned long)s_ready_len);
    }
    __sync_synchronize();
    s_ready_full = false;
}

static void request(char kind, uint32_t hash) {
    if (!ble_is_connected()) return;
    uint32_t now = millis();
    for (auto &r : s_reqs)
        if (r.hash == hash && now - r.ms < RETRY_MS) return;
    s_reqs[s_req_next] = { hash, now };
    s_req_next = (s_req_next + 1) % 8;
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"e\":\"img\",\"k\":\"%c\",\"h\":%lu}", kind, (unsigned long)hash);
    ble_link_send(msg);
}

const uint16_t *phone_icon(uint32_t hash) {
    if (!hash) return nullptr;
    Slot *s = find(s_icons, ICON_SLOTS, hash);
    if (s) { s->used = s_stamp++; return s->img.px; }
    request('i', hash);
    return nullptr;
}

const uint16_t *phone_art(uint32_t hash, int *w, int *h) {
    if (!hash) return nullptr;
    Slot *s = find(s_art, ART_SLOTS, hash);
    if (s) {
        s->used = s_stamp++;
        if (w) *w = s->img.w;
        if (h) *h = s->img.h;
        return s->img.px;
    }
    request('a', hash);
    return nullptr;
}

void phone_image_draw_round(Arduino_GFX *g, const uint16_t *px, int w, int h, int cx, int cy, int r,
                            uint8_t dim) {
    if (!g || !px || w <= 0 || h <= 0 || r <= 0) return;
    static uint16_t row[2 * PHONE_ART_SIZE + 64];
    int d = 2 * r;
    if (d > (int)(sizeof(row) / sizeof(row[0]))) return;
    // Cover: the shorter side spans the diameter.
    int side = w < h ? w : h;
    int ox = (w - side) / 2, oy = (h - side) / 2;
    for (int dy = -r; dy < r; dy++) {
        float fy = dy + 0.5f;
        int half = (int)sqrtf((float)(r * r) - fy * fy);
        if (half <= 0) continue;
        int sy = oy + (dy + r) * side / d;
        const uint16_t *src = px + (size_t)sy * w + ox;
        int n = 2 * half;
        for (int i = 0; i < n; i++) {
            uint16_t c = src[(r - half + i) * side / d];
            row[i] = dim >= 32 ? c : fx_scale565(c, dim);
        }
        g->draw16bitRGBBitmap(cx - half, cy + dy, row, n, 1);
    }
}
