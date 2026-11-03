import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:image_picker/image_picker.dart';

import '../providers/ble_provider.dart';
import '../services/photo_face.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// A photo as the watch face, the time behind its subject.
class PhotoFaceScreen extends ConsumerStatefulWidget {
  const PhotoFaceScreen({super.key});

  @override
  ConsumerState<PhotoFaceScreen> createState() => _PhotoFaceScreenState();
}

class _PhotoFaceScreenState extends ConsumerState<PhotoFaceScreen> {
  static const _colors = [
    Color(0xFFFFFFFF),
    Color(0xFF111111),
    Color(0xFFFFD54F),
    Color(0xFFFF8A80),
    Color(0xFF80DEEA),
    Color(0xFFB388FF),
  ];
  static const _positions = {'Top': 150, 'Middle': 233, 'Bottom': 320};

  Uint8List? _file; // the picked photo
  Uint8List? _rgba; // cropped to the watch
  Uint8List? _mask;
  PhotoFaceGlyphs? _glyphs;
  ui.Image? _preview;
  PhotoFaceStyle _style = const PhotoFaceStyle();
  double _focus = 0.5;
  bool _working = false, _noSubject = false;
  String? _status;
  double _progress = 0;
  int _job = 0;

  @override
  void dispose() {
    _preview?.dispose();
    super.dispose();
  }

  Future<void> _pick() async {
    final x = await ImagePicker().pickImage(source: ImageSource.gallery, maxWidth: 2048, maxHeight: 2048);
    if (x == null) return;
    _file = await x.readAsBytes();
    _focus = 0.5;
    await _crop();
  }

  Future<void> _crop() async {
    if (_file == null) return;
    final job = ++_job;
    setState(() {
      _working = true;
      _status = 'Finding the subject...';
    });
    try {
      final rgba = await PhotoFace.crop(_file!, focusY: _focus, focusX: _focus);
      Uint8List? mask;
      try {
        mask = await PhotoFace.subjectMask(rgba);
      } catch (e) {
        if (mounted)
          showSnack(context, 'Couldn\'t find the subject ($e). Try again in a minute if the model is downloading.');
      }
      if (job != _job) return;
      _rgba = rgba;
      _mask = mask;
      _noSubject = mask == null;
      await _render();
    } finally {
      if (mounted && job == _job) setState(() => _working = false);
    }
  }

  Future<void> _render({bool glyphs = true}) async {
    if (_rgba == null) return;
    if (glyphs || _glyphs == null) _glyphs = await PhotoFace.glyphs(_style);
    final img = await PhotoFace.preview(_rgba!, _mask, _glyphs!, _style);
    if (!mounted) {
      img.dispose();
      return;
    }
    setState(() {
      _preview?.dispose();
      _preview = img;
      _status = null;
    });
  }

  void _set(PhotoFaceStyle s, {bool glyphs = false}) {
    _style = s;
    _render(glyphs: glyphs);
  }

  Future<void> _send() async {
    final ble = ref.read(bleServiceProvider);
    if (!ble.isConnected) {
      showSnack(context, 'Connect the watch first');
      return;
    }
    final bytes = PhotoFace.encode(_rgba!, _mask, _glyphs!, _style);
    setState(() {
      _working = true;
      _progress = 0;
      _status = 'Sending ${(bytes.length / 1024).round()} KB...';
    });
    try {
      await ble.sendFileWithProgress(
        'pf:photo.pwf',
        bytes,
        onProgress: (sent) {
          if (mounted) setState(() => _progress = sent / bytes.length);
        },
      );
      if (mounted) showSnack(context, 'It\'s the watch face now');
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

  Widget _chip(String label, bool on, VoidCallback onTap) => Padding(
    padding: const EdgeInsets.only(right: 8),
    child: ChoiceChip(label: Text(label), selected: on, onSelected: _working ? null : (_) => onTap()),
  );

  @override
  Widget build(BuildContext context) {
    final ready = _rgba != null && _glyphs != null;
    return Scaffold(
      appBar: AppBar(title: const Text('Photo face')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          const Text(
            'A photo as the watch face, with the time behind the person, pet or thing in it. '
            'The phone finds the subject (Google ML Kit, on the phone) and the watch keeps the photo on its SD card.',
            style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
          ),
          const SizedBox(height: 16),
          Center(
            child: GestureDetector(
              onTap: _working ? null : _pick,
              child: Container(
                width: 280,
                height: 280,
                decoration: const BoxDecoration(shape: BoxShape.circle, color: Color(0xFF1C1C24)),
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
          if (_noSubject && ready) ...[
            const SizedBox(height: 10),
            const Text(
              'No clear subject found: the time goes on top of the photo.',
              textAlign: TextAlign.center,
              style: TextStyle(color: AppColors.warning, fontSize: 12),
            ),
          ],
          const SizedBox(height: 16),
          if (ready) ...[
            const SectionLabel('Framing'),
            Slider(
              value: _focus,
              onChanged: _working ? null : (v) => setState(() => _focus = v),
              onChangeEnd: (_) => _crop(),
            ),
            const SectionLabel('Time'),
            Wrap(
              children: [
                for (final e in _positions.entries)
                  _chip(e.key, _style.timeY == e.value, () => _set(_style.copyWith(timeY: e.value))),
              ],
            ),
            const SizedBox(height: 8),
            Wrap(
              children: [
                _chip(
                  'Light',
                  _style.weight == FontWeight.w300,
                  () => _set(_style.copyWith(weight: FontWeight.w300), glyphs: true),
                ),
                _chip(
                  'Bold',
                  _style.weight == FontWeight.w800,
                  () => _set(_style.copyWith(weight: FontWeight.w800), glyphs: true),
                ),
              ],
            ),
            Row(
              children: [
                const Text('Size', style: TextStyle(color: AppColors.textDim)),
                Expanded(
                  child: Slider(
                    min: 80,
                    max: 150,
                    value: _style.fontSize,
                    onChanged: _working ? null : (v) => setState(() => _style = _style.copyWith(fontSize: v)),
                    onChangeEnd: (_) => _render(),
                  ),
                ),
              ],
            ),
            Wrap(
              spacing: 10,
              children: [
                for (final c in _colors)
                  GestureDetector(
                    onTap: () => _set(_style.copyWith(timeColor: c, dateColor: c)),
                    child: Container(
                      width: 34,
                      height: 34,
                      decoration: BoxDecoration(
                        color: c,
                        shape: BoxShape.circle,
                        border: Border.all(
                          color: _style.timeColor == c ? AppColors.accent : AppColors.surfaceLight,
                          width: 3,
                        ),
                      ),
                    ),
                  ),
              ],
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Depth', style: TextStyle(color: AppColors.text)),
              subtitle: const Text(
                'The subject in front of the time',
                style: TextStyle(color: AppColors.textDim, fontSize: 12),
              ),
              value: _style.depth && _mask != null,
              onChanged: _mask == null || _working ? null : (v) => _set(_style.copyWith(depth: v)),
            ),
            const SizedBox(height: 8),
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
