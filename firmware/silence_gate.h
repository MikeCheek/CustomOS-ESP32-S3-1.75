/*
 * silence_gate.h
 * "Skip silences" for the Recorder (hal_audio.cpp's rec_task): feed it the
 * level (0-100) of each block as it's recorded; it says whether to write
 * the block. It learns the noise floor (the quietest of the last ~2 s,
 * rising slowly), keeps the first 0.6 s of every pause so speech still
 * breathes, and drops the rest of a pause until the sound comes back.
 * Header-only, no Arduino; tools/silence_test.cpp checks it.
 */
#pragma once
#include <stdint.h>

class SilenceGate {
public:
    void reset() { floor_ = 100; quiet_ms_ = 0; t_ = 0; min_ = 100; min_t_ = 0; }
    // level 0-100 of a block that lasted block_ms; true = keep it.
    bool keep(int level, int block_ms) {
        t_ += block_ms;
        // floor: the quietest level, forgotten slowly (2 s window)
        if (level < min_) { min_ = level; min_t_ = t_; }
        if (t_ - min_t_ > 2000) { min_ = level; min_t_ = t_; }
        if (min_ < floor_) floor_ = min_;
        else if (t_ % 500 < (uint32_t)block_ms && floor_ < min_) floor_++;
        bool sound = level > floor_ + 4;
        if (sound) { quiet_ms_ = 0; return true; }
        quiet_ms_ += block_ms;
        return quiet_ms_ <= 600;
    }
    int floor() const { return floor_; }
private:
    int floor_ = 100, min_ = 100;
    uint32_t quiet_ms_ = 0, t_ = 0, min_t_ = 0;
};
