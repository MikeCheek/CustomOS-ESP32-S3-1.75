import 'dart:typed_data';

import 'package:amoled_companion/services/tts_service.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a NaN or Infinity sample becomes silence, not a crash', () {
    final x = Float32List.fromList([0.5, double.nan, double.infinity, -0.5]);
    final w = pcm16Wav(x, 16000);
    final b = ByteData.sublistView(w);
    expect(b.getInt16(44, Endian.little), closeTo(16383, 1));
    expect(b.getInt16(46, Endian.little), 0);
    expect(b.getInt16(48, Endian.little), 0);
  });

  test('no audio at a rate of 0 is an empty file, not "Infinity or NaN toInt"', () {
    expect(pcm16Wav(Float32List(0), 0).length, 44);
  });

  test('resampling 22.05 kHz to 16 kHz', () {
    final w = pcm16Wav(Float32List(22050), 22050);
    expect(w.length, 44 + 16000 * 2);
  });
}
