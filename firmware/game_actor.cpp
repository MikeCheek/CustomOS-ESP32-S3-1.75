#include "game_actor.h"

void actor_anim_start(ActorAnim *a, SpriteSheet *sheet, int start_frame, int frame_count,
                       uint32_t frame_ms, bool loop) {
    if (!a) return;
    a->sheet = sheet;
    a->start_frame = start_frame;
    a->frame_count = frame_count > 0 ? frame_count : 1;
    a->frame_ms = frame_ms > 0 ? frame_ms : 150;
    a->loop = loop;
    a->current = 0;
    a->last_ms = millis();
    a->finished = false;
}

void actor_anim_update(ActorAnim *a) {
    if (!a || a->finished) return;
    uint32_t now = millis();
    if (now - a->last_ms < a->frame_ms) return;
    a->last_ms = now;
    a->current++;
    if (a->current >= a->frame_count) {
        if (a->loop) {
            a->current = 0;
        } else {
            a->current = a->frame_count - 1;
            a->finished = true;
        }
    }
}

void actor_anim_draw(Arduino_GFX *g, const ActorAnim *a, int x, int y, bool flip_h) {
    if (!a || !a->sheet) return;
    int frame_idx = a->start_frame + a->current;
    if (flip_h) {
        spritesheet_draw_flipped(g, a->sheet, frame_idx, x, y);
    } else {
        spritesheet_draw(g, a->sheet, frame_idx, x, y);
    }
}
