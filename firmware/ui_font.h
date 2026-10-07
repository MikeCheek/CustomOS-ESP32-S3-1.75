/*
 * ui_font.h
 * Smooth, proportional fonts with accented letters (Latin-1), replacing the
 * blocky built-in 5x7 font. Text is UTF-8.
 *
 * The old API took a "text size" (1 = 8 px tall cells, 2 = 16 px, ...);
 * ui_font_for_size() maps those to the font with the same cap height, so
 * every screen that draws through ui.h's helpers (ui_text_center,
 * ui_draw_centered_text, list rows, headers, toasts, dialogs) switched over
 * without layout changes. ui_print() is the drop-in for the
 * setTextSize/setCursor/print pattern used directly in some screens.
 *
 * Settings > Display > Smooth fonts turns it off (back to the old font).
 */
#pragma once
#include <stdint.h>

enum UiFont : uint8_t {
    FONT_S,     // ~ size 1 (cap height 11)
    FONT_M,     // ~ size 2 (14)
    FONT_M_B,   // size 2, bold - titles
    FONT_L,     // ~ size 3 (20, bold)
    FONT_XL,    // ~ size 4 (25, bold)
    FONT_XXL,   // ~ size 5 (30, bold)
    FONT_HUGE,  // size 6+ (42, bold)
};

void ui_fonts_set_smooth(bool on);
bool ui_fonts_smooth();

UiFont ui_font_for_size(int size);

// Pixel width (advance) of UTF-8 text.
int  ui_font_width(UiFont f, const char *utf8);
int  ui_font_cap_height(UiFont f);

// Draw with the baseline at y.
void ui_font_draw(int x, int baseline_y, UiFont f, uint16_t color, const char *utf8);
// Draw centred on (cx, cy) - horizontally and on the capital letters.
void ui_font_center(int cx, int cy, UiFont f, uint16_t color, const char *utf8);

// Smooth text is only drawn where s_clip_x0 <= x < x1 (marquees).
void ui_font_set_clip_x(int x0, int x1);
void ui_font_clear_clip();

// Old-API equivalents: (x, y) is the top-left of a classic text cell of
// `size` - i.e. exactly what setTextSize(size); setCursor(x, y); print()
// used. With smooth fonts off, that's literally what it does.
void ui_print(int x, int y, int size, uint16_t color, const char *utf8);
int  ui_text_width(const char *utf8, int size);

// Cuts `s` (in place) so it fits max_px wide, ending with ".." when cut.
// Never splits a UTF-8 character.
void ui_font_fit(char *s, UiFont f, int max_px);
void ui_fit(char *s, int size, int max_px);

// Drops an incomplete UTF-8 character at the end of s (after strncpy).
void ui_utf8_trim(char *s);

// First character of s as its own string (UTF-8 aware), e.g. initials.
void ui_first_char(const char *s, char out[5]);
