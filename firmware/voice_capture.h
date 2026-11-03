/*
 * voice_capture.h - keeps what was said after the wake word (voice_sr.cpp).
 *
 * While MultiNet listens for a command, the same cleaned-up audio also
 * goes in here. When no command matches but someone spoke, that's the
 * question for the assistant: it's sent to the phone as it is, instead of
 * asking for it again. Speech is told from the room by level: the quietest
 * 20 ms chunk of the first 0.3 s is the floor, speech is well above it.
 * Header-only, no hardware: tools/voice_capture_test.cpp runs it on a PC.
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

class VoiceCapture {
public:
    static const int RATE = 16000;
    static const int QUIET_END_MS = 1300;   // this long quiet after speech ends the question (as the assistant)

    void begin(int16_t *buf, int capacity) {
        buf_ = buf;
        cap_ = capacity;
        n_ = 0;
        floor_ = 1e9f;
        spoke_ = false;
        last_voice_ = 0;
    }

    // One chunk of 16 kHz mono; what doesn't fit is dropped.
    void add(const int16_t *s, int k) {
        if (!buf_ || k <= 0) return;
        int room = cap_ - n_;
        int take = k < room ? k : room;
        if (take > 0) memcpy(buf_ + n_, s, take * sizeof(int16_t));
        double sum = 0;
        for (int i = 0; i < k; i++) sum += (double)s[i] * s[i];
        float rms = (float)sqrt(sum / k);
        int end = n_ + take;
        if (n_ < RATE * 3 / 10) {
            if (rms < floor_) floor_ = rms;
        } else if (rms > threshold()) {
            spoke_ = true;
            last_voice_ = end;
        }
        n_ = end;
    }

    bool spoke() const { return spoke_; }
    bool full() const { return n_ >= cap_; }
    int samples() const { return n_; }
    const int16_t *data() const { return buf_; }
    // Spoke, and the last speech was less than QUIET_END_MS ago: keep listening.
    bool still_talking() const { return spoke_ && n_ - last_voice_ < RATE * QUIET_END_MS / 1000; }

    // Louder for the transcriber: the peak to ~70% of full scale, at most 8x.
    void normalize() {
        int peak = 1;
        for (int i = 0; i < n_; i++) {
            int a = buf_[i] < 0 ? -buf_[i] : buf_[i];
            if (a > peak) peak = a;
        }
        float g = 23000.0f / peak;
        if (g > 8) g = 8;
        if (g <= 1.05f) return;
        for (int i = 0; i < n_; i++) {
            int v = (int)(buf_[i] * g);
            buf_[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }

private:
    // Clearly above the room: 3x its level (~10 dB), and never below -40 dBFS.
    float threshold() const { return floor_ * 3 > 330 ? floor_ * 3 : 330; }

    int16_t *buf_ = nullptr;
    int cap_ = 0, n_ = 0, last_voice_ = 0;
    float floor_ = 1e9f;
    bool spoke_ = false;
};
