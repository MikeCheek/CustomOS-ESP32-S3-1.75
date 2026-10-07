#include "hal_tilemap.h"

void tilemap_draw(Arduino_GFX *g, const TileAtlas *atlas, const uint8_t *map,
                   int map_w, int map_h, int cam_x_px, int cam_y_px,
                   int view_x, int view_y, int view_w, int view_h) {
    if (!g || !atlas || !map || !atlas->sheet.loaded) return;
    int tile = atlas->sheet.frame_w; // square tiles - frame_w == frame_h
    if (tile <= 0) return;

    // Which map columns/rows are visible through the view rectangle,
    // with a one-tile margin on each side so partially-visible edge
    // tiles still get drawn (Arduino_GFX's own bitmap draw clips
    // anything past the physical screen bounds already).
    int first_col = cam_x_px / tile - 1;
    int first_row = cam_y_px / tile - 1;
    int last_col = (cam_x_px + view_w) / tile + 1;
    int last_row = (cam_y_px + view_h) / tile + 1;

    if (first_col < 0) first_col = 0;
    if (first_row < 0) first_row = 0;
    if (last_col >= map_w) last_col = map_w - 1;
    if (last_row >= map_h) last_row = map_h - 1;

    for (int row = first_row; row <= last_row; row++) {
        for (int col = first_col; col <= last_col; col++) {
            uint8_t idx = map[row * map_w + col];
            int sx = view_x + (col * tile - cam_x_px);
            int sy = view_y + (row * tile - cam_y_px);
            // Skip tiles that would land entirely outside the view
            // rectangle - a cheap bounds check before the blit, not
            // relying on the display driver to no-op it for us.
            if (sx + tile < view_x || sx > view_x + view_w) continue;
            if (sy + tile < view_y || sy > view_y + view_h) continue;
            spritesheet_draw(g, &atlas->sheet, idx, sx, sy);
        }
    }
}
