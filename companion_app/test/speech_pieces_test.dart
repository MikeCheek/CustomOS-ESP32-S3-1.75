import 'dart:math' as math;
import 'dart:typed_data';

import 'package:amoled_companion/services/sherpa_asr.dart';
import 'package:flutter_test/flutter_test.dart';

/// A made-up recording: (seconds, speaking?) one after another, over a
/// quiet room.
Float32List _audio(List<(double, bool)> parts) {
  const rate = 16000;
  final out = <double>[];
  final rnd = math.Random(3);
  var t = 0;
  for (final (sec, speech) in parts) {
    final n = (sec * rate).round();
    for (var i = 0; i < n; i++, t++) {
      final room = (rnd.nextDouble() - 0.5) * 0.002;
      out.add(room + (speech ? 0.3 * math.sin(2 * math.pi * 180 * t / rate) : 0));
    }
  }
  return Float32List.fromList(out);
}

List<(double, double)> _secs(List<(int, int)> p) => [for (final (a, b) in p) (a / 16000, b / 16000)];

void main() {
  test('a pause of half a second or more splits turns, a short one doesn\'t', () {
    final p = _secs(speechPieces(_audio([
      (0.5, false),
      (3, true), // turn 1
      (0.8, false), // a turn change
      (2, true), // turn 2...
      (0.2, false), // ...with a breath inside
      (2, true),
      (1.5, false),
    ])));
    expect(p.length, 2);
    expect(p[0].$2, inInclusiveRange(3.5, 4.3)); // turn 1 ends in the pause
    expect(p[1].$1, inInclusiveRange(3.5, 4.3));
  });

  test('silence alone is left out', () {
    expect(speechPieces(_audio([(4, false)])), isEmpty);
    final p = _secs(speechPieces(_audio([(2, true), (6, false), (2, true)])));
    expect(p.length, 2);
  });

  test('long speech is split under 28 s', () {
    final p = _secs(speechPieces(_audio([(1, false), (60, true), (1, false)])));
    expect(p.length, greaterThanOrEqualTo(3));
    for (final (a, b) in p) {
      expect(b - a, lessThanOrEqualTo(28.0));
    }
  });

  test('a blip under a second joins the piece before', () {
    final p = _secs(speechPieces(_audio([(3, true), (0.7, false), (0.4, true), (0.6, false)])));
    expect(p.length, 1);
  });
}
