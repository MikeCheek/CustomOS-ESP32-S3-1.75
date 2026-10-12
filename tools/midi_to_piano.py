#!/usr/bin/env python3
"""
midi_to_piano.py - turn a MIDI file's melody into a Piano Tiles song.

    python3 tools/midi_to_piano.py song.mid > song.txt
    python3 tools/midi_to_piano.py song.mid --track 2 --title "My Song" > song.txt

Copy the .txt into /piano/ on the watch's SD card (USB File storage, or the
companion app's file transfer) and it shows up at the end of the song list.

The melody is the highest note at each moment of the chosen track (by
default the track with the most notes in a singing range). Onsets are
snapped to a sixteenth-note grid, each note lasts until the next one
(capped at a whole note, with rests for longer gaps), and the result is
moved by octaves into the range a small speaker plays well. Format: see
firmware/piano_songs.h. No third-party modules needed.

MIDI files of public-domain music are freely available, e.g. from the
Mutopia Project (mutopiaproject.org) - only convert music you may use.
"""
import argparse
import os
import struct
import sys

NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def read_var(data, i):
    v = 0
    while True:
        b = data[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, i


def parse_midi(path):
    data = open(path, "rb").read()
    if data[:4] != b"MThd":
        sys.exit("not a MIDI file")
    hlen, fmt, ntrk, div = struct.unpack(">IHHH", data[4:14])
    if div & 0x8000:
        sys.exit("SMPTE time division isn't supported")
    i = 8 + hlen
    tracks, tempo = [], None
    while i < len(data) and len(tracks) < ntrk:
        tag, tlen = data[i:i + 4], struct.unpack(">I", data[i + 4:i + 8])[0]
        i += 8
        end = i + tlen
        if tag != b"MTrk":
            i = end
            continue
        t, status, notes, on, name = 0, 0, [], {}, ""
        while i < end:
            dt, i = read_var(data, i)
            t += dt
            b = data[i]
            if b & 0x80:
                status = b
                i += 1
            if status == 0xFF:
                kind, ln = data[i], None
                ln, i = read_var(data, i + 1)
                if kind == 0x51 and tempo is None:
                    tempo = int.from_bytes(data[i:i + 3], "big")
                elif kind == 0x03:
                    name = data[i:i + ln].decode("latin-1", "replace")
                i += ln
            elif status in (0xF0, 0xF7):
                ln, i = read_var(data, i)
                i += ln
            else:
                hi = status & 0xF0
                if hi in (0xC0, 0xD0):
                    i += 1
                    continue
                a, b2 = data[i], data[i + 1]
                i += 2
                ch = status & 0x0F
                if hi == 0x90 and b2 > 0 and ch != 9:
                    on[(ch, a)] = t
                elif hi == 0x80 or (hi == 0x90 and b2 == 0):
                    s = on.pop((ch, a), None)
                    if s is not None:
                        notes.append((s, t, a))
        tracks.append((name, sorted(notes)))
        i = end
    bpm = round(60_000_000 / tempo) if tempo else 120
    return div, bpm, tracks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("midi")
    ap.add_argument("--track", type=int, help="track number (default: the best melody guess)")
    ap.add_argument("--title")
    ap.add_argument("--composer", default="")
    ap.add_argument("--bpm", type=int, help="override the tempo")
    ap.add_argument("--max-notes", type=int, default=400)
    a = ap.parse_args()

    div, bpm, tracks = parse_midi(a.midi)
    if a.track is not None:
        notes = tracks[a.track][1]
    else:
        def score(tr):
            ns = tr[1]
            return sum(1 for n in ns if 55 <= n[2] <= 88)
        best = max(range(len(tracks)), key=lambda k: score(tracks[k]))
        notes = tracks[best][1]
        print("# track %d (%s)" % (best, tracks[best][0] or "unnamed"), file=sys.stderr)
    if not notes:
        sys.exit("no notes in that track")

    grid = max(1, div // 4)                          # a sixteenth
    top = {}
    for s, e, n in notes:                            # highest note per onset
        q = round(s / grid)
        top[q] = max(top.get(q, 0), n)
    onsets = sorted(top)[: a.max_notes]
    mel = [top[q] for q in onsets]
    avg = sum(mel) / len(mel)
    shift = 12 * round((72 - avg) / 12)               # centre around C5
    out, prev_end = [], onsets[0]
    for k, q in enumerate(onsets):
        if q > prev_end:
            out.append(("R", q - prev_end))
        nxt = onsets[k + 1] if k + 1 < len(onsets) else q + 4
        ln = min(nxt - q, 16)
        m = max(48, min(96, mel[k] + shift))
        out.append(("%s%d" % (NAMES[m % 12], m // 12 - 1), ln))
        prev_end = q + ln
    # lengths in sixteenths -> tokens ("/16", "/8", "/8." ...)
    LEN = {1: "16", 2: "8", 3: "8.", 4: "4", 6: "4.", 8: "2", 12: "2.", 16: "1"}
    toks, last = [], None
    for name, ln in out:
        while ln > 0:
            part = max(p for p in LEN if p <= ln)
            ln -= part
            tok = name if LEN[part] == last else "%s/%s" % (name, LEN[part])
            last = LEN[part]
            toks.append(tok)
            if name != "R":
                name = "R"                           # the rest of a split note is silence

    title = a.title or os.path.splitext(os.path.basename(a.midi))[0].replace("_", " ")
    print("title: %s" % title)
    if a.composer:
        print("composer: %s" % a.composer)
    print("bpm: %d" % (a.bpm or bpm))
    for k in range(0, len(toks), 16):
        print(" ".join(toks[k:k + 16]))


if __name__ == "__main__":
    main()
