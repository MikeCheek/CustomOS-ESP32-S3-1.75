/*
 * ui_keypad.h
 * Phone-style T9 keypad for the round 466px screen - the "old style"
 * keyboard from Meta's MUSE firmware (components/muse/muse_keypad.c),
 * redrawn for this project's Arduino_GFX renderer.
 *
 * Twelve big keys (96x50) instead of a 10-across QWERTY row of ~38px
 * keys:
 *
 *     .-_@1   abc2   def3
 *     ghi4    jkl5   mno6
 *     pqrs7   tuv8   wxyz9
 *     shift   " 0"   backspace
 *        [123]  [Show]  [ OK ]        <- pulled in from the curved edge
 *
 *  - Tap a key again within 1 s to step to its next character; the
 *    character still being chosen is highlighted.
 *  - Hold a letter key for 0.6 s to type its digit.
 *  - Keys fire on release (slide off to cancel); backspace fires on press
 *    and repeats while held.
 *  - The mode key cycles letters -> digits -> symbols.
 *  - Password fields get a Show/Hide key; while hidden, the character you
 *    are typing is still shown briefly.
 *
 * Only one keypad exists at a time (module state), like MUSE's.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct Arduino_GFX;

enum KeypadEvent : uint8_t { KP_NONE, KP_CHANGED, KP_DONE };

// Starts editing `buf` (NUL-terminated, existing text kept), at most
// cap-1 characters. auto_cap upper-cases the first letter of each word
// (for names).
void keypad_begin(char *buf, size_t cap, bool password, const char *ok_label = "OK",
                  bool auto_cap = false);

// Call when the keypad appears while a finger is already down (e.g. it
// was opened by a press): that press is ignored until it lifts, so it
// can't land on whatever key now sits under it.
void keypad_ignore_current_touch();

// Keys, rows y=140..408. The area above (y < 132) is yours: title and
// keypad_draw_field().
void keypad_draw(Arduino_GFX *g);

// A 300x44 text field centred at row y (84 fits the layout above), with
// a caret and the in-progress character highlighted.
void keypad_draw_field(Arduino_GFX *g, int y, const char *placeholder);

// Feed every touch event of the screen (pressed and release, raw - not
// edge-gated: holds and repeats need the in-between events).
KeypadEvent keypad_touch(int x, int y, bool pressed);

// Call every frame: finishes the multi-tap character after its timeout.
void keypad_tick();
