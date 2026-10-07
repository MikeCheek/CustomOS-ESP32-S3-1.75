#include "hal_spritesheet.h"
#include "config.h"
#include <SD.h>

bool spritesheet_load(SpriteSheet *sheet, const char *sd_path) {
    if (!sheet) return false;
    spritesheet_free(sheet);

    File f = SD.open(sd_path, FILE_READ);
    if (!f) {
        DEBUG_PRINTF("[spritesheet] open failed: %s\n", sd_path);
        return false;
    }

    uint8_t header[8];
    if (f.read(header, 8) != 8) {
        DEBUG_PRINTF("[spritesheet] short header: %s\n", sd_path);
        f.close();
        return false;
    }
    sheet->frame_w = header[0] | (header[1] << 8);
    sheet->frame_h = header[2] | (header[3] << 8);
    sheet->frame_count = header[4] | (header[5] << 8);

    if (sheet->frame_w == 0 || sheet->frame_h == 0 || sheet->frame_count == 0) {
        DEBUG_PRINTF("[spritesheet] invalid header: %s\n", sd_path);
        f.close();
        return false;
    }

    size_t total_pixels = (size_t)sheet->frame_w * sheet->frame_h * sheet->frame_count;
    sheet->pixels = (uint16_t *)ps_malloc(total_pixels * sizeof(uint16_t));
    if (!sheet->pixels) {
        DEBUG_PRINTF("[spritesheet] PSRAM alloc failed (%u pixels): %s\n", (unsigned)total_pixels, sd_path);
        f.close();
        return false;
    }

    size_t want = total_pixels * sizeof(uint16_t);
    size_t got = f.read((uint8_t *)sheet->pixels, want);
    f.close();
    if (got != want) {
        DEBUG_PRINTF("[spritesheet] short read (%u/%u): %s\n", (unsigned)got, (unsigned)want, sd_path);
        free(sheet->pixels);
        sheet->pixels = nullptr;
        return false;
    }

    sheet->loaded = true;
    DEBUG_PRINTF("[spritesheet] loaded %s: %d frames of %dx%d\n", sd_path,
                 sheet->frame_count, sheet->frame_w, sheet->frame_h);
    return true;
}

void spritesheet_free(SpriteSheet *sheet) {
    if (!sheet) return;
    if (sheet->pixels) { free(sheet->pixels); sheet->pixels = nullptr; }
    sheet->loaded = false;
    sheet->frame_w = sheet->frame_h = sheet->frame_count = 0;
}

void spritesheet_draw(Arduino_GFX *g, const SpriteSheet *sheet, int frame_idx, int x, int y) {
    if (!g || !sheet || !sheet->loaded) return;
    if (frame_idx < 0 || frame_idx >= sheet->frame_count) return;
    size_t offset = (size_t)frame_idx * sheet->frame_w * sheet->frame_h;
    g->draw16bitRGBBitmapWithTranColor(x, y, sheet->pixels + offset, SPRITE_TRANS_KEY,
                                       sheet->frame_w, sheet->frame_h);
}

// Scratch buffer for flipped blits, sized for the largest frame this
// project currently uses (50x50, the boss sheets) - grows on demand if
// a future game's sprites are ever bigger, so this stays reusable
// rather than needing a per-game constant.
static uint16_t *s_flip_scratch = nullptr;
static size_t s_flip_scratch_pixels = 0;

void spritesheet_draw_flipped(Arduino_GFX *g, const SpriteSheet *sheet, int frame_idx, int x, int y) {
    if (!g || !sheet || !sheet->loaded) return;
    if (frame_idx < 0 || frame_idx >= sheet->frame_count) return;

    size_t needed = (size_t)sheet->frame_w * sheet->frame_h;
    if (needed > s_flip_scratch_pixels) {
        if (s_flip_scratch) free(s_flip_scratch);
        s_flip_scratch = (uint16_t *)ps_malloc(needed * sizeof(uint16_t));
        s_flip_scratch_pixels = s_flip_scratch ? needed : 0;
    }
    if (!s_flip_scratch) {
        spritesheet_draw(g, sheet, frame_idx, x, y); // fall back to unflipped rather than draw nothing
        return;
    }

    const uint16_t *src = sheet->pixels + (size_t)frame_idx * sheet->frame_w * sheet->frame_h;
    for (int row = 0; row < sheet->frame_h; row++) {
        const uint16_t *src_row = src + row * sheet->frame_w;
        uint16_t *dst_row = s_flip_scratch + row * sheet->frame_w;
        for (int col = 0; col < sheet->frame_w; col++) {
            dst_row[col] = src_row[sheet->frame_w - 1 - col];
        }
    }
    g->draw16bitRGBBitmapWithTranColor(x, y, s_flip_scratch, SPRITE_TRANS_KEY,
                                       sheet->frame_w, sheet->frame_h);
}
