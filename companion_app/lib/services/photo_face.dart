import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/painting.dart';
import 'package:flutter/services.dart';
import 'package:google_mlkit_subject_segmentation/google_mlkit_subject_segmentation.dart';
import 'package:path_provider/path_provider.dart';

/// How the time looks on a photo face.
class PhotoFaceStyle {
  final int timeY; // centre of the time, pixels from the top of the 466 screen
  final Color timeColor;
  final Color dateColor;
  final bool depth; // the subject in front of the time
  final double fontSize;
  final FontWeight weight;

  const PhotoFaceStyle({
    this.timeY = 150,
    this.timeColor = const Color(0xFFFFFFFF),
    this.dateColor = const Color(0xFFFFFFFF),
    this.depth = true,
    this.fontSize = 124,
    this.weight = FontWeight.w300,
  });

  PhotoFaceStyle copyWith({
    int? timeY,
    Color? timeColor,
    Color? dateColor,
    bool? depth,
    double? fontSize,
    FontWeight? weight,
  }) => PhotoFaceStyle(
    timeY: timeY ?? this.timeY,
    timeColor: timeColor ?? this.timeColor,
    dateColor: dateColor ?? this.dateColor,
    depth: depth ?? this.depth,
    fontSize: fontSize ?? this.fontSize,
    weight: weight ?? this.weight,
  );
}

/// The digits "0123456789:" as alpha images, all [height] tall.
class PhotoFaceGlyphs {
  final int height;
  final List<int> widths;
  final List<Uint8List> alpha;
  const PhotoFaceGlyphs(this.height, this.widths, this.alpha);
}

/// A photo watch face with depth (firmware/app_photo_watchface.h): the
/// photo cropped to the watch, the subject found on the phone (ML Kit
/// subject segmentation), digits drawn here in the app's font. The watch
/// draws photo, time, then the subject again over the time.
class PhotoFace {
  static const size = 466;
  static const chars = '0123456789:';
  static const _segment = MethodChannel('com.amoledwatch.segment');

  /// The photo's centre square (moved by [focusY], 0 = top .. 1 = bottom
  /// for tall photos), scaled to 466 x 466, as RGBA.
  static Future<Uint8List> crop(Uint8List file, {double focusY = 0.5, double focusX = 0.5}) async {
    final codec = await ui.instantiateImageCodec(file);
    final img = (await codec.getNextFrame()).image;
    final side = math.min(img.width, img.height).toDouble();
    final left = (img.width - side) * focusX.clamp(0.0, 1.0);
    final top = (img.height - side) * focusY.clamp(0.0, 1.0);
    final rec = ui.PictureRecorder();
    final c = Canvas(rec);
    c.drawImageRect(
      img,
      Rect.fromLTWH(left, top, side, side),
      Rect.fromLTWH(0, 0, size.toDouble(), size.toDouble()),
      Paint()..filterQuality = FilterQuality.high,
    );
    final out = await rec.endRecording().toImage(size, size);
    final bytes = (await out.toByteData(format: ui.ImageByteFormat.rawRgba))!.buffer.asUint8List();
    img.dispose();
    out.dispose();
    return bytes;
  }

  /// The subject: per pixel 0..255 (null when there's none or it can't
  /// run). Android: ML Kit (the first time, Google Play services downloads
  /// the model). iPhone: Apple's Vision foreground mask (iOS 17+,
  /// ios/AmoledNative/Classes/Segmenter.swift).
  static Future<Uint8List?> subjectMask(Uint8List rgba) async {
    if (!Platform.isAndroid && !Platform.isIOS) return null;
    final png = await _png(rgba);
    final dir = await getTemporaryDirectory();
    final f = File('${dir.path}/photo_face_in.png');
    await f.writeAsBytes(png);
    if (Platform.isIOS) {
      try {
        final m = await _segment.invokeMethod<Uint8List>('mask', {'path': f.path});
        if (m == null || m.length != size * size) return null;
        var any = 0;
        for (final v in m) {
          if (v > 128) any++;
        }
        return any > 200 ? m : null;
      } finally {
        try {
          await f.delete();
        } catch (_) {}
      }
    }
    final seg = SubjectSegmenter(
      options: SubjectSegmenterOptions(
        enableForegroundBitmap: false,
        enableForegroundConfidenceMask: true,
        enableMultipleSubjects: SubjectResultOptions(enableConfidenceMask: false, enableSubjectBitmap: false),
      ),
    );
    try {
      final r = await seg.processImage(InputImage.fromFilePath(f.path));
      final conf = r.foregroundConfidenceMask;
      if (conf == null || conf.length != size * size) return null;
      final mask = Uint8List(size * size);
      var any = 0;
      for (var i = 0; i < mask.length; i++) {
        // a soft edge: 0.35 and under is background, 0.75 and over subject
        final a = ((conf[i] - 0.35) / 0.4).clamp(0.0, 1.0);
        mask[i] = (a * 255).round();
        if (mask[i] > 128) any++;
      }
      return any > 200 ? mask : null;
    } finally {
      await seg.close();
      try {
        await f.delete();
      } catch (_) {}
    }
  }

  static Future<Uint8List> _png(Uint8List rgba) async {
    final buf = await ui.ImmutableBuffer.fromUint8List(rgba);
    final desc = ui.ImageDescriptor.raw(buf, width: size, height: size, pixelFormat: ui.PixelFormat.rgba8888);
    final img = (await (await desc.instantiateCodec()).getNextFrame()).image;
    final png = (await img.toByteData(format: ui.ImageByteFormat.png))!.buffer.asUint8List();
    img.dispose();
    return png;
  }

  /// The digits, cropped to the rows any of them uses.
  static Future<PhotoFaceGlyphs> glyphs(PhotoFaceStyle s) async {
    final full = <Uint8List>[];
    final widths = <int>[];
    var h = 0;
    for (final ch in chars.split('')) {
      final tp = TextPainter(
        text: TextSpan(
          text: ch,
          style: TextStyle(
            fontSize: s.fontSize,
            fontWeight: s.weight,
            color: const Color(0xFFFFFFFF),
            height: 1.0,
            fontFeatures: const [ui.FontFeature.tabularFigures()],
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      final w = tp.width.ceil() + (ch == ':' ? 2 : 4);
      h = math.max(h, tp.height.ceil() + 4);
      final rec = ui.PictureRecorder();
      tp.paint(Canvas(rec), Offset((w - tp.width) / 2, 2));
      final img = await rec.endRecording().toImage(w, tp.height.ceil() + 4);
      final rgba = (await img.toByteData(format: ui.ImageByteFormat.rawRgba))!.buffer.asUint8List();
      final a = Uint8List(w * img.height);
      for (var i = 0; i < a.length; i++) {
        a[i] = rgba[i * 4 + 3];
      }
      img.dispose();
      full.add(a);
      widths.add(w);
    }
    // the rows with ink in any glyph
    var top = h, bottom = -1;
    for (var g = 0; g < full.length; g++) {
      final w = widths[g], rows = full[g].length ~/ w;
      for (var y = 0; y < rows; y++) {
        for (var x = 0; x < w; x++) {
          if (full[g][y * w + x] > 0) {
            top = math.min(top, y);
            bottom = math.max(bottom, y);
            break;
          }
        }
      }
    }
    if (bottom < top) {
      top = 0;
      bottom = 0;
    }
    final gh = bottom - top + 1;
    final out = <Uint8List>[];
    for (var g = 0; g < full.length; g++) {
      final w = widths[g], rows = full[g].length ~/ w;
      final a = Uint8List(w * gh);
      for (var y = 0; y < gh; y++) {
        final sy = top + y;
        if (sy < rows) a.setRange(y * w, y * w + w, full[g], sy * w);
      }
      out.add(a);
    }
    return PhotoFaceGlyphs(gh, widths, out);
  }

  static int _rgb565(Color c) {
    final r = (c.r * 255).round(), g = (c.g * 255).round(), b = (c.b * 255).round();
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  }

  /// The file the watch reads (/faces/photo.pwf).
  static Uint8List encode(Uint8List rgba, Uint8List? mask, PhotoFaceGlyphs g, PhotoFaceStyle s) {
    final depth = s.depth && mask != null;
    final b = BytesBuilder(copy: false);
    final head = ByteData(40);
    head.setUint8(0, 0x50); // "PWF1"
    head.setUint8(1, 0x57);
    head.setUint8(2, 0x46);
    head.setUint8(3, 0x31);
    head.setUint16(4, size, Endian.little);
    head.setUint16(6, size, Endian.little);
    head.setUint16(8, s.timeY, Endian.little);
    head.setUint16(10, _rgb565(s.timeColor), Endian.little);
    head.setUint16(12, _rgb565(s.dateColor), Endian.little);
    head.setUint16(14, depth ? 1 : 0, Endian.little);
    head.setUint16(16, g.height, Endian.little);
    for (var i = 0; i < 11; i++) {
      head.setUint16(18 + i * 2, g.widths[i], Endian.little);
    }
    b.add(head.buffer.asUint8List());
    final photo = ByteData(size * size * 2);
    for (var i = 0; i < size * size; i++) {
      final r = rgba[i * 4], gg = rgba[i * 4 + 1], bb = rgba[i * 4 + 2];
      photo.setUint16(i * 2, ((r >> 3) << 11) | ((gg >> 2) << 5) | (bb >> 3), Endian.little);
    }
    b.add(photo.buffer.asUint8List());
    for (final a in g.alpha) {
      b.add(a);
    }
    final spans = BytesBuilder(copy: false);
    final alpha = BytesBuilder(copy: false);
    var count = 0;
    if (depth) {
      for (var y = 0; y < size; y++) {
        var x = 0;
        while (x < size) {
          while (x < size && mask[y * size + x] < 8) {
            x++;
          }
          if (x >= size) break;
          final x0 = x;
          while (x < size && mask[y * size + x] >= 8) {
            x++;
          }
          final span = ByteData(6)
            ..setUint16(0, y, Endian.little)
            ..setUint16(2, x0, Endian.little)
            ..setUint16(4, x - x0, Endian.little);
          spans.add(span.buffer.asUint8List());
          alpha.add(Uint8List.sublistView(mask, y * size + x0, y * size + x));
          count++;
        }
      }
    }
    b.add((ByteData(4)..setUint32(0, count, Endian.little)).buffer.asUint8List());
    b.add(spans.takeBytes());
    b.add(alpha.takeBytes());
    return b.takeBytes();
  }

  /// What the watch will show (without the date), as RGBA - the same steps.
  static Future<ui.Image> preview(
    Uint8List rgba,
    Uint8List? mask,
    PhotoFaceGlyphs g,
    PhotoFaceStyle s, {
    String time = '10:09',
  }) async {
    final out = Uint8List.fromList(rgba);
    final tr = (s.timeColor.r * 255).round(), tg = (s.timeColor.g * 255).round(), tb = (s.timeColor.b * 255).round();
    var total = 0;
    for (final ch in time.split('')) {
      final i = chars.indexOf(ch);
      if (i >= 0) total += g.widths[i];
    }
    var x0 = (size - total) ~/ 2;
    final y0 = s.timeY - g.height ~/ 2;
    for (final ch in time.split('')) {
      final gi = chars.indexOf(ch);
      if (gi < 0) continue;
      final w = g.widths[gi];
      for (var y = 0; y < g.height; y++) {
        final sy = y0 + y;
        if (sy < 0 || sy >= size) continue;
        for (var x = 0; x < w; x++) {
          final a = g.alpha[gi][y * w + x];
          final sx = x0 + x;
          if (a == 0 || sx < 0 || sx >= size) continue;
          final p = (sy * size + sx) * 4;
          out[p] = (out[p] * (255 - a) + tr * a) ~/ 255;
          out[p + 1] = (out[p + 1] * (255 - a) + tg * a) ~/ 255;
          out[p + 2] = (out[p + 2] * (255 - a) + tb * a) ~/ 255;
        }
      }
      x0 += w;
    }
    if (s.depth && mask != null) {
      for (var i = 0; i < size * size; i++) {
        final a = mask[i];
        if (a < 8) continue;
        final p = i * 4;
        for (var k = 0; k < 3; k++) {
          out[p + k] = (out[p + k] * (255 - a) + rgba[p + k] * a) ~/ 255;
        }
      }
    }
    final buf = await ui.ImmutableBuffer.fromUint8List(out);
    final desc = ui.ImageDescriptor.raw(buf, width: size, height: size, pixelFormat: ui.PixelFormat.rgba8888);
    return (await (await desc.instantiateCodec()).getNextFrame()).image;
  }
}
