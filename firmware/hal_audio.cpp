#include "hal_audio.h"
#include "board_pins.h"
#include "config.h"
#include <Arduino.h>

#if FEATURE_AUDIO

#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "hal_expander.h"
#include "hal_nvs.h"
#include "app_settings_state.h"

#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

// ---- I2S port/rate (used throughout this file — defined early since
// the guarded-write helper below needs I2S_PORT) --------------------
#define I2S_PORT   I2S_NUM_0
#define SAMPLE_RATE 16000

static bool s_ok = false;
static uint8_t s_volume = 80; // 0-100
static volatile bool s_mp3_playing = false;
// True from just before the MP3 decoder task is created until its last
// line - set by the creator, not the task, so a task that ends before
// xTaskCreate...() even returns (tiny/corrupt file) can't leave it stale.
static volatile bool s_mp3_task_alive = false;
static TaskHandle_t s_mp3_task_handle = NULL; // for "am I the decoder task?" checks
static volatile bool s_paused = false;        // WAV/MP3 file playback paused (see audio_set_paused())
static volatile uint8_t s_out_level = 0;      // recent output peak 0-100 (audio_get_stream_info())
static volatile bool s_recording = false;
static volatile bool s_wav_playing = false;

// I2S is a single shared peripheral, but now up to three independent
// contexts can want to write to it (the main loop feeding WAV playback,
// the mp3_task decoding background music, and the new sfx_task below
// for game sound effects) — this mutex serializes those writes so two
// tasks never interleave into the same DMA buffer. It does NOT mix
// audio (that would need real sample-level summing); a queued SFX
// simply takes its brief turn — typically 15-40ms — while whatever
// else is writing waits, then resumes immediately after.
static SemaphoreHandle_t s_i2s_mutex = nullptr;

// Speaker amp power: the class-D amp draws a few mA even when silent, so
// its GPIO enable is dropped after a few idle seconds (audio_idle_power())
// and raised again by the next write. Only the GPIO is toggled here - any
// task may write audio, and the IO-expander line needs the I2C bus.
#define AUDIO_PA_ACTIVE_HIGH 1
static volatile uint32_t s_last_out_ms = 0;
static volatile bool s_pa_on = true;

// With power management (PlatformIO build, CONFIG_PM_ENABLE) a running
// I2S port holds a PM lock that keeps the chip out of light sleep, and its
// MCLK keeps both codecs clocked. So it stops after the same 4 s of quiet
// as the speaker amp, and starts again before the next sound or recording.
// The prebuilt Arduino core can't light-sleep anyway: there it keeps running.
static volatile bool s_i2s_running = true;

static inline void i2s_ensure_running() {
#if CONFIG_PM_ENABLE
    if (!s_i2s_running) {
        i2s_start(I2S_PORT);
        s_i2s_running = true;
    }
#endif
}

static inline void pa_touch() {
    i2s_ensure_running();
    s_last_out_ms = millis();
    if (!s_pa_on) {
        s_pa_on = true;
        digitalWrite(PIN_AUDIO_PA, AUDIO_PA_ACTIVE_HIGH ? HIGH : LOW);
        delay(3);   // amp start-up, so the first milliseconds of a click aren't lost
    }
}

static esp_err_t i2s_write_guarded(const void *data, size_t size, size_t *bytes_written, TickType_t timeout) {
    if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, timeout) != pdTRUE) {
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_TIMEOUT;
    }
    pa_touch();   // under the mutex: audio_idle_power() can't switch it off mid-write
    esp_err_t r = i2s_write(I2S_PORT, data, size, bytes_written, timeout);
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    return r;
}

// ---- Non-blocking SFX queue (see hal_audio.h) --------------------------
// Declared up here (ahead of audio_init(), which creates the queue/task)
// — the task body and audio_play_sfx() are defined further down, once
// play_tone_blocking() (the tone synthesis they share with audio_beep)
// is available.
struct SfxRequest { uint16_t freq_hz; uint16_t duration_ms; };
static QueueHandle_t s_sfx_queue = nullptr;
static TaskHandle_t  s_sfx_task_handle = nullptr;
static void sfx_task(void *param);

// ---- ES7210 mic ADC register init ---------------------------------------
static bool es7210_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES7210_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es7210_read(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(ES7210_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false; // repeated start, keep bus held
    if (Wire.requestFrom((int)ES7210_I2C_ADDR, 1) != 1) return false;
    *val = Wire.read();
    return true;
}

// Mic sensitivity (Settings > Microphone), level 1..5:
//   analog PGA gain of the ES7210 (reg 0x43/0x44 low nibble: 0..10 = 0..30 dB
//   in 3 dB steps, then 11=33, 12=34.5, 13=36, 14=37.5 dB) plus, at the top
//   levels, a digital gain with a soft limiter in rec_task.
// The old fixed 24 dB made recordings quiet when played back.
static const uint8_t MIC_PGA[5]   = { 8, 10, 12, 14, 14 };        // 24, 30, 34.5, 37.5, 37.5 dB
static const uint16_t MIC_DIG_Q8[5] = { 256, 256, 256, 410, 640 }; // x1, x1, x1, x1.6 (+4 dB), x2.5 (+8 dB)
static volatile uint8_t s_mic_level = 3;   // 1..5

static inline int16_t mic_gain_sample(int32_t v, uint16_t q8) {
    v = (v * q8) >> 8;
    // Soft knee above ~-3 dBFS instead of hard clipping.
    const int32_t knee = 23000;
    if (v > knee) v = knee + (v - knee) / 4;
    else if (v < -knee) v = -knee + (v + knee) / 4;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void mic_apply_gain(int16_t *buf, int count) {
    uint16_t q8 = MIC_DIG_Q8[s_mic_level - 1];
    if (q8 == 256) return;
    for (int i = 0; i < count; i++) buf[i] = mic_gain_sample(buf[i], q8);
}

static bool es7210_set_pga(uint8_t regv) {
    bool ok = true;
    const uint8_t gain_regs[2] = { 0x43, 0x44 };
    for (int m = 0; m < 2; m++) {
        uint8_t r;
        if (es7210_read(gain_regs[m], &r)) {
            r = (uint8_t)((r & ~0x0F) | 0x10 | (regv & 0x0F));   // 0x10 = PGA enable
            ok &= es7210_write(gain_regs[m], r);
        } else ok = false;
    }
    return ok;
}

static bool es7210_register_init() {
    // Rewritten against ESPHome's es7210 driver (api-docs.esphome.io/
    // es7210_8cpp_source — a mature, verified-working driver), after
    // finding this project's previous register map didn't match the
    // real chip at all: e.g. treating regs 0x41/0x42 as "PGA gain/input
    // select" when they're actually MIC12_BIAS/MIC34_BIAS, and 0x4C as
    // a "TDM slot" register when it's really MIC34_POWER. The single
    // most likely cause of empty recordings: the old code issued only
    // ONE reset write (0x00=0xFF) and never the two "bring the chip out
    // of reset and enable it" writes the real sequence ends on (0x00=
    // 0x71 then 0x00=0x41) — so the ADC path was very plausibly never
    // actually enabled, regardless of every other register.
    //
    // Also newly set: reg 0x12, which selects mic 1&2 onto SDOUT1 in
    // non-TDM mode (0x00) — matching the plain 2-channel I2S RX this
    // project reads, versus the 4-slot TDM the official BSP uses. This
    // register was never written before at all.
    //
    // Clock-divider coefficients (regs 0x02/0x04/0x05/0x07) are now
    // verified against the reference driver's own coefficient table
    // for this project's exact MCLK/sample-rate pair (4.096MHz /
    // 16kHz) — see the inline comment at those writes below. REG02 was
    // the one actually wrong; the previous "not independently verified"
    // gap here is now closed.
    bool ok = true;

    // Reset
    ok &= es7210_write(0x00, 0xFF);
    ok &= es7210_write(0x00, 0x32);
    ok &= es7210_write(0x01, 0x3F); // clocks off during config

    // Power-up timing
    ok &= es7210_write(0x09, 0x30);
    ok &= es7210_write(0x0A, 0x30);

    // ADC high-pass filters (both channel pairs)
    ok &= es7210_write(0x23, 0x2A);
    ok &= es7210_write(0x22, 0x0A);
    ok &= es7210_write(0x20, 0x0A);
    ok &= es7210_write(0x21, 0x2A);

    // Secondary (slave) I2S mode - clear bit0 of the mode-config register
    {
        uint8_t regv = 0;
        if (es7210_read(0x08, &regv)) {
            regv &= ~0x01;
            ok &= es7210_write(0x08, regv);
        } else {
            ok = false;
        }
    }

    // Clock dividers for MCLK=4.096MHz / 16kHz sample rate — this
    // project's I2S config (use_apll=true, fixed_mclk=0) auto-derives
    // MCLK as 256x the sample rate, i.e. exactly 4.096MHz at 16kHz.
    // Values below are taken directly from the reference driver's own
    // coefficient table for that exact (mclk, rate) pair: adc_div=0x01,
    // dll=0x01, doubler=0x01, osr=0x20, lrck_h=0x01, lrck_l=0x00.
    // REG02 combines adc_div | (doubler<<6) | (dll<<7) = 0xC1 - this
    // was the one still wrong (previously written as 0x00, silently
    // misconfiguring the ADC's actual sampling clock while every other
    // register reported "success" over I2C, which is a strong
    // candidate for captured audio being near-silent/garbage despite
    // the recording pipeline otherwise working correctly end to end).
    // The other three happened to already be correct.
    ok &= es7210_write(0x02, 0xC1);
    ok &= es7210_write(0x07, 0x20);
    ok &= es7210_write(0x04, 0x01);
    ok &= es7210_write(0x05, 0x00);

    // Analog power
    ok &= es7210_write(0x40, 0xC3);

    // Mic bias (both pairs)
    ok &= es7210_write(0x41, 0x70);
    ok &= es7210_write(0x42, 0x70);

    // I2S format: 16-bit, mics 1&2 on SDOUT1 (non-TDM) - matches the
    // plain 2-channel stream rec_task()/i2s_read() actually reads.
    ok &= es7210_write(0x11, 0x60);
    ok &= es7210_write(0x12, 0x00);

    // Mic PGA gain (24dB default, matching the reference driver's own
    // default). This was missing entirely before: each mic's gain
    // register has a separate enable bit (0x10) on top of the gain
    // value itself (low nibble) - without setting that enable bit, the
    // PGA stage plausibly never actually turns on even though the rest
    // of the ADC path reports "enabled", which would explain captured
    // audio being at or near zero amplitude regardless of every other
    // register being correct. Mirrors the reference driver's exact
    // per-mic dance (toggle the relevant power register around the
    // gain write) for mic 1 and mic 2.
    {
        const uint8_t GAIN_REGV = MIC_PGA[s_mic_level - 1]; // see MIC_PGA (was a fixed 8 = 24 dB)
        const uint8_t gain_regs[2] = { 0x43, 0x44 }; // MIC1_GAIN, MIC2_GAIN
        for (int m = 0; m < 2; m++) {
            uint8_t r;
            if (es7210_read(0x01, &r)) { r &= ~0x0B; ok &= es7210_write(0x01, r); } else ok = false;
            ok &= es7210_write(0x4B, 0x00);
            if (es7210_read(gain_regs[m], &r)) { r |= 0x10; ok &= es7210_write(gain_regs[m], r); } else ok = false;
            if (es7210_read(gain_regs[m], &r)) {
                r = (uint8_t)((r & ~0x0F) | (GAIN_REGV & 0x0F));
                ok &= es7210_write(gain_regs[m], r);
            } else ok = false;
        }
    }

    // Power on mic 1 & 2 individually
    ok &= es7210_write(0x47, 0x08);
    ok &= es7210_write(0x48, 0x08);

    // Power down DLL
    ok &= es7210_write(0x06, 0x04);

    // Power on MIC1/2 bias & ADC & PGA
    ok &= es7210_write(0x4B, 0x0F);
    ok &= es7210_write(0x4C, 0x0F);

    // Bring the chip out of reset and enable it - this final pair of
    // writes was completely missing before (see header note).
    ok &= es7210_write(0x00, 0x71);
    ok &= es7210_write(0x00, 0x41);

    DEBUG_PRINTF("[audio] ES7210 init %s\n", ok ? "OK" : "PARTIAL");
    return ok;
}

// ---- ES8311 speaker codec register init ---------------------------------
// Cross-checked against ESPHome's es8311 component (a mature, widely-used
// driver — api-docs.esphome.io/es8311_8cpp_source), which fixed two real
// bugs found here:
//   1. (Superseded - see apply_volume(): each register step is 0.5 dB,
//      0xBF is 0 dB and anything above it is positive gain.)
//      REG0x32 (DAC volume) is a LINEAR 0-255 register where 0x00 is
//      near-silent and 0xFF is max gain (ESPHome: remap(volume, 0..1,
//      0..255) — their own comment confirms 0.75 -> 0xBF -> 0dB, i.e.
//      HIGHER register values are LOUDER). The previous code here had
//      this backwards — (100-volume)*0xBF/100 — so turning the in-app
//      volume UP actually wrote a LOWER register value, meaning max
//      volume (100%) wrote 0x00 (near-mute) and mute (0%) wrote 0xBF
//      (~75% of max gain). That inversion alone is a very plausible
//      full explanation for "no sound" if the volume was ever turned up.
//   2. REG0x00 (csm_on / "Power On") needs to be written LAST, after all
//      clock/format/power-management registers are configured — not
//      immediately after reset, which is the order this file used
//      before. ESPHome's verified sequence does every other register
//      first and issues Power On as its final step.
// Still not verified against this exact board's schematic/demo package,
// per the honesty note this file already carried — if sound is still
// wrong after this, diff against Waveshare's own "08_ES8311" demo next.
static bool es8311_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool es8311_read(uint8_t reg, uint8_t *val) {
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false; // repeated start, keep bus held
    if (Wire.requestFrom((int)ES8311_I2C_ADDR, 1) != 1) return false;
    *val = Wire.read();
    return true;
}

// ES8311 register init — MUST be called AFTER I2S is running (needs MCLK)
static bool es8311_register_init() {
    bool ok = true;
    // Reset sequence
    ok &= es8311_write(0x00, 0x1F);
    delay(5);
    ok &= es8311_write(0x00, 0x00);
    delay(10);

    // Clock config
    // Clock config for MCLK=4.096MHz / 16kHz sample rate — this
    // project's I2S config (use_apll=true, fixed_mclk=0) auto-derives
    // MCLK as 256x the sample rate, i.e. exactly 4.096MHz at 16kHz.
    // Values below are taken directly from the reference driver's own
    // coefficient table for that exact (mclk, rate) pair: pre_div=1,
    // pre_mult=1, adc_div=1, dac_div=1, fs_mode=0, lrck_h=0,
    // lrck_l=0xFF, bclk_div=4, adc_osr=0x10, dac_osr=0x20.
    //
    // This whole block used to be just two writes (REG01, and a wrong
    // REG02). Registers 03/04/05/06/07/08 — which set the actual
    // ADC/DAC oversampling ratio and the BCLK/LRCK dividers — were
    // never written at all, left sitting at power-on-reset defaults
    // that don't match a 16kHz target. That alone is a complete,
    // sufficient explanation for audio playing back faster (or
    // otherwise mistimed) than it should, independent of anything
    // upstream (I2S config, decoder, etc.) being correct.
    ok &= es8311_write(0x01, 0x3F); // enable all clocks

    {
        uint8_t r;
        if (es8311_read(0x02, &r)) { // pre-divider/pre-multiplier, preserve low 3 bits
            r &= 0x07;
            r |= (uint8_t)((1 - 1) << 5); // pre_div=1
            r |= (uint8_t)(1 << 3);       // pre_mult=1
            ok &= es8311_write(0x02, r);
        } else ok = false;
    }
    ok &= es8311_write(0x03, (uint8_t)((0 << 6) | 0x10)); // fs_mode=0, adc_osr=0x10
    ok &= es8311_write(0x04, 0x20);                       // dac_osr=0x20
    ok &= es8311_write(0x05, (uint8_t)(((1 - 1) << 4) | (1 - 1))); // adc_div=1, dac_div=1
    {
        uint8_t r;
        if (es8311_read(0x06, &r)) { // bclk divider, preserve upper 3 bits (sclk invert etc.)
            r &= 0xE0;
            r |= (uint8_t)(4 - 1); // bclk_div=4 (< 19, so bclk_div-1)
            ok &= es8311_write(0x06, r);
        } else ok = false;
    }
    {
        uint8_t r;
        if (es8311_read(0x07, &r)) { // lrck_h in the low bits, preserve upper 2 (tri-state) bits
            r &= 0xC0;
            r |= 0x00; // lrck_h=0
            ok &= es8311_write(0x07, r);
        } else ok = false;
    }
    ok &= es8311_write(0x08, 0xFF); // lrck_l=0xFF

    // Serial digital port: 16-bit word, I2S standard format
    ok &= es8311_write(0x09, 0x06); // 16-bit word length, I2S format
    ok &= es8311_write(0x0A, 0x00); // I2S standard

    // Gain and volume (before power-up, matching the verified reference order)
    ok &= es8311_write(0x14, 0x1F); // PGA gain max
    ok &= es8311_write(0x16, 0x24); // DAC: enable, normal (not muted)
    ok &= es8311_write(0x31, 0x00); // unmute DAC
    ok &= es8311_write(0x32, 0xBF); // 0 dB as a sane init default;
                                     // apply_volume() overwrites this right
                                     // after with the real user setting

    // Power management
    ok &= es8311_write(0x0D, 0x01); // power up analog circuitry
    ok &= es8311_write(0x0E, 0x02); // Vref
    ok &= es8311_write(0x12, 0x00); // DAC block powered on
    ok &= es8311_write(0x13, 0x10); // enable output
    ok &= es8311_write(0x37, 0x08); // bypass DAC equalizer

    // Power On — LAST, after everything above is configured
    ok &= es8311_write(0x00, 0x80);
    delay(50);

    DEBUG_PRINTF("[audio] ES8311 init %s\n", ok ? "OK" : "FAILED");
    return ok;
}

// Volume 0-100 -> ES8311 DAC volume, REG0x32.
//
// REG0x32 is in 0.5 dB steps: 0x00 = -95.5 dB, 0xBF = 0 dB (unity),
// 0xFF = +32 dB (Espressif esp_codec_dev es8311.c: vol_range -95.5..+32).
// The old mapping wrote pct*255/100 straight into it, i.e. it spread
// 0-100% evenly over that whole -95.5..+32 dB range. That put 50% at
// about -32 dB (inaudible on this speaker - and the old code also scaled
// every sample by the volume in software on top, another -6 dB at 50%)
// and 100% at +32 dB of digital gain, which just clips and overdrives the
// small speaker.
//
// Now: 1..100% maps linearly in dB (how loudness is perceived), 0% is
// mute. The codec handles VOLUME_MIN_DB..0 dB (reached at ~76%). Above that
// the codec stays at unity and the remaining VOLUME_MAX_DB of gain is
// applied in software by boost_pcm() with a soft limiter: positive digital
// gain in the codec would just hard-clip loud music (harsh distortion),
// while the soft knee keeps peaks round and raises the average loudness,
// which is what makes the small speaker sound louder.
#define VOLUME_MIN_DB        (-32.0f) // at 1%
#define VOLUME_MAX_DB        (10.0f)  // at 100% (codec 0 dB + 10 dB boost)
static volatile int32_t s_boost_q12 = 4096; // software gain, Q12 (4096 = unity)

static float volume_pct_to_db(int pct) {
    return VOLUME_MIN_DB + (VOLUME_MAX_DB - VOLUME_MIN_DB) * (float)(pct - 1) / 99.0f;
}

static void apply_volume() {
    if (!s_ok) return;
    uint8_t reg = 0x00; // 0% -> -95.5 dB, effectively muted
    float boost_db = 0.0f;
    if (s_volume > 0) {
        float db = volume_pct_to_db(s_volume);
        if (db > 0.0f) { boost_db = db; db = 0.0f; }
        int r = (int)lroundf((db + 95.5f) * 2.0f);
        if (r < 0) r = 0;
        if (r > 0xBF) r = 0xBF; // never positive gain in the codec
        reg = (uint8_t)r;
    }
    s_boost_q12 = (int32_t)lroundf(4096.0f * powf(10.0f, boost_db / 20.0f));
    es8311_write(0x32, reg);
}

// Gain + soft knee: linear up to BOOST_KNEE, then x/(1+x) compression that
// approaches full scale smoothly instead of clipping.
#define BOOST_KNEE 22000
static inline int16_t boost_sample(int32_t v, int32_t g) {
    int32_t x = (v * g) >> 12;
    int32_t a = x < 0 ? -x : x;
    if (a > BOOST_KNEE) {
        const int32_t room = 32767 - BOOST_KNEE;
        int32_t u = a - BOOST_KNEE;                 // excess over the knee
        a = BOOST_KNEE + (int32_t)((int64_t)room * u / (room + u));
        x = x < 0 ? -a : a;
    }
    return (int16_t)x;
}
static void boost_pcm(int16_t *dst, const int16_t *src, int n_samples, int32_t g) {
    for (int i = 0; i < n_samples; i++) dst[i] = boost_sample(src[i], g);
}

// ---- Codec bring-up with retries ----------------------------------------
// Both codecs sit on the shared I2C bus (ES8311 0x18, ES7210 0x40). If one
// doesn't answer at boot (rail still ramping, bus glitch), the whole audio
// path used to stay dead until the next reboot: silent speaker, blank
// recordings. Now each codec is retried a few times at boot, and again
// (throttled) when something wants to play or record.
static bool s_spk_ok = false, s_mic_ok = false;
static uint32_t s_codec_retry_ms = 0;

static bool i2c_present(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static void i2c_scan_log() {
    char buf[160];
    int n = 0;
    buf[0] = 0;
    for (uint8_t a = 0x08; a < 0x78 && n < (int)sizeof(buf) - 6; a++) {
        if (i2c_present(a)) n += snprintf(buf + n, sizeof(buf) - n, " %02X", a);
    }
    DEBUG_PRINTF("[audio] I2C devices answering:%s\n", buf[0] ? buf : " none");
}

static void codecs_init() {
    for (int attempt = 0; attempt < 4 && !(s_spk_ok && s_mic_ok); attempt++) {
        if (attempt) delay(80);
        if (!s_spk_ok && i2c_present(ES8311_I2C_ADDR)) {
            s_spk_ok = es8311_register_init();
            delay(20);
        }
        if (!s_mic_ok && i2c_present(ES7210_I2C_ADDR)) s_mic_ok = es7210_register_init();
    }
    if (!s_spk_ok) DEBUG_PRINTF("[audio] ES8311 (speaker) not answering at 0x%02X\n", ES8311_I2C_ADDR);
    if (!s_mic_ok) DEBUG_PRINTF("[audio] ES7210 (mics) not answering at 0x%02X\n", ES7210_I2C_ADDR);
    if (!s_spk_ok || !s_mic_ok) i2c_scan_log();
    if (s_spk_ok) apply_volume();
}

// Called before playing / recording: one more try if a codec is missing.
static void codecs_retry_if_needed() {
    if (!s_ok || (s_spk_ok && s_mic_ok)) return;
    if (s_codec_retry_ms && millis() - s_codec_retry_ms < 3000) return;
    s_codec_retry_ms = millis();
    // Codec writes while the PCM stream runs would be glitchy but harmless;
    // take the I2S lock so nothing is mid-write.
    bool locked = s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(200)) == pdTRUE;
    codecs_init();
    if (locked) xSemaphoreGive(s_i2s_mutex);
    DEBUG_PRINTF("[audio] codec retry: speaker=%d mic=%d\n", s_spk_ok, s_mic_ok);
}

bool audio_speaker_ok() { return s_spk_ok; }

void audio_set_mic_sensitivity(uint8_t level) {
    if (level < 1) level = 1;
    if (level > 5) level = 5;
    bool pga_change = MIC_PGA[level - 1] != MIC_PGA[s_mic_level - 1];
    s_mic_level = level;
    if (pga_change && s_mic_ok) {
        bool ok = es7210_set_pga(MIC_PGA[level - 1]);
        DEBUG_PRINTF("[audio] mic sensitivity %u (PGA reg %u)%s\n", level, MIC_PGA[level - 1], ok ? "" : " - write failed");
    }
}

uint8_t audio_get_mic_sensitivity() { return s_mic_level; }
bool audio_mic_ok() { return s_mic_ok; }

// ---- I2S setup (shared bus for mic in / speaker out) -------------------

bool audio_init() {
    // Step 1: PA enable FIRST — the physical speaker amplifier enable.
    // This drives BOTH an IO-expander pin and a direct GPIO for "PA
    // enable", which is unusual (most boards have one enable line, not
    // two independent ones) and neither was ever verified against this
    // board's actual schematic — unlike most of board_pins.h, which went
    // through an explicit verification pass. If ES8311/ES7210 both
    // report OK over I2C (check the serial log) but there's still no
    // sound, this pin/polarity is the top remaining suspect. To test
    // the opposite polarity, flip AUDIO_PA_ACTIVE_HIGH below and reflash
    // — one line, no need to hunt through the rest of this function.
#define AUDIO_PA_ACTIVE_HIGH 1
    expander_set_pin(EXPANDER_PIN_SPK_EN, AUDIO_PA_ACTIVE_HIGH ? true : false);
    pinMode(PIN_AUDIO_PA, OUTPUT);
    digitalWrite(PIN_AUDIO_PA, AUDIO_PA_ACTIVE_HIGH ? HIGH : LOW);

    // Step 2: I2S driver install — MUST come before codec init
    // The ES8311/ES7210 need MCLK from I2S to configure their clocks.
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        // DMA buffer depth: this used to be 4x256 = 1024 frames total
        // (~64ms at 16kHz, but only ~23ms once MP3 playback reconfigures
        // the clock to 44.1kHz), which caused an audible glitch roughly
        // once a second — see the mp3_task refill-buffer comment below
        // for why. I widened this once already to 8x1024, but that was
        // a real bug: dma_buf_len=1024 frames * 2 channels * 2 bytes =
        // 4096 bytes per DMA descriptor buffer, and the ESP32's DMA
        // descriptor length field is 12 bits — hard-capped at 4095
        // bytes (4092 with alignment). 4096 exceeds that, which makes
        // i2s_driver_install() fail validation and return an error —
        // breaking I2S entirely, both playback AND mic capture at once
        // (exactly the "music stopped, mic waveform went flat,
        // simultaneously" regression this caused). dma_buf_len=512
        // keeps each descriptor at 2048 bytes, safely under the limit,
        // while dma_buf_count=8 still gives 4096 total frames (~93ms
        // at 44.1kHz) — a solid improvement over the original ~23ms
        // without hitting the hardware ceiling again.
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pins = {
        .mck_io_num = PIN_I2S_MCLK,
        .bck_io_num = PIN_I2S_BCLK,
        .ws_io_num = PIN_I2S_WS,
        // Verified against HARDWARE_REFERENCE.md's schematic-checked GPIO
        // table: GPIO8 is ESP32->ES8311 (speaker/playback data, i.e. this
        // chip's TX/data_out), GPIO10 is ES7210->ESP32 (mic/capture data,
        // i.e. RX/data_in). These used to be swapped here — speaker audio
        // was being written out on the mic input pin and vice versa.
        .data_out_num = PIN_I2S_DOUT,
        .data_in_num  = PIN_I2S_DIN
    };

    esp_err_t i2s_install_err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (i2s_install_err != ESP_OK) {
        DEBUG_PRINTF("[audio] i2s_driver_install failed: %s (%d)\n",
                     esp_err_to_name(i2s_install_err), (int)i2s_install_err);
        return false;
    }
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        DEBUG_PRINTF("[audio] i2s_set_pin failed\n");
        return false;
    }
    // Small delay so MCLK stabilizes before talking to codecs
    delay(50);
    DEBUG_PRINTF("[audio] I2S driver installed, MCLK running\n");

    // Step 3: Wire is already initialized on the correct pins by the
    // .ino's setup() (Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL, IIC_CLOCK_HZ),
    // called well before audio_init()). Do NOT call Wire.begin() again
    // here: on the ESP32 Arduino core, a second Wire.begin() call
    // (especially one with no SDA/SCL arguments, as this used to be)
    // can silently rebind the I2C peripheral to its default pins rather
    // than this board's actual PIN_IIC_SDA/PIN_IIC_SCL — which would
    // make every es8311_write()/es7210_write() below go out on the
    // wrong GPIOs and fail silently, explaining total audio silence
    // despite the rest of this function looking correct. Every other
    // HAL file in this project (hal_expander.cpp, hal_touch.cpp) already
    // follows this — they only ever call Wire.beginTransmission(), never
    // Wire.begin() again — hal_audio.cpp was the one exception.

    // Steps 4-5: the two codecs (both need MCLK from the running I2S).
    // Retried, and again later on the first play / record if they didn't
    // answer at boot - see codecs_init().
    s_ok = true;
    codecs_init();
    bool speaker_ok = s_spk_ok, mic_ok = s_mic_ok;

    // Shared I2S write mutex + the persistent SFX task/queue (see
    // audio_play_sfx()). Created once here regardless of feature use,
    // same lifetime as the rest of the audio subsystem.
    if (!s_i2s_mutex) s_i2s_mutex = xSemaphoreCreateMutex();
    if (!s_sfx_queue) s_sfx_queue = xQueueCreate(4, sizeof(SfxRequest));
    if (!s_sfx_task_handle && s_sfx_queue) {
        if (xTaskCreatePinnedToCore(sfx_task, "sfx_task", 4096, NULL, 3, &s_sfx_task_handle, 1) != pdPASS) {
            DEBUG_PRINTF("[audio] sfx_task creation FAILED - game sound effects will be silent\n");
            s_sfx_task_handle = NULL;
        }
    }

    DEBUG_PRINTF("[audio] Audio init done (speaker=%d, mic=%d)\n", speaker_ok, mic_ok);
    DEBUG_PRINTF("[audio] free heap: %u bytes total, %u bytes internal/DMA-capable\n",
                 (unsigned)ESP.getFreeHeap(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void audio_set_volume(uint8_t pct) {
    if (pct > 100) pct = 100;
    s_volume = pct;
    apply_volume();
}

uint8_t audio_get_volume() {
    return s_volume;
}

static volatile int s_rec_mic_level = 0;

// Shared tone synthesis, callable from any task context (goes through
// the guarded write so it's safe even if another task is also writing
// to I2S right now). audio_beep() and the sfx_task below both use this.
static void play_tone_blocking(uint16_t freq_hz, uint16_t duration_ms) {
    if (!s_ok) return;

    const int buf_len = 256;
    int16_t buf[buf_len * 2]; // stereo: L+R pairs
    int samples_total = SAMPLE_RATE * duration_ms / 1000;
    int samples_written = 0;
    double phase = 0;
    double phase_inc = 2.0 * PI * freq_hz / SAMPLE_RATE;
    int16_t amplitude = 8000; // volume is applied in the codec (apply_volume())

    while (samples_written < samples_total) {
        int n = min(buf_len, samples_total - samples_written);
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)(sin(phase) * amplitude);
            buf[i * 2] = s;     // left channel
            buf[i * 2 + 1] = s; // right channel
            phase += phase_inc;
        }
        size_t written = 0;
        i2s_write_guarded(buf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
        samples_written += n;
    }
}

void audio_beep(uint16_t freq_hz, uint16_t duration_ms) {
    play_tone_blocking(freq_hz, duration_ms);
}

// ---- SFX task body (queue/handles declared earlier, near audio_init) --

static void sfx_task(void *param) {
    (void)param;
    SfxRequest req;
    for (;;) {
        if (xQueueReceive(s_sfx_queue, &req, portMAX_DELAY) == pdTRUE) {
            if (s_ok) play_tone_blocking(req.freq_hz, req.duration_ms);
        }
    }
}

void audio_play_sfx(uint16_t freq_hz, uint16_t duration_ms) {
    if (!s_ok || !s_sfx_queue) return;
    SfxRequest req = { freq_hz, duration_ms };
    xQueueSend(s_sfx_queue, &req, 0); // non-blocking — drop if the queue is full
}

int audio_mic_level_percent() {
    if (!s_ok) return 0;
    // During recording, rec_task already reads I2S — use cached level
    // to avoid contending for the I2S driver mutex (portMAX_DELAY)
    // which would deadlock the main loop.
    if (s_recording) return s_rec_mic_level;
    // During playback I2S is used for TX — don't try to read RX
    if (s_wav_playing || s_mp3_playing) return 0;

    const int N = 256;
    int16_t samples[N];
    size_t bytes_read = 0;
    s_last_out_ms = millis();   // keeps I2S running while the meter is open
    i2s_ensure_running();
    i2s_read(I2S_PORT, samples, sizeof(samples), &bytes_read, 20 / portTICK_PERIOD_MS);

    int count = bytes_read / sizeof(int16_t);
    if (count == 0) return 0;
    mic_apply_gain(samples, count);

    double sum_sq = 0;
    for (int i = 0; i < count; i++) sum_sq += (double)samples[i] * samples[i];
    double rms = sqrt(sum_sq / count);

    int pct = (int)(rms / 32768.0 * 100.0 * 8.0);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    return pct;
}

// ---- WAV file playback from SD --------------------------------------------
#include <SD.h>
#include "hal_sd.h"
#include "hal_usb_msc.h"

static File s_wav_file;
static uint16_t s_wav_channels = 0;
static uint32_t s_wav_sample_rate = 0;
static uint16_t s_wav_bits_per_sample = 0;
static uint32_t s_wav_data_offset = 0;
static uint32_t s_wav_data_size = 0;
static uint32_t s_wav_bytes_read = 0;
static int16_t *s_wav_buf = nullptr;
static const int WAV_BUF_SAMPLES = 256;

// Shared by both the WAV and MP3 playback paths below. The ES8311's
// clock registers are only verified correct for one specific (MCLK,
// rate) pair - this project's fixed 16kHz default (see the long
// comment in es8311_register_init()). This used to call i2s_set_clk()
// to retune the ESP32's I2S peripheral (and, via MCLK, the codec) to
// whatever rate each file actually needs - but nothing ever retuned
// the codec's OWN internal clock dividers to match, so any file at a
// different native rate (which is most real music - 44.1kHz, not this
// project's own 16kHz recordings) played back at the wrong speed. The
// exact MCLK the ESP32's APLL would produce for an arbitrary requested
// rate isn't something this project has verified register values for,
// so guessing coefficients for it blind carried real risk of making
// things worse rather than better. Keeping the codec's clock fixed
// permanently and resampling everything else to match it in software
// is the safer fix - a basic linear-interpolation resampler is a well
// understood, easily-verified technique, unlike reverse-engineering
// codec register tables for untested rates.
static int16_t *s_resample_buf = nullptr;
// Sized to comfortably cover both callers that share this buffer: MP3
// frames (MINIMP3_MAX_SAMPLES_PER_FRAME, ~1152 samples) and the video
// player's raw PCM chunks (up to 4096 samples per audio_pcm_write()
// call, see app_media.cpp's s_aud_buf_sz) - with margin left over even
// for the (unusual, but not impossible) case of upsampling a
// below-16kHz audio track, where the output needs MORE pairs than the
// input rather than fewer.
#define RESAMPLE_BUF_SAMPLES 8192

// ---- Streaming band-limited resampler (any rate -> SAMPLE_RATE) ----------
// Replaces the old per-call linear interpolator, which caused most of the
// audible MP3 artifacts:
//  - it restarted from scratch on every call (every ~26ms MP3 frame), so
//    each frame boundary was a waveform discontinuity - a ~38Hz buzz -
//    and the last few input samples of every frame were silently dropped;
//  - it had no low-pass filter, so going 44.1kHz -> 16kHz folded
//    everything between 8 and 22kHz back down into the audible band as
//    harsh, metallic aliasing.
// This one keeps its phase and the last RS_TAPS input samples between
// calls (one continuous stream, no seams), and interpolates with a
// windowed-sinc low-pass (Blackman, cutoff 0.42 x the lower of the two
// rates), precomputed as an RS_PHASES-phase polyphase table.
#define RS_TAPS    48
#define RS_PHASES  64
#define RS_CHUNK   1024   // max input pairs per internal pass

struct StreamResampler {
    uint32_t src_rate = 0;
    float    step = 1.0f;   // input samples per output sample
    float    t = 0.0f;      // position of the next output, in work[] samples
    float   *coef = nullptr;            // (RS_PHASES + 1) x RS_TAPS
    int16_t *work = nullptr;            // (RS_TAPS + RS_CHUNK) stereo pairs
    int16_t  hist[RS_TAPS * 2];         // last RS_TAPS input pairs
};
static StreamResampler s_rs;

static bool rs_configure(uint32_t src_rate) {
    StreamResampler &r = s_rs;
    if (!r.coef) {
        size_t n = (RS_PHASES + 1) * RS_TAPS * sizeof(float);
        r.coef = (float *)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!r.coef) r.coef = (float *)ps_malloc(n);
    }
    if (!r.work) r.work = (int16_t *)ps_malloc((RS_TAPS + RS_CHUNK) * 2 * sizeof(int16_t));
    if (!r.coef || !r.work) return false;

    if (src_rate != r.src_rate) {
        // Normalised cutoff in cycles/input-sample, relative to Nyquist.
        float lower = (float)(src_rate < SAMPLE_RATE ? src_rate : SAMPLE_RATE);
        float fc = 2.0f * 0.42f * lower / (float)src_rate;
        const float half = RS_TAPS / 2.0f;
        for (int p = 0; p <= RS_PHASES; p++) {
            float frac = (float)p / RS_PHASES;
            float *row = r.coef + p * RS_TAPS;
            float sum = 0.0f;
            for (int k = 0; k < RS_TAPS; k++) {
                // distance (in input samples) from the output instant
                float d = (float)(k - RS_TAPS / 2 + 1) - frac;
                float x = fc * d;
                float sinc = fabsf(x) < 1e-6f ? 1.0f : sinf((float)M_PI * x) / ((float)M_PI * x);
                float w = 0.0f;
                if (fabsf(d) < half) {
                    float u = (float)M_PI * d / half;
                    w = 0.42f + 0.5f * cosf(u) + 0.08f * cosf(2.0f * u);
                }
                row[k] = fc * sinc * w;
                sum += row[k];
            }
            if (sum != 0.0f) for (int k = 0; k < RS_TAPS; k++) row[k] /= sum; // unity DC gain
        }
        r.src_rate = src_rate;
        r.step = (float)src_rate / (float)SAMPLE_RATE;
    }
    return true;
}

// Forget the previous stream (call when a new file/stream starts).
static void rs_reset() {
    memset(s_rs.hist, 0, sizeof(s_rs.hist));
    s_rs.t = RS_TAPS / 2.0f;
}

// Up to RS_CHUNK input pairs in; returns output pairs written to out.
static int rs_process_chunk(const int16_t *in, int in_pairs, int16_t *out, int max_out) {
    StreamResampler &r = s_rs;
    if (in_pairs > RS_CHUNK) in_pairs = RS_CHUNK; // work[] holds RS_TAPS + RS_CHUNK pairs (play_pairs() splits)
    memcpy(r.work, r.hist, sizeof(r.hist));
    memcpy(r.work + RS_TAPS * 2, in, (size_t)in_pairs * 2 * sizeof(int16_t));
    const int n = RS_TAPS + in_pairs;
    int produced = 0;
    while (produced < max_out) {
        int i0 = (int)r.t;
        if (i0 + RS_TAPS / 2 >= n) break; // needs input we don't have yet
        int ph = (int)((r.t - (float)i0) * RS_PHASES + 0.5f);
        const float *c = r.coef + ph * RS_TAPS;
        const int16_t *x = r.work + (i0 - RS_TAPS / 2 + 1) * 2;
        float L = 0.0f, R = 0.0f;
        for (int k = 0; k < RS_TAPS; k++) {
            L += (float)x[2 * k] * c[k];
            R += (float)x[2 * k + 1] * c[k];
        }
        if (L > 32767.0f) L = 32767.0f; else if (L < -32768.0f) L = -32768.0f;
        if (R > 32767.0f) R = 32767.0f; else if (R < -32768.0f) R = -32768.0f;
        out[produced * 2]     = (int16_t)lrintf(L);
        out[produced * 2 + 1] = (int16_t)lrintf(R);
        produced++;
        r.t += r.step;
    }
    // Keep the newest RS_TAPS pairs as history for the next call.
    int drop = n - RS_TAPS;
    memcpy(r.hist, r.work + drop * 2, sizeof(r.hist));
    r.t -= (float)drop;
    return produced;
}

// Writes every byte, retrying partial writes (the old code dropped
// whatever didn't fit within one timeout - lost audio, i.e. a skip and a
// small speed-up). `running` lets a stop request break out.
static void i2s_write_raw(const int16_t *pairs, int n_pairs, volatile bool *running);
// Applies the >0 dB part of the volume (see apply_volume()) on the way out.
// Every music/video/WAV path funnels through here; UI beeps and SFX don't.
static void i2s_write_all(const int16_t *pairs, int n_pairs, volatile bool *running) {
    int32_t g = s_boost_q12;
    if (g <= 4096) { i2s_write_raw(pairs, n_pairs, running); return; }
    int16_t tmp[256 * 2]; // 1 KB, chunked so callers' buffers stay untouched
    while (n_pairs > 0 && (!running || *running)) {
        int n = n_pairs > 256 ? 256 : n_pairs;
        boost_pcm(tmp, pairs, n * 2, g);
        i2s_write_raw(tmp, n, running);
        pairs += n * 2;
        n_pairs -= n;
    }
}
static void i2s_write_raw(const int16_t *pairs, int n_pairs, volatile bool *running) {
    const uint8_t *p = (const uint8_t *)pairs;
    size_t left = (size_t)n_pairs * 2 * sizeof(int16_t);
    int stalls = 0;
    while (left > 0) {
        size_t w = 0;
        i2s_write_guarded(p, left, &w, pdMS_TO_TICKS(200));
        p += w;
        left -= w;
        if (w == 0) {
            if ((running && !*running) || ++stalls > 10) break; // stopped, or I2S is wedged
        } else {
            stalls = 0;
        }
    }
}

// Volume-scaled interleaved stereo pairs at src_rate -> speaker, resampled
// to SAMPLE_RATE through the persistent stream resampler when needed.
static void play_pairs(const int16_t *in, int in_pairs, uint32_t src_rate, volatile bool *running) {
    // Peak meter for the UI's visualizer (cheap: one pass, before resampling).
    {
        int peak = 0;
        for (int i = 0; i < in_pairs * 2; i += 2) {
            int v = in[i] < 0 ? -in[i] : in[i];
            if (v > peak) peak = v;
        }
        int lvl = peak * 100 / 32768;
        // fast attack, slow release
        s_out_level = (uint8_t)(lvl > s_out_level ? lvl : (s_out_level * 7 + lvl) / 8);
    }
    if (src_rate == SAMPLE_RATE || src_rate == 0) {
        i2s_write_all(in, in_pairs, running);
        return;
    }
    if (!s_resample_buf) s_resample_buf = (int16_t *)ps_malloc(RESAMPLE_BUF_SAMPLES * 2 * sizeof(int16_t));
    if (!s_resample_buf || !rs_configure(src_rate)) return;
    while (in_pairs > 0 && (!running || *running)) {
        int n = in_pairs > RS_CHUNK ? RS_CHUNK : in_pairs;
        int out = rs_process_chunk(in, n, s_resample_buf, RESAMPLE_BUF_SAMPLES);
        i2s_write_all(s_resample_buf, out, running);
        in += n * 2;
        in_pairs -= n;
    }
}

bool audio_play_wav(const char *filename) {
    codecs_retry_if_needed();
    if (!s_ok) {
        DEBUG_PRINTF("[audio] WAV play failed: audio not initialized (s_ok=0)\n");
        return false;
    }
    if (s_mp3_playing || s_mp3_task_alive) audio_stop_playback(); // one stream at a time
    if (s_wav_file) {
        DEBUG_PRINTF("[audio] WAV play: file already open, skipping open\n");
    } else {
        // filename already starts with / (e.g. "/song.wav")
        s_wav_file = SD.open(filename, FILE_READ);
        if (!s_wav_file) {
            DEBUG_PRINTF("[audio] WAV open failed: %s\n", filename);
            return false;
        }
    }

    // Parse WAV header (standard RIFF/WAVE)
    DEBUG_PRINTF("[audio] WAV open OK: %s, size=%lu, pos=%lu\n",
                 filename, (unsigned long)s_wav_file.size(), (unsigned long)s_wav_file.position());
    uint8_t hdr[44];
    if (s_wav_file.read(hdr, 44) != 44) {
        DEBUG_PRINTF("[audio] Header too short\n");
        s_wav_file.close();
        return false;
    }
    DEBUG_PRINTF("[audio] hdr: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                 hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5],
                 hdr[6], hdr[7], hdr[8], hdr[9], hdr[10], hdr[11]);

    // Detect non-WAV formats early with helpful messages
    if (hdr[0] == 'I' && hdr[1] == 'D' && hdr[2] == '3') {
        DEBUG_PRINTF("[audio] MP3 file detected (ID3 tag) - not supported\n");
        s_wav_file.close();
        return false;
    }
    if (hdr[0] == 0xFF && (hdr[1] & 0xE0) == 0xE0) {
        DEBUG_PRINTF("[audio] MP3 file detected (frame sync) - not supported\n");
        s_wav_file.close();
        return false;
    }
    if (hdr[0] == 'O' && hdr[1] == 'g' && hdr[2] == 'g' && hdr[3] == 'S') {
        DEBUG_PRINTF("[audio] OGG file detected - not supported\n");
        s_wav_file.close();
        return false;
    }
    if (hdr[0] == 'f' && hdr[1] == 'L' && hdr[2] == 'a' && hdr[3] == 'C') {
        DEBUG_PRINTF("[audio] FLAC file detected - not supported\n");
        s_wav_file.close();
        return false;
    }

    // Verify RIFF/WAVE
    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F') {
        DEBUG_PRINTF("[audio] Not a RIFF file\n");
        s_wav_file.close();
        return false;
    }
    if (hdr[8] != 'W' || hdr[9] != 'A' || hdr[10] != 'V' || hdr[11] != 'E') {
        DEBUG_PRINTF("[audio] Not a WAVE file\n");
        s_wav_file.close();
        return false;
    }

    s_wav_channels = hdr[22] | (hdr[23] << 8);
    s_wav_sample_rate = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
    s_wav_bits_per_sample = hdr[34] | (hdr[35] << 8);
    s_wav_data_size = hdr[40] | (hdr[41] << 8) | (hdr[42] << 16) | (hdr[43] << 24);

    // Find data chunk (skip any extra fmt chunks, fact chunks, etc.)
    s_wav_data_offset = 0; // don't inherit the previous file's offset
    s_wav_file.seek(36); // skip standard header
    while (s_wav_file.available()) {
        uint8_t chunkHdr[8];
        if (s_wav_file.read(chunkHdr, 8) != 8) break;
        uint32_t chunkSize = chunkHdr[4] | (chunkHdr[5] << 8) | (chunkHdr[6] << 16) | (chunkHdr[7] << 24);
        if (chunkHdr[0] == 'd' && chunkHdr[1] == 'a' && chunkHdr[2] == 't' && chunkHdr[3] == 'a') {
            s_wav_data_offset = s_wav_file.position();
            s_wav_data_size = chunkSize;
            break;
        }
        s_wav_file.seek(s_wav_file.position() + chunkSize);
    }

    if (s_wav_data_offset == 0) {
        // Fallback: assume data starts at offset 44
        s_wav_data_offset = 44;
    }

    DEBUG_PRINTF("[audio] WAV: %d Hz, %d-bit, %d ch, %lu bytes data\n",
                 s_wav_sample_rate, s_wav_bits_per_sample, s_wav_channels, s_wav_data_size);

    // Validate header values
    if (s_wav_bits_per_sample != 8 && s_wav_bits_per_sample != 16) {
        DEBUG_PRINTF("[audio] Unsupported bit depth: %d\n", s_wav_bits_per_sample);
        s_wav_file.close();
        return false;
    }
    if (s_wav_sample_rate == 0 || s_wav_sample_rate > 192000) {
        DEBUG_PRINTF("[audio] Invalid sample rate: %lu\n", s_wav_sample_rate);
        s_wav_file.close();
        return false;
    }
    if (s_wav_channels == 0 || s_wav_channels > 2) {
        DEBUG_PRINTF("[audio] Invalid channels: %d\n", s_wav_channels);
        s_wav_file.close();
        return false;
    }
    if (s_wav_data_size == 0) {
        DEBUG_PRINTF("[audio] Zero data size\n");
        s_wav_file.close();
        return false;
    }

    // I2S stays fixed at SAMPLE_RATE always now - see the resample_
    // to_native() comment near audio_update() for why. s_wav_sample_rate
    // and s_wav_channels are kept as-is; audio_update() resamples from
    // them to SAMPLE_RATE on the fly instead of ever retuning the codec.

    s_wav_bytes_read = 0;
    rs_reset(); // new stream for the resampler
    s_paused = false;
    s_wav_playing = true;

    if (!s_wav_buf) {
        s_wav_buf = (int16_t *)ps_malloc(WAV_BUF_SAMPLES * 2 * sizeof(int16_t));
    }

    DEBUG_PRINTF("[audio] playing WAV: %s\n", filename);
    return true;
}

// ---- Voice recorder (dual-mic capture to WAV on SD) -------------------
static File s_rec_file;

// Live per-channel waveform snapshot for a debug scope display (see
// audio_get_mic_waveform()). Updated from rec_task() with the most
// recent block of raw samples each read cycle - deliberately not
// mutex-guarded: this is a cosmetic visualization, and a torn read
// here is at worst one visually-glitched frame, not worth the
// complexity of locking against the recording task for.
#define SCOPE_POINTS 128
static volatile int16_t s_scope_l[SCOPE_POINTS];
static volatile int16_t s_scope_r[SCOPE_POINTS];
static volatile int     s_scope_count = 0;
static TaskHandle_t s_rec_task_handle = NULL;
// Allocated from PSRAM (see the creation site below) rather than a
// plain xTaskCreatePinnedToCore() call, which needs a single
// contiguous block of internal RAM for the stack — a 16KB ask that
// can fail under memory pressure elsewhere, even with plenty of total
// free heap, if it's fragmented into smaller pieces. PSRAM is
// abundant on this board (8MB) and idle most of the time, so this is
// a much safer home for a stack this large.
static StaticTask_t s_rec_task_tcb;
static uint8_t     *s_rec_task_stack = nullptr;
static uint32_t s_rec_start_ms = 0;
static uint32_t s_rec_bytes_written = 0;
static const int REC_BUF_SAMPLES = 512;  // 1024 bytes per read (stereo 16-bit)
static const int REC_SD_BUF_SIZE = 4096; // 4KB SD write buffer for performance

// Write a minimal 44-byte RIFF/WAV header (updated on stop)
static bool s_rec_mono = false;   // voice replies: mic1+mic2 averaged, half the size

static void wav_write_header(File &f, uint32_t data_size) {
    uint32_t file_size = data_size + 36;
    uint32_t sample_rate = SAMPLE_RATE;
    uint16_t bits = 16;
    uint16_t channels = s_rec_mono ? 1 : 2;
    uint32_t byte_rate = sample_rate * channels * bits / 8;
    uint16_t block_align = channels * bits / 8;

    f.seek(0);
    // RIFF header
    f.write((const uint8_t *)"RIFF", 4);
    uint8_t b[4];
    b[0]=file_size&0xff; b[1]=(file_size>>8)&0xff; b[2]=(file_size>>16)&0xff; b[3]=(file_size>>24)&0xff;
    f.write(b, 4);
    f.write((const uint8_t *)"WAVE", 4);
    // fmt subchunk
    f.write((const uint8_t *)"fmt ", 4);
    uint32_t fmt_size = 16;
    b[0]=fmt_size&0xff; b[1]=(fmt_size>>8)&0xff; b[2]=(fmt_size>>16)&0xff; b[3]=(fmt_size>>24)&0xff;
    f.write(b, 4);
    uint16_t fmt = 1; // PCM
    b[0]=fmt&0xff; b[1]=(fmt>>8)&0xff; f.write(b, 2);
    b[0]=channels&0xff; b[1]=(channels>>8)&0xff; f.write(b, 2);
    b[0]=sample_rate&0xff; b[1]=(sample_rate>>8)&0xff; b[2]=(sample_rate>>16)&0xff; b[3]=(sample_rate>>24)&0xff;
    f.write(b, 4);
    b[0]=byte_rate&0xff; b[1]=(byte_rate>>8)&0xff; b[2]=(byte_rate>>16)&0xff; b[3]=(byte_rate>>24)&0xff;
    f.write(b, 4);
    b[0]=block_align&0xff; b[1]=(block_align>>8)&0xff; f.write(b, 2);
    b[0]=bits&0xff; b[1]=(bits>>8)&0xff; f.write(b, 2);
    // data subchunk
    f.write((const uint8_t *)"data", 4);
    b[0]=data_size&0xff; b[1]=(data_size>>8)&0xff; b[2]=(data_size>>16)&0xff; b[3]=(data_size>>24)&0xff;
    f.write(b, 4);
}

static void rec_task(void *param) {
    int16_t buf[REC_BUF_SAMPLES];
    uint8_t sd_buf[REC_SD_BUF_SIZE];
    int sd_buf_pos = 0;

    while (s_recording) {
        // Read mic samples from I2S
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_PORT, buf, sizeof(buf), &bytes_read, 100 / portTICK_PERIOD_MS);
        if (err != ESP_OK || bytes_read == 0) continue;
        if (!s_recording) break;
        mic_apply_gain(buf, (int)(bytes_read / sizeof(int16_t)));

        // Compute mic level from recorded samples for UI
        {
            int count = bytes_read / sizeof(int16_t);
            double sum_sq = 0;
            for (int i = 0; i < count; i++) sum_sq += (double)buf[i] * buf[i];
            double rms = sqrt(sum_sq / count);
            int pct = (int)(rms / 32768.0 * 100.0 * 8.0);
            if (pct > 100) pct = 100;
            if (pct < 0) pct = 0;
            s_rec_mic_level = pct;
        }

        // Snapshot the most recent block into the scope buffers, one
        // point per stereo frame (mic1=left, mic2=right per the
        // ES7210's non-TDM SDOUT1 routing), downsampled if this read
        // has more frames than the display needs.
        {
            int frames = (int)bytes_read / (int)(2 * sizeof(int16_t));
            if (frames > 0) {
                int step = frames > SCOPE_POINTS ? frames / SCOPE_POINTS : 1;
                int n = 0;
                for (int i = 0; i < frames && n < SCOPE_POINTS; i += step) {
                    s_scope_l[n] = buf[i * 2];
                    s_scope_r[n] = buf[i * 2 + 1];
                    n++;
                }
                s_scope_count = n;
            }
        }

        if (s_rec_mono) {
            int frames = (int)bytes_read / 4;
            for (int i = 0; i < frames; i++) buf[i] = (int16_t)(((int32_t)buf[2 * i] + buf[2 * i + 1]) / 2);
            bytes_read = frames * 2;
        }

        // Copy to SD write buffer
        int remaining = (int)bytes_read;
        uint8_t *src = (uint8_t *)buf;
        while (remaining > 0 && s_recording) {
            int space = REC_SD_BUF_SIZE - sd_buf_pos;
            int to_copy = remaining < space ? remaining : space;
            memcpy(sd_buf + sd_buf_pos, src, to_copy);
            sd_buf_pos += to_copy;
            src += to_copy;
            remaining -= to_copy;
            s_rec_bytes_written += to_copy;

            // Flush when buffer full
            if (sd_buf_pos >= REC_SD_BUF_SIZE) {
                s_rec_file.write(sd_buf, sd_buf_pos);
                sd_buf_pos = 0;
            }
        }
    }

    // Flush the rest of the SD buffer. (This used to be skipped - the
    // loop only ends once s_recording is false - and the bytes were
    // counted twice, so the header claimed data the file didn't have.)
    if (sd_buf_pos > 0) {
        s_rec_file.write(sd_buf, sd_buf_pos);
    }

    // Write final WAV header with actual data size
    uint32_t data_size = s_rec_bytes_written;
    wav_write_header(s_rec_file, data_size);
    s_rec_file.close();

    DEBUG_PRINTF("[audio] recording stopped: %lu bytes written\n", (unsigned long)data_size);
    s_rec_mic_level = 0;
    s_scope_count = 0;
    s_recording = false;
    s_rec_task_handle = NULL;
    vTaskDelete(NULL);
}

static bool start_record(const char *filename);

bool audio_start_record(const char *filename) {
    if (s_recording) return false;
    codecs_retry_if_needed();
    s_rec_mono = false;
    return start_record(filename);
}

bool audio_start_record_mono(const char *filename) {
    codecs_retry_if_needed();
    if (s_recording) return false;
    s_rec_mono = true;
    return start_record(filename);
}

static bool start_record(const char *filename) {
    if (!s_ok) return false;
    if (s_recording || s_wav_playing || s_mp3_playing) {
        DEBUG_PRINTF("[audio] cannot record: audio bus busy\n");
        return false;
    }
    if (!sd_is_safe_filename(filename)) {
        DEBUG_PRINTF("[audio] record rejected: unsafe filename '%s'\n", filename ? filename : "(null)");
        return false;
    }
#if FEATURE_USB_MSC
    if (usb_msc_host_active()) {
        DEBUG_PRINTF("[audio] record rejected: SD card is mounted over USB\n");
        return false;
    }
#endif

    // Ensure /Recordings/ directory exists
    if (!SD.exists("/Recordings")) {
        SD.mkdir("/Recordings");
    }

    String path = String("/Recordings/") + filename;
    s_rec_file = SD.open(path, FILE_WRITE);
    if (!s_rec_file) {
        DEBUG_PRINTF("[audio] record open failed: %s\n", path.c_str());
        return false;
    }

    // Write placeholder 44-byte WAV header (updated on stop with actual size)
    uint8_t hdr[44] = {0};
    s_rec_file.write(hdr, 44);

    s_rec_bytes_written = 0;
    s_rec_start_ms = millis();
    s_last_out_ms = millis();
    i2s_ensure_running();
    s_recording = true;

    // Create recording task on core 1 with a 16KB stack allocated
    // from PSRAM (see s_rec_task_stack's declaration above for why) -
    // xTaskCreateStaticPinnedToCore() returns the task handle directly
    // (NULL on failure), unlike the dynamic version's BaseType_t.
    if (!s_rec_task_stack) s_rec_task_stack = (uint8_t *)ps_malloc(16384);
    if (!s_rec_task_stack) {
        DEBUG_PRINTF("[audio] rec_task stack alloc from PSRAM FAILED - aborting record\n");
        s_recording = false;
        s_rec_file.close();
        SD.remove(path);
        return false;
    }
    s_rec_task_handle = xTaskCreateStaticPinnedToCore(
        rec_task, "rec_task", 16384, NULL, 5, s_rec_task_stack, &s_rec_task_tcb, 1);
    if (!s_rec_task_handle) {
        DEBUG_PRINTF("[audio] rec_task creation FAILED - aborting record\n");
        s_recording = false;
        s_rec_file.close();
        SD.remove(path);
        return false;
    }

    DEBUG_PRINTF("[audio] recording started: %s\n", path.c_str());
    return true;
}

void audio_stop_record() {
    if (!s_recording) return;
    s_recording = false;
    // Wait for task to exit (it closes the file)
    if (s_rec_task_handle) {
        for (int i = 0; i < 100 && s_rec_task_handle; i++) delay(10);
    }
}

bool audio_is_recording() {
    return s_recording;
}

uint32_t audio_record_duration_s() {
    if (!s_recording) return 0;
    return (millis() - s_rec_start_ms) / 1000;
}

int audio_get_mic_waveform(int16_t *out_l, int16_t *out_r, int max_points) {
    if (!s_recording) return 0;
    int n = s_scope_count < max_points ? s_scope_count : max_points;
    for (int i = 0; i < n; i++) {
        out_l[i] = s_scope_l[i];
        out_r[i] = s_scope_r[i];
    }
    return n;
}

// State for the fixed-clock + resample approach - mirrors the WAV/MP3
// playback paths exactly (see the long comment by resample_to_native()
// above for why). This used to call i2s_set_clk() directly with
// whatever rate the caller passed in, the same bug already found and
// fixed for WAV/MP3 but never applied here - the ESP32's I2S
// peripheral clock would change, but the ES8311 codec's own internal
// clock dividers stayed fixed at 16kHz regardless, so any video with a
// non-16kHz audio track (virtually all of them - 22050/44100/48000 are
// standard, 16000 isn't) played back at the wrong speed with audible
// clock-mismatch artifacts. That's "accelerated and noisy" exactly.
static uint32_t s_pcm_src_rate = 0;
static uint8_t  s_pcm_channels = 2;

bool audio_pcm_configure(uint32_t sample_rate, uint8_t channels) {
    if (!s_ok) return false;
    // I2S itself is already running at SAMPLE_RATE/stereo from
    // audio_init() and stays that way - no i2s_set_clk() call here at
    // all. Just remember the caller's actual rate/channels so
    // audio_pcm_write() can resample to match on the way out.
    s_pcm_src_rate = sample_rate;
    s_pcm_channels = (channels == 1) ? 1 : 2;
    rs_reset(); // new stream - don't carry the previous one's tail into it
    return true;
}

void audio_pcm_write(const int16_t *samples, size_t sample_count) {
    if (!s_ok || !samples || sample_count == 0) return;

    int16_t amp = 32767; // unity - volume is applied once, in the codec (apply_volume())

    // Convert to volume-scaled interleaved-stereo pairs first, the
    // same input shape play_pairs() expects for the WAV/MP3
    // paths too - matches app_media.cpp's largest chunk size
    // (s_aud_buf_sz = 8192 bytes = 4096 int16_t samples) with room
    // to spare.
    // 16 KB - in PSRAM, not internal RAM (see s_mp3_dec below).
    static int16_t *stereo_buf = (int16_t *)heap_caps_malloc(4096 * 2 * sizeof(int16_t), (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!stereo_buf) return;
    int in_pairs;
    if (s_pcm_channels == 1) {
        in_pairs = (int)sample_count;
        if (in_pairs > 4096) in_pairs = 4096;
        for (int i = 0; i < in_pairs; i++) {
            int16_t s = (int16_t)((int32_t)samples[i] * amp / 32767);
            stereo_buf[i * 2] = s;
            stereo_buf[i * 2 + 1] = s;
        }
    } else {
        in_pairs = (int)(sample_count / 2);
        if (in_pairs > 4096) in_pairs = 4096;
        for (int i = 0; i < in_pairs * 2; i++) {
            stereo_buf[i] = (int16_t)((int32_t)samples[i] * amp / 32767);
        }
    }

    play_pairs(stereo_buf, in_pairs, s_pcm_src_rate, nullptr);
}

void audio_pcm_restore_default() {
    // I2S was never reconfigured away from SAMPLE_RATE/stereo in the
    // first place now, so there's nothing to actually restore - kept
    // as a real function (not removed) so app_media.cpp's existing
    // call sites don't need to change at all, and to reset the
    // remembered source rate/channels for the next playback session.
    s_pcm_src_rate = 0;
    s_pcm_channels = 2;
}

// ---- MP3 file playback from SD (via minimp3) ------------------------------
// Decoder state and output frame live in PSRAM: together they were ~11 KB
// of internal RAM, which WiFi and the BLE controller need - BLE init failed
// with "Malloc failed" without it. Allocated on the first play, not by a
// global initialiser: those run before PSRAM is up in some core builds
// (the PlatformIO one), and a null decoder crashed mp3dec_init().
static File s_mp3_file;
static mp3dec_t *s_mp3_dec = nullptr;
static int16_t *s_mp3_pcm = nullptr;

// Compressed-input buffer, kept topped up so minimp3 always sees at least
// MP3_MIN_BUFFERED bytes (several whole frames) - see mp3_task(). In PSRAM:
// 16KB is too big an ask for internal RAM next to WiFi/BLE.
#define MP3_INBUF_SIZE    (16 * 1024)
#define MP3_MIN_BUFFERED  (4 * 1024)
static uint8_t *s_mp3_input_buf = nullptr;
static int s_mp3_input_len = 0;
static int s_mp3_input_pos = 0;
static bool s_mp3_eof = false;
// Playback position/duration bookkeeping (audio_get_stream_info()).
static uint32_t s_mp3_audio_start = 0;   // byte offset after the ID3v2 tag
static uint32_t s_mp3_file_size = 0;
static volatile uint64_t s_mp3_samples = 0; // per-channel samples decoded so far
static volatile uint64_t s_mp3_bytes = 0;   // compressed bytes those samples came from
static volatile uint32_t s_mp3_hz = 0;
static volatile uint8_t  s_mp3_ch = 0;
static volatile uint16_t s_mp3_kbps = 0;
static volatile float    s_mp3_seek_req = -1.0f; // set by audio_seek(), applied by mp3_task
// Same PSRAM-backed static task pattern as s_rec_task_stack above, and
// for the same reason - this stack is the one that was actually
// observed failing to allocate (see the serial log discussion this
// change came from): 32KB is a large single contiguous internal-RAM
// request, and minimp3's frame decode genuinely needs most of it
// (~20KB for the DCT/IDCT), so shrinking it isn't a safe fix. PSRAM
// has plenty of room for it instead.
static StaticTask_t s_mp3_task_tcb;
static uint8_t     *s_mp3_task_stack = nullptr;

// Moves the unread tail to the front and fills the rest from SD.
static void mp3_refill() {
    int remain = s_mp3_input_len - s_mp3_input_pos;
    if (remain < 0) remain = 0;
    if (remain > 0 && s_mp3_input_pos > 0)
        memmove(s_mp3_input_buf, s_mp3_input_buf + s_mp3_input_pos, remain);
    s_mp3_input_len = remain;
    s_mp3_input_pos = 0;
    if (s_mp3_eof) return;
    int got = s_mp3_file.read(s_mp3_input_buf + s_mp3_input_len, MP3_INBUF_SIZE - s_mp3_input_len);
    if (got > 0) s_mp3_input_len += got;
    else s_mp3_eof = true;
}

// MP3 decoder task - runs on its own stack to avoid loopTask overflow
// minimp3 mp3dec_decode_frame() needs ~20KB stack for DCT/IDCT
//
// Buffering rule (the main fix for the artifacts and the too-fast
// playback): never hand minimp3 a buffer that ends inside a frame. Its
// fast path needs the current frame AND the next frame's header; when
// that isn't there it wipes its own state - including the bit reservoir
// the next frames depend on - and goes hunting for a new sync point,
// skipping whatever real audio it has to. The old 4KB buffer was only
// refilled once completely empty, so that happened at every refill,
// several times a second: a garble each time, and the skipped audio made
// songs run fast. Now the buffer is topped up whenever fewer than
// MP3_MIN_BUFFERED bytes (3+ frames even at 320kbps) are left.
static void mp3_task(void *param) {
    uint32_t cur_hz = 0;
    while (s_mp3_playing) {
        float seek = s_mp3_seek_req;
        if (seek >= 0.0f) {
            s_mp3_seek_req = -1.0f;
            uint32_t audio_len = s_mp3_file_size > s_mp3_audio_start ? s_mp3_file_size - s_mp3_audio_start : 0;
            uint32_t off = (uint32_t)(seek * (float)audio_len);
            s_mp3_file.seek(s_mp3_audio_start + off);
            s_mp3_input_len = s_mp3_input_pos = 0;
            s_mp3_eof = false;
            mp3dec_init(s_mp3_dec); // resyncs on the next frame header by itself
            rs_reset();
            // Keep the elapsed clock consistent with the new position,
            // using the average bytes-per-sample seen so far.
            if (s_mp3_bytes > 0 && s_mp3_samples > 0) {
                double bps = (double)s_mp3_bytes / (double)s_mp3_samples;
                s_mp3_samples = (uint64_t)((double)off / bps);
                s_mp3_bytes = off;
            }
        }
        if (s_paused) {
            s_out_level = 0;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!s_mp3_eof && s_mp3_input_len - s_mp3_input_pos < MP3_MIN_BUFFERED) mp3_refill();
        if (s_mp3_input_pos >= s_mp3_input_len) {
            DEBUG_PRINTF("[audio] MP3: end of file\n");
            break;
        }

        mp3dec_frame_info_t info;
        int samples_per_ch = mp3dec_decode_frame(s_mp3_dec,
            s_mp3_input_buf + s_mp3_input_pos,
            s_mp3_input_len - s_mp3_input_pos,
            s_mp3_pcm, &info);

        // Always advance past whatever minimp3 consumed or skipped
        // (a decoded frame, a Xing/VBR info frame, or junk while resyncing).
        s_mp3_input_pos += info.frame_bytes;

        if (samples_per_ch == 0) {
            if (info.frame_bytes == 0) {
                // Not even one whole frame left: refill, or we're done.
                if (s_mp3_eof) {
                    DEBUG_PRINTF("[audio] MP3: end of file\n");
                    break;
                }
                mp3_refill();
            }
            continue;
        }

        s_mp3_samples += (uint64_t)samples_per_ch;
        s_mp3_bytes += (uint64_t)info.frame_bytes;
        s_mp3_hz = (uint32_t)info.hz;
        s_mp3_ch = (uint8_t)info.channels;
        s_mp3_kbps = (uint16_t)info.bitrate_kbps;

        if ((uint32_t)info.hz != cur_hz) {
            cur_hz = (uint32_t)info.hz;
            DEBUG_PRINTF("[audio] MP3: %d Hz, %d ch, %d kbps -> resampled to %d Hz\n",
                         info.hz, info.channels, info.bitrate_kbps, SAMPLE_RATE);
        }

        // Volume-scale into interleaved stereo pairs.
        int16_t amp = 32767; // unity - volume is applied once, in the codec (apply_volume())
        static int16_t *stereo_buf = (int16_t *)heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t), (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!stereo_buf) break;
        if (info.channels == 1) {
            for (int i = 0; i < samples_per_ch; i++) {
                int16_t v = (int16_t)((int32_t)s_mp3_pcm[i] * amp / 32767);
                stereo_buf[i * 2] = v;
                stereo_buf[i * 2 + 1] = v;
            }
        } else {
            for (int i = 0; i < samples_per_ch * 2; i++) {
                stereo_buf[i] = (int16_t)((int32_t)s_mp3_pcm[i] * amp / 32767);
            }
        }

        // Codec clock stays fixed at SAMPLE_RATE (see the comment above
        // the stream resampler) - play_pairs() resamples continuously
        // across frames and blocks until every sample is queued, which
        // is what paces this task at exactly real time.
        play_pairs(stereo_buf, samples_per_ch, (uint32_t)info.hz, &s_mp3_playing);
    }

    // Clean up here, in the task itself. (End of file used to call
    // audio_stop_playback() from inside this task, which then waited up
    // to 500ms for this very task to exit and restarted I2S - a gap and a
    // pop every time game music looped.)
    s_mp3_playing = false;
    if (s_mp3_file) s_mp3_file.close();
    DEBUG_PRINTF("[audio] MP3: task exiting\n");
    s_mp3_task_alive = false;
    vTaskDelete(NULL);
}

static uint32_t mp3_skip_id3(const uint8_t *data, int len) {
    if (len >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        uint32_t tag_size = ((uint32_t)data[6] << 21) | ((uint32_t)data[7] << 14) |
                            ((uint32_t)data[8] << 7) | data[9];
        return 10 + tag_size;
    }
    return 0;
}

bool audio_play_mp3(const char *filename) {
    codecs_retry_if_needed();
    if (!s_ok) {
        DEBUG_PRINTF("[audio] MP3 play failed: not initialized\n");
        return false;
    }
    // One stream at a time - and never a second decoder task on the
    // same static stack while the first is still running.
    if (s_wav_playing || s_mp3_playing || s_mp3_task_alive) audio_stop_playback();
    if (s_mp3_task_alive) {
        DEBUG_PRINTF("[audio] MP3 play failed: previous decoder still running\n");
        return false;
    }
    if (!s_mp3_input_buf) s_mp3_input_buf = (uint8_t *)ps_malloc(MP3_INBUF_SIZE);
    if (!s_mp3_dec) s_mp3_dec = (mp3dec_t *)heap_caps_calloc(1, sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mp3_pcm)
        s_mp3_pcm = (int16_t *)heap_caps_calloc(MINIMP3_MAX_SAMPLES_PER_FRAME, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mp3_input_buf || !s_mp3_dec || !s_mp3_pcm) {
        DEBUG_PRINTF("[audio] MP3 play failed: no memory for the decoder\n");
        return false;
    }

    s_mp3_file = SD.open(filename, FILE_READ);
    if (!s_mp3_file) {
        DEBUG_PRINTF("[audio] MP3 open failed: %s\n", filename);
        return false;
    }

    // Skip ID3v2 tag
    uint8_t peek[10];
    if (s_mp3_file.read(peek, 10) == 10) {
        uint32_t skip = mp3_skip_id3(peek, 10);
        s_mp3_audio_start = skip;
        if (skip > 0) {
            DEBUG_PRINTF("[audio] MP3: skipping ID3v2 tag (%lu bytes)\n", (unsigned long)skip);
            s_mp3_file.seek(skip);
        } else {
            s_mp3_file.seek(0);
        }
    } else {
        s_mp3_file.close();
        return false;
    }

    mp3dec_init(s_mp3_dec);
    s_mp3_input_len = 0;
    s_mp3_input_pos = 0;
    s_mp3_eof = false;
    rs_reset();
    s_mp3_file_size = (uint32_t)s_mp3_file.size();
    s_mp3_samples = 0;
    s_mp3_bytes = 0;
    s_mp3_hz = 0;
    s_mp3_ch = 0;
    s_mp3_kbps = 0;
    s_mp3_seek_req = -1.0f;
    s_paused = false;
    s_mp3_playing = true;

    // Create dedicated task with a 32KB stack allocated from PSRAM
    // (see s_mp3_task_stack's declaration above) - this is the
    // allocation that was actually observed failing when it lived in
    // internal RAM, confirmed via the serial log ("mp3_task creation
    // FAILED (heap too low for 32KB stack?)").
    if (!s_mp3_task_stack) s_mp3_task_stack = (uint8_t *)ps_malloc(32768);
    if (!s_mp3_task_stack) {
        DEBUG_PRINTF("[audio] mp3_task stack alloc from PSRAM FAILED - aborting playback\n");
        s_mp3_playing = false;
        s_mp3_file.close();
        return false;
    }
    DEBUG_PRINTF("[audio] MP3 playing: %s, size=%lu\n", filename, (unsigned long)s_mp3_file.size());
    s_mp3_task_alive = true;
    s_mp3_task_handle = xTaskCreateStaticPinnedToCore(
        mp3_task, "mp3_dec", 32768, NULL, 5, s_mp3_task_stack, &s_mp3_task_tcb, 1);
    if (!s_mp3_task_handle) {
        DEBUG_PRINTF("[audio] mp3_task creation FAILED - aborting playback\n");
        s_mp3_task_alive = false;
        s_mp3_playing = false;
        s_mp3_file.close();
        return false;
    }
    return true;
}

void audio_stop_playback() {
    s_paused = false;
    s_out_level = 0;
    s_wav_playing = false;
    if (s_wav_file) {
        s_wav_file.close();
    }
    s_mp3_playing = false;
    // Wait for the MP3 task to finish (it closes its own file). Never
    // wait from inside that task itself.
    if (s_mp3_task_alive && xTaskGetCurrentTaskHandle() != s_mp3_task_handle) {
        for (int i = 0; i < 50 && s_mp3_task_alive; i++) delay(10);
    }
    if (!s_mp3_task_alive && s_mp3_file) {
        s_mp3_file.close();
    }
    // Reconfigure I2S back to default sample rate
    if (s_ok) {
        i2s_stop(I2S_PORT);
        i2s_set_clk(I2S_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
        i2s_start(I2S_PORT);
        s_i2s_running = true;
    }
}

bool audio_is_playing() {
    return s_wav_playing || s_mp3_playing;
}

void audio_set_paused(bool paused) {
    if (!s_wav_playing && !s_mp3_playing) paused = false;
    s_paused = paused;
    if (paused) s_out_level = 0;
}

bool audio_is_paused() { return s_paused; }

void audio_seek(float fraction) {
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 0.995f) fraction = 0.995f;
    if (s_mp3_playing) {
        s_mp3_seek_req = fraction; // the decoder task applies it between frames
    } else if (s_wav_playing && s_wav_file) {
        // WAV is fed from loop() (audio_update()), the same context as the
        // UI calling this - so it can seek directly.
        int frame = (s_wav_bits_per_sample / 8) * (s_wav_channels ? s_wav_channels : 1);
        if (frame <= 0) return;
        uint32_t off = (uint32_t)(fraction * (float)s_wav_data_size) / frame * frame;
        s_wav_file.seek(s_wav_data_offset + off);
        s_wav_bytes_read = off;
        rs_reset();
    }
}

bool audio_get_stream_info(AudioStreamInfo *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->paused = s_paused;
    out->level = s_out_level;
    if (s_mp3_playing) {
        out->is_mp3 = true;
        uint32_t hz = s_mp3_hz;
        uint64_t samples = s_mp3_samples, bytes = s_mp3_bytes;
        out->sample_rate = hz;
        out->channels = s_mp3_ch;
        out->kbps = s_mp3_kbps;
        if (hz) {
            out->position_ms = (uint32_t)(samples * 1000ULL / hz);
            uint32_t audio_len = s_mp3_file_size > s_mp3_audio_start ? s_mp3_file_size - s_mp3_audio_start : 0;
            // Duration from the average compressed bytes per sample so far
            // (works for VBR too); settles within the first second.
            if (bytes > 0 && samples > 0)
                out->duration_ms = (uint32_t)((double)audio_len * (double)samples / (double)bytes * 1000.0 / hz);
        }
        return true;
    }
    if (s_wav_playing) {
        uint32_t byte_rate = s_wav_sample_rate * (s_wav_bits_per_sample / 8) * (s_wav_channels ? s_wav_channels : 1);
        out->sample_rate = s_wav_sample_rate;
        out->channels = (uint8_t)s_wav_channels;
        out->kbps = (uint16_t)(byte_rate * 8 / 1000);
        if (byte_rate) {
            out->position_ms = (uint32_t)((uint64_t)s_wav_bytes_read * 1000ULL / byte_rate);
            out->duration_ms = (uint32_t)((uint64_t)s_wav_data_size * 1000ULL / byte_rate);
        }
        return true;
    }
    return false;
}

void audio_idle_power() {
    if (!s_pa_on || millis() - s_last_out_ms <= 4000 || audio_is_playing()) return;
    // Same mutex as the writers; if one holds it, audio is in flight anyway.
    if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, 0) != pdTRUE) return;
    if (millis() - s_last_out_ms > 4000) {
        s_pa_on = false;
        digitalWrite(PIN_AUDIO_PA, AUDIO_PA_ACTIVE_HIGH ? LOW : HIGH);
#if CONFIG_PM_ENABLE
        if (s_i2s_running && !s_recording) {
            i2s_zero_dma_buffer(I2S_PORT);
            i2s_stop(I2S_PORT);
            s_i2s_running = false;
        }
#endif
    }
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
}

bool audio_update() {
    audio_idle_power();
    if (s_wav_playing) {
        if (!s_wav_file) { audio_stop_playback(); return false; }
        if (s_paused) { s_out_level = 0; return true; }

        // Fill buffer from SD
        int bytes_per_sample = s_wav_bits_per_sample / 8;
        int channels = s_wav_channels > 0 ? s_wav_channels : 1;
        int bytes_per_frame = bytes_per_sample * channels;
        uint8_t raw[WAV_BUF_SAMPLES * 4];

        int bytes_to_read = WAV_BUF_SAMPLES * bytes_per_frame;
        if (s_wav_bytes_read + bytes_to_read > s_wav_data_size) {
            bytes_to_read = s_wav_data_size - s_wav_bytes_read;
            if (bytes_to_read <= 0) {
                audio_stop_playback();
                return false;
            }
        }

        int bytesRead = s_wav_file.read(raw, bytes_to_read);
        if (bytesRead <= 0) {
            audio_stop_playback();
            return false;
        }

        s_wav_bytes_read += bytesRead;
        int frames_read = bytesRead / bytes_per_frame;
        int16_t amp = 32767; // unity - volume is applied once, in the codec (apply_volume())

        if (!s_wav_buf) {
            audio_stop_playback();
            return false;
        }

        for (int i = 0; i < frames_read; i++) {
            int16_t sampleL, sampleR;
            if (s_wav_bits_per_sample == 16) {
                sampleL = (int16_t)(raw[i * bytes_per_frame] | (raw[i * bytes_per_frame + 1] << 8));
                sampleR = (channels == 2) ?
                    (int16_t)(raw[i * bytes_per_frame + 2] | (raw[i * bytes_per_frame + 3] << 8)) :
                    sampleL;
            } else {
                sampleL = ((int16_t)raw[i * bytes_per_frame] - 128) * 256;
                sampleR = (channels == 2) ?
                    ((int16_t)raw[i * bytes_per_frame + 1] - 128) * 256 :
                    sampleL;
            }
            s_wav_buf[i * 2] = (int16_t)((int32_t)sampleL * amp / 32767);
            s_wav_buf[i * 2 + 1] = (int16_t)((int32_t)sampleR * amp / 32767);
        }

        // The codec's clock stays fixed at SAMPLE_RATE always now (see
        // the comment above the stream resampler for why) - anything
        // at a different native rate gets resampled here in software
        // instead.
        play_pairs(s_wav_buf, frames_read, s_wav_sample_rate, &s_wav_playing);

        if (s_wav_bytes_read >= s_wav_data_size) {
            audio_stop_playback();
        }
        return s_wav_playing;
    }
    return false;
}

#else // FEATURE_AUDIO disabled

bool audio_init() { return false; }
bool audio_speaker_ok() { return false; }
bool audio_mic_ok() { return false; }
void audio_set_mic_sensitivity(uint8_t) {}
uint8_t audio_get_mic_sensitivity() { return 3; }
void audio_set_volume(uint8_t) {}
uint8_t audio_get_volume() { return 80; }
int audio_mic_level_percent() { return 0; }
void audio_beep(uint16_t, uint16_t) {}
void audio_play_sfx(uint16_t, uint16_t) {}
bool audio_play_wav(const char *) { return false; }
bool audio_play_mp3(const char *) { return false; }
bool audio_update() { return false; }
void audio_idle_power() {}
void audio_stop_playback() {}
bool audio_is_playing() { return false; }
void audio_set_paused(bool) {}
bool audio_is_paused() { return false; }
void audio_seek(float) {}
bool audio_get_stream_info(AudioStreamInfo *) { return false; }
bool audio_start_record(const char *) { return false; }
bool audio_start_record_mono(const char *) { return false; }
void audio_stop_record() {}
bool audio_is_recording() { return false; }
uint32_t audio_record_duration_s() { return 0; }
int audio_get_mic_waveform(int16_t *, int16_t *, int) { return 0; }
bool audio_pcm_configure(uint32_t, uint8_t) { return false; }
void audio_pcm_write(const int16_t *, size_t) {}
void audio_pcm_restore_default() {}

#endif
