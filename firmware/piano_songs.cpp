/*
 * piano_songs.cpp
 * The built-in Piano Tiles songs - all public-domain melodies (folk tunes,
 * carols and classical works), transcribed in the format described in
 * piano_songs.h, grouped by genre and ordered roughly by difficulty within
 * each: clearing one unlocks the next. More can be added on the SD card
 * (/piano/*.txt).
 */
#include "piano_songs.h"

const char *const PIANO_GENRES[PG_COUNT] = { "Classical", "Folk & Kids", "Holiday", "Dance & Opera" };

const PianoSong PIANO_SONGS[] = {
    // ---- Classical
    { "Ode to Joy", "L. v. Beethoven", 110, PG_CLASSICAL,
      "E5/4 E5 F5 G5 | G5 F5 E5 D5 | C5 C5 D5 E5 | E5/4. D5/8 D5/2 |"
      "E5/4 E5 F5 G5 | G5 F5 E5 D5 | C5 C5 D5 E5 | D5/4. C5/8 C5/2 |"
      "D5/4 D5 E5 C5 | D5 E5/8 F5 E5/4 C5 | D5 E5/8 F5 E5/4 D5 | C5 D5 G4/2 |"
      "E5/4 E5 F5 G5 | G5 F5 E5 D5 | C5 C5 D5 E5 | D5/4. C5/8 C5/2" },

    { "Lullaby", "J. Brahms", 80, PG_CLASSICAL,
      "E5/8 E5 | G5/2 E5/8 E5 | G5/2 E5/8 G5 | C6/4 B5/4. A5/8 | A5/4 G5 D5/8 E5 |"
      "F5/4 D5 D5/8 E5 | F5/2 D5/8 F5 | B5/8 A5 G5/4 B5 | C6/2. |"
      "C5/8 C5 | C6/2 A5/8 F5 | G5/2 E5/8 C5 | F5/4 G5 A5 | G5/2 C5/8 C5 |"
      "C6/2 A5/8 F5 | G5/2 E5/8 C5 | F5/8 E5 D5/4 C5 | C5/2." },

    { "Minuet in G", "C. Petzold", 120, PG_CLASSICAL,
      "D5/4 G4/8 A4 B4 C5 | D5/4 G4 G4 | E5/4 C5/8 D5 E5 F#5 | G5/4 G4 G4 |"
      "C5/4 D5/8 C5 B4 A4 | B4/4 C5/8 B4 A4 G4 | F#4/4 G4/8 A4 B4 G4 | A4/2. |"
      "D5/4 G4/8 A4 B4 C5 | D5/4 G4 G4 | E5/4 C5/8 D5 E5 F#5 | G5/4 G4 G4 |"
      "C5/4 D5/8 C5 B4 A4 | B4/4 C5/8 B4 A4 G4 | A4/4 B4/8 A4 G4 F#4 | G4/2." },

    { "New World Largo", "A. Dvorak", 60, PG_CLASSICAL,
      "E5/4. G5/8 G5/2 | E5/4. D5/8 C5/2 | D5/4 E5 G5 E5 | D5/1 |"
      "E5/4. G5/8 G5/2 | E5/4. D5/8 C5/2 | D5/4 E5 D5/4. C5/8 | C5/1" },

    { "Swan Lake", "P. I. Tchaikovsky", 84, PG_CLASSICAL,
      "F#5/2 B4/8 C#5 D5 E5 | F#5/4. D5/8 F#5/4. D5/8 | F#5/4. B4/8 D5 B4 G4 D5 | B4/1 |"
      "F#5/2 B4/8 C#5 D5 E5 | F#5/4. D5/8 F#5/4. D5/8 | F#5/4. B4/8 D5 B4 G4 D5 | B4/1 |"
      "C#5/8 D5 E5 F#5 G5/2 | F#5/8 G5 A5/4. G5/8 F#5/4 | E5/8 F#5 G5/4. F#5/8 E5/4 | D5/8 E5 F#5/2. |"
      "F#5/2 B4/8 C#5 D5 E5 | F#5/4. D5/8 F#5/4. D5/8 | F#5/4. B4/8 D5 B4 G4 D5 | B4/1" },

    { "Canon in D", "J. Pachelbel", 76, PG_CLASSICAL,
      "F#5/2 E5 | D5 C#5 | B4 A4 | B4 C#5 |"
      "D5/4 C#5 B4 A4 | G4 F#4 G4 E4 |"
      "D4/8 F#4 A4 G4 F#4 D4 F#4 E4 | D4 B3 D4 A4 G4 B4 A4 G4 |"
      "F#4/8 D4 E4 C#5 D5 F#5 A5 A4 | B4 G4 A4 F#4 D4 D5 D5/4 |"
      "D5/8 C#5 D5 D4 C#4 A4 E4 F#4 | D4 D5 C#5 B4 C#5 F#5 A5 B5 |"
      "G5/8 F#5 E5 G5 F#5 E5 D5 C#5 | B4 A4 G4 F#4 E4 G4 F#4 E4 | D4/2." },

    { "Spring", "A. Vivaldi", 100, PG_CLASSICAL,
      "E5/4 | G#5/8 G#5 G#5 F#5/16 E5 B5/4. B5/16 A5 | G#5/8 G#5 G#5 F#5/16 E5 B5/4. B5/16 A5 |"
      "G#5/8 A5 B5 A5 G#5/4 F#5 | E5/2. R/4 |"
      "G#5/8 G#5 G#5 F#5/16 E5 B5/4. B5/16 A5 | G#5/8 G#5 G#5 F#5/16 E5 B5/4. B5/16 A5 |"
      "G#5/8 A5 B5 A5 G#5/4 F#5 | E5/2." },

    { "Blue Danube", "J. Strauss II", 160, PG_CLASSICAL,
      "D5/4 D5 F#5 | A5/4 R/2 | A5/4 R A5 | F#5 R F#5 |"
      "D5/4 D5 F#5 | A5/4 R/2 | B5/4 R B5 | G5 R G5 |"
      "C#5/4 C#5 E5 | B5/4 R/2 | B5/4 R B5 | G5 R G5 |"
      "C#5/4 C#5 E5 | B5/4 R/2 | B5/4 R B5 | F#5 R F#5 |"
      "D5/4 D5 F#5 | A5/4 D6/2 | D6/4 R D6 | A5 R A5 |"
      "D5/4 D5 F#5 | A5/4 D6/2 | D6/4 R D6 | B5 R B5 | A5/2." },

    { "Morning Mood", "E. Grieg", 90, PG_CLASSICAL,
      "G5/8 E5 D5 C5 D5 E5 | G5 E5 D5 C5 D5/16 E5 D5/8 E5 |"
      "G5/8 E5 G5 A5 E5 A5 | G5 E5 D5 C5/4. |"
      "G5/8 E5 D5 C5 D5 E5 | G5 E5 D5 C5 D5/16 E5 D5/8 E5 |"
      "G5/8 E5 G5 A5 E5 A5 | B5 G#5 F#5 E5/4." },

    { "Fur Elise", "L. v. Beethoven", 72, PG_CLASSICAL,
      "E5/16 D#5 | E5 D#5 E5 B4 D5 C5 | A4/8 R/16 C4 E4 A4 | B4/8 R/16 E4 G#4 B4 |"
      "C5/8 R/16 E4 E5 D#5 | E5 D#5 E5 B4 D5 C5 | A4/8 R/16 C4 E4 A4 | B4/8 R/16 E4 C5 B4 |"
      "A4/8 R/16 B4 C5 D5 | E5/8. G4/16 F5 E5 | D5/8. F4/16 E5 D5 | C5/8. E4/16 D5 C5 |"
      "B4/8 R/16 E4 E5 E4 | E5 E5 E6 D#5 E5 D#5 | E5 D#5 E5 D#5 E5 D#5 |"
      "E5 D#5 E5 B4 D5 C5 | A4/8 R/16 C4 E4 A4 | B4/8 R/16 E4 C5 B4 | A4/4." },

    { "Symphony No. 5", "L. v. Beethoven", 108, PG_CLASSICAL,
      "R/8 G5/8 G5 G5 | Eb5/1 | R/8 F5/8 F5 F5 | D5/1 |"
      "R/8 G5/8 G5 G5 | Eb5 Ab5 Ab5 Ab5 | G5 Eb6 Eb6 Eb6 | C6/2 R/8 G5/8 G5 G5 |"
      "D5 Ab5 Ab5 Ab5 | G5 F6 F6 F6 | D6/2. R/4 | G6/8 G6 F6 Eb6/2. R/4" },

    { "Eine kleine Nachtmusik", "W. A. Mozart", 120, PG_CLASSICAL,
      "G5/4 R/8 D5/8 G5/4 R/8 D5/8 | G5/8 D5 G5 B5 D6/4 R/4 |"
      "C6/4 R/8 A5/8 C6/4 R/8 A5/8 | C6/8 A5 F#5 A5 D5/4 R/4 |"
      "G5/4 G5/8. B5/16 A5/8. G5/16 G5/8. F#5/16 | F#5/4 A5/8. C6/16 F#5/8. A5/16 G5/8. B5/16 |"
      "A5/4 G5/8. B5/16 A5/8. G5/16 G5/8. F#5/16 | F#5/4 A5/8. C6/16 F#5/8. A5/16 G5/4 |"
      "G5/4 R/8 D5/8 G5/4 R/8 D5/8 | G5/8 D5 G5 B5 D6/4 R/4 | G5/2" },

    { "Mountain King", "E. Grieg", 120, PG_CLASSICAL,
      "B4/8 C#5 D5 E5 | F#5 D5 F#5/4 | F5/8 C#5 F5/4 | E5/8 C5 E5/4 |"
      "B4/8 C#5 D5 E5 | F#5 D5 F#5 B5 | A5 F#5 D5 F#5 | A5/2 |"
      "F#5/8 G#5 A#5 B5 | C#6 A#5 C#6/4 | D6/8 A#5 D6/4 | C#6/8 A#5 C#6/4 |"
      "F#5/8 G#5 A#5 B5 | C#6 A#5 C#6/4 | D6/8 A#5 D6/4 | C#6/2 |"
      "B4/8 C#5 D5 E5 | F#5 D5 F#5/4 | F5/8 C#5 F5/4 | E5/8 C5 E5/4 |"
      "B4/8 C#5 D5 E5 | F#5 D5 F#5 B5 | A5 F#5 D5 F#5 | B5/2" },

    { "Moonlight Sonata", "L. v. Beethoven", 56, PG_CLASSICAL,
      "G#4/12 C#5 E5 G#4 C#5 E5 G#4 C#5 E5 G#4 C#5 E5 |"
      "G#4 C#5 E5 G#4 C#5 E5 G#4 C#5 E5 G#4 C#5 E5 |"
      "A4 C#5 E5 A4 C#5 E5 A4 D5 F#5 A4 D5 F#5 |"
      "G#4 C5 F#5 G#4 C#5 E5 G#4 C#5 D#5 F#4 C5 D#5 |"
      "E4 G#4 C#5 G#4 C#5 E5 G#4 C#5 E5 G#4 C#5 E5 |"
      "G#4 C#5 E5 G#4 C#5 E5 G#4 C#5 E5 G#4 C#5 E5 |"
      "A4 C#5 E5 A4 C#5 E5 A4 D5 F#5 A4 D5 F#5 |"
      "G#4 C5 F#5 G#4 C#5 E5 G#4 C5 D#5 C#5/2." },

    { "William Tell Overture", "G. Rossini", 150, PG_CLASSICAL,
      "G4/8 G4/16 G4 G4/8 G4/16 G4 | G4/8 G4/16 G4 C5/4 | D5/4 E5/4 |"
      "G4/8 G4/16 G4 G4/8 G4/16 G4 | C5/8 E5/16 E5 D5/4 | B4/4 G4/4 |"
      "G4/8 G4/16 G4 G4/8 G4/16 G4 | G4/8 G4/16 G4 C5/4 | D5/4 E5/4 |"
      "C5/8 E5/16 E5 G5/8 E5 | D5/8 B4 G4/4 | C5/2 |"
      "E5/8 E5/16 E5 E5/8 E5 | F5/8 F5/16 F5 F5/8 F5 | G5/8 G5/16 G5 A5/8 G5 | F5/4 D5/4 |"
      "E5/8 E5/16 E5 E5/8 E5 | F5/8 F5/16 F5 F5/8 F5 | G5/8 A5 B5 D6 | C6/2" },

    { "Turkish March", "W. A. Mozart", 120, PG_CLASSICAL,
      "B4/16 A4 G#4 A4 C5/8 R | D5/16 C5 B4 C5 E5/8 R | F5/16 E5 D#5 E5 B5 A5 G#5 A5 | B5 A5 G#5 A5 C6/4 |"
      "A5/8 C6 B5 A5 | G5 A5 B5 A5 | G5 A5 B5 A5 | G5 F#5 E5/4 |"
      "E5/8 F5 G5 G5 | A5/16 G5 F5 E5 D5/4 | E5/8 F5 G5 G5 | A5/16 G5 F5 E5 D5/4 |"
      "C5/8 D5 E5 E5 | F5/16 E5 D5 C5 B4/4 | C5/8 D5 E5 E5 | F5/16 E5 D5 C5 B4/4 |"
      "B4/16 A4 G#4 A4 C5/8 R | D5/16 C5 B4 C5 E5/8 R | F5/16 E5 D#5 E5 B5 A5 G#5 A5 | B5 A5 G#5 A5 C6/4 |"
      "A5/8 B5 C6 B5 | A5 G#5 A5 E5 | F5 D5 C5 B4 | A4/4" },

    { "Flight of the Bumblebee", "N. Rimsky-Korsakov", 110, PG_CLASSICAL,
      "E6/16 D#6 D6 C#6 D6 C#6 C6 B5 | C6 B5 A#5 A5 G#5 G5 F#5 F5 |"
      "E5 D#5 D5 C#5 D5 C#5 C5 B4 | C5 B4 A#4 A4 G#4 G4 F#4 F4 |"
      "E4 D#4 E4 F4 E4 D#4 E4 F4 | E4 D#4 E4 F4 E4 D#4 E4 F4 |"
      "E4 D#4 D4 C#4 D4 D#4 E4 F4 | E4 D#4 D4 C#4 D4 D#4 E4 F4 |"
      "E5 D#5 E5 F5 E5 D#5 E5 F5 | E5 D#5 D5 C#5 D5 D#5 E5 F5 |"
      "G5 F#5 G5 G#5 A5 A#5 B5 C6 | C#6 D6 D#6 E6 F6 F#6 G6 G#6 | A6/4 R/4 E6/4" },

    // ---- Folk & Kids
    { "Twinkle Twinkle", "Traditional", 100, PG_FOLK,
      "C5/4 C5 G5 G5 | A5 A5 G5/2 | F5/4 F5 E5 E5 | D5 D5 C5/2 |"
      "G5/4 G5 F5 F5 | E5 E5 D5/2 | G5/4 G5 F5 F5 | E5 E5 D5/2 |"
      "C5/4 C5 G5 G5 | A5 A5 G5/2 | F5/4 F5 E5 E5 | D5 D5 C5/2" },

    { "Mary Had a Little Lamb", "Traditional", 110, PG_FOLK,
      "E5/4 D5 C5 D5 | E5 E5 E5/2 | D5/4 D5 D5/2 | E5/4 G5 G5/2 |"
      "E5/4 D5 C5 D5 | E5 E5 E5 E5 | D5 D5 E5 D5 | C5/1" },

    { "Row Row Row Your Boat", "Traditional", 150, PG_FOLK,
      "C5/4. C5/4. | C5/4 D5/8 E5/4. | E5/4 D5/8 E5/4 F5/8 | G5/2. |"
      "C6/8 C6 C6 G5 G5 G5 | E5 E5 E5 C5 C5 C5 | G5/4 F5/8 E5/4 D5/8 | C5/2." },

    { "Frere Jacques", "Traditional", 120, PG_FOLK,
      "C5/4 D5 E5 C5 | C5 D5 E5 C5 | E5 F5 G5/2 | E5/4 F5 G5/2 |"
      "G5/8 A5 G5 F5 E5/4 C5 | G5/8 A5 G5 F5 E5/4 C5 | C5/4 G4 C5/2 | C5/4 G4 C5/2" },

    { "London Bridge", "Traditional", 120, PG_FOLK,
      "G5/4. A5/8 G5/4 F5 | E5 F5 G5/2 | D5/4 E5 F5/2 | E5/4 F5 G5/2 |"
      "G5/4. A5/8 G5/4 F5 | E5 F5 G5/2 | D5/2 G5/2 | E5/4 C5/2." },

    { "Old MacDonald", "Traditional", 130, PG_FOLK,
      "G5/4 G5 G5 D5 | E5 E5 D5/2 | B5/4 B5 A5 A5 | G5/2. D5/4 |"
      "G5/4 G5 G5 D5 | E5 E5 D5/2 | B5/4 B5 A5 A5 | G5/1" },

    { "Happy Birthday", "Traditional", 100, PG_FOLK,
      "G4/8. G4/16 | A4/4 G4 C5 | B4/2 G4/8. G4/16 | A4/4 G4 D5 | C5/2 G4/8. G4/16 |"
      "G5/4 E5 C5 | B4 A4 F5/8. F5/16 | E5/4 C5 D5 | C5/2." },

    { "Yankee Doodle", "Traditional", 130, PG_FOLK,
      "C5/8 C5 D5 E5 C5 E5 D5 G4 | C5 C5 D5 E5 C5/4 B4 |"
      "C5/8 C5 D5 E5 F5 E5 D5 C5 | B4 G4 A4 B4 C5/4 C5 |"
      "A4/8. B4/16 A4/8 G4 A4 B4 C5/4 | G4/8. A4/16 G4/8 F4 E4/4 G4 |"
      "A4/8. B4/16 A4/8 G4 A4 B4 C5 A4 | G4 C5 B4 D5 C5/4 C5" },

    { "Oh! Susanna", "S. Foster", 120, PG_FOLK,
      "C5/8 D5 | E5/4 G5 G5/4. A5/8 | G5/4 E5 C5/4. D5/8 | E5/4 E5 D5 C5 | D5/2. C5/8 D5 |"
      "E5/4 G5 G5/4. A5/8 | G5/4 E5 C5/4. D5/8 | E5/4 E5 D5 D5 | C5/2. R/4 |"
      "F5/2 F5/2 | A5/4 A5/2 A5/4 | G5/4 G5 E5 C5 | D5/2. C5/8 D5 |"
      "E5/4 G5 G5/4. A5/8 | G5/4 E5 C5/4. D5/8 | E5/4 E5 D5 D5 | C5/2." },

    { "Amazing Grace", "Traditional", 80, PG_FOLK,
      "G4/4 | C5/2 E5/8 C5 | E5/2 D5/4 | C5/2 A4/4 | G4/2 G4/4 |"
      "C5/2 E5/8 C5 | E5/2 D5/4 | G5/2. | G5/2 E5/8 G5 |"
      "G5/8 E5 C5/2 | G4/2 A4/8 C5 | C5/8 A4 G4/2 | G4/2 G4/4 |"
      "C5/2 E5/8 C5 | E5/2 D5/4 | C5/2." },

    { "Auld Lang Syne", "Traditional", 90, PG_FOLK,
      "C5/4 | F5/4. F5/8 F5/4 A5 | G5/4. F5/8 G5/4 A5/8 G5 | F5/4. F5/8 A5/4 C6 | D6/2. D6/4 |"
      "C6/4. A5/8 A5/4 F5 | G5/4. F5/8 G5/4 A5/8 G5 | F5/4. D5/8 D5/4 C5 | F5/2." },

    { "Greensleeves", "Traditional", 100, PG_FOLK,
      "A4/8 | C5/4 D5/8 E5/8. F5/16 E5/8 | D5/4 B4/8 G4/8. A4/16 B4/8 | C5/4 A4/8 A4/8. G#4/16 A4/8 |"
      "B4/4 G#4/8 E4/4 A4/8 | C5/4 D5/8 E5/8. F5/16 E5/8 | D5/4 B4/8 G4/8. A4/16 B4/8 |"
      "C5/8. B4/16 A4/8 G#4/8. F#4/16 G#4/8 | A4/4. A4/4. |"
      "G5/4. G5/8. F#5/16 E5/8 | D5/4 B4/8 G4/8. A4/16 B4/8 | C5/4 A4/8 A4/8. G#4/16 A4/8 |"
      "B4/4 G#4/8 E4/4. | G5/4. G5/8. F#5/16 E5/8 | D5/4 B4/8 G4/8. A4/16 B4/8 |"
      "C5/8. B4/16 A4/8 G#4/8. F#4/16 G#4/8 | A4/4. A4/4." },

    { "Scarborough Fair", "Traditional", 120, PG_FOLK,
      "D5/2 D5/4 | A5/2 A5/4 | E5/4. F5/8 E5/4 | D5/2. | R/4 A5/4 C6/4 |"
      "D6/2 C6/4 | A5/4 B5 G5 | A5/2. | R/4 D6/2 | D6/2 C6/4 | A5/2 A5/4 |"
      "G5/4 F5/2 | E5/4 C5/2 | D5/2 A5/4 | G5/2 F5/4 | E5/4 D5 C5 | D5/2." },

    // ---- Holiday
    { "Silent Night", "F. X. Gruber", 100, PG_HOLIDAY,
      "G5/4. A5/8 G5/4 E5/2. | G5/4. A5/8 G5/4 E5/2. | D6/2 D6/4 B5/2. | C6/2 C6/4 G5/2. |"
      "A5/2 A5/4 C6/4. B5/8 A5/4 | G5/4. A5/8 G5/4 E5/2. | A5/2 A5/4 C6/4. B5/8 A5/4 |"
      "G5/4. A5/8 G5/4 E5/2. | D6/2 D6/4 F6/4. D6/8 B5/4 | C6/2. E6/2. |"
      "C6/4 G5 E5 G5/4. F5/8 D5/4 | C5/2." },

    { "Jingle Bells", "J. Pierpont", 130, PG_HOLIDAY,
      "B4/4 B4 B4/2 | B4/4 B4 B4/2 | B4/4 D5 G4/4. A4/8 | B4/1 |"
      "C5/4 C5 C5/4. C5/8 | C5/4 B4 B4 B4/8 B4 | B4/4 A4 A4 B4 | A4/2 D5/2 |"
      "B4/4 B4 B4/2 | B4/4 B4 B4/2 | B4/4 D5 G4/4. A4/8 | B4/1 |"
      "C5/4 C5 C5 C5 | C5 B4 B4 B4/8 B4 | D5/4 D5 C5 A4 | G4/1" },

    { "We Wish You a Merry Christmas", "Traditional", 150, PG_HOLIDAY,
      "D5/4 | G5/4 G5/8 A5 G5 F#5 | E5/4 E5 E5 | A5/4 A5/8 B5 A5 G5 | F#5/4 D5 D5 |"
      "B5/4 B5/8 C6 B5 A5 | G5/4 E5 D5/8 D5 | E5/4 A5 F#5 | G5/2. |"
      "D5/4 | G5/4 G5 G5 | F#5/2 F#5/4 | G5/4 F#5 E5 | D5/2 A5/4 |"
      "B5/4 A5/8 A5 G5 G5 | D6/4 D5 D5/8 D5 | E5/4 A5 F#5 | G5/2." },

    { "Joy to the World", "G. F. Handel", 100, PG_HOLIDAY,
      "C6/4 B5/8. A5/16 G5/4. F5/8 | E5/4 D5 C5/4. G5/8 | A5/4. A5/8 B5/4. B5/8 | C6/2. C6/8 C6 |"
      "B5/8 A5 G5 G5/8. F5/16 E5/8 C6 C6 B5 | A5 G5 G5/8. F5/16 E5/8 E5 E5 E5 |"
      "E5/16 F5 G5/4. F5/16 E5 D5/8 D5 D5 | D5/16 E5 F5/4. E5/16 D5 C5/8 C6/4 A5/8 |"
      "G5/8. F5/16 E5/8 F5 E5/4 D5 | C5/2." },

    { "Deck the Halls", "Traditional", 130, PG_HOLIDAY,
      "D6/4. C6/8 B5/4 A5 | G5 A5 B5 G5 | A5/8 B5 C6 A5 B5/4. A5/8 | G5/4 F#5 G5/2 |"
      "D6/4. C6/8 B5/4 A5 | G5 A5 B5 G5 | A5/8 B5 C6 A5 B5/4. A5/8 | G5/4 F#5 G5/2 |"
      "A5/4. B5/8 C6/4 A5 | B5/4. C6/8 D6/4 A5 | B5/8 C#6 D6/4 E6/8 F#6 G6/4 | F#6 E6 D6/2 |"
      "D6/4. C6/8 B5/4 A5 | G5 A5 B5 G5 | E6/8 E6 E6 E6 D6/4. C6/8 | B5/4 A5 G5/2" },

    // ---- Dance & Opera
    { "Bridal Chorus", "R. Wagner", 72, PG_DANCE,
      "C5/4 F5/8. F5/16 F5/2 | C5/4 G5/8. E5/16 F5/2 | C5/4 F5/8. Bb5/16 Bb5/4 A5/8. G5/16 |"
      "F5/8. E5/16 F5/8. G5/16 C5/2 | C5/4 F5/8. F5/16 F5/2 | C5/4 G5/8. E5/16 F5/2 |"
      "C5/4 F5/8. A5/16 C6/4 A5/8. F5/16 | D5/4 F5/8. G5/16 F5/2" },

    { "When the Saints", "Traditional", 150, PG_DANCE,
      "C5/4 E5 F5 | G5/1 | R/4 C5 E5 F5 | G5/1 | R/4 C5 E5 F5 | G5/2 E5/2 | C5/2 E5/2 | D5/1 |"
      "R/4 E5 E5 D5 | C5/2. C5/4 | E5/2 G5/2 | G5/4 F5/2. | R/4 E5 F5 G5 | E5/2 C5/2 | D5/2 C5/2" },

    { "Toreador Song", "G. Bizet", 100, PG_DANCE,
      "C5/4. D5/8 C5/4 A4 | A4/4 A4/8 G4 A4 Bb4 A4/4 | Bb4/2. G4/4 | C5/4. A4/8 F4/2 |"
      "D4/4 G4/8 D4 C4/2 | R/4 G4/8 A4 Bb4/4. A4/8 | G4/4 D5 C5/2 |"
      "C5/4. D5/8 C5/4 A4 | A4/4 A4/8 G4 A4 Bb4 A4/4 | Bb4/2. G4/4 | C5/4. A4/8 F4/2" },

    { "Habanera", "G. Bizet", 100, PG_DANCE,
      "D5/4. C#5/8 | C5/4 C5/8 B4 | Bb4/4. A4/8 | Ab4/8 G4 R/4 |"
      "F4/8 F4 E4 F4 | G4 F4 E4/4 | D4/4. R/8 | R/2 |"
      "A4/4. G#4/8 | G4/4 G4/8 F#4 | F4/4. E4/8 | Eb4/8 D4 R/4 |"
      "C4/8 D4 E4 F4 | G4 A4 Bb4/4 | A4/2 | R/2 |"
      "D5/4. C#5/8 | C5/4 C5/8 B4 | Bb4/4. A4/8 | Ab4/8 G4 R/4 |"
      "F4/8 F4 E4 F4 | G4 F4 E4/4 | D4/2." },

    { "La Cucaracha", "Traditional", 160, PG_DANCE,
      "C5/8 C5 C5 F5/4. A5/8 | R/8 C5 C5 C5 F5/4. A5/8 | R/4 F5/8 F5 E5 E5 D5 D5 | C5/2. R/4 |"
      "C5/8 C5 C5 E5/4. G5/8 | R/8 C5 C5 C5 E5/4. G5/8 | R/4 C6/8 D6 C6 Bb5 A5 G5 | F5/2. R/4" },

    { "Can-Can", "J. Offenbach", 140, PG_DANCE,
      "C5/4 D5/8 F5 E5 D5 | G5/4 G5 | G5/8 A5 E5 F5 | D5/4 D5 |"
      "D5/8 F5 E5 D5 | C5 C6 B5 A5 | G5 F5 E5 D5 |"
      "C5/4 D5/8 F5 E5 D5 | G5/4 G5 | G5/8 A5 E5 F5 | D5/4 D5 |"
      "D5/8 F5 E5 D5 | C5 G5 D5 E5 | C5/2 |"
      "G5/8 G5 G5 G5 | A5 F5 R/4 | F5/8 F5 F5 A5 | G5 E5 R/4 |"
      "E5/8 E5 E5 G5 | F5 D5 R/4 | D5/8 F5 E5 D5 | C5/2" },

    { "The Entertainer", "S. Joplin", 80, PG_DANCE,
      "D5/16 D#5 | E5 C6/8 E5/16 C6/8 E5/16 C6/4. | C6/16 D6 D#6 E6 C6 D6 E6/8 B5/16 D6/8 C6/4 R/16 |"
      "D5/16 D#5 | E5 C6/8 E5/16 C6/8 E5/16 C6/4. | A5/16 G5 F#5 A5 C6 E6/8 D6/16 C6 A5 D6/4 R/8 |"
      "D5/16 D#5 | E5 C6/8 E5/16 C6/8 E5/16 C6/4. | C6/16 D6 D#6 E6 C6 D6 E6/8 B5/16 D6/8 C6/4 R/8 |"
      "C6/16 D6 E6 C6 D6 E6/8 C6/16 D6 C6 E6 C6 D6 E6/8 | C6/16 D6 C6 E6 C6 D6 E6/8 B5/16 D6/8 C6/4" },

};

const int PIANO_SONG_COUNT = sizeof(PIANO_SONGS) / sizeof(PIANO_SONGS[0]);
