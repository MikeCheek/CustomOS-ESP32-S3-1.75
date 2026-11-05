import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:image_picker/image_picker.dart';

import '../providers/ble_provider.dart';
import '../providers/photo_faces_provider.dart';
import '../services/photo_face.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Makes one photo face: pick a photo, frame it (drag, pinch, zoom - smaller
/// than the circle too), put the time anywhere (drag it), choose what's behind
/// the subject, a look, a glow and a shade, send it.
/// The list of the watch's photo faces is PhotoFaceScreen (photo_faces_screen.dart).
class PhotoFaceEditor extends ConsumerStatefulWidget {
  const PhotoFaceEditor({super.key});

  @override
  ConsumerState<PhotoFaceEditor> createState() => _PhotoFaceEditorState();
}

class _PhotoFaceEditorState extends ConsumerState<PhotoFaceEditor> {
  static const _colors = [
    Color(0xFFFFFFFF),
    Color(0xFF111111),
    Color(0xFFFFD54F),
    Color(0xFFFF8A80),
    Color(0xFF80DEEA),
    Color(0xFFB388FF),
    Color(0xFFA5D6A7),
    Color(0xFFFFAB40),
  ];
  static const _view = 300.0; // the preview's size on screen (the watch is 466)

  PhotoSource? _src;
  PhotoFaceImage? _img; // framed, as the watch will have it
  PhotoFaceGlyphs? _glyphs;
  ui.Image? _preview;
  PhotoFaceStyle _style = const PhotoFaceStyle();
  PhotoFrame _frame = const PhotoFrame();
  PhotoLookSettings _fx = const PhotoLookSettings();
  Color? _suggested; // a time colour from the photo
  bool _moveTime = false; // the drag moves the time, not the photo
  bool _working = false;
  String? _status;
  double _progress = 0;
  int _job = 0;
  bool _rendering = false, _again = false;
  // the gesture
  PhotoFrame _frame0 = const PhotoFrame();
  Offset _focal0 = Offset.zero, _time0 = Offset.zero;

  @override
  void initState() {
    super.initState();
    // Straight to the gallery: that's why this screen was opened.
    WidgetsBinding.instance.addPostFrameCallback((_) => _pick());
  }

  @override
  void dispose() {
    _preview?.dispose();
    _src?.dispose();
    super.dispose();
  }

  Future<void> _pick() async {
    final x = await ImagePicker().pickImage(source: ImageSource.gallery, maxWidth: 2048, maxHeight: 2048);
    if (x == null) return;
    final file = await x.readAsBytes();
    final job = ++_job;
    setState(() {
      _working = true;
      _status = 'Finding the subject...';
    });
    try {
      final src = await PhotoFace.load(file);
      if (job != _job) {
        src.dispose();
        return;
      }
      _src?.dispose();
      _src = src;
      _frame = const PhotoFrame();
      if (src.mask == null && mounted) {
        showSnack(
          context,
          'No clear subject found - the time goes over the photo. (The first time, the model may still be downloading.)',
        );
      }
      await _render(glyphs: true);
      if (_img != null) _suggested = PhotoFace.suggestColor(_img!.rgba);
    } finally {
      if (mounted && job == _job) {
        setState(() {
          _working = false;
          _status = null;
        });
      }
    }
  }

  /// Frames and draws it again - the newest settings win if a render is already running.
  Future<void> _render({bool glyphs = false}) async {
    final src = _src;
    if (src == null) return;
    if (_rendering) {
      _again = true;
      return;
    }
    _rendering = true;
    try {
      do {
        _again = false;
        if (glyphs || _glyphs == null) _glyphs = await PhotoFace.glyphs(_style);
        glyphs = false;
        final img = await PhotoFace.compose(src, _frame, _fx, _style);
        final pv = await PhotoFace.preview(img.rgba, img.mask, _glyphs!, _style);
        if (!mounted) {
          pv.dispose();
          return;
        }
        setState(() {
          _img = img;
          _preview?.dispose();
          _preview = pv;
        });
      } while (_again);
    } finally {
      _rendering = false;
    }
  }

  void _setStyle(PhotoFaceStyle s, {bool glyphs = false}) {
    _style = s;
    _render(glyphs: glyphs);
  }

  void _setFx(PhotoLookSettings f) {
    setState(() => _fx = f);
    _render();
  }

  void _setFrame(PhotoFrame f) {
    _frame = f;
    _render();
  }

  // ---- gestures on the preview: the photo (drag, pinch) or the time (drag) --------------
  double get _k => 466 / _view;

  void _scaleStart(ScaleStartDetails d) {
    _frame0 = _frame;
    _focal0 = d.localFocalPoint;
    _time0 = Offset(_style.timeX.toDouble(), _style.timeY.toDouble());
  }

  void _scaleUpdate(ScaleUpdateDetails d) {
    final move = (d.localFocalPoint - _focal0) * _k;
    if (_moveTime) {
      final t = _time0 + move;
      _style = _style.copyWith(timeX: t.dx.round().clamp(60, 406), timeY: t.dy.round().clamp(50, 416));
      _render();
      return;
    }
    final zoom = (_frame0.zoom * d.scale).clamp(0.35, 4.0);
    _setFrame(PhotoFrame(zoom: zoom, offset: _clampOffset(_frame0.offset + move, zoom)));
  }

  /// Anywhere, as long as some of the photo stays on the screen.
  Offset _clampOffset(Offset o, double zoom) {
    final src = _src;
    if (src == null) return o;
    final r = PhotoFace.frameRect(src.image, PhotoFrame(zoom: zoom));
    final mx = r.width / 2 + 160, my = r.height / 2 + 160;
    return Offset(o.dx.clamp(-mx, mx), o.dy.clamp(-my, my));
  }

  Future<void> _send() async {
    final ble = ref.read(bleServiceProvider);
    if (!ble.isConnected) {
      showSnack(context, 'Connect the watch first');
      return;
    }
    final img = _img!;
    final bytes = PhotoFace.encode(img.rgba, img.mask, _glyphs!, _style);
    setState(() {
      _working = true;
      _progress = 0;
      _status = 'Sending ${(bytes.length / 1024).round()} KB...';
    });
    try {
      final name = PhotoFacesNotifier.newName();
      await ble.sendFileWithProgress(
        'pf:$name.pwf',
        bytes,
        onProgress: (sent) {
          if (mounted) setState(() => _progress = sent / bytes.length);
        },
      );
      final png = await _preview?.toByteData(format: ui.ImageByteFormat.png);
      if (png != null) await PhotoFacesNotifier.saveThumb(name, png.buffer.asUint8List());
      if (mounted) {
        showSnack(context, 'It\'s the watch face now');
        Navigator.pop(context, true);
      }
    } catch (e) {
      if (mounted) showSnack(context, 'Couldn\'t send it: $e');
    } finally {
      if (mounted) {
        setState(() {
          _working = false;
          _status = null;
        });
      }
    }
  }

  Widget _chip(String label, bool on, VoidCallback? onTap, {IconData? icon}) => Padding(
    padding: const EdgeInsets.only(right: 8, bottom: 6),
    child: ChoiceChip(
      avatar: icon == null ? null : Icon(icon, size: 16),
      label: Text(label),
      selected: on,
      onSelected: _working || onTap == null ? null : (_) => onTap(),
    ),
  );

  Widget _swatch(Color c, {String? label}) => GestureDetector(
    onTap: () => _setStyle(_style.copyWith(timeColor: c, dateColor: c)),
    child: Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Container(
          width: 34,
          height: 34,
          decoration: BoxDecoration(
            color: c,
            shape: BoxShape.circle,
            border: Border.all(color: _style.timeColor == c ? AppColors.accent : AppColors.surfaceLight, width: 3),
          ),
          child: label == null ? null : const Icon(Icons.auto_awesome, size: 15, color: Color(0x99000000)),
        ),
        if (label != null) Text(label, style: const TextStyle(color: AppColors.textDim, fontSize: 10)),
      ],
    ),
  );

  @override
  Widget build(BuildContext context) {
    final ready = _img != null && _glyphs != null;
    final subject = _img?.mask != null;
    return Scaffold(
      appBar: AppBar(title: const Text('New photo face')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          const Text(
            'A photo as the watch face, with the time behind the person, pet or thing in it. '
            'Drag and pinch the photo to place it - smaller than the screen too - or switch to Time and drag the time.',
            style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
          ),
          const SizedBox(height: 14),
          if (ready)
            Center(
              child: SegmentedButton<bool>(
                segments: const [
                  ButtonSegment(value: false, icon: Icon(Icons.open_with_rounded), label: Text('Move photo')),
                  ButtonSegment(value: true, icon: Icon(Icons.schedule_rounded), label: Text('Move time')),
                ],
                selected: {_moveTime},
                onSelectionChanged: (v) => setState(() => _moveTime = v.first),
              ),
            ),
          const SizedBox(height: 12),
          Center(
            child: GestureDetector(
              onTap: _working || ready ? null : _pick,
              onScaleStart: ready && !_working ? _scaleStart : null,
              onScaleUpdate: ready && !_working ? _scaleUpdate : null,
              child: Container(
                width: _view,
                height: _view,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: const Color(0xFF1C1C24),
                  border: Border.all(color: _moveTime ? AppColors.accent : const Color(0xFF2A2A34), width: 3),
                ),
                clipBehavior: Clip.antiAlias,
                child: Stack(
                  fit: StackFit.expand,
                  children: [
                    if (_preview != null)
                      RawImage(image: _preview, fit: BoxFit.cover)
                    else
                      const Center(
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            Icon(Icons.add_photo_alternate_outlined, color: AppColors.textDim, size: 40),
                            SizedBox(height: 8),
                            Text('Pick a photo', style: TextStyle(color: AppColors.textDim)),
                          ],
                        ),
                      ),
                    if (_working) const ColoredBox(color: Color(0x66000000)),
                    if (_working) const Center(child: CircularProgressIndicator()),
                  ],
                ),
              ),
            ),
          ),
          if (_status != null) ...[
            const SizedBox(height: 10),
            Text(
              _status!,
              textAlign: TextAlign.center,
              style: const TextStyle(color: AppColors.textDim, fontSize: 12),
            ),
          ],
          if (_working && _progress > 0) ...[
            const SizedBox(height: 8),
            LinearProgressIndicator(value: _progress, minHeight: 6),
          ],
          const SizedBox(height: 14),
          if (ready) ...[
            // ---- framing
            const SectionLabel('Framing'),
            Row(
              children: [
                const Icon(Icons.zoom_out_rounded, color: AppColors.textDim, size: 20),
                Expanded(
                  child: Slider(
                    min: 0.35,
                    max: 4,
                    value: _frame.zoom,
                    label: '${(_frame.zoom * 100).round()}%',
                    divisions: 73,
                    onChanged: _working
                        ? null
                        : (v) => _setFrame(PhotoFrame(zoom: v, offset: _clampOffset(_frame.offset, v))),
                  ),
                ),
                const Icon(Icons.zoom_in_rounded, color: AppColors.textDim, size: 20),
              ],
            ),
            Wrap(
              children: [
                _chip('Fill', _frame.zoom == 1 && _frame.offset == Offset.zero, () => _setFrame(const PhotoFrame())),
                _chip('Whole photo', false, () {
                  final src = _src!;
                  final k = (src.image.width > src.image.height
                      ? src.image.height / src.image.width
                      : src.image.width / src.image.height);
                  _setFrame(PhotoFrame(zoom: k * 0.94));
                }),
                if (subject)
                  _chip(
                    'Subject left',
                    false,
                    () => _setFrame(_frame.copyWith(offset: _clampOffset(Offset(-80, _frame.offset.dy), _frame.zoom))),
                  ),
                if (subject)
                  _chip(
                    'Subject right',
                    false,
                    () => _setFrame(_frame.copyWith(offset: _clampOffset(Offset(80, _frame.offset.dy), _frame.zoom))),
                  ),
              ],
            ),
            // ---- background
            const SectionLabel('Background'),
            Wrap(
              children: [
                for (final (b, label, icon) in [
                  (PhotoBackground.photo, 'Photo', Icons.image_outlined),
                  (PhotoBackground.black, 'Black', Icons.circle),
                  (PhotoBackground.dim, 'Dim', Icons.brightness_4_outlined),
                  (PhotoBackground.blur, 'Blur', Icons.blur_on_rounded),
                  (PhotoBackground.mono, 'B&W', Icons.filter_b_and_w_rounded),
                ])
                  _chip(
                    label,
                    _fx.background == b,
                    subject || b == PhotoBackground.photo || b == PhotoBackground.dim
                        ? () => _setFx(_fx.copyWith(background: b))
                        : null,
                    icon: icon,
                  ),
              ],
            ),
            if (!subject)
              const Text(
                'Black, Blur and B&W keep the subject: there\'s none found in this photo.',
                style: TextStyle(color: AppColors.textDim, fontSize: 12),
              ),
            // ---- look
            const SectionLabel('Look'),
            Wrap(
              children: [
                for (final (l, label) in [
                  (PhotoLook.natural, 'Natural'),
                  (PhotoLook.vivid, 'Vivid'),
                  (PhotoLook.warm, 'Warm'),
                  (PhotoLook.cool, 'Cool'),
                  (PhotoLook.mono, 'B&W'),
                ])
                  _chip(label, _fx.look == l, () => _setFx(_fx.copyWith(look: l))),
              ],
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Glow', style: TextStyle(color: AppColors.text)),
              subtitle: const Text(
                'A soft outline round the subject, in the time\'s colour',
                style: TextStyle(color: AppColors.textDim, fontSize: 12),
              ),
              value: _fx.outline && subject,
              onChanged: !subject || _working ? null : (v) => _setFx(_fx.copyWith(outline: v)),
            ),
            // ---- time
            const SectionLabel('Time'),
            Wrap(
              children: [
                _chip('Top', _style.timeY == 130, () => _setStyle(_style.copyWith(timeY: 130))),
                _chip('Middle', _style.timeY == 233, () => _setStyle(_style.copyWith(timeY: 233))),
                _chip('Bottom', _style.timeY == 336, () => _setStyle(_style.copyWith(timeY: 336))),
                _chip('Centred', _style.timeX == 233, () => _setStyle(_style.copyWith(timeX: 233))),
              ],
            ),
            Row(
              children: [
                const Text('Up / down', style: TextStyle(color: AppColors.textDim, fontSize: 12)),
                Expanded(
                  child: Slider(
                    min: 50,
                    max: 416,
                    value: _style.timeY.toDouble().clamp(50, 416),
                    onChanged: _working ? null : (v) => _setStyle(_style.copyWith(timeY: v.round())),
                  ),
                ),
              ],
            ),
            Row(
              children: [
                const Text('Left / right', style: TextStyle(color: AppColors.textDim, fontSize: 12)),
                Expanded(
                  child: Slider(
                    min: 60,
                    max: 406,
                    value: _style.timeX.toDouble().clamp(60, 406),
                    onChanged: _working ? null : (v) => _setStyle(_style.copyWith(timeX: v.round())),
                  ),
                ),
              ],
            ),
            Wrap(
              children: [
                for (final (w, label) in [
                  (FontWeight.w200, 'Thin'),
                  (FontWeight.w300, 'Light'),
                  (FontWeight.w500, 'Regular'),
                  (FontWeight.w800, 'Bold'),
                ])
                  _chip(label, _style.weight == w, () => _setStyle(_style.copyWith(weight: w), glyphs: true)),
              ],
            ),
            Row(
              children: [
                const Text('Size', style: TextStyle(color: AppColors.textDim)),
                Expanded(
                  child: Slider(
                    min: 64,
                    max: 170,
                    value: _style.fontSize,
                    onChanged: _working ? null : (v) => setState(() => _style = _style.copyWith(fontSize: v)),
                    onChangeEnd: (_) => _render(glyphs: true),
                  ),
                ),
              ],
            ),
            Wrap(
              spacing: 10,
              runSpacing: 8,
              children: [
                if (_suggested != null) _swatch(_suggested!, label: 'Photo'),
                for (final c in _colors) _swatch(c),
              ],
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Shade behind the time', style: TextStyle(color: AppColors.text)),
              subtitle: const Text(
                'Easier to read on a bright photo',
                style: TextStyle(color: AppColors.textDim, fontSize: 12),
              ),
              value: _fx.shadow,
              onChanged: _working ? null : (v) => _setFx(_fx.copyWith(shadow: v)),
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Date', style: TextStyle(color: AppColors.text)),
              value: _style.showDate,
              onChanged: _working ? null : (v) => _setStyle(_style.copyWith(showDate: v)),
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Depth', style: TextStyle(color: AppColors.text)),
              subtitle: const Text(
                'The subject in front of the time',
                style: TextStyle(color: AppColors.textDim, fontSize: 12),
              ),
              value: _style.depth && subject,
              onChanged: !subject || _working ? null : (v) => _setStyle(_style.copyWith(depth: v)),
            ),
            if (_style.needsV2)
              const Padding(
                padding: EdgeInsets.only(bottom: 8),
                child: Text(
                  'The time off centre or no date needs watch firmware 3.5 or later.',
                  style: TextStyle(color: AppColors.textDim, fontSize: 12),
                ),
              ),
            FilledButton.icon(
              onPressed: _working ? null : _send,
              icon: const Icon(Icons.watch_rounded),
              label: const Text('Set on the watch'),
            ),
            const SizedBox(height: 8),
            TextButton(onPressed: _working ? null : _pick, child: const Text('Another photo')),
          ],
        ],
      ),
    );
  }
}
