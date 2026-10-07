/*
 * app_pet.cpp
 * A small vector-drawn creature that wanders around the watchface's
 * circular rim - walking, occasionally "sliding" into a direction
 * change with a little skid, and pausing now and then to play (a
 * bounce + tail-wag idle animation) before moving on again. Draggable
 * (see pet_handle_touch()), and comes in a few selectable species
 * (see pet_screen, the small picker app).
 *
 * State machine, not a script: PET_WALK moves it along the rim at a
 * fixed angular speed; it randomly transitions either into PET_PAUSE
 * (stop and play for a couple of seconds) or PET_SLIDE (a brief faster
 * skid that ends with the direction reversed) before returning to
 * PET_WALK. No two trips around the dial look quite the same.
 *
 * Kept as simple filled shapes (rounded rect body, circle head,
 * triangle/circle ears/tail) rather than a bitmap sprite - consistent
 * with every other piece of art in this project, and there's no
 * sprite pipeline here to load one through anyway.
 */
#include "app_pet.h"
#include "config.h"
#include "board_pins.h"
#include <math.h>
#include <string.h>

#define DEG2RAD (3.14159265f / 180.0f)

// Walks just inside the true bezel edge - PET_R plus the sprite's own
// extent (ears/tail reach out a further ~20px at most) stays under
// the display's actual 233px radius.
#define PET_R      195.0f
#define PET_GRAB_R  32.0f // touch hit-radius for picking the pet up

enum PetState { PET_WALK, PET_PAUSE, PET_SLIDE };
enum EarStyle { EAR_POINTY, EAR_SMALL, EAR_TALL, EAR_ROUND };
enum TailStyle { TAIL_BUSHY, TAIL_THIN, TAIL_POMPOM };

struct SpeciesStyle {
    const char *name;
    uint8_t r, g, b;
    EarStyle ears;
    TailStyle tail;
};

static const SpeciesStyle SPECIES[PET_SPECIES_COUNT] = {
    { "Fox",   0xFF, 0xB8, 0x6B, EAR_POINTY, TAIL_BUSHY  },
    { "Cat",   0xB4, 0xAA, 0xC8, EAR_SMALL,  TAIL_THIN   },
    { "Bunny", 0xFF, 0xDC, 0xE6, EAR_TALL,   TAIL_POMPOM },
    { "Bear",  0x96, 0x64, 0x46, EAR_ROUND,  TAIL_POMPOM },
};

static int s_species = 0;

static bool s_inited = false;
static PetState s_state = PET_WALK;
static float s_angle_deg = 0.0f;
static float s_dir = 1.0f; // +1 clockwise, -1 counter-clockwise
static uint32_t s_state_until_ms = 0;
static uint32_t s_last_update_ms = 0;
static bool s_dragging = false;

int pet_get_species() { return s_species; }
void pet_set_species(int index) {
    if (index < 0 || index >= PET_SPECIES_COUNT) return;
    s_species = index;
}
const char *pet_species_name(int index) {
    if (index < 0 || index >= PET_SPECIES_COUNT) return "";
    return SPECIES[index].name;
}

static void pet_init_once(uint32_t now) {
    s_angle_deg = (float)random(360);
    s_dir = random(2) ? 1.0f : -1.0f;
    s_state = PET_WALK;
    s_state_until_ms = now + 3000 + (uint32_t)random(4000);
    s_last_update_ms = now;
    s_inited = true;
}

static void pet_current_pos(int *cx, int *cy) {
    *cx = LCD_WIDTH / 2 + (int)(cosf(s_angle_deg * DEG2RAD) * PET_R);
    *cy = LCD_HEIGHT / 2 + (int)(sinf(s_angle_deg * DEG2RAD) * PET_R);
}

static void pet_update(uint32_t now) {
    if (!s_inited) { pet_init_once(now); return; }
    if (s_dragging) return; // position is being driven by the touch instead

    float dt = (float)(now - s_last_update_ms) / 1000.0f;
    s_last_update_ms = now;
    if (dt > 0.2f) dt = 0.2f; // clamp a big gap (e.g. screen was off) to one reasonable step

    switch (s_state) {
    case PET_WALK:
        s_angle_deg += s_dir * 24.0f * dt; // deg/sec
        if (s_angle_deg >= 360.0f) s_angle_deg -= 360.0f;
        if (s_angle_deg < 0.0f) s_angle_deg += 360.0f;
        if (now >= s_state_until_ms) {
            if (random(100) < 55) {
                s_state = PET_PAUSE;
                s_state_until_ms = now + 1500 + (uint32_t)random(2500);
            } else {
                s_state = PET_SLIDE;
                s_state_until_ms = now + 400;
            }
        }
        break;
    case PET_PAUSE:
        if (now >= s_state_until_ms) {
            s_state = PET_WALK;
            s_state_until_ms = now + 3000 + (uint32_t)random(5000);
        }
        break;
    case PET_SLIDE:
        s_angle_deg += s_dir * 55.0f * dt; // faster during the skid
        if (s_angle_deg >= 360.0f) s_angle_deg -= 360.0f;
        if (s_angle_deg < 0.0f) s_angle_deg += 360.0f;
        if (now >= s_state_until_ms) {
            s_dir = -s_dir; // the skid ends with a reversal
            s_state = PET_WALK;
            s_state_until_ms = now + 3000 + (uint32_t)random(4000);
        }
        break;
    }
}

static void draw_pet_creature(Arduino_GFX *g, int cx, int cy, uint32_t now,
                               PetState state, float dir, int species) {
    if (species < 0 || species >= PET_SPECIES_COUNT) species = 0;
    const SpeciesStyle &sp = SPECIES[species];
    uint16_t body_c = (uint16_t)COLOR565(sp.r, sp.g, sp.b);
    uint16_t detail_c = (uint16_t)COLOR565(sp.r / 3, sp.g / 3, sp.b / 3);
    int flip = (dir > 0) ? 1 : -1; // mirrors the sprite to face its direction of travel

    int bob = 0;
    if (state == PET_WALK) {
        bob = ((now / 150) % 2 == 0) ? -2 : 2;
    } else if (state == PET_SLIDE) {
        bob = 3; // leans lower into the skid
    }
    int by = cy + bob;

    // Body + head
    g->fillRoundRect(cx - 14, by - 10, 28, 20, 9, body_c);
    g->fillCircle(cx + flip * 10, by - 12, 9, body_c);

    // Ears - shape/size varies by species
    switch (sp.ears) {
        case EAR_POINTY:
            g->fillTriangle(cx + flip * 6, by - 18, cx + flip * 14, by - 24, cx + flip * 14, by - 14, body_c);
            g->fillTriangle(cx + flip * 14, by - 18, cx + flip * 20, by - 22, cx + flip * 18, by - 12, body_c);
            break;
        case EAR_SMALL:
            g->fillTriangle(cx + flip * 7, by - 16, cx + flip * 12, by - 21, cx + flip * 13, by - 13, body_c);
            g->fillTriangle(cx + flip * 13, by - 16, cx + flip * 18, by - 20, cx + flip * 17, by - 12, body_c);
            break;
        case EAR_TALL:
            g->fillRoundRect(cx + flip * 6, by - 32, 5, 20, 2, body_c);
            g->fillRoundRect(cx + flip * 14, by - 30, 5, 18, 2, body_c);
            break;
        case EAR_ROUND:
            g->fillCircle(cx + flip * 6, by - 20, 6, body_c);
            g->fillCircle(cx + flip * 16, by - 19, 6, body_c);
            break;
    }

    // Eye
    g->fillCircle(cx + flip * 14, by - 13, 2, detail_c);

    // Legs - alternate during a walk cycle
    int leg_off = (state == PET_WALK && (now / 150) % 2 == 0) ? 2 : -2;
    g->fillRoundRect(cx - 10, by + 8, 5, 6 + leg_off, 2, detail_c);
    g->fillRoundRect(cx + 4, by + 8, 5, 6 - leg_off, 2, detail_c);

    // Tail - wags while paused/playing, otherwise trails behind at a fixed angle
    float wag = (state == PET_PAUSE) ? sinf((float)now / 120.0f) * 14.0f : 4.0f;
    int tx = cx - flip * 16;
    int ty = by - 4 + (int)wag;
    switch (sp.tail) {
        case TAIL_BUSHY:
            g->fillTriangle(cx - flip * 12, by - 2, tx, ty, tx, ty + 8, body_c);
            break;
        case TAIL_THIN:
            g->drawLine(cx - flip * 12, by - 2, tx, ty + 4, detail_c);
            g->drawLine(cx - flip * 11, by - 1, tx, ty + 5, detail_c);
            break;
        case TAIL_POMPOM:
            g->fillCircle(tx, ty + 4, 5, body_c);
            break;
    }

    if (state == PET_SLIDE) {
        // A few short motion-streak lines behind it while skidding.
        for (int i = 1; i <= 3; i++) {
            int lx0 = cx - flip * (16 + i * 6);
            int lx1 = cx - flip * (10 + i * 6);
            g->drawLine(lx0, by + 6, lx1, by + 6, COLOR_TEXT_DIM);
        }
    }
}

void pet_step_and_draw(Arduino_GFX *g) {
    if (!g) return;
    uint32_t now = millis();
    pet_update(now);
    int cx, cy;
    pet_current_pos(&cx, &cy);
    draw_pet_creature(g, cx, cy, now, s_state, s_dir, s_species);
}

void pet_draw_icon(Arduino_GFX *g, int cx, int cy) {
    if (!g) return;
    draw_pet_creature(g, cx, cy, 0, PET_PAUSE, 1.0f, s_species);
}

bool pet_handle_touch(int x, int y, bool pressed) {
    if (!s_inited) pet_init_once(millis());

    if (pressed) {
        if (!s_dragging) {
            int cx, cy;
            pet_current_pos(&cx, &cy);
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy > (int)(PET_GRAB_R * PET_GRAB_R)) return false; // not touching the pet
            s_dragging = true;
        }
        // Follow the finger, projected onto the rim - dragging repositions
        // the pet along its walking path rather than lifting it off the
        // rim entirely, which would fight with its "lives on the edge"
        // design and its own draw/collision assumptions elsewhere.
        float fdx = (float)x - LCD_WIDTH / 2.0f, fdy = (float)y - LCD_HEIGHT / 2.0f;
        s_angle_deg = atan2f(fdy, fdx) / DEG2RAD;
        if (s_angle_deg < 0.0f) s_angle_deg += 360.0f;
        s_state = PET_PAUSE; // held = not autonomously walking
        s_state_until_ms = millis() + 500;
        return true;
    } else {
        if (!s_dragging) return false;
        s_dragging = false;
        s_state = PET_PAUSE;
        s_state_until_ms = millis() + 800; // brief settle before it wanders off again
        s_last_update_ms = millis();
        return true;
    }
}

// ---- Pet picker app -------------------------------------------------

static int s_picker_xs[PET_SPECIES_COUNT];
static int s_picker_y;

static void pet_picker_create() {
    s_picker_y = LCD_HEIGHT / 2 - 10;
    int spacing = 110;
    int start_x = LCD_WIDTH / 2 - spacing * (PET_SPECIES_COUNT - 1) / 2;
    for (int i = 0; i < PET_SPECIES_COUNT; i++) s_picker_xs[i] = start_x + i * spacing;
}

static void pet_picker_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    ui_draw_centered_text(LCD_HEIGHT / 2 - 110, COLOR_TEXT, "Choose your pet", 2);

    for (int i = 0; i < PET_SPECIES_COUNT; i++) {
        bool selected = (i == s_species);
        int r = selected ? 42 : 36;
        g->fillCircle(s_picker_xs[i], s_picker_y, r, COLOR_PANEL);
        g->drawCircle(s_picker_xs[i], s_picker_y, r, selected ? COLOR_ACCENT2 : COLOR_TEXT_DIM);
        draw_pet_creature(g, s_picker_xs[i], s_picker_y + 6, 0, PET_PAUSE, 1.0f, i);
    }

    ui_draw_centered_text(LCD_HEIGHT / 2 + 90, COLOR_TEXT, SPECIES[s_species].name, 3);
    ui_draw_centered_text(LCD_HEIGHT / 2 + 130, COLOR_TEXT_DIM, "Tap a pet to select it", 2);
}

static void pet_picker_touch(int x, int y, bool pressed) {
    static bool s_prev_pressed = false;
    bool tap_edge = pressed && !s_prev_pressed;
    s_prev_pressed = pressed;
    if (!tap_edge) return;
    for (int i = 0; i < PET_SPECIES_COUNT; i++) {
        int dx = x - s_picker_xs[i], dy = y - s_picker_y;
        if (dx * dx + dy * dy <= 44 * 44) {
            pet_set_species(i);
            return;
        }
    }
}

Screen pet_screen = {
    "Pet", GESTURE_MODE_EDGE,
    UI_FRAME_MS_DEFAULT,
    pet_picker_create, pet_picker_draw, pet_picker_touch, nullptr, nullptr, nullptr
};
