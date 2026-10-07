#include "ui_font.h"
#include "ui.h"
#include "ui_fonts_data.h"
#include <Arduino_GFX_Library.h>
#include <string.h>

// Anti-aliased text: glyphs are 4-bit coverage masks (Inter, see
// ui_fonts_data.h), blended straight into the canvas framebuffer so edges
// are smooth against whatever is already drawn underneath.

static bool s_smooth = true;
static int s_clip_x0 = -32768, s_clip_x1 = 32767;   // horizontal clip for smooth text

void ui_font_set_clip_x(int x0, int x1) { s_clip_x0 = x0; s_clip_x1 = x1; }
void ui_font_clear_clip() { s_clip_x0 = -32768; s_clip_x1 = 32767; }

static const UiFontData *const FONTS[] = {
    &ui_font_S, &ui_font_M, &ui_font_M_B, &ui_font_L, &ui_font_XL, &ui_font_XXL, &ui_font_HUGE,
};

void ui_fonts_set_smooth(bool on) { s_smooth = on; }
bool ui_fonts_smooth() { return s_smooth; }

UiFont ui_font_for_size(int size) {
    switch (size) {
        case 0:
        case 1: return FONT_S;
        case 2: return FONT_M;
        case 3: return FONT_L;
        case 4: return FONT_XL;
        case 5: return FONT_XXL;
        default: return FONT_HUGE;
    }
}

int ui_font_cap_height(UiFont f) { return FONTS[f]->cap; }

// ---- UTF-8 -------------------------------------------------------------------

static uint32_t next_cp(const char *&p) {
    uint8_t c = (uint8_t)*p++;
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = n == 3 ? (c & 0x07) : n == 2 ? (c & 0x0F) : (c & 0x1F);
    for (int i = 0; i < n; i++) {
        if (((uint8_t)*p & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
    }
    return n ? cp : '?';
}

static const UiGlyph *find_glyph(const UiFontData *f, uint32_t cp) {
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t c = f->glyphs[mid].cp;
        if (c == cp) return &f->glyphs[mid];
        if (c < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

static const UiGlyph *glyph_or_fallback(const UiFontData *f, uint32_t cp) {
    const UiGlyph *g = find_glyph(f, cp);
    if (!g && cp != '?') g = find_glyph(f, '?');
    return g;
}

int ui_font_width(UiFont f, const char *utf8) {
    if (!utf8) return 0;
    const UiFontData *fd = FONTS[f];
    int w = 0;
    const char *p = utf8;
    while (*p) {
        const UiGlyph *g = glyph_or_fallback(fd, next_cp(p));
        if (g) w += g->adv;
    }
    return w;
}

// ---- drawing -------------------------------------------------------------------

static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint32_t a /*0..15*/) {
    uint32_t inv = 15 - a;
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv) / 15;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * inv) / 15;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * inv) / 15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void ui_font_draw(int x, int baseline_y, UiFont f, uint16_t color, const char *utf8) {
    Arduino_GFX *gfx = ui_gfx();
    if (!gfx || !utf8) return;
    const UiFontData *fd = FONTS[f];
    // The UI always draws into an (unrotated) Arduino_Canvas; blend straight
    // into its framebuffer. Without one, plain pixels for the solid parts.
    Arduino_Canvas *canvas = static_cast<Arduino_Canvas *>(gfx);
    uint16_t *fb = gfx->getRotation() == 0 ? canvas->getFramebuffer() : nullptr;
    const int W = gfx->width(), H = gfx->height();

    const char *p = utf8;
    while (*p) {
        const UiGlyph *g = glyph_or_fallback(fd, next_cp(p));
        if (!g) continue;
        const uint8_t *bits = fd->bits + g->off;
        int gx = x + g->xo, gy = baseline_y + g->yo;
        int idx = 0;
        for (int r = 0; r < g->h; r++) {
            int py = gy + r;
            for (int c = 0; c < g->w; c++, idx++) {
                uint8_t byte = bits[idx >> 1];
                uint32_t a = (idx & 1) ? (byte & 0x0F) : (byte >> 4);
                if (!a) continue;
                int px = gx + c;
                if ((unsigned)px >= (unsigned)W || (unsigned)py >= (unsigned)H) continue;
                if (px < s_clip_x0 || px >= s_clip_x1) continue;
                if (fb) {
                    uint16_t *d = fb + py * W + px;
                    *d = a >= 15 ? color : blend565(color, *d, a);
                } else if (a >= 8) {
                    gfx->writePixel(px, py, color);
                }
            }
        }
        x += g->adv;
    }
}

void ui_font_center(int cx, int cy, UiFont f, uint16_t color, const char *utf8) {
    int w = ui_font_width(f, utf8);
    ui_font_draw(cx - w / 2, cy + FONTS[f]->cap / 2, f, color, utf8);
}

// ---- classic (old built-in font) fallback ----------------------------------------

// The built-in font has no accents: fold Latin-1 to plain letters for it.
static void fold_for_classic(const char *in, char *out, size_t n) {
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs" "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
    size_t o = 0;
    const char *p = in;
    while (*p && o + 1 < n) {
        uint32_t cp = next_cp(p);
        char c;
        if (cp < 0x80) c = (char)cp;
        else if (cp >= 0xC0 && cp <= 0xFF) c = latin1[cp - 0xC0];
        else if (cp == 0x2026) c = '.';
        else if (cp == 0x2013 || cp == 0x2014) c = '-';
        else if (cp == 0x2018 || cp == 0x2019) c = '\'';
        else if (cp == 0x201C || cp == 0x201D) c = '"';
        else if (cp == 0xA0) c = ' ';
        else c = '?';
        out[o++] = c;
    }
    out[o] = 0;
}

static void classic_print(Arduino_GFX *g, int x, int y, int size, uint16_t color, const char *utf8) {
    char buf[160];
    fold_for_classic(utf8, buf, sizeof(buf));
    g->setTextSize(size);
    g->setTextColor(color);
    g->setCursor(x, y);
    g->print(buf);
}

void ui_print(int x, int y, int size, uint16_t color, const char *utf8) {
    Arduino_GFX *g = ui_gfx();
    if (!g || !utf8) return;
    if (!s_smooth) {
        classic_print(g, x, y, size, color, utf8);
        return;
    }
    UiFont f = ui_font_for_size(size);
    // Classic cell: top y, capitals 7*size tall from the top. Put the smooth
    // capitals in the same band (centred on it) so layouts don't move.
    int band_mid = y + (7 * size) / 2;
    ui_font_draw(x, band_mid + FONTS[f]->cap / 2, f, color, utf8);
}

int ui_text_width(const char *utf8, int size) {
    if (!utf8) return 0;
    if (!s_smooth) {
        char buf[160];
        fold_for_classic(utf8, buf, sizeof(buf));
        return (int)strlen(buf) * 6 * size;
    }
    return ui_font_width(ui_font_for_size(size), utf8);
}

// ---- fitting / helpers --------------------------------------------------------------

void ui_utf8_trim(char *s) {
    if (!s) return;
    int n = (int)strlen(s);
    int i = n;
    while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) i--; // trailing continuation bytes
    if (i == 0) return;
    uint8_t lead = (uint8_t)s[i - 1];
    if (lead < 0x80) return;
    int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < need) s[i - 1] = 0; // incomplete last character
}

static int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && ((uint8_t)s[i] & 0xC0) == 0x80) i--;
    return i;
}

// Cut so text + `tail` fits; the tail is written in place, so at least as
// many bytes as it has are removed first.
static void fit_with(char *s, int max_px, int (*width)(const char *, int), int arg, const char *tail) {
    if (!s || width(s, arg) <= max_px) return;
    int len = (int)strlen(s);
    const int orig = len;
    const int tl = (int)strlen(tail);
    int tw = width(tail, arg);
    while (len > 0) {
        len = utf8_prev(s, len);
        char save = s[len];
        s[len] = 0;
        int w = width(s, arg);
        s[len] = save;
        if (w + tw <= max_px && len <= orig - tl) break;
    }
    while (len > 0 && s[len - 1] == ' ') len--;
    if (orig < tl) { s[0] = 0; return; }
    memcpy(s + len, tail, tl + 1);
}

static int width_font(const char *s, int f) { return ui_font_width((UiFont)f, s); }
static int width_size(const char *s, int size) { return ui_text_width(s, size); }

void ui_font_fit(char *s, UiFont f, int max_px) {
    fit_with(s, max_px, width_font, (int)f, "\xE2\x80\xA6"); // ellipsis (3 bytes)
}

void ui_fit(char *s, int size, int max_px) {
    fit_with(s, max_px, width_size, size, s_smooth ? "\xE2\x80\xA6" : "..");
}

void ui_first_char(const char *s, char out[5]) {
    out[0] = '?'; out[1] = 0;
    if (!s || !*s) return;
    uint8_t c = (uint8_t)s[0];
    int n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    int i = 0;
    for (; i < n && s[i]; i++) out[i] = s[i];
    out[i] = 0;
    // upper-case ASCII and Latin-1 lower-case letters (U+00E0..U+00FE except U+00F7)
    if (n == 1 && out[0] >= 'a' && out[0] <= 'z') out[0] -= 32;
    if (n == 2 && (uint8_t)out[0] == 0xC3 && (uint8_t)out[1] >= 0xA0 && (uint8_t)out[1] != 0xB7 && (uint8_t)out[1] != 0xBF)
        out[1] = (char)((uint8_t)out[1] - 0x20);
}
