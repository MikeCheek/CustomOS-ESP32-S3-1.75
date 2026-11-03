/// Piano Tiles songs from a hummed tune or a MIDI file, in the watch's text
/// format (firmware/piano_songs.h), sent to /piano/ on its SD card.
///
///  - Humming is one note at a time, so a classic pitch tracker does it
///    well and needs no model: YIN (de Cheveigné & Kawahara, 2002) finds the
///    pitch every 10 ms, steady stretches become notes, the tempo comes from
///    how far apart they start.
///  - MIDI: the same melody pick as tools/midi_to_piano.py (highest note per
///    onset of the track with the most notes in a singing range).
/// Pure Dart, tested in test/piano_song_test.dart.
library;

import 'dart:math' as math;
import 'dart:typed_data';

/// A note heard in a recording: seconds, MIDI number.
class HeardNote {
  final double start, end;
  final int midi;
  const HeardNote(this.start, this.end, this.midi);
  @override
  String toString() => 'HeardNote($start-$end, $midi)';
}

/// A melody on a sixteenth-note grid: onset (in sixteenths) -> MIDI note.
class GridMelody {
  final int bpm;
  final List<int> onsets; // sorted, sixteenths
  final List<int> notes; // MIDI
  const GridMelody(this.bpm, this.onsets, this.notes);
  int get length => notes.length;
}

// ---- pitch tracking ------------------------------------------------------------------

const _frame = 1024; // 64 ms at 16 kHz: room for two periods of 80 Hz
const _hopSec = 0.01;

/// The pitch (Hz) of one frame with YIN, or 0 when it isn't a clear tone.
double yinPitch(Float32List x, int off, int n, int rate, {double minHz = 80, double maxHz = 1000}) {
  final maxTau = math.min(n ~/ 2, (rate / minHz).ceil());
  final minTau = math.max(2, (rate / maxHz).floor());
  final w = n - maxTau;
  final d = Float64List(maxTau + 1);
  for (var tau = 1; tau <= maxTau; tau++) {
    var s = 0.0;
    for (var i = 0; i < w; i++) {
      final v = x[off + i] - x[off + i + tau];
      s += v * v;
    }
    d[tau] = s;
  }
  // cumulative mean normalised difference
  final c = Float64List(maxTau + 1)..[0] = 1;
  var run = 0.0;
  for (var tau = 1; tau <= maxTau; tau++) {
    run += d[tau];
    c[tau] = run == 0 ? 1 : d[tau] * tau / run;
  }
  var best = -1;
  for (var tau = minTau; tau < maxTau; tau++) {
    if (c[tau] < 0.15) {
      while (tau + 1 < maxTau && c[tau + 1] < c[tau]) {
        tau++;
      }
      best = tau;
      break;
    }
  }
  if (best < 0) return 0;
  // parabolic interpolation around the dip
  var t = best.toDouble();
  if (best > 1 && best < maxTau) {
    final a = c[best - 1], b = c[best], cc = c[best + 1];
    final den = a - 2 * b + cc;
    if (den.abs() > 1e-12) t += 0.5 * (a - cc) / den;
  }
  return rate / t;
}

double _midiOf(double hz) => 69 + 12 * math.log(hz / 440) / math.ln2;

double _median(List<double> v) {
  final s = [...v]..sort();
  return s[s.length ~/ 2];
}

/// The notes in a hummed / sung / whistled recording (mono, any rate).
List<HeardNote> humToNotes(Float32List x, int rate) {
  final hop = (rate * _hopSec).round();
  if (x.length < _frame + hop) return const [];
  final frames = (x.length - _frame) ~/ hop + 1;
  final rms = Float64List(frames);
  var peak = 0.0;
  for (var f = 0; f < frames; f++) {
    var s = 0.0;
    for (var i = 0; i < _frame; i++) {
      final v = x[f * hop + i];
      s += v * v;
    }
    rms[f] = math.sqrt(s / _frame);
    if (rms[f] > peak) peak = rms[f];
  }
  final gate = math.max(0.01, peak * 0.12);
  final pitch = List<double>.filled(frames, double.nan);
  for (var f = 0; f < frames; f++) {
    if (rms[f] < gate) continue;
    final hz = yinPitch(x, f * hop, _frame, rate);
    if (hz > 0) pitch[f] = _midiOf(hz);
  }

  // Steady stretches -> notes. A jump of most of a semitone that lasts
  // 3 frames starts a new note; 3 frames of no pitch end one.
  final notes = <HeardNote>[];
  var cur = <double>[];
  var curStart = 0, quiet = 0, jump = 0;
  void close(int endFrame) {
    if (cur.length >= 5) {
      notes.add(HeardNote(curStart * _hopSec, endFrame * _hopSec, _median(cur).round()));
    }
    cur = [];
  }

  for (var f = 0; f < frames; f++) {
    final m = pitch[f];
    if (m.isNaN) {
      if (cur.isNotEmpty && ++quiet >= 3) close(f - quiet + 1);
      continue;
    }
    quiet = 0;
    if (cur.isEmpty) {
      cur = [m];
      curStart = f;
      jump = 0;
      continue;
    }
    if ((m - _median(cur)).abs() > 0.8) {
      if (++jump >= 3) {
        final keep = cur.sublist(cur.length - (jump - 1));
        cur.removeRange(cur.length - (jump - 1), cur.length);
        close(f - jump + 1);
        cur = [...keep, m];
        curStart = f - jump + 1;
        jump = 0;
      } else {
        cur.add(m);
      }
    } else {
      jump = 0;
      cur.add(m);
    }
  }
  close(frames);
  return notes;
}

/// Puts heard notes on a grid: the tempo makes the most common gap
/// between onsets an eighth note.
GridMelody gridFromHeard(List<HeardNote> notes) {
  if (notes.isEmpty) return const GridMelody(100, [], []);
  final gaps = <double>[
    for (var i = 1; i < notes.length; i++) notes[i].start - notes[i - 1].start,
  ].where((g) => g > 0.08).toList();
  var bpm = gaps.isEmpty ? 100.0 : 30 / _median(gaps);
  while (bpm > 160) {
    bpm /= 2;
  }
  while (bpm < 60) {
    bpm *= 2;
  }
  final sixteenth = 15 / bpm;
  final t0 = notes.first.start;
  final top = <int, int>{};
  for (final n in notes) {
    final q = ((n.start - t0) / sixteenth).round();
    top[q] = math.max(top[q] ?? 0, n.midi);
  }
  final onsets = top.keys.toList()..sort();
  return GridMelody(bpm.round(), onsets, [for (final q in onsets) top[q]!]);
}

// ---- MIDI ----------------------------------------------------------------------------------

class _Track {
  final String name;
  final List<List<int>> notes; // [start, end, note] in ticks
  _Track(this.name, this.notes);
}

/// The melody of a Standard MIDI File. Throws FormatException on files it
/// can't read.
GridMelody midiToGrid(Uint8List data, {int? track}) {
  final bd = ByteData.sublistView(data);
  if (data.length < 14 || String.fromCharCodes(data.sublist(0, 4)) != 'MThd') {
    throw const FormatException('Not a MIDI file');
  }
  final hlen = bd.getUint32(4), ntrk = bd.getUint16(10), div = bd.getUint16(12);
  if (div & 0x8000 != 0) throw const FormatException('SMPTE time isn\'t supported');
  var i = 8 + hlen;
  int? tempo;
  final tracks = <_Track>[];
  int readVar() {
    var v = 0;
    while (true) {
      final b = data[i++];
      v = (v << 7) | (b & 0x7F);
      if (b & 0x80 == 0) return v;
    }
  }

  while (i + 8 <= data.length && tracks.length < ntrk) {
    final tag = String.fromCharCodes(data.sublist(i, i + 4));
    final tlen = bd.getUint32(i + 4);
    i += 8;
    final end = math.min(i + tlen, data.length);
    if (tag != 'MTrk') {
      i = end;
      continue;
    }
    var t = 0, status = 0;
    var name = '';
    final notes = <List<int>>[];
    final on = <int, int>{};
    while (i < end) {
      t += readVar();
      final b = data[i];
      if (b & 0x80 != 0) {
        status = b;
        i++;
      }
      if (status == 0xFF) {
        final kind = data[i++];
        final ln = readVar();
        if (kind == 0x51 && tempo == null && ln >= 3) {
          tempo = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        } else if (kind == 0x03) {
          name = String.fromCharCodes(data.sublist(i, i + ln));
        }
        i += ln;
      } else if (status == 0xF0 || status == 0xF7) {
        i += readVar();
      } else {
        final hi = status & 0xF0;
        if (hi == 0xC0 || hi == 0xD0) {
          i += 1;
          continue;
        }
        final a = data[i], v = data[i + 1];
        i += 2;
        final ch = status & 0x0F;
        final key = ch * 128 + a;
        if (hi == 0x90 && v > 0 && ch != 9) {
          on[key] = t;
        } else if (hi == 0x80 || (hi == 0x90 && v == 0)) {
          final s = on.remove(key);
          if (s != null) notes.add([s, t, a]);
        }
      }
    }
    notes.sort((x, y) => x[0] != y[0] ? x[0] - y[0] : x[2] - y[2]);
    tracks.add(_Track(name, notes));
    i = end;
  }
  if (tracks.isEmpty) throw const FormatException('No tracks in this MIDI file');
  final bpm = tempo != null && tempo > 0 ? (60000000 / tempo).round() : 120;
  int score(_Track tr) => tr.notes.where((n) => n[2] >= 55 && n[2] <= 88).length;
  final pick = track ?? List.generate(tracks.length, (k) => k).reduce((a, b) => score(tracks[b]) > score(tracks[a]) ? b : a);
  final notes = tracks[pick].notes;
  if (notes.isEmpty) throw const FormatException('No notes in this MIDI file');
  final grid = math.max(1, div ~/ 4);
  final top = <int, int>{};
  for (final n in notes) {
    final q = (n[0] / grid).round();
    top[q] = math.max(top[q] ?? 0, n[2]);
  }
  final onsets = top.keys.toList()..sort();
  return GridMelody(bpm.clamp(30, 300), onsets, [for (final q in onsets) top[q]!]);
}

// ---- the watch's format -----------------------------------------------------------------

const _names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const _len = {1: '16', 2: '8', 3: '8.', 4: '4', 6: '4.', 8: '2', 12: '2.', 16: '1'};

/// The song file for /piano/ on the watch (same output as midi_to_piano.py).
String pianoSongText(GridMelody m, {required String title, String composer = '', int maxNotes = 400}) {
  if (m.length == 0) throw const FormatException('No notes');
  final onsets = m.onsets.take(maxNotes).toList();
  final mel = m.notes.take(maxNotes).toList();
  final avg = mel.reduce((a, b) => a + b) / mel.length;
  final shift = 12 * ((72 - avg) / 12).round(); // centre around C5
  final out = <(String, int)>[];
  var prevEnd = onsets.first;
  for (var k = 0; k < onsets.length; k++) {
    final q = onsets[k];
    if (q > prevEnd) out.add(('R', q - prevEnd));
    final nxt = k + 1 < onsets.length ? onsets[k + 1] : q + 4;
    final ln = math.min(nxt - q, 16);
    final n = (mel[k] + shift).clamp(48, 96);
    out.add(('${_names[n % 12]}${n ~/ 12 - 1}', ln));
    prevEnd = q + ln;
  }
  final toks = <String>[];
  String? last;
  for (var (name, ln) in out) {
    while (ln > 0) {
      final part = _len.keys.where((p) => p <= ln).reduce(math.max);
      ln -= part;
      toks.add(_len[part] == last ? name : '$name/${_len[part]}');
      last = _len[part];
      if (name != 'R') name = 'R'; // the rest of a split note is silence
    }
  }
  final clean = title.replaceAll(RegExp(r'[\r\n]'), ' ').trim();
  final b = StringBuffer('title: ${clean.isEmpty ? 'My song' : clean}\n');
  if (composer.trim().isNotEmpty) b.write('composer: ${composer.trim()}\n');
  b.write('bpm: ${m.bpm}\n');
  for (var k = 0; k < toks.length; k += 16) {
    b.write('${toks.sublist(k, math.min(k + 16, toks.length)).join(' ')}\n');
  }
  return b.toString();
}

/// A file name the watch accepts for /piano/ ("My Song!" -> "my_song.txt").
String pianoFileName(String title) {
  var s = title.toLowerCase().replaceAll(RegExp(r'[^a-z0-9]+'), '_').replaceAll(RegExp(r'^_+|_+$'), '');
  if (s.isEmpty) s = 'song';
  if (s.length > 40) s = s.substring(0, 40);
  return '$s.txt';
}

/// 16-bit PCM WAV -> mono samples in [-1, 1] and the rate.
(Float32List, int) readWavMono(Uint8List bytes) {
  final bd = ByteData.sublistView(bytes);
  if (bytes.length < 44 || String.fromCharCodes(bytes.sublist(0, 4)) != 'RIFF') {
    throw const FormatException('Not a WAV file');
  }
  var channels = 1, rate = 16000, bits = 16, p = 12;
  while (p + 8 <= bytes.length) {
    final id = String.fromCharCodes(bytes.sublist(p, p + 4));
    var len = bd.getUint32(p + 4, Endian.little);
    if (id == 'fmt ') {
      channels = bd.getUint16(p + 10, Endian.little);
      rate = bd.getUint32(p + 12, Endian.little);
      bits = bd.getUint16(p + 22, Endian.little);
    } else if (id == 'data') {
      if (bits != 16 || channels < 1) throw const FormatException('Only 16-bit WAV');
      if (len == 0 || p + 8 + len > bytes.length) len = bytes.length - p - 8;
      final frames = len ~/ (2 * channels);
      final out = Float32List(frames);
      for (var f = 0; f < frames; f++) {
        var s = 0;
        for (var c = 0; c < channels; c++) {
          s += bd.getInt16(p + 8 + (f * channels + c) * 2, Endian.little);
        }
        out[f] = s / channels / 32768.0;
      }
      return (out, rate);
    }
    p += 8 + len + (len & 1);
  }
  throw const FormatException('No audio in this WAV');
}
