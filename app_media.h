#pragma once
#include "ui.h"

extern Screen media_screen;
extern Screen media_audio_screen;
extern Screen media_text_screen;

// Opens an image (or video) in the Gallery viewer, with its folder's
// other media a swipe away.
void media_navigate_to(const char *path);

// Music app: play `list` from `pos` in `order` (nullptr = list order;
// n entries), with previous/next and auto-advance in the player.
struct MediaList;
void media_play_playlist(const MediaList *list, const int *order, int n, int pos);
void media_playlist_clear();
// Path of the song loaded in the player (playing or paused), else nullptr.
const char *media_now_playing_path();
