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
  final int timeX; // centre of the time across (233: the middle)
  final int timeY; // centre of the time, pixels from the top of the 466 screen
  final Color timeColor;
  final Color dateColor;
  final bool depth; // the subject in front of the time
  final double fontSize;
  final FontWeight weight;
  final bool showDate;
  final String? fontFamily; // null: the phone's own font

  const PhotoFaceStyle({
    this.timeX = 233,
    this.timeY = 150,
    this.timeColor = const Color(0xFFFFFFFF),
    this.dateColor = const Color(0xFFFFFFFF),
    this.depth = true,
    this.fontSize = 124,
    this.weight = FontWeight.w300,
    this.showDate = true,
    this.fontFamily,
  });

  /// Needs the PWF2 format (firmware 3.5+): the time off centre, or no date.
  bool get needsV2 => timeX != 233 || !showDate;

  PhotoFaceStyle copyWith({
    int? timeX,
    int? timeY,
    Color? timeColor,
    Color? dateColor,
    bool? depth,
    double? fontSize,
    FontWeight? weight,
    bool? showDate,
    String? fontFamily,
  }) => PhotoFaceStyle(
    timeX: timeX ?? this.timeX,
    timeY: timeY ?? this.timeY,
    timeColor: timeColor ?? this.timeColor,
    dateColor: dateColor ?? this.dateColor,
    depth: depth ?? this.depth,
    fontSize: fontSize ?? this.fontSize,
    weight: weight ?? this.weight,
    showDate: showDate ?? this.showDate,
    fontFamily: fontFamily ?? this.fontFamily,
  );
}

/// What's behind the subject.
enum PhotoBackground { photo, black, dim, blur, mono }

/// The photo's colours.
enum PhotoLook { natural, vivid, warm, cool, mono }

/// Where the photo sits on the round screen: [zoom] 1 fills the circle (the shorter
/// side across it), less shows all of it with room round it, more goes in close;
/// [offset] moves its centre, in screen pixels.
class PhotoFrame {
  final double zoom;
  final Offset offset;
  const PhotoFrame({this.zoom = 1, this.offset = Offset.zero});
  PhotoFrame copyWith({double? zoom, Offset? offset}) =>
      PhotoFrame(zoom: zoom ?? this.zoom, offset: offset ?? this.offset);
}

/// Everything the photo gets before it goes to the watch.
class PhotoLookSettings {
  final PhotoBackground background;
  final PhotoLook look;
  final bool outline; // a soft glow round the subject, in the time's colour
  final bool shadow; // a soft shade behind the time, so it reads on a bright photo
  const PhotoLookSettings({
    this.background = PhotoBackground.photo,
    this.look = PhotoLook.natural,
    this.outline = false,
    this.shadow = false,
  });
  PhotoLookSettings copyWith({PhotoBackground? background, PhotoLook? look, bool? outline, bool? shadow}) =>
      PhotoLookSettings(
        background: background ?? this.background,
        look: look ?? this.look,
        outline: outline ?? this.outline,
        shadow: shadow ?? this.shadow,
      );
}

/// The picked photo, decoded once, with its subject found once (on the whole
/// photo, at its own size): framing it again only moves both.
class PhotoSource {
  final ui.Image image;
  final ui.Image? mask; // the subject as a grey image, the same size as [image]
  const PhotoSource(this.image, this.mask);
  void dispose() {
    image.dispose();
    mask?.dispose();
  }
}

/// A framed photo, ready to preview and send: 466 x 466 RGBA and its subject mask.
class PhotoFaceImage {
  final Uint8List rgba;
  final Uint8List? mask;
  const PhotoFaceImage(this.rgba, this.mask);
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

  /// Decodes the photo (the long side up to 1600 px) and finds its subject on all of it.
  static Future<PhotoSource> load(Uint8List file) async {
    final codec = await ui.instantiateImageCodec(file);
    var img = (await codec.getNextFrame()).image;
    const maxSide = 1600;
    if (math.max(img.width, img.height) > maxSide) {
      final k = maxSide / math.max(img.width, img.height);
      final w = (img.width * k).round(), h = (img.height * k).round();
      final rec = ui.PictureRecorder();
      Canvas(rec).drawImageRect(
        img,
        Rect.fromLTWH(0, 0, img.width.toDouble(), img.height.toDouble()),
        Rect.fromLTWH(0, 0, w.toDouble(), h.toDouble()),
        Paint()..filterQuality = FilterQuality.high,
      );
      final small = await rec.endRecording().toImage(w, h);
      img.dispose();
      img = small;
    }
    final rgba = (await img.toByteData(format: ui.ImageByteFormat.rawRgba))!.buffer.asUint8List();
    Uint8List? m;
    try {
      m = await subjectMask(rgba, img.width, img.height);
    } catch (_) {
      m = null;
    }
    ui.Image? maskImg;
    if (m != null) {
      final g = Uint8List(m.length * 4);
      for (var i = 0; i < m.length; i++) {
        g[i * 4] = m[i];
        g[i * 4 + 1] = m[i];
        g[i * 4 + 2] = m[i];
        g[i * 4 + 3] = 255;
      }
      maskImg = await _image(g, img.width, img.height);
    }
    return PhotoSource(img, maskImg);
  }

  static Future<ui.Image> _image(Uint8List rgba, int w, int h) async {
    final buf = await ui.ImmutableBuffer.fromUint8List(rgba);
    final desc = ui.ImageDescriptor.raw(buf, width: w, height: h, pixelFormat: ui.PixelFormat.rgba8888);
    return (await (await desc.instantiateCodec()).getNextFrame()).image;
  }

  /// Where [frame] puts the photo on the 466 screen.
  static Rect frameRect(ui.Image img, PhotoFrame frame) {
    final k = size / math.min(img.width, img.height) * frame.zoom;
    final w = img.width * k, h = img.height * k;
    final c = Offset(size / 2, size / 2) + frame.offset;
    return Rect.fromCenter(center: c, width: w, height: h);
  }

  static Future<Uint8List> _draw(ui.Image img, Rect dst, {double blur = 0}) async {
    final rec = ui.PictureRecorder();
    final c = Canvas(rec);
    c.drawRect(Rect.fromLTWH(0, 0, size.toDouble(), size.toDouble()), Paint()..color = const Color(0xFF000000));
    final p = Paint()..filterQuality = FilterQuality.high;
    if (blur > 0) p.imageFilter = ui.ImageFilter.blur(sigmaX: blur, sigmaY: blur, tileMode: TileMode.clamp);
    c.drawImageRect(img, Rect.fromLTWH(0, 0, img.width.toDouble(), img.height.toDouble()), dst, p);
    final out = await rec.endRecording().toImage(size, size);
    final bytes = (await out.toByteData(format: ui.ImageByteFormat.rawRgba))!.buffer.asUint8List();
    out.dispose();
    return bytes;
  }

  static int _clamp(num v) => v < 0 ? 0 : (v > 255 ? 255 : v.round());

  /// The photo framed, its colours, its background and the extras, as the watch will have it.
  static Future<PhotoFaceImage> compose(
    PhotoSource src,
    PhotoFrame frame,
    PhotoLookSettings fx,
    PhotoFaceStyle st,
  ) async {
    final dst = frameRect(src.image, frame);
    final rgba = await _draw(src.image, dst);
    Uint8List? mask;
    if (src.mask != null) {
      final m = await _draw(src.mask!, dst);
      mask = Uint8List(size * size);
      var any = 0;
      for (var i = 0; i < mask.length; i++) {
        mask[i] = m[i * 4];
        if (mask[i] > 128) any++;
      }
      if (any < 200) mask = null; // framed out of sight
    }
    final n = size * size;
    // the look, on everything
    if (fx.look != PhotoLook.natural) {
      for (var i = 0; i < n; i++) {
        final p = i * 4;
        final r = rgba[p], g = rgba[p + 1], b = rgba[p + 2];
        final l = 0.299 * r + 0.587 * g + 0.114 * b;
        switch (fx.look) {
          case PhotoLook.vivid:
            rgba[p] = _clamp(l + (r - l) * 1.45 + 6);
            rgba[p + 1] = _clamp(l + (g - l) * 1.45 + 6);
            rgba[p + 2] = _clamp(l + (b - l) * 1.45 + 6);
          case PhotoLook.warm:
            rgba[p] = _clamp(r * 1.08 + 10);
            rgba[p + 1] = _clamp(g * 1.02 + 2);
            rgba[p + 2] = _clamp(b * 0.86);
          case PhotoLook.cool:
            rgba[p] = _clamp(r * 0.88);
            rgba[p + 1] = _clamp(g * 1.0 + 2);
            rgba[p + 2] = _clamp(b * 1.1 + 10);
          case PhotoLook.mono:
            final v = _clamp((l - 128) * 1.12 + 128);
            rgba[p] = v;
            rgba[p + 1] = v;
            rgba[p + 2] = v;
          case PhotoLook.natural:
            break;
        }
      }
    }
    // behind the subject (without one, only Dim works - on the whole photo)
    final bg = fx.background;
    if (bg != PhotoBackground.photo && (mask != null || bg == PhotoBackground.dim)) {
      final blurred = bg == PhotoBackground.blur ? await _draw(src.image, dst, blur: 14) : null;
      for (var i = 0; i < n; i++) {
        final a = mask == null ? 0 : mask[i];
        if (a >= 250) continue;
        final p = i * 4;
        for (var k = 0; k < 3; k++) {
          final v = rgba[p + k];
          int b;
          switch (bg) {
            case PhotoBackground.black:
              b = 0;
            case PhotoBackground.dim:
              b = v * 40 ~/ 100;
            case PhotoBackground.blur:
              b = blurred![p + k] * 85 ~/ 100;
            case PhotoBackground.mono:
              final l = (rgba[p] * 299 + rgba[p + 1] * 587 + rgba[p + 2] * 114) ~/ 1000;
              b = l;
            case PhotoBackground.photo:
              b = v;
          }
          rgba[p + k] = (b * (255 - a) + v * a) ~/ 255;
        }
      }
    }
    // a glow round the subject, in the time's colour
    if (fx.outline && mask != null && src.mask != null) {
      final glow = await _draw(src.mask!, dst, blur: 7);
      final cr = (st.timeColor.r * 255).round(),
          cg = (st.timeColor.g * 255).round(),
          cb = (st.timeColor.b * 255).round();
      for (var i = 0; i < n; i++) {
        final h = (glow[i * 4] * 2 - mask[i] * 2).clamp(0, 255);
        if (h < 6) continue;
        final a = h * 220 ~/ 255;
        final p = i * 4;
        rgba[p] = (rgba[p] * (255 - a) + cr * a) ~/ 255;
        rgba[p + 1] = (rgba[p + 1] * (255 - a) + cg * a) ~/ 255;
        rgba[p + 2] = (rgba[p + 2] * (255 - a) + cb * a) ~/ 255;
      }
    }
    // a soft shade where the time goes (not on the subject: it stays as it is)
    if (fx.shadow) {
      final rx = st.fontSize * 1.75, ry = st.fontSize * 0.75;
      for (var y = (st.timeY - ry).floor().clamp(0, size - 1); y < (st.timeY + ry).ceil().clamp(0, size); y++) {
        for (var x = (st.timeX - rx).floor().clamp(0, size - 1); x < (st.timeX + rx).ceil().clamp(0, size); x++) {
          final dx = (x - st.timeX) / rx, dy = (y - st.timeY) / ry;
          final d = dx * dx + dy * dy;
          if (d >= 1) continue;
          final i = y * size + x;
          final keep = mask == null ? 0 : mask[i];
          final a = ((1 - d) * (1 - d) * 150 * (255 - keep) / 255).round();
          final p = i * 4;
          for (var k = 0; k < 3; k++) {
            rgba[p + k] = rgba[p + k] * (255 - a) ~/ 255;
          }
        }
      }
    }
    return PhotoFaceImage(rgba, mask);
  }

  /// How much of the time (as "10:09") the subject hides with depth on: 0..1.
  static double timeCovered(Uint8List? mask, PhotoFaceGlyphs g, PhotoFaceStyle s, {String time = '10:09'}) {
    if (mask == null || !s.depth) return 0;
    var total = 0;
    for (final ch in time.split('')) {
      final i = chars.indexOf(ch);
      if (i >= 0) total += g.widths[i];
    }
    var x0 = s.timeX - total ~/ 2, ink = 0, hidden = 0;
    final y0 = s.timeY - g.height ~/ 2;
    for (final ch in time.split('')) {
      final gi = chars.indexOf(ch);
      if (gi < 0) continue;
      final w = g.widths[gi];
      for (var y = 0; y < g.height; y += 2) {
        final sy = y0 + y;
        for (var x = 0; x < w; x += 2) {
          if (g.alpha[gi][y * w + x] < 128) continue;
          final sx = x0 + x;
          ink++;
          if (sy < 0 || sy >= size || sx < 0 || sx >= size || mask[sy * size + sx] > 128) hidden++;
        }
      }
      x0 += w;
    }
    return ink == 0 ? 0 : hidden / ink;
  }

  /// A colour for the time from the photo: its most colourful bright tone, lifted.
  static Color suggestColor(Uint8List rgba) {
    var best = 0.0;
    var pick = const Color(0xFFFFFFFF);
    for (var i = 0; i < size * size; i += 37) {
      final p = i * 4;
      final c = Color.fromARGB(255, rgba[p], rgba[p + 1], rgba[p + 2]);
      final hsv = HSVColor.fromColor(c);
      final score = hsv.saturation * hsv.value;
      if (score > best) {
        best = score;
        pick = hsv.withValue(1).withSaturation(math.min(1, hsv.saturation * 0.7 + 0.15)).toColor();
      }
    }
    return best < 0.15 ? const Color(0xFFFFFFFF) : pick;
  }

  /// The subject: per pixel 0..255 (null when there's none or it can't
  /// run). Android: ML Kit (the first time, Google Play services downloads
  /// the model). iPhone: Apple's Vision foreground mask (iOS 17+,
  /// ios/AmoledNative/Classes/Segmenter.swift).
  static Future<Uint8List?> subjectMask(Uint8List rgba, [int w = size, int h = size]) async {
    if (!Platform.isAndroid && !Platform.isIOS) return null;
    final png = await _png(rgba, w, h);
    final dir = await getTemporaryDirectory();
    final f = File('${dir.path}/photo_face_in.png');
    await f.writeAsBytes(png);
    if (Platform.isIOS) {
      try {
        final m = await _segment.invokeMethod<Uint8List>('mask', {'path': f.path});
        if (m == null || m.length != w * h) return null;
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
      if (conf == null || conf.length != w * h) return null;
      final mask = Uint8List(w * h);
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

  static Future<Uint8List> _png(Uint8List rgba, int w, int h) async {
    final buf = await ui.ImmutableBuffer.fromUint8List(rgba);
    final desc = ui.ImageDescriptor.raw(buf, width: w, height: h, pixelFormat: ui.PixelFormat.rgba8888);
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
            fontFamily: s.fontFamily,
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
    final v2 = s.needsV2; // PWF1 when it can be, so older watches still read it
    final b = BytesBuilder(copy: false);
    final head = ByteData(v2 ? 44 : 40);
    head.setUint8(0, 0x50); // "PWF1" / "PWF2"
    head.setUint8(1, 0x57);
    head.setUint8(2, 0x46);
    head.setUint8(3, v2 ? 0x32 : 0x31);
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
    if (v2) {
      head.setUint16(40, s.timeX.clamp(0, size - 1), Endian.little);
      head.setUint16(42, s.showDate ? 0 : 1, Endian.little);
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
    String date = 'Thu 8 Oct',
  }) async {
    final out = Uint8List.fromList(rgba);
    final tr = (s.timeColor.r * 255).round(), tg = (s.timeColor.g * 255).round(), tb = (s.timeColor.b * 255).round();
    var total = 0;
    for (final ch in time.split('')) {
      final i = chars.indexOf(ch);
      if (i >= 0) total += g.widths[i];
    }
    var x0 = s.timeX - total ~/ 2;
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
    final base = await _image(out, size, size);
    if (!s.showDate) return base;
    // the date, where the watch puts it: under the time, or over it when the time is low
    final rec = ui.PictureRecorder();
    final c = Canvas(rec);
    c.drawImage(base, Offset.zero, Paint());
    final tp = TextPainter(
      text: TextSpan(
        text: date,
        style: TextStyle(color: s.dateColor, fontSize: 21, fontWeight: FontWeight.w600, fontFamily: s.fontFamily),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    final dy = s.timeY > size / 2
        ? s.timeY - g.height / 2 - 20 - tp.height * 0.8
        : s.timeY + g.height / 2 + 24 - tp.height * 0.8;
    tp.paint(c, Offset(s.timeX - tp.width / 2, dy));
    final img = await rec.endRecording().toImage(size, size);
    base.dispose();
    return img;
  }
}
