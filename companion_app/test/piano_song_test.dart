import 'dart:math' as math;
import 'dart:typed_data';
import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/piano_song.dart';

/// Hummed-like test signal: each (midi, seconds) a tone with a few
/// harmonics and a soft start/end, a short silence between them.
Float32List _hum(List<(int, double)> notes, {int rate = 16000}) {
  final out = <double>[];
  for (final (m, d) in notes) {
    final f = 440 * math.pow(2, (m - 69) / 12);
    final n = (d * rate).round();
    for (var i = 0; i < n; i++) {
      final env = math.min(1.0, math.min(i, n - i) / (0.02 * rate));
      final t = i / rate;
      out.add(env * 0.4 * (math.sin(2 * math.pi * f * t) + 0.4 * math.sin(4 * math.pi * f * t) + 0.2 * math.sin(6 * math.pi * f * t)) / 1.6);
    }
    out.addAll(List.filled((0.08 * rate).round(), 0.0));
  }
  return Float32List.fromList(out);
}

void main() {
  test('YIN finds the pitch of a tone', () {
    final x = _hum([(57, 0.3)]); // A3, 220 Hz
    final hz = yinPitch(x, 1600, 1024, 16000);
    expect((hz - 220).abs() < 2, true);
  });

  test('a hummed tune becomes its notes and a song', () {
    final x = _hum([(60, 0.3), (60, 0.3), (67, 0.3), (67, 0.3), (69, 0.3), (69, 0.3), (67, 0.6)]);
    final heard = humToNotes(x, 16000);
    expect(heard.map((n) => n.midi).toList(), [60, 60, 67, 67, 69, 69, 67]);
    final g = gridFromHeard(heard);
    expect(g.length, 7);
    expect(g.bpm >= 60 && g.bpm <= 160, true);
    final txt = pianoSongText(g, title: 'Twinkle');
    expect(txt.startsWith('title: Twinkle\nbpm: '), true);
    expect(txt.contains('C5/'), true); // moved up an octave round C5
  });

  test('MIDI melody and file names', () {
    // format 0, 1 track, 96 ticks per quarter: C4 then E4, a quarter each
    final trk = <int>[
      0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20, // 120 bpm
      0x00, 0x90, 60, 100, 0x60, 0x80, 60, 0,
      0x00, 0x90, 64, 100, 0x60, 0x80, 64, 0,
      0x00, 0xFF, 0x2F, 0x00,
    ];
    final bytes = Uint8List.fromList([
      ...'MThd'.codeUnits, 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
      ...'MTrk'.codeUnits, 0, 0, 0, trk.length, ...trk,
    ]);
    final g = midiToGrid(bytes);
    expect(g.bpm, 120);
    expect(g.onsets, [0, 4]);
    expect(g.notes, [60, 64]);
    expect(pianoSongText(g, title: 'Two').split('\n')[2], 'C5/4 E5');
    expect(pianoFileName('My Song!'), 'my_song.txt');
  });
}
