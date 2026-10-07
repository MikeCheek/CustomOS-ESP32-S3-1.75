/*
 * hal_tilemap.h
 * Generic tile atlas + tilemap renderer, reusable for any future
 * top-down/grid-based game, not specific to the dungeon game. A tile
 * atlas is loaded exactly like a sprite sheet (see hal_spritesheet.h) -
 * a tile is just a "sprite" that happens to tile seamlessly - so this
 * reuses that same loader rather than inventing a second format.
 *
 * Rendering draws into a caller-chosen rectangular viewport rather
 * than assuming any particular screen region, since what counts as a
 * "safe" area of this round display is a decision each game makes for
 * itself (see game_breakout.cpp's arena / game_fruitninja.cpp's play
 * field for the established, already-verified-safe rectangle this
 * project reuses for exactly this reason).
 */
#pragma once
#include "hal_spritesheet.h"

struct TileAtlas {
    SpriteSheet sheet;
};

inline bool tileatlas_load(TileAtlas *atlas, const char *sd_path) {
    return spritesheet_load(&atlas->sheet, sd_path);
}
inline void tileatlas_free(TileAtlas *atlas) {
    spritesheet_free(&atlas->sheet);
}

// Draws the portion of `map` (map_w x map_h tile indices, row-major)
// visible through a view rectangle (view_x, view_y, view_w, view_h),
// with the camera scrolled so world pixel (cam_x_px, cam_y_px) lands
// at the view's top-left corner. Tiles are always square, sized by
// the atlas's own frame_w (== frame_h).
void tilemap_draw(Arduino_GFX *g, const TileAtlas *atlas, const uint8_t *map,
                   int map_w, int map_h, int cam_x_px, int cam_y_px,
                   int view_x, int view_y, int view_w, int view_h);
