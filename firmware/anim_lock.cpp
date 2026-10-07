/*
 * anim_lock.cpp
 * A short particle-swarm transition played when locking (sleeping) or
 * unlocking (waking) the watch. A random shape (sphere, torus, or
 * cube) is generated each time, then the whole thing tumbles while
 * expanding from a point (waking) or contracting to a point
 * (sleeping), with a weak-perspective projection for depth.
 *
 * Tuned for a finer, more premium look than a first pass would give
 * you for free:
 *  - More, smaller particles (fine stardust rather than a handful of
 *    chunky balls), each drawn as a soft dim halo + a bright core -
 *    real glow isn't available (no alpha blending on this display
 *    path), so it's faked with two solid circles per particle.
 *  - An even Fibonacci-sphere point distribution instead of naive
 *    random theta/phi, which visibly clumps points near the poles -
 *    the even spacing reads as "precise" rather than "scattered".
 *  - A warm gold/champagne/platinum palette instead of the app's
 *    usual UI accent colors, dimmed per-particle by simulated depth
 *    so far particles recede convincingly instead of just shrinking.
 *  - Smoothstep easing on the expand/contract motion instead of
 *    linear, plus a gentle second rotation axis for a tumbling-jewel
 *    feel rather than a flat spin.
 *  - A slow per-particle twinkle (brightness pulsing) for polish.
 *
 * Implemented as an ordinary Screen (see ui.h) rather than a bespoke
 * blocking draw loop, so it gets driven by the normal render loop
 * (on_tick/on_draw) and pops itself off the stack when done - see
 * AmoledSmartWatchOS.ino for where it gets pushed.
 */
#include "anim_lock.h"
#include "config.h"
#include "board_pins.h"
#include "hal_sleep.h"
#include <Arduino_GFX_Library.h>
#include <math.h>

#define ANIM_PARTICLES    130
#define ANIM_DURATION_MS  850
#define ANIM_RADIUS       160.0f

enum AnimShape { SHAPE_SPHERE, SHAPE_TORUS, SHAPE_CUBE };

struct AnimParticle {
    float x, y, z;
    uint16_t color;
    float size_mul;      // per-particle size variety (fine dust vs slightly bigger accents)
    float twinkle_phase; // per-particle phase offset for the brightness pulse
};
static AnimParticle s_particles[ANIM_PARTICLES];

static bool s_waking = true;
static bool s_sleep_after = false;
static uint32_t s_start_ms;

void lock_anim_set_mode(bool waking, bool sleep_after) {
    s_waking = waking;
    s_sleep_after = sleep_after;
}

// Scales an RGB565 color's channels by `factor` (0..1) - used both for
// depth dimming and for the halo/twinkle brightness.
static uint16_t dim_color565(uint16_t c, float factor) {
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * factor);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3F) * factor);
    uint8_t b = (uint8_t)((c & 0x1F) * factor);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void gen_shape(AnimShape shape, float R) {
    // Warm gold / champagne / platinum / a single icy accent - a
    // deliberately narrow, cohesive palette rather than the app's
    // usual bright multi-hue UI accents, for a more premium feel.
    static const uint16_t palette[4] = {
        (uint16_t)COLOR565(255, 223, 150), // warm gold
        (uint16_t)COLOR565(255, 240, 210), // champagne
        (uint16_t)COLOR565(235, 240, 250), // platinum / soft white
        (uint16_t)COLOR565(190, 215, 255), // icy accent, used sparingly
    };
    static const uint8_t weights[4] = { 40, 30, 25, 5 }; // percent, sums to 100

    const float golden_angle = 3.14159265f * (1.0f + sqrtf(5.0f));

    for (int i = 0; i < ANIM_PARTICLES; i++) {
        float x, y, z;
        switch (shape) {
        case SHAPE_TORUS: {
            // Even spacing around the main ring (golden-angle stepped,
            // same trick as the sphere below), random around the tube
            // for a bit of natural thickness variation.
            float theta = fmodf((float)i * golden_angle, 2.0f * (float)M_PI);
            float phi   = (float)random(6283) / 1000.0f;
            float Rb = R * 0.7f, Rt = R * 0.30f;
            x = (Rb + Rt * cosf(phi)) * cosf(theta);
            y = (Rb + Rt * cosf(phi)) * sinf(theta);
            z = Rt * sinf(phi);
            break;
        }
        case SHAPE_CUBE: {
            int axis = random(3);
            float a = R * (random(2) ? 1.0f : -1.0f);
            float b = ((float)random(2001) / 1000.0f - 1.0f) * R;
            float c = ((float)random(2001) / 1000.0f - 1.0f) * R;
            if (axis == 0)      { x = a; y = b; z = c; }
            else if (axis == 1) { x = b; y = a; z = c; }
            else                { x = b; y = c; z = a; }
            break;
        }
        case SHAPE_SPHERE:
        default: {
            // Fibonacci sphere: evenly distributes N points with no
            // pole-clumping, unlike naive random theta/phi - reads as
            // "precisely arranged" rather than "randomly scattered",
            // which is the whole difference between fine and cheap.
            float idx = (float)i + 0.5f;
            float phi = acosf(1.0f - 2.0f * idx / (float)ANIM_PARTICLES);
            float theta = golden_angle * idx;
            x = R * sinf(phi) * cosf(theta);
            y = R * sinf(phi) * sinf(theta);
            z = R * cosf(phi);
            break;
        }
        }
        s_particles[i].x = x;
        s_particles[i].y = y;
        s_particles[i].z = z;

        int r = random(100);
        uint16_t c = palette[0];
        int acc = 0;
        for (int p = 0; p < 4; p++) {
            acc += weights[p];
            if (r < acc) { c = palette[p]; break; }
        }
        s_particles[i].color = c;
        // Mostly fine dust, occasional slightly larger accent point.
        s_particles[i].size_mul = (random(100) < 82) ? (0.55f + (float)random(30) / 100.0f)
                                                       : (1.1f + (float)random(40) / 100.0f);
        s_particles[i].twinkle_phase = (float)random(628) / 100.0f;
    }
}

static void anim_create() {
    gen_shape((AnimShape)random(3), ANIM_RADIUS);
    s_start_ms = millis();
}

static void anim_draw() {
    Arduino_GFX *g = ui_gfx();
    if (!g) return;

    uint32_t elapsed = millis() - s_start_ms;
    float t = (float)elapsed / (float)ANIM_DURATION_MS;
    if (t > 1.0f) t = 1.0f;

    // Smoothstep easing - starts and settles gently instead of the
    // mechanical, constant-velocity feel of raw linear motion.
    float eased = t * t * (3.0f - 2.0f * t);

    // Waking: particles grow outward from a point (progress 0 -> 1).
    // Sleeping: particles shrink inward to a point (progress 1 -> 0).
    float progress = s_waking ? eased : (1.0f - eased);

    // Primary spin plus a gentle secondary wobble (peaks mid-animation)
    // for a tumbling-jewel feel rather than a flat, single-axis spin.
    float rot_y = t * 3.2f;
    float cyaw = cosf(rot_y), syaw = sinf(rot_y);
    float rot_x = sinf(t * (float)M_PI) * 0.30f;
    float cpitch = cosf(rot_x), spitch = sinf(rot_x);

    for (int i = 0; i < ANIM_PARTICLES; i++) {
        float x = s_particles[i].x * progress;
        float y = s_particles[i].y * progress;
        float z = s_particles[i].z * progress;

        // Rotate around the vertical axis first...
        float rx = x * cyaw - z * syaw;
        float rz1 = x * syaw + z * cyaw;
        // ...then a slight tilt around the horizontal axis.
        float ry = y * cpitch - rz1 * spitch;
        float rz = y * spitch + rz1 * cpitch;

        // Weak perspective: closer particles (rz<0, toward camera)
        // render a bit larger, farther ones a bit smaller.
        float scale = 220.0f / (220.0f + rz);
        if (scale < 0.15f) scale = 0.15f;

        int sx = LCD_WIDTH / 2 + (int)(rx * scale);
        int sy = LCD_HEIGHT / 2 + (int)(ry * scale * 0.95f);

        // Depth dimming: farther particles (small scale) read as
        // dimmer/cooler, not just smaller - sells real depth instead
        // of a flat sprite cloud.
        float depth_dim = 0.45f + 0.55f * (scale > 1.0f ? 1.0f : scale);
        float twinkle = 0.8f + 0.2f * sinf(t * 16.0f + s_particles[i].twinkle_phase);
        float brightness = depth_dim * twinkle;

        int core_r = (int)(1.6f * s_particles[i].size_mul * scale);
        if (core_r < 1) core_r = 1;
        int halo_r = core_r * 2 + 1;

        uint16_t halo_c = dim_color565(s_particles[i].color, 0.22f * brightness);
        uint16_t core_c = dim_color565(s_particles[i].color, brightness);

        g->fillCircle(sx, sy, halo_r, halo_c);
        g->fillCircle(sx, sy, core_r, core_c);
    }

    // Soft pulsing core glow at the center, in the same palette rather
    // than a flat UI-gray ring.
    if (progress > 0.04f) {
        float pulse = 0.7f + 0.3f * sinf(t * 10.0f);
        int glow_r = (int)(18 * progress);
        uint16_t glow_c = dim_color565((uint16_t)COLOR565(255, 235, 205), 0.5f * pulse);
        g->drawCircle(LCD_WIDTH / 2, LCD_HEIGHT / 2, glow_r, glow_c);
    }
}

static void anim_touch(int, int, bool) {}

static void anim_tick() {
    if (millis() - s_start_ms >= ANIM_DURATION_MS) {
        if (!s_waking && s_sleep_after) {
            // Let the "lock" finish visually before the display
            // actually goes to sleep, rather than sleeping first and
            // hiding the tail end of the animation.
            sleep_force_sleep();
        }
        ui_pop_screen();
    }
}

Screen lock_anim_screen = {
    "", GESTURE_MODE_FREE,
    16, // ~60fps cap - smooth without hogging the render loop
    anim_create, anim_draw, anim_touch, anim_tick, nullptr, nullptr,
    0, false, false,
    true, // no_transition - this IS the transition (boot/lock/unlock)
};
