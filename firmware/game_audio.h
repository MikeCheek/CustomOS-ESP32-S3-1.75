/*
 * game_audio.h
 * Shared sound-effect + background-music helpers for games, so each
 * game file doesn't duplicate raw frequencies or check
 * g_app_settings.sfx_enabled/music_enabled itself.
 */
#pragma once
#include <stdint.h>

// Named short sound effects. Each is a no-op if the player has SFX
// turned off (Settings screen or the quick top panel) - games can call
// these unconditionally. Non-blocking (see audio_play_sfx()) - safe to
// call from on_tick/on_touch every frame without stalling the game.
void game_sfx_shoot();
void game_sfx_hit();
void game_sfx_explosion();
void game_sfx_score();
void game_sfx_gameover();
void game_sfx_levelup();
void game_sfx_powerup();
// A distinct tone per Simon-style quadrant/button (0-3) - see
// game_audio.cpp for the actual note mapping.
void game_sfx_simon_note(int idx);

// Looping background music for a game screen.
//
//   game_music_start("/music/plane.mp3")  from on_create
//   game_music_poll()                     from on_tick, every frame
//   game_music_stop()                     from on_destroy
//
// This project doesn't ship any music files - game_music_start() just
// remembers the path and tries to play it; if the file isn't on the SD
// card, playback fails silently (screen stays quiet) rather than
// erroring, and it'll try again shortly in case a card gets inserted.
// No-ops entirely if Music is off, and responds live if the player
// toggles Music on/off mid-game via the quick panel or Settings.
void game_music_start(const char *path);
void game_music_poll();
void game_music_stop();
