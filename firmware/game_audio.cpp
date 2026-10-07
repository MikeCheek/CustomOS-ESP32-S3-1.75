#include "game_audio.h"
#include "hal_audio.h"
#include "app_settings_state.h"
#include <Arduino.h>
#include <string.h>

// ---- Named SFX ----------------------------------------------------------
// Short, cheap, distinguishable procedural blips - tuned by ear against
// no hardware in front of me, so these are a reasonable starting point
// rather than verified-final. If any land wrong once you hear them on
// the device, tell me which and I'll adjust the freq/duration pairs.

void game_sfx_shoot()     { if (g_app_settings.sfx_enabled) audio_play_sfx(1400, 40); }
void game_sfx_hit()       { if (g_app_settings.sfx_enabled) audio_play_sfx(220, 90); }
void game_sfx_explosion() { if (g_app_settings.sfx_enabled) audio_play_sfx(140, 130); }
void game_sfx_score()     { if (g_app_settings.sfx_enabled) audio_play_sfx(1800, 60); }
void game_sfx_gameover()  { if (g_app_settings.sfx_enabled) audio_play_sfx(180, 260); }

// Two-tone ascending chirp for a level clear - a single audio_play_sfx()
// call can't easily do "two notes", so this queues two short beeps back
// to back via the same non-blocking SFX task.
void game_sfx_levelup() {
    if (!g_app_settings.sfx_enabled) return;
    audio_play_sfx(1200, 70);
    audio_play_sfx(1800, 90);
}
void game_sfx_powerup()   { if (g_app_settings.sfx_enabled) audio_play_sfx(2400, 50); }

// A distinct musical note per Simon quadrant (0-3) - a simple ascending
// major arpeggio (C4/E4/G4/C5), the classic "each button has its own
// tone" pattern this kind of game is built around. idx is clamped to
// the valid range rather than trusting every caller to range-check
// first.
void game_sfx_simon_note(int idx) {
    if (!g_app_settings.sfx_enabled) return;
    static const uint16_t notes[4] = { 262, 330, 392, 523 };
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    audio_play_sfx(notes[idx], 220);
}

// ---- Background music ---------------------------------------------------

static char s_music_path[64] = "";
static bool s_music_wanted = false; // a game screen wants music playing,
                                     // independent of the current toggle

static bool play_music_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".wav") == 0) return audio_play_wav(path);
    return audio_play_mp3(path);
}

void game_music_start(const char *path) {
    strncpy(s_music_path, path, sizeof(s_music_path) - 1);
    s_music_path[sizeof(s_music_path) - 1] = '\0';
    s_music_wanted = true;
    if (g_app_settings.music_enabled) play_music_file(s_music_path);
}

void game_music_poll() {
    if (!s_music_wanted) return;

    if (!g_app_settings.music_enabled) {
        if (audio_is_playing()) audio_stop_playback();
        return;
    }

    if (!audio_is_playing()) {
        // Loop the file, and (harmlessly) retry a missing file every
        // 500ms rather than hammering SD.open() every single frame.
        static uint32_t s_last_retry_ms = 0;
        uint32_t now = millis();
        if (now - s_last_retry_ms < 500) return;
        s_last_retry_ms = now;
        play_music_file(s_music_path);
    }
}

void game_music_stop() {
    s_music_wanted = false;
    if (audio_is_playing()) audio_stop_playback();
}
