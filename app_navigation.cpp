#include "app_navigation.h"
#include "dnd.h"
#include "config.h"
#include "board_pins.h"
#include "ui_font.h"
#include "hal_ble.h"
#include "hal_sleep.h"
#include "hal_audio.h"
#include "app_settings_state.h"
#include "phone_link.h"
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <ctype.h>

// =========================================================================
//  Turn-by-turn navigation, mirrored from the phone's navigation
//  notification (Google Maps and other apps that post one).
//
//  phone -> watch  {"t":"nav","st":1,"d":"200 m","i":"Turn right onto ...","x":"12 min · 10:42","h":<icon hash>}
//                  {"t":"nav","st":0}                                    navigation ended
//                  {"t":"navi","h":<hash>,"w":48,"b":"<base64>"}         maneuver icon, 1 bit/pixel,
//                                                                         rows MSB first, sent once per hash
// =========================================================================

#define NAV_ICON_W     48
#define NAV_ICON_BYTES (NAV_ICON_W * NAV_ICON_W / 8)
#define NAV_ICON_SLOTS 12

struct NavIcon { uint32_t hash; uint8_t bits[NAV_ICON_BYTES]; };
static NavIcon *s_icons = nullptr;      // PSRAM cache
static int s_icon_next = 0;

static bool s_active = false;
static char s_dist[24] = "";
static char s_instr[160] = "";
static char s_extra[64] = "";
static uint32_t s_hash = 0;
static uint32_t s_ended_ms = 0;
static uint32_t s_updated_ms = 0;
static bool s_shown = false;            // the screen was put up for this trip
static bool s_dismissed = false;        // ...and the user has closed it since
#define NAV_STALE_MS (10UL * 60 * 1000) // no update for this long = trip is over

static void copy_txt(char *dst, size_t n, const char *src) {
    strncpy(dst, src ? src : "", n - 1);
    dst[n - 1] = 0;
    ble_fold_utf8(dst);
    ui_utf8_trim(dst);
}

static const NavIcon *find_icon(uint32_t h) {
    if (!s_icons || !h) return nullptr;
    for (int i = 0; i < NAV_ICON_SLOTS; i++) if (s_icons[i].hash == h) return &s_icons[i];
    return nullptr;
}

static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int b64_decode(const char *in, uint8_t *out, int cap) {
    int n = 0, acc = 0, bits = 0;
    for (; *in; in++) {
        int v = b64v(*in);
        if (v < 0) continue;   // '=' padding, whitespace
        acc = ((acc << 6) | v) & 0xFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < cap) out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return n;
}

extern Screen navigation_screen;

void navigation_on_link(JsonDocument &doc, bool icon_msg) {
    if (icon_msg) {
        if (!s_icons) s_icons = (NavIcon *)heap_caps_calloc(NAV_ICON_SLOTS, sizeof(NavIcon), MALLOC_CAP_SPIRAM);
        if (!s_icons || (doc["w"] | 0) != NAV_ICON_W) return;
        uint32_t h = doc["h"] | 0u;
        if (!h || find_icon(h)) return;
        NavIcon &ic = s_icons[s_icon_next];
        s_icon_next = (s_icon_next + 1) % NAV_ICON_SLOTS;
        if (b64_decode(doc["b"] | "", ic.bits, NAV_ICON_BYTES) == NAV_ICON_BYTES) ic.hash = h;
        else ic.hash = 0;
        return;
    }

    if (!(doc["st"] | 0)) {
        if (s_active) { s_active = false; s_ended_ms = millis(); }
        return;
    }
    char instr[sizeof(s_instr)];
    copy_txt(instr, sizeof(instr), doc["i"] | "");
    bool new_step = !s_active || strcmp(instr, s_instr) != 0;
    bool starting = !s_active;
    memcpy(s_instr, instr, sizeof(s_instr));
    copy_txt(s_dist, sizeof(s_dist), doc["d"] | "");
    copy_txt(s_extra, sizeof(s_extra), doc["x"] | "");
    s_hash = doc["h"] | 0u;
    s_active = true;
    s_ended_ms = 0;
    s_updated_ms = millis();
    if (starting) { s_dismissed = false; s_shown = false; }
    // However the user closed it (back button, swipe, BOOT...), it's gone
    // from the stack: don't force it back up on this trip.
    if (s_shown && !ui_screen_in_stack(&navigation_screen)) s_dismissed = true;

    // A new maneuver wakes the watch and shows it - but never on top of a
    // call.
    if (new_step && !s_dismissed) {
        sleep_register_activity();
        if (!ui_screen_in_stack(&navigation_screen) && phone_link_call().state == CALL_NONE) {
            ui_push(&navigation_screen);
            s_shown = true;
        }
#if FEATURE_AUDIO
        if (!starting && g_app_settings.sfx_enabled && !dnd_active()) audio_play_sfx(988, 70);
#endif
    }
}

bool navigation_active() {
    // The phone ends trips explicitly; this catches one that ended while
    // the phone was away.
    if (s_active && millis() - s_updated_ms > NAV_STALE_MS) { s_active = false; s_ended_ms = millis(); }
    return s_active;
}

// ---- drawing ------------------------------------------------------------------------

static const int CX = LCD_WIDTH / 2, CY = LCD_HEIGHT / 2;

// Fallback arrow from the instruction text, when there's no icon.
enum Arrow { A_STRAIGHT, A_LEFT, A_RIGHT, A_UTURN, A_ROUND, A_ARRIVE };

static bool has(const char *hay, const char *needle) {
    // case-insensitive ASCII search
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < n && p[k] && tolower((unsigned char)p[k]) == needle[k]) k++;
        if (k == n) return true;
    }
    return false;
}

static Arrow guess_arrow(const char *s) {
    if (has(s, "u-turn") || has(s, "inversione") || has(s, "media vuelta") || has(s, "demi-tour") || has(s, "wenden"))
        return A_UTURN;
    if (has(s, "roundabout") || has(s, "rotonda") || has(s, "rotatoria") || has(s, "glorieta") || has(s, "rond-point") || has(s, "kreisverkehr"))
        return A_ROUND;
    if (has(s, "arriv") || has(s, "destinat") || has(s, "ziel")) return A_ARRIVE;
    if (has(s, "left") || has(s, "sinistra") || has(s, "izquierda") || has(s, "gauche") || has(s, "links")) return A_LEFT;
    if (has(s, "right") || has(s, "destra") || has(s, "derecha") || has(s, "droite") || has(s, "rechts")) return A_RIGHT;
    return A_STRAIGHT;
}

static void draw_arrow(Arduino_GFX *g, int cx, int cy, Arrow a, uint16_t c) {
    switch (a) {
    case A_LEFT:
        g->fillRect(cx - 4, cy - 6, 14, 44, c);
        g->fillRect(cx - 22, cy - 6, 32, 14, c);
        g->fillTriangle(cx - 40, cy + 1, cx - 18, cy - 22, cx - 18, cy + 24, c);
        break;
    case A_RIGHT:
        g->fillRect(cx - 10, cy - 6, 14, 44, c);
        g->fillRect(cx - 10, cy - 6, 32, 14, c);
        g->fillTriangle(cx + 40, cy + 1, cx + 18, cy - 22, cx + 18, cy + 24, c);
        break;
    case A_UTURN:
        g->fillRect(cx + 6, cy - 14, 14, 52, c);
        g->fillRoundRect(cx - 26, cy - 34, 46, 30, 14, c);
        g->fillRoundRect(cx - 12, cy - 20, 18, 16, 6, COLOR_ACCENT);
        g->fillRect(cx - 26, cy - 18, 14, 26, c);
        g->fillTriangle(cx - 19, cy + 30, cx - 38, cy + 6, cx + 0, cy + 6, c);
        break;
    case A_ROUND:
        g->fillCircle(cx, cy + 6, 22, c);
        g->fillCircle(cx, cy + 6, 11, COLOR_ACCENT);
        g->fillRect(cx - 6, cy + 26, 12, 16, c);
        g->fillRect(cx + 14, cy - 26, 12, 22, c);
        g->fillTriangle(cx + 20, cy - 42, cx + 6, cy - 24, cx + 34, cy - 24, c);
        break;
    case A_ARRIVE:
        g->fillCircle(cx, cy - 8, 22, c);
        g->fillTriangle(cx - 19, cy + 2, cx + 19, cy + 2, cx, cy + 36, c);
        g->fillCircle(cx, cy - 8, 9, COLOR_ACCENT);
        break;
    default:
        g->fillRect(cx - 7, cy - 14, 14, 52, c);
        g->fillTriangle(cx, cy - 40, cx - 24, cy - 12, cx + 24, cy - 12, c);
        break;
    }
}

// 48x48 1-bit icon drawn at 2x, one run of set pixels per fillRect.
static void draw_icon(Arduino_GFX *g, const NavIcon &ic, int cx, int cy, uint16_t c) {
    const int S = 2, x0 = cx - NAV_ICON_W * S / 2, y0 = cy - NAV_ICON_W * S / 2;
    for (int r = 0; r < NAV_ICON_W; r++) {
        int run = -1;
        for (int col = 0; col <= NAV_ICON_W; col++) {
            bool on = col < NAV_ICON_W && (ic.bits[r * (NAV_ICON_W / 8) + col / 8] & (0x80 >> (col % 8)));
            if (on && run < 0) run = col;
            else if (!on && run >= 0) {
                g->fillRect(x0 + run * S, y0 + r * S, (col - run) * S, S, c);
                run = -1;
            }
        }
    }
}

static void nav_create() {
    navigation_active();   // expire a stale trip
    // A trip that ended while this screen wasn't up: don't auto-close now.
    if (!s_active && s_ended_ms && millis() - s_ended_ms > 6000) s_ended_ms = 0;
}

static void nav_tick() {
    navigation_active();
    // "Arrived" stays up a few seconds after the phone ends navigation.
    if (!s_active && s_ended_ms && millis() - s_ended_ms > 6000) {
        s_ended_ms = 0;
        ui_pop_screen();
    }
}

static void nav_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;
    if (!navigation_active()) {
        g->fillCircle(CX, CY - 50, 56, COLOR_GOOD);
        draw_arrow(g, CX, CY - 50, A_ARRIVE, COLOR_TEXT);
        ui_text_center(CX, CY + 40, COLOR_TEXT, s_ended_ms ? "Navigation ended" : "No navigation", 2);
        ui_text_center(CX, CY + 72, COLOR_TEXT_DIM,
                       s_ended_ms ? "" : "Start directions in Maps on your phone", 1);
        return;
    }
    // maneuver
    g->fillCircle(CX, 128, 62, COLOR_ACCENT);
    const NavIcon *ic = find_icon(s_hash);
    if (ic) draw_icon(g, *ic, CX, 128, COLOR_TEXT);
    else draw_arrow(g, CX, 128, guess_arrow(s_instr), COLOR_TEXT);

    // distance to it
    ui_text_center(CX, 232, COLOR_TEXT, s_dist[0] ? s_dist : "--", 5);

    // instruction, up to 3 lines
    const char *p = s_instr;
    int y = 282;
    for (int line = 0; line < 3 && *p; line++) {
        char buf[64];
        int take = (int)strlen(p);
        if (take > 63) take = 63;
        memcpy(buf, p, take); buf[take] = 0;
        bool last = line == 2;
        if (!last) {
            while (take > 1 && ui_text_width(buf, 2) > 340) {
                int sp = take - 1;
                while (sp > 0 && buf[sp] != ' ') sp--;
                take = sp > 0 ? sp : take - 1;
                buf[take] = 0;
            }
        } else {
            ui_fit(buf, 2, 300);
        }
        ui_utf8_trim(buf);
        take = (int)strlen(buf);
        if (!take) break;
        ui_text_center(CX, y, COLOR_TEXT, buf, 2);
        y += 28;
        p += last ? strlen(p) : take;
        while (*p == ' ') p++;
    }
    // ETA / remaining
    if (s_extra[0]) {
        char x[64];
        snprintf(x, sizeof(x), "%s", s_extra);
        ui_fit(x, 1, 280);
        ui_text_center(CX, LCD_HEIGHT - 62, COLOR_TEXT_DIM, x, 1);
    }
    if (!ble_is_connected()) ui_text_center(CX, LCD_HEIGHT - 40, COLOR_WARN, "phone disconnected", 1);
}

static void nav_touch(int x, int y, bool pressed) {
    (void)x;
    static bool back = false;
    if (pressed && x < 54 && y < 54) back = true;
    if (!pressed && back) { back = false; ui_pop_screen(); }
}

static void nav_gesture(Gesture g) {
    if (g == GESTURE_SWIPE_RIGHT) ui_pop_screen();
}

Screen navigation_screen = {
    "Navigation", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    nav_create, nav_draw, nav_touch, nav_tick, nullptr, nav_gesture,
    1000, false, false, false, false, true,
};

const char *navigation_distance() { return s_dist; }
