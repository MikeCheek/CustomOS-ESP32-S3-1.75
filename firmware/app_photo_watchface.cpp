// Photo watch face with depth - see app_photo_watchface.h for the file.
#include "app_photo_watchface.h"
#include "config.h"
#include "board_pins.h"
#include "ui.h"
#include "app_face_nav.h"
#include "ui_font.h"
#include "apps.h"
#include "app_menu.h"
#include "hal_rtc.h"
#include "hal_sd.h"
#include "watchface_registry.h"
#include "clock_fmt.h"
#include "app_settings_state.h"
#include "hal_nvs.h"
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <string.h>
#if FEATURE_SD_CARD
#include <SD.h>
#endif

static const char *DIR_PATH = "/faces/photos";
static const char *LEGACY_PATH = "/faces/photo.pwf";   // the one photo before there could be several
static const int HEADER = 40;       // PWF1
static const int HEADER2 = 44;      // PWF2: + u16 time_x, u16 options
static bool is_pwf(const uint8_t *b) { return !memcmp(b, "PWF1", 4) || !memcmp(b, "PWF2", 4); }
static const int GLYPHS = 11;   // "0123456789:"

static uint8_t *s_file = nullptr;
static size_t s_file_len = 0;
static bool s_tried = false;    // looked for the file since the last reload

// ---- the photos on the card ------------------------------------------------------------
static char s_names[PHOTO_FACE_MAX][PHOTO_FACE_NAME_MAX + 1];
static int s_count = 0;
static bool s_scanned = false;
static uint32_t s_switched_ms = 0;   // when the photo last changed: page dots for a moment

bool photo_face_name_ok(const char *n) {
    int l = n ? strlen(n) : 0;
    if (l < 1 || l > PHOTO_FACE_NAME_MAX) return false;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
            return false;
    return true;
}

static void path_for(char *buf, int n, const char *name) { snprintf(buf, n, "%s/%s.pwf", DIR_PATH, name); }

static void scan() {
    s_scanned = true;
    s_count = 0;
#if FEATURE_SD_CARD
    if (!sd_is_mounted()) return;
    if (SD.exists(LEGACY_PATH)) {          // the single photo of firmware 3.5 betas: into the folder
        SD.mkdir("/faces");
        SD.mkdir(DIR_PATH);
        SD.rename(LEGACY_PATH, "/faces/photos/photo.pwf");
    }
    File d = SD.open(DIR_PATH);
    if (!d || !d.isDirectory()) return;
    for (File f = d.openNextFile(); f && s_count < PHOTO_FACE_MAX; f = d.openNextFile()) {
        bool dir = f.isDirectory();
        String nm = f.name();
        f.close();
        int slash = nm.lastIndexOf('/');
        if (slash >= 0) nm = nm.substring(slash + 1);
        if (dir || !nm.endsWith(".pwf")) continue;
        nm = nm.substring(0, nm.length() - 4);
        if (!photo_face_name_ok(nm.c_str())) continue;
        strlcpy(s_names[s_count++], nm.c_str(), PHOTO_FACE_NAME_MAX + 1);
    }
    d.close();
    for (int i = 1; i < s_count; i++)       // sorted: the app names them by date
        for (int j = i; j > 0 && strcmp(s_names[j - 1], s_names[j]) > 0; j--) {
            char tmp[PHOTO_FACE_NAME_MAX + 1];
            strcpy(tmp, s_names[j]);
            strcpy(s_names[j], s_names[j - 1]);
            strcpy(s_names[j - 1], tmp);
        }
#endif
}

static int cur_index() {
    if (!s_scanned) scan();
    if (!s_count) return -1;
    for (int i = 0; i < s_count; i++)
        if (!strcmp(s_names[i], g_app_settings.photo_face)) return i;
    return 0;
}

struct PhotoFace {
    int w, h, time_x, time_y, glyph_h;
    bool show_date;
    uint16_t time_color, date_color;
    bool depth;
    int glyph_w[GLYPHS];
    const uint8_t *glyph[GLYPHS];
    const uint16_t *photo;
    uint32_t spans;
    const uint8_t *span_tab;    // span_count x 6 bytes
    const uint8_t *alpha;
};
static PhotoFace s_face;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void unload() {
    if (s_file) heap_caps_free(s_file);
    s_file = nullptr;
    s_file_len = 0;
}

// Checks every size against the file: a bad or cut-off one is ignored.
static bool parse() {
    const uint8_t *b = s_file;
    size_t n = s_file_len;
    if (n < (size_t)HEADER || !is_pwf(b)) return false;
    bool v2 = b[3] == '2';
    if (v2 && n < (size_t)HEADER2) return false;
    PhotoFace &f = s_face;
    f.w = rd16(b + 4);
    f.h = rd16(b + 6);
    f.time_y = rd16(b + 8);
    f.time_color = rd16(b + 10);
    f.date_color = rd16(b + 12);
    f.depth = rd16(b + 14) & 1;
    f.glyph_h = rd16(b + 16);
    if (f.w != LCD_WIDTH || f.h != LCD_HEIGHT || f.glyph_h <= 0 || f.glyph_h > LCD_HEIGHT) return false;
    // PWF2: the time anywhere across, the date optional
    f.time_x = v2 ? rd16(b + 40) : f.w / 2;
    f.show_date = v2 ? !(rd16(b + 42) & 1) : true;
    if (f.time_x >= f.w || f.time_y >= f.h) return false;
    size_t off = v2 ? HEADER2 : HEADER;
    size_t photo_bytes = (size_t)f.w * f.h * 2;
    if (off + photo_bytes > n) return false;
    f.photo = (const uint16_t *)(b + off);   // offset 40: 2-byte aligned
    off += photo_bytes;
    for (int i = 0; i < GLYPHS; i++) {
        f.glyph_w[i] = rd16(b + 18 + i * 2);
        if (f.glyph_w[i] > LCD_WIDTH) return false;
        f.glyph[i] = b + off;
        off += (size_t)f.glyph_w[i] * f.glyph_h;
        if (off > n) return false;
    }
    if (off + 4 > n) return false;
    f.spans = rd32(b + off);
    off += 4;
    if (f.spans > (uint32_t)f.w * f.h || off + (size_t)f.spans * 6 > n) return false;
    f.span_tab = b + off;
    off += (size_t)f.spans * 6;
    f.alpha = b + off;
    size_t alpha_total = 0;
    for (uint32_t i = 0; i < f.spans; i++) {
        const uint8_t *s = f.span_tab + i * 6;
        int y = rd16(s), x = rd16(s + 2), len = rd16(s + 4);
        if (y >= f.h || x + len > f.w) return false;
        alpha_total += len;
    }
    if (off + alpha_total > n) return false;
    if (!f.spans) f.depth = false;
    return true;
}

static bool load() {
    s_tried = true;
    unload();
#if FEATURE_SD_CARD
    if (!sd_is_mounted()) return false;
    int idx = cur_index();
    if (idx < 0) return false;
    char path[64];
    path_for(path, sizeof(path), s_names[idx]);
    File file = SD.open(path, FILE_READ);
    if (!file) return false;
    size_t n = file.size();
    if (n < (size_t)HEADER || n > 4 * 1024 * 1024) { file.close(); return false; }
    s_file = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_file) { file.close(); return false; }
    size_t got = 0;
    while (got < n) {
        size_t k = file.read(s_file + got, n - got);
        if (k == 0) break;
        got += k;
    }
    file.close();
    s_file_len = got;
    if (got != n || !parse()) {
        DEBUG_PRINTF("[photo_wf] %s isn't a photo face\n", path);
        unload();
        return false;
    }
    DEBUG_PRINTF("[photo_wf] loaded: %u bytes, %u subject runs\n", (unsigned)n, (unsigned)s_face.spans);
    return true;
#else
    return false;
#endif
}

void photo_face_reload() {
    unload();
    s_tried = false;
    s_scanned = false;
}

int photo_face_count() {
    if (!s_scanned) scan();
    return s_count;
}

const char *photo_face_name(int i) { return i >= 0 && i < photo_face_count() ? s_names[i] : ""; }

const char *photo_face_current() {
    int i = cur_index();
    return i >= 0 ? s_names[i] : "";
}

bool photo_face_show(const char *name) {
    if (!s_scanned) scan();
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_names[i], name)) continue;
        if (strcmp(g_app_settings.photo_face, name)) {
            strlcpy(g_app_settings.photo_face, name, sizeof(g_app_settings.photo_face));
            nvs_save_settings(g_app_settings);
        }
        unload();
        s_tried = false;
        s_switched_ms = millis();
        return true;
    }
    return false;
}

void photo_face_next() {
    int i = cur_index();
    if (i < 0 || s_count < 2) return;
    photo_face_show(s_names[(i + 1) % s_count]);
}

bool photo_face_delete(const char *name) {
#if FEATURE_SD_CARD
    if (!photo_face_name_ok(name) || !sd_is_mounted()) return false;
    char path[64];
    path_for(path, sizeof(path), name);
    bool ok = SD.remove(path);
    photo_face_reload();
    return ok;
#else
    return false;
#endif
}

bool photo_face_save(const char *name, const uint8_t *data, int len) {
#if FEATURE_SD_CARD
    if (!photo_face_name_ok(name) || !sd_is_mounted() || len <= HEADER || !is_pwf(data)) return false;
    if (!s_scanned) scan();
    bool exists = false;
    for (int i = 0; i < s_count; i++) exists |= !strcmp(s_names[i], name);
    if (!exists && s_count >= PHOTO_FACE_MAX) return false;   // full: delete one in the app first
    SD.mkdir("/faces");
    SD.mkdir(DIR_PATH);
    char path[64];
    path_for(path, sizeof(path), name);
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;
    bool ok = f.write(data, len) == (size_t)len;
    f.close();
    if (!ok) { SD.remove(path); return false; }
    photo_face_reload();
    return photo_face_show(name);
#else
    return false;
#endif
}

bool photo_face_available() {
    if (!s_file && !s_tried) load();
    return s_file != nullptr;
}

// a: 0..255 of f over b
static inline uint16_t blend565(uint16_t b, uint16_t f, uint8_t a) {
    uint32_t a5 = ((uint32_t)a + 4) >> 3;   // 0..32
    uint32_t bb = (b | ((uint32_t)b << 16)) & 0x07E0F81F;
    uint32_t ff = (f | ((uint32_t)f << 16)) & 0x07E0F81F;
    bb += ((ff - bb) * a5) >> 5;
    bb &= 0x07E0F81F;
    return (uint16_t)(bb | (bb >> 16));
}

static int glyph_index(char c) { return c == ':' ? 10 : (c >= '0' && c <= '9') ? c - '0' : -1; }

// Returns where the time ends on the right.
static int draw_time(uint16_t *fb, const char *text) {
    const PhotoFace &f = s_face;
    int total = 0;
    for (const char *p = text; *p; p++) {
        int gi = glyph_index(*p);
        if (gi >= 0) total += f.glyph_w[gi];
    }
    int x0 = f.time_x - total / 2;
    int y0 = f.time_y - f.glyph_h / 2;
    for (const char *p = text; *p; p++) {
        int gi = glyph_index(*p);
        if (gi < 0) continue;
        int gw = f.glyph_w[gi];
        const uint8_t *g = f.glyph[gi];
        for (int y = 0; y < f.glyph_h; y++) {
            int sy = y0 + y;
            if (sy < 0 || sy >= f.h) continue;
            uint16_t *row = fb + sy * f.w;
            const uint8_t *grow = g + y * gw;
            for (int x = 0; x < gw; x++) {
                uint8_t a = grow[x];
                int sx = x0 + x;
                if (!a || sx < 0 || sx >= f.w) continue;
                row[sx] = a == 255 ? f.time_color : blend565(row[sx], f.time_color, a);
            }
        }
        x0 += gw;
    }
    return x0;
}

// The subject's pixels back over the time.
static void draw_subject(uint16_t *fb) {
    const PhotoFace &f = s_face;
    const uint8_t *a = f.alpha;
    for (uint32_t i = 0; i < f.spans; i++) {
        const uint8_t *s = f.span_tab + i * 6;
        int y = rd16(s), x = rd16(s + 2), len = rd16(s + 4);
        int p = y * f.w + x;
        for (int k = 0; k < len; k++, p++) {
            uint8_t al = *a++;
            if (al == 255) fb[p] = f.photo[p];
            else if (al) fb[p] = blend565(fb[p], f.photo[p], al);
        }
    }
}

static void photo_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    WatchTime t = rtc_now();
    char hm[8];
    clock_hm(hm, sizeof(hm), t.valid ? t.hour : 0, t.valid ? t.minute : 0);
    uint16_t *fb = ui_framebuffer();

    if (!photo_face_available() || !fb) {
        g->fillScreen(COLOR_BG);
        ui_font_center(LCD_WIDTH / 2, LCD_HEIGHT / 2 - 30, FONT_HUGE, COLOR_TEXT, hm);
        ui_font_center(LCD_WIDTH / 2, LCD_HEIGHT / 2 + 40, FONT_M, COLOR_TEXT_DIM, "Pick a photo in the");
        ui_font_center(LCD_WIDTH / 2, LCD_HEIGHT / 2 + 66, FONT_M, COLOR_TEXT_DIM, "companion app: Photo face");
        return;
    }
    // Every hour / every day: the next photo.
    static int rot_key = -1;
    if (g_app_settings.photo_rotate && t.valid && photo_face_count() > 1) {
        int key = g_app_settings.photo_rotate == 1 ? t.day * 24 + t.hour : t.month * 32 + t.day;
        if (rot_key >= 0 && key != rot_key) photo_face_next();
        rot_key = key;
        if (!photo_face_available()) return;
    }
    const PhotoFace &f = s_face;
    memcpy(fb, f.photo, (size_t)f.w * f.h * 2);
    int right = draw_time(fb, hm);
    if (f.depth) draw_subject(fb);
    if (!clock_24h() && t.valid)
        ui_font_draw(right + 6, f.time_y - f.glyph_h / 2 + ui_font_cap_height(FONT_M_B), FONT_M_B, f.time_color,
                     clock_ampm(t.hour));

    if (t.valid && f.show_date) {
        static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char date[24];
        snprintf(date, sizeof(date), "%s %d %s", DAYS[t.weekday % 7], t.day,
                 MONTHS[(t.month >= 1 && t.month <= 12) ? t.month - 1 : 0]);
        // Under the time, or over it when the time is low on the screen.
        int dy = f.time_y > f.h / 2 ? f.time_y - f.glyph_h / 2 - 20 : f.time_y + f.glyph_h / 2 + 24;
        ui_font_center(f.time_x, dy, FONT_M_B, f.date_color, date);
    }
    // Which photo, for a moment after it changed.
    int n = photo_face_count();
    if (n > 1 && millis() - s_switched_ms < 1500) {
        int cur = cur_index(), shown = n > 12 ? 12 : n;
        int x0 = LCD_WIDTH / 2 - (shown - 1) * 7;
        for (int i = 0; i < shown; i++)
            g->fillCircle(x0 + i * 14, LCD_HEIGHT - 46, i == cur % 12 ? 4 : 3, i == cur % 12 ? COLOR_TEXT : COLOR_TEXT_DIM);
    }
}

// A tap (not a swipe): the next photo - when the home tap is set to the
// face's own (home_gestures.h); otherwise the tap opens what it's set to.
static void photo_touch(int x, int y, bool pressed) {
    static bool down = false;
    static int sx = 0, sy = 0;
    static uint32_t t0 = 0;
    if (pressed && !down) { down = true; sx = x; sy = y; t0 = millis(); return; }
    if (!pressed && down) {
        down = false;
        int dx = x - sx, dy = y - sy;
        if (millis() - t0 < 400 && dx * dx + dy * dy < 24 * 24 && photo_face_count() > 1 && face_nav_tap_is_faces())
            photo_face_next();
    }
}

static void photo_gesture(Gesture g) { face_nav_gesture(g); }   // home_gestures.h

Screen photo_watchface_screen = {
    nullptr, GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nullptr, photo_draw, photo_touch,
    nullptr, nullptr, photo_gesture,
    1000,   // a clock: once a second when idle
};
