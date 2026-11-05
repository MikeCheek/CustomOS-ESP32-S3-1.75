import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:amoled_companion/services/photo_face.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

/// A 600 x 400 "photo": blue, with a red disc (the subject) in the middle; its mask.
Future<PhotoSource> _source() async {
  const w = 600, h = 400;
  final px = Uint8List(w * h * 4), mk = Uint8List(w * h * 4);
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      final i = (y * w + x) * 4;
      final inside = (x - 300) * (x - 300) + (y - 200) * (y - 200) < 80 * 80;
      px[i] = inside ? 255 : 30;
      px[i + 1] = inside ? 0 : 60;
      px[i + 2] = inside ? 0 : 200;
      px[i + 3] = 255;
      mk[i] = mk[i + 1] = mk[i + 2] = inside ? 255 : 0;
      mk[i + 3] = 255;
    }
  }
  Future<ui.Image> img(Uint8List b) async {
    final buf = await ui.ImmutableBuffer.fromUint8List(b);
    final d = ui.ImageDescriptor.raw(buf, width: w, height: h, pixelFormat: ui.PixelFormat.rgba8888);
    return (await (await d.instantiateCodec()).getNextFrame()).image;
  }

  return PhotoSource(await img(px), await img(mk));
}

List<int> _px(Uint8List rgba, int x, int y) {
  final i = (y * 466 + x) * 4;
  return [rgba[i], rgba[i + 1], rgba[i + 2]];
}

void main() {
  testWidgets('framing, black background, zoomed out', (tester) async {
    await tester.runAsync(() async {
      final src = await _source();
      // filled: the photo's shorter side across the circle, the subject in the middle
      var f = await PhotoFace.compose(src, const PhotoFrame(), const PhotoLookSettings(), const PhotoFaceStyle());
      expect(_px(f.rgba, 233, 233)[0], greaterThan(240)); // the red subject
      expect(_px(f.rgba, 10, 233)[2], greaterThan(180)); // blue round it
      expect(f.mask![233 * 466 + 233], greaterThan(240));
      // a black background keeps only the subject
      f = await PhotoFace.compose(
        src,
        const PhotoFrame(),
        const PhotoLookSettings(background: PhotoBackground.black),
        const PhotoFaceStyle(),
      );
      expect(_px(f.rgba, 10, 233), [0, 0, 0]);
      expect(_px(f.rgba, 233, 233)[0], greaterThan(240));
      // moved right: the subject follows
      f = await PhotoFace.compose(
        src,
        const PhotoFrame(offset: Offset(100, 0)),
        const PhotoLookSettings(),
        const PhotoFaceStyle(),
      );
      expect(_px(f.rgba, 333, 233)[0], greaterThan(240));
      expect(f.mask![233 * 466 + 333], greaterThan(240));
      // zoomed out: room round the photo (black)
      f = await PhotoFace.compose(src, const PhotoFrame(zoom: 0.5), const PhotoLookSettings(), const PhotoFaceStyle());
      expect(_px(f.rgba, 233, 20), [0, 0, 0]);
      expect(_px(f.rgba, 233, 233)[0], greaterThan(240));
      src.dispose();
    });
  });

  test('how much of the time the subject hides', () {
    final g = PhotoFaceGlyphs(
      10,
      List.filled(11, 10),
      List.generate(11, (_) => Uint8List(100)..fillRange(0, 100, 255)),
    );
    final none = Uint8List(466 * 466);
    final all = Uint8List(466 * 466)..fillRange(0, 466 * 466, 255);
    const st = PhotoFaceStyle(timeY: 100);
    expect(PhotoFace.timeCovered(none, g, st), 0);
    expect(PhotoFace.timeCovered(all, g, st), 1);
    expect(PhotoFace.timeCovered(all, g, st.copyWith(depth: false)), 0);
  });

  test('PWF1 by default, PWF2 for the time off centre or no date', () {
    final rgba = Uint8List(466 * 466 * 4);
    final g = PhotoFaceGlyphs(2, List.filled(11, 1), List.generate(11, (_) => Uint8List(2)));
    final v1 = PhotoFace.encode(rgba, null, g, const PhotoFaceStyle());
    expect(String.fromCharCodes(v1.sublist(0, 4)), 'PWF1');
    final v2 = PhotoFace.encode(rgba, null, g, const PhotoFaceStyle(timeX: 150, showDate: false));
    expect(String.fromCharCodes(v2.sublist(0, 4)), 'PWF2');
    final b = ByteData.sublistView(v2);
    expect(b.getUint16(40, Endian.little), 150);
    expect(b.getUint16(42, Endian.little) & 1, 1);
    expect(v2.length, v1.length + 4);
  });
}
