/*
 * game_actor.h
 * Generic animated-actor timing/playback helper. Reusable for
 * players, enemies, and NPCs in ANY future sprite-based game, not
 * specific to the ninja/dungeon game - it only knows about frame
 * ranges within a SpriteSheet and timing, nothing about health,
 * combat, or AI.
 */
#pragma once
#include "hal_spritesheet.h"

struct ActorAnim {
    SpriteSheet *sheet = nullptr;
    int start_frame = 0;
    int frame_count = 1;
    uint32_t frame_ms = 150;
    bool loop = true;
    int current = 0;     // 0-based within [0, frame_count)
    uint32_t last_ms = 0;
    bool finished = false; // true once a non-looping animation completes
};

// (Re)starts an animation from its first frame. Call this whenever the
// actor switches action (e.g. idle -> attack), not every tick.
void actor_anim_start(ActorAnim *a, SpriteSheet *sheet, int start_frame, int frame_count,
                       uint32_t frame_ms, bool loop);

// Advances the animation's current frame based on elapsed time - call
// once per game tick.
void actor_anim_update(ActorAnim *a);

// Draws the animation's current frame at (x,y), the world/screen
// position of the actor's sprite origin (top-left, matching
// spritesheet_draw's own convention).
void actor_anim_draw(Arduino_GFX *g, const ActorAnim *a, int x, int y, bool flip_h);
