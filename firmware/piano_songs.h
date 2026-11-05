/*
 * piano_songs.h
 * Songs for Piano Tiles (game_piano.cpp), in a small text note format
 * the game turns into timed tiles.
 *
 * Format - whitespace-separated tokens, bars ("|") ignored:
 *
 *   C4  D#5  Bb3          a note: name, optional # or b, octave (C4 = middle C)
 *   C4+E4+G4              a chord (one tile, the top note sounds)
 *   R                     a rest (a gap between tiles)
 *   .../8   .../4.        a length: 1 whole, 2 half, 4 quarter, 8 eighth,
 *                         16 sixteenth, 32; 3/6/12/24 are triplets
 *                         (12 = triplet eighth); a trailing "." dots it
 *
 * A token without "/len" keeps the previous length, so a run of eighths
 * is "E5/8 D5 C5 B4". The tempo is the song's bpm in quarter notes.
 *
 * The same format works as a text file on the SD card - see
 * piano_load_sd_songs() - and tools/midi_to_piano.py writes it from a
 * MIDI file's melody.
 */
#pragma once
#include <stdint.h>

enum PianoGenre : uint8_t { PG_CLASSICAL, PG_FOLK, PG_HOLIDAY, PG_DANCE, PG_COUNT };
extern const char *const PIANO_GENRES[PG_COUNT];

struct PianoSong {
    const char *title;
    const char *composer;
    uint16_t bpm;
    uint8_t genre;            // PianoGenre
    const char *notes;
};

// In each genre, ordered roughly by difficulty: clearing one unlocks the next.
extern const PianoSong PIANO_SONGS[];
extern const int PIANO_SONG_COUNT;
