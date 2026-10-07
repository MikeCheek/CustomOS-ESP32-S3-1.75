/*
 * hal_audio.h
 * Dual-mic array (ES7210 ADC) capture + speaker output (ES8311 DAC) over
 * a shared I2S bus.
 *
 * Honesty check: driving ES7210/ES8311's exact register sets correctly
 * (mic gain, PDM/TDM mode, DAC volume curve, power sequencing) usually
 * needs the vendor's own driver (Waveshare's "08_ES8311" demo, or
 * Espressif's esp-adf / esp-sr components). What's implemented here is
 * a best-effort default register init plus a raw I2S read/write path -
 * enough to prove audio data is flowing both directions (mic level
 * meter, speaker test tones) but not a production audio pipeline. If
 * levels/volume look wrong, diff this against demo 08_ES8311 from
 * Waveshare's package and adjust es7210_register_init() /
 * es8311_register_init() accordingly.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

bool audio_init();
bool audio_speaker_ok();   // ES8311 configured (speaker path works)
bool audio_mic_ok();       // ES7210 configured (mics work)
// Mic sensitivity 1 (low) .. 5 (max), default 3. Call before audio_init()
// to apply it at boot, or any time after.
void audio_set_mic_sensitivity(uint8_t level);
uint8_t audio_get_mic_sensitivity();

// Volume control (0 = mute, 100 = max). Persists via NVS.
void audio_set_volume(uint8_t pct);
uint8_t audio_get_volume();

// Returns 0-100 mic input level (simple RMS over a short window), for a
// level-meter test screen. Safe to call at UI refresh rate (~20-30 Hz).
int audio_mic_level_percent();

// Blocking beep for `duration_ms` at `freq_hz`. Fine for UI feedback /
// a "does the speaker work" test button; don't call from time-critical
// code since it blocks. Respects the volume setting.
void audio_beep(uint16_t freq_hz, uint16_t duration_ms);

// Non-blocking sound-effect trigger for games: queues a short
// procedural tone to a dedicated background task instead of writing to
// I2S directly on the calling thread, so calling this from a game's
// on_tick/on_touch never stalls the render loop the way audio_beep()
// would (audio_beep blocks for the full duration — fine for a one-off
// "tap to beep" test button, not for a shot fired every frame). If a
// lot of SFX are already queued, a new request is silently dropped
// rather than blocking — acceptable for rapid-fire game feedback.
// See game_audio.h for the named sounds games actually call.
void audio_play_sfx(uint16_t freq_hz, uint16_t duration_ms);

// WAV file playback from SD card.
// Starts playing <filename> (root of SD) through the speaker.
// Non-blocking: call audio_update() from loop() to feed data.
bool audio_play_wav(const char *filename);
// MP3 file playback from SD card (same interface as WAV).
bool audio_play_mp3(const char *filename);
// Call from loop() to feed buffered audio data to I2S. Returns true while playing.
bool audio_update();
// Powers the speaker amp down after a few silent seconds (called by
// audio_update(); call it while the screen is off too).
void audio_idle_power();
// Stop any currently-playing audio.
void audio_stop_playback();
// Returns true if audio is currently playing (also while paused - a
// file is loaded and can resume).
bool audio_is_playing();

// Pause/resume the current WAV/MP3 file without losing its position.
void audio_set_paused(bool paused);
bool audio_is_paused();
// Jump to a position in the current file, 0.0 = start .. 1.0 = end.
void audio_seek(float fraction);

struct AudioStreamInfo {
    bool     is_mp3;
    bool     paused;
    uint32_t sample_rate;   // source file's rate (0 until known)
    uint8_t  channels;
    uint16_t kbps;          // MP3: current frame bitrate; WAV: computed
    uint32_t position_ms;   // elapsed
    uint32_t duration_ms;   // 0 = not known yet (MP3: estimated after the first frames)
    uint8_t  level;         // recent output peak 0-100, for visualizers
};
// False when nothing is loaded.
bool audio_get_stream_info(AudioStreamInfo *out);

// Voice recorder — captures dual-mic audio to a WAV file on SD card.
// Runs in a background FreeRTOS task, non-blocking.
// Only one of record/play can be active at a time (shared I2S bus).
bool audio_start_record(const char *filename);
// Same, mono (both mics averaged) - for voice replies, half the transfer.
bool audio_start_record_mono(const char *filename);
void audio_stop_record();
bool audio_is_recording();
uint32_t audio_record_duration_s();

// Live per-channel waveform snapshot while recording, for a real-time
// scope/debug display (see app_recorder.cpp). Copies up to max_points
// raw signed 16-bit samples per channel into out_l/out_r (caller-
// allocated, each at least max_points long) and returns how many
// points were written — 0 if not currently recording or no data has
// arrived yet. Channel mapping: out_l is mic1, out_r is mic2, per the
// ES7210's non-TDM SDOUT1 routing (see hal_audio.cpp's ES7210 init).
// Safe to call at UI refresh rate.
int audio_get_mic_waveform(int16_t *out_l, int16_t *out_r, int max_points);

// ---- Raw PCM playback (for callers with their own decoded/uncoded
// samples, e.g. app_media.cpp's video player feeding an AVI's PCM
// audio track — no MP3/WAV file machinery needed since there's
// nothing to decode). 16-bit only.
//
//   audio_pcm_configure(rate, channels)   once, before playing
//   audio_pcm_write(samples, count)       per chunk, as data is ready
//   audio_pcm_restore_default()           once, when done
bool audio_pcm_configure(uint32_t sample_rate, uint8_t channels);
void audio_pcm_write(const int16_t *samples, size_t sample_count);
void audio_pcm_restore_default();
