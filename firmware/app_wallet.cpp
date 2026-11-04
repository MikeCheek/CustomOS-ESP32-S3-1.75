// Wallet - see app_wallet.h.
#include "app_wallet.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "phone_link.h"
#include "hal_display.h"
#include "hal_sleep.h"
#include "app_settings_state.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stddef.h>
#include "store.h"
#if FEATURE_SD_CARD
#include "hal_sd.h"
#include <SD.h>
#endif

static const int MAX_BYTES = 4096;   // a QR code of version 40 is 177 x 177 = 3917 bytes

struct Pass {
    char name[32];
    char sub[48];
    char kind[12];
    uint16_t w, h;
    uint8_t ratio10;    // a module's height, tenths of its width
    uint16_t nbytes;
    uint32_t crc;
    uint8_t *bits;      // PSRAM, row by row, MSB first; nullptr = empty slot
};
static Pass s_pass[WALLET_MAX];

// ---- storage ------------------------------------------------------------------------------
// In the store (store.h: the SD card, else the watch's flash) as
// "wallet/<slot>": [magic]["Pass" up to bits][the modules]. Passes from
// before (the SD card's /wallet/<slot>.wal) are read once and moved.
static const uint32_t MAGIC = 0x314C4157;   // "WAL1"
static const size_t HEAD = offsetof(Pass, bits);

static void free_slot(int i) {
    if (s_pass[i].bits) heap_caps_free(s_pass[i].bits);
    memset(&s_pass[i], 0, sizeof(Pass));
}

static void save_slot(int i) {
    char name[16];
    snprintf(name, sizeof(name), "wallet/%d", i);
    const Pass &x = s_pass[i];
    if (!x.bits) { store_remove(name); return; }
    size_t n = 4 + HEAD + x.nbytes;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    memcpy(buf, &MAGIC, 4);
    memcpy(buf + 4, &x, HEAD);
    memcpy(buf + 4 + HEAD, x.bits, x.nbytes);
    store_write(name, buf, n);
    heap_caps_free(buf);
}

// a file's bytes -> slot i (false: not a pass)
static bool load_from(int i, const uint8_t *buf, int n) {
    uint32_t m;
    Pass x;
    if (n < (int)(4 + HEAD)) return false;
    memcpy(&m, buf, 4);
    memcpy(&x, buf + 4, HEAD);
    if (m != MAGIC || !x.nbytes || x.nbytes > MAX_BYTES || !x.w || !x.h || (uint32_t)x.w * x.h > (uint32_t)x.nbytes * 8 ||
        n < (int)(4 + HEAD + x.nbytes))
        return false;
    x.bits = (uint8_t *)heap_caps_malloc(x.nbytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!x.bits) return false;
    memcpy(x.bits, buf + 4 + HEAD, x.nbytes);
    x.name[sizeof(x.name) - 1] = x.sub[sizeof(x.sub) - 1] = x.kind[sizeof(x.kind) - 1] = 0;
    s_pass[i] = x;
    return true;
}

void wallet_init() {
    const int MAXF = 4 + HEAD + MAX_BYTES;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(MAXF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    for (int i = 0; i < WALLET_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "wallet/%d", i);
        int n = store_read(name, buf, MAXF);
        if (n > 0 && load_from(i, buf, n)) continue;
#if FEATURE_SD_CARD
        // the first Wallet kept them in /wallet on the card: move them
        if (!sd_is_mounted()) continue;
        char p[32];
        snprintf(p, sizeof(p), "/wallet/%d.wal", i);
        File f = SD.open(p, FILE_READ);
        if (!f) continue;
        n = (int)f.read(buf, MAXF);
        f.close();
        if (n > 0 && load_from(i, buf, n)) {
            save_slot(i);
            SD.remove(p);
        }
#endif
    }
    heap_caps_free(buf);
}

void wallet_send() {
    JsonDocument d;
    d["e"] = "wal";
    JsonArray l = d["l"].to<JsonArray>();
    for (int i = 0; i < WALLET_MAX; i++) {
        JsonArray e = l.add<JsonArray>();
        if (s_pass[i].bits) { e.add(s_pass[i].name); e.add(s_pass[i].crc); }
    }
    phone_link_send(d);
}

// ---- from the phone ---------------------------------------------------------------------
static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}
static int b64_decode(const char *in, uint8_t *out, int max) {
    int n = 0, acc = 0, bits = 0;
    for (; *in; in++) {
        int v = b64v(*in);
        if (v < 0) continue;   // '=' and anything else
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

static Pass s_rx;            // the one arriving
static int s_rx_slot = -1;

void wallet_on_link(JsonDocument &doc) {
    const char *op = doc["op"] | "";
    if (!strcmp(op, "ask")) { wallet_send(); return; }
    int i = doc["i"] | -1;
    if (i < 0 || i >= WALLET_MAX) return;
    if (!strcmp(op, "del")) {
        free_slot(i);
        save_slot(i);   // removes the file
        wallet_send();
        return;
    }
    if (strcmp(op, "put")) return;
    int tot = doc["tot"] | 0, off = doc["o"] | 0;
    if (tot <= 0 || tot > MAX_BYTES || off < 0 || off >= tot) return;
    if (off == 0) {   // a new one starts
        if (s_rx.bits) heap_caps_free(s_rx.bits);
        memset(&s_rx, 0, sizeof(s_rx));
        strlcpy(s_rx.name, doc["n"] | "Pass", sizeof(s_rx.name));
        strlcpy(s_rx.sub, doc["s"] | "", sizeof(s_rx.sub));
        strlcpy(s_rx.kind, doc["k"] | "qr", sizeof(s_rx.kind));
        s_rx.w = doc["w"] | 0;
        s_rx.h = doc["h"] | 0;
        s_rx.ratio10 = doc["r"] | 10;
        s_rx.crc = doc["crc"] | 0;
        s_rx.nbytes = tot;
        if (!s_rx.w || !s_rx.h || (uint32_t)s_rx.w * s_rx.h > (uint32_t)tot * 8) return;
        s_rx.bits = (uint8_t *)heap_caps_malloc(tot, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_rx_slot = i;
    }
    if (!s_rx.bits || s_rx_slot != i || s_rx.nbytes != tot) return;
    int n = b64_decode(doc["b"] | "", s_rx.bits + off, tot - off);
    if (n <= 0) return;
    if (off + n < tot) return;   // more to come
    free_slot(i);
    s_pass[i] = s_rx;            // takes the buffer
    s_rx.bits = nullptr;
    s_rx_slot = -1;
    save_slot(i);
    wallet_send();
}

// ---- the screens --------------------------------------------------------------------------
static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

static int s_show = -1;              // the pass shown full screen, -1 = the list
static uint8_t s_prev_bright = 0;
static UiScroll s_scroll;
static const int ROW_TOP = 96, ROW_H = 78, ROW_GAP = 10;

static int count() {
    int n = 0;
    for (auto &p : s_pass) n += p.bits != nullptr;
    return n;
}
static int nth(int k) {   // the k-th filled slot
    for (int i = 0; i < WALLET_MAX; i++)
        if (s_pass[i].bits && k-- == 0) return i;
    return -1;
}

static bool module(const Pass &p, int x, int y) {
    uint32_t i = (uint32_t)y * p.w + x;
    return (p.bits[i >> 3] >> (7 - (i & 7))) & 1;
}

static void show(int i) {
    s_show = i;
    if (i >= 0) {
        s_prev_bright = g_app_settings.brightness;
        display_set_brightness(255);   // scanners like a bright screen
    } else if (s_prev_bright) {
        display_set_brightness(s_prev_bright);
        s_prev_bright = 0;
    }
}

static void draw_code(Arduino_GFX *g, const Pass &p) {
    g->fillScreen(COLOR_BG);
    bool linear = p.h == 1;
    // the biggest whole module that fits: a square inside the circle for 2D
    // codes, a wide band across the middle for barcodes and PDF417
    float rh = p.ratio10 / 10.0f;
    int box_w, box_h;
    if (linear) { box_w = 400; box_h = 150; }
    else if (p.w > p.h * rh * 1.6f) { box_w = 404; box_h = 210; }   // wide (PDF417)
    else { box_w = box_h = 300; }
    int m = box_w / p.w;
    if (!linear) {
        int mh = (int)(box_h / (p.h * rh));
        if (mh < m) m = mh;
    }
    if (m < 1) m = 1;
    int mhpx = linear ? box_h : (int)(m * rh + 0.5f);
    if (mhpx < 1) mhpx = 1;
    int cw = p.w * m, ch = linear ? box_h : p.h * mhpx;
    int quiet = m * (linear ? 6 : 2);   // a white margin round it
    int x0 = CX - cw / 2, y0 = CY - ch / 2 + 12;
    g->fillRoundRect(x0 - quiet, y0 - quiet, cw + quiet * 2, ch + quiet * 2, 18, 0xFFFF);
    for (int y = 0; y < (linear ? 1 : p.h); y++) {
        int yy = y0 + y * mhpx;
        int x = 0;
        while (x < p.w) {   // runs of dark modules as one rectangle
            if (!module(p, x, y)) { x++; continue; }
            int s = x;
            while (x < p.w && module(p, x, y)) x++;
            g->fillRect(x0 + s * m, yy, (x - s) * m, linear ? box_h : mhpx, 0x0000);
        }
    }
    char t[40];
    strlcpy(t, p.name, sizeof(t));
    ui_text_center(CX, y0 - quiet - 26, COLOR_TEXT, t, 2);
    if (p.sub[0]) ui_text_center(CX, y0 + ch + quiet + 26, COLOR_TEXT_DIM, p.sub, 1);
}

static int s_open_next = -1;
void wallet_open_next(int slot) { s_open_next = slot; }

static void wallet_create() {
    s_show = -1;
    s_prev_bright = 0;
    int n = count();
    ui_scroll_reset(&s_scroll, n * (ROW_H + ROW_GAP) - (LCD_HEIGHT - ROW_TOP - 110), ROW_H + ROW_GAP);
    if (s_open_next >= 0 && s_open_next < WALLET_MAX && s_pass[s_open_next].bits) show(s_open_next);
    else if (n == 1) show(nth(0));   // just one: straight to it
    s_open_next = -1;
}

static void wallet_destroy() { show(-1); }

static void wallet_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (s_show >= 0 && s_pass[s_show].bits) { draw_code(g, s_pass[s_show]); return; }
    ui_scroll_tick(&s_scroll);
    g->fillScreen(COLOR_BG);
    ui_text_center(CX, 62, COLOR_TEXT, "Wallet", 2);
    int n = count();
    if (!n) {
        ui_text_center(CX, CY - 20, COLOR_TEXT, "No passes yet", 2);
        ui_text_center(CX, CY + 16, COLOR_TEXT_DIM, "Add them in the app: Wallet", 1);
        ui_text_center(CX, CY + 40, COLOR_TEXT_DIM, "(from a photo of the code)", 1);
        return;
    }
    int off = ui_scroll_offset(&s_scroll);
    for (int k = 0; k < n; k++) {
        int y = ROW_TOP + k * (ROW_H + ROW_GAP) - off;
        if (y > LCD_HEIGHT || y + ROW_H < 0) continue;
        const Pass &p = s_pass[nth(k)];
        char d[64];
        snprintf(d, sizeof(d), "%s%s%s", p.sub, p.sub[0] ? " - " : "", p.kind);
        ui_draw_list_row(CX - 170, y, 340, ROW_H, p.name, d, false);
    }
}

static void wallet_touch(int x, int y, bool pressed) {
    (void)x;
    if (s_show >= 0) {
        // a tap anywhere goes back to the list (or out, with just one)
        static bool down = false;
        if (pressed) { down = true; return; }
        if (!down) return;
        down = false;
        if (count() <= 1) { show(-1); ui_pop_screen(); }
        else show(-1);
        return;
    }
    if (!ui_scroll_touch(&s_scroll, y, pressed)) return;
    int k = (y + ui_scroll_offset(&s_scroll) - ROW_TOP) / (ROW_H + ROW_GAP);
    if (k >= 0 && k < count() && (y + ui_scroll_offset(&s_scroll) - ROW_TOP) % (ROW_H + ROW_GAP) < ROW_H) {
        sleep_register_activity();
        show(nth(k));
    }
}

static void wallet_gesture(Gesture) {}

Screen wallet_screen = {
    nullptr, GESTURE_MODE_EDGE, UI_FRAME_MS_SMOOTH,   // no header: the code gets the whole screen
    wallet_create, wallet_draw, wallet_touch, nullptr, wallet_destroy, wallet_gesture,
    500,
    true,    // suppress_idle: a code being scanned mustn't time out
};
