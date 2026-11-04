// Feedback patterns - see haptics.h.
#include "haptics.h"
#include "config.h"
#include "app_settings_state.h"
#include "dnd.h"
#include "hal_audio.h"
#include "hal_vibrate.h"
#include <Arduino.h>

struct Step { uint16_t freq, on_ms, off_ms; };
struct Pattern { const Step *steps; uint8_t n; };

static const Step NOTIFY[] = {{1319, 35, 70}, {1319, 35, 0}};
static const Step SUCCESS[] = {{1047, 55, 15}, {1319, 55, 15}, {1568, 90, 0}};
static const Step FAILURE[] = {{784, 90, 50}, {523, 150, 0}};
static const Step START[] = {{880, 60, 40}, {1175, 120, 0}};
static const Step STOP[] = {{1175, 60, 40}, {880, 120, 0}};
static const Step LEFT[] = {{784, 30, 50}, {784, 30, 170}, {784, 30, 50}, {784, 30, 170}, {784, 30, 50}, {784, 30, 0}};
static const Step RIGHT[] = {{1175, 30, 90}, {1175, 30, 90}, {1175, 30, 90}, {1175, 30, 0}};
static const Step ARRIVED[] = {{1047, 70, 10}, {1319, 70, 10}, {1568, 70, 10}, {2093, 160, 0}};
static const Step REMIND[] = {{1568, 45, 80}, {1175, 90, 0}};
static const Step CLICK[] = {{2000, 8, 0}};
static const Step URGENT[] = {{1760, 180, 90}, {1760, 180, 90}, {1760, 180, 0}};
#define P(a) {a, (uint8_t)(sizeof(a) / sizeof(a[0]))}
static const Pattern PATTERNS[HAP_COUNT] = {
    P(NOTIFY), P(SUCCESS), P(FAILURE), P(START), P(STOP), P(LEFT), P(RIGHT), P(ARRIVED), P(REMIND), P(CLICK), P(URGENT),
};

static const Step *s_steps = nullptr;
static uint8_t  s_n = 0, s_i = 0;
static uint32_t s_next_ms = 0;
static bool     s_sound = true;

void haptic(HapticKind k) {
    if (k >= HAP_COUNT) return;
    bool urgent = k == HAP_URGENT;
    if (!urgent && dnd_active()) return;
    s_sound = g_app_settings.sound_cues && g_app_settings.sfx_enabled;
    s_steps = PATTERNS[k].steps;
    s_n = PATTERNS[k].n;
    s_i = 0;
    s_next_ms = millis();
    haptics_update();
}

void haptics_update() {
    if (!s_steps || s_i >= s_n) return;
    uint32_t now = millis();
    if ((int32_t)(now - s_next_ms) < 0) return;
    const Step &st = s_steps[s_i++];
    if (s_sound) audio_play_sfx(st.freq, st.on_ms);
    vibrate_ms(st.on_ms);   // a no-op without a motor
    s_next_ms = now + st.on_ms + st.off_ms;
    if (s_i >= s_n) s_steps = nullptr;
}
