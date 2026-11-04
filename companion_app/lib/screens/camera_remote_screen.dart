import 'dart:async';
import 'dart:typed_data';

import 'package:camera/camera.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:gal/gal.dart';
import 'package:image/image.dart' as img;

import '../providers/ble_provider.dart';
import '../services/camera_remote.dart';
import '../theme/app_theme.dart';

/// The phone's camera, run from the watch: open this, prop the phone up,
/// and the watch shows a live view with the shutter, a 3 / 10 s self-timer,
/// flash, front/back, zoom and video. The buttons here do the same.
/// Photos and videos go to the gallery.
class CameraRemoteScreen extends ConsumerStatefulWidget {
  const CameraRemoteScreen({super.key});

  @override
  ConsumerState<CameraRemoteScreen> createState() => _CameraRemoteScreenState();
}

class _CameraRemoteScreenState extends ConsumerState<CameraRemoteScreen> with WidgetsBindingObserver {
  List<CameraDescription> _cams = const [];
  CameraController? _c;
  bool _front = false, _video = false, _rec = false, _busy = false;
  int _timer = 0; // 0, 3, 10
  FlashMode _flash = FlashMode.off;
  double _zoom = 1, _minZoom = 1, _maxZoom = 1;
  int _countdown = 0;
  String? _error;
  // the live view for the watch: a small JPEG every ~0.5 s
  bool _sending = false;
  int _frame = 1;
  DateTime _lastFrame = DateTime(2000);

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    CameraRemote.instance.screenOpen = true;
    CameraRemote.instance.onAction = _onWatch;
    _start();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    CameraRemote.instance.screenOpen = false;
    CameraRemote.instance.onAction = null;
    _link({'on': 0});
    _c?.dispose();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState s) {
    // the camera is given back when the app goes to the background
    if (s == AppLifecycleState.paused) {
      _c?.dispose();
      _c = null;
      _link({'on': 0});
      if (mounted) setState(() {});
    } else if (s == AppLifecycleState.resumed && _c == null) {
      _start();
    }
  }

  Future<void> _link(Map<String, dynamic> m) async {
    final ble = ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    await ble.sendLink({'t': 'cam', ...m});
  }

  Future<void> _sendState() => _link({
    'on': 1,
    'tm': _timer,
    'fl': switch (_flash) {
      FlashMode.auto => 'auto',
      FlashMode.always || FlashMode.torch => 'on',
      _ => 'off',
    },
    'fr': _front ? 1 : 0,
    'vm': _video ? 1 : 0,
    'z': (_zoom * 10).round(),
    'rec': _rec ? 1 : 0,
  });

  Future<void> _start() async {
    try {
      _cams = await availableCameras();
      final want = _cams.where(
        (c) => c.lensDirection == (_front ? CameraLensDirection.front : CameraLensDirection.back),
      );
      final cam = want.isNotEmpty ? want.first : (_cams.isNotEmpty ? _cams.first : null);
      if (cam == null) {
        setState(() => _error = 'No camera on this device');
        return;
      }
      final c = CameraController(
        cam,
        ResolutionPreset.high,
        enableAudio: true,
        imageFormatGroup: ImageFormatGroup.yuv420,
      );
      await c.initialize();
      _minZoom = await c.getMinZoomLevel();
      _maxZoom = await c.getMaxZoomLevel();
      _zoom = _zoom.clamp(_minZoom, _maxZoom);
      await c.setZoomLevel(_zoom);
      await c.setFlashMode(_flash);
      _c = c;
      _error = null;
      if (mounted) setState(() {});
      await _sendState();
      await _startPreview();
    } catch (e) {
      if (mounted) setState(() => _error = 'The camera didn\'t open: $e');
    }
  }

  Future<void> _startPreview() async {
    final c = _c;
    if (c == null || c.value.isStreamingImages || _rec) return;
    try {
      await c.startImageStream(_onFrame);
    } catch (_) {
      // some devices can't stream and record/shoot at once: the watch just has no live view
    }
  }

  Future<void> _stopPreview() async {
    final c = _c;
    if (c != null && c.value.isStreamingImages) {
      try {
        await c.stopImageStream();
      } catch (_) {}
    }
  }

  void _onFrame(CameraImage f) {
    if (_sending || DateTime.now().difference(_lastFrame) < const Duration(milliseconds: 450)) return;
    final ble = ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    _sending = true;
    _lastFrame = DateTime.now();
    final jpeg = _toJpeg(f, _c?.description.sensorOrientation ?? 90, _front);
    if (jpeg == null) {
      _sending = false;
      return;
    }
    _frame = (_frame % 0x7FFFFFF0) + 1;
    ble.sendImage('p', _frame, jpeg).whenComplete(() => _sending = false);
  }

  /// A small square JPEG of the frame, turned upright (the watch draws it in a circle).
  static Uint8List? _toJpeg(CameraImage f, int orientation, bool mirror) {
    const side = 144;
    try {
      final w = f.width, h = f.height;
      final crop = w < h ? w : h, ox = (w - crop) ~/ 2, oy = (h - crop) ~/ 2;
      final out = img.Image(width: side, height: side);
      final yuv = f.planes.length >= 3;
      final p0 = f.planes[0];
      for (var j = 0; j < side; j++) {
        for (var i = 0; i < side; i++) {
          // where this output pixel comes from, turned by the sensor's angle
          var u = i, v = j;
          switch (orientation) {
            case 90:
              u = j;
              v = side - 1 - i;
            case 180:
              u = side - 1 - i;
              v = side - 1 - j;
            case 270:
              u = side - 1 - j;
              v = i;
          }
          if (mirror) u = side - 1 - u;
          final x = ox + u * crop ~/ side, y = oy + v * crop ~/ side;
          int r, g, b;
          if (yuv) {
            final yy = p0.bytes[y * p0.bytesPerRow + x];
            final pu = f.planes[1], pv = f.planes[2];
            final cx = x ~/ 2, cy = y ~/ 2, ps = pu.bytesPerPixel ?? 1;
            final uu = pu.bytes[cy * pu.bytesPerRow + cx * ps] - 128;
            final vv = pv.bytes[cy * pv.bytesPerRow + cx * ps] - 128;
            r = (yy + 1.402 * vv).round();
            g = (yy - 0.344 * uu - 0.714 * vv).round();
            b = (yy + 1.772 * uu).round();
          } else {
            // BGRA (iOS)
            final o = y * p0.bytesPerRow + x * 4;
            b = p0.bytes[o];
            g = p0.bytes[o + 1];
            r = p0.bytes[o + 2];
          }
          out.setPixelRgb(i, j, r.clamp(0, 255), g.clamp(0, 255), b.clamp(0, 255));
        }
      }
      return Uint8List.fromList(img.encodeJpg(out, quality: 60));
    } catch (_) {
      return null;
    }
  }

  // ---- what the watch (or these buttons) asks ------------------------------------------
  void _onWatch(String a) {
    switch (a) {
      case 'shot':
        _shutter();
      case 'flip':
        _flip();
      case 'mode':
        if (!_rec) setState(() => _video = !_video);
        _sendState();
      case 'timer':
        setState(() => _timer = _timer == 0 ? 3 : (_timer == 3 ? 10 : 0));
        _sendState();
      case 'flash':
        _cycleFlash();
      case 'zoom+':
        _setZoom(_zoom * 1.4);
      case 'zoom-':
        _setZoom(_zoom / 1.4);
    }
  }

  Future<void> _setZoom(double z) async {
    final c = _c;
    if (c == null) return;
    _zoom = z.clamp(_minZoom, _maxZoom > 8 ? 8.0 : _maxZoom);
    await c.setZoomLevel(_zoom);
    if (mounted) setState(() {});
    await _sendState();
  }

  Future<void> _cycleFlash() async {
    _flash = switch (_flash) {
      FlashMode.off => FlashMode.auto,
      FlashMode.auto => FlashMode.always,
      _ => FlashMode.off,
    };
    try {
      await _c?.setFlashMode(_flash);
    } catch (_) {}
    if (mounted) setState(() {});
    await _sendState();
  }

  Future<void> _flip() async {
    if (_rec || _cams.length < 2) return;
    _front = !_front;
    await _stopPreview();
    await _c?.dispose();
    _c = null;
    if (mounted) setState(() {});
    await _start();
  }

  Future<void> _shutter() async {
    final c = _c;
    if (c == null || _busy) return;
    if (_video) {
      await (_rec ? _stopVideo() : _startVideo());
      return;
    }
    _busy = true;
    try {
      for (var s = _timer; s > 0; s--) {
        if (!mounted) return;
        setState(() => _countdown = s);
        await _link({'cd': s});
        await Future.delayed(const Duration(seconds: 1));
      }
      if (mounted) setState(() => _countdown = 0);
      await _stopPreview();
      final file = await c.takePicture();
      await _link({'ok': 1});
      await Gal.putImage(file.path);
      if (mounted) {
        ScaffoldMessenger.of(
          context,
        ).showSnackBar(const SnackBar(content: Text('Saved to the gallery'), duration: Duration(seconds: 1)));
      }
    } catch (e) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Couldn\'t take it: $e')));
    } finally {
      _busy = false;
      await _startPreview();
    }
  }

  Future<void> _startVideo() async {
    final c = _c;
    if (c == null) return;
    await _stopPreview();
    try {
      await c.startVideoRecording();
      _rec = true;
    } catch (_) {
      _rec = false;
    }
    if (mounted) setState(() {});
    await _sendState();
  }

  Future<void> _stopVideo() async {
    final c = _c;
    if (c == null) return;
    try {
      final f = await c.stopVideoRecording();
      await Gal.putVideo(f.path);
    } catch (_) {}
    _rec = false;
    if (mounted) setState(() {});
    await _sendState();
    await _startPreview();
  }

  @override
  Widget build(BuildContext context) {
    final c = _c;
    final connected = ref.watch(bleProvider).isConnected;
    return Scaffold(
      backgroundColor: Colors.black,
      appBar: AppBar(
        title: const Text('Camera remote'),
        backgroundColor: Colors.black,
        actions: [
          Padding(
            padding: const EdgeInsets.only(right: 12),
            child: Icon(Icons.watch_rounded, color: connected ? AppColors.success : AppColors.textFaint),
          ),
        ],
      ),
      body: Column(
        children: [
          Expanded(
            child: Center(
              child: _error != null
                  ? Padding(
                      padding: const EdgeInsets.all(24),
                      child: Text(_error!, style: const TextStyle(color: AppColors.textDim)),
                    )
                  : c == null || !c.value.isInitialized
                  ? const CircularProgressIndicator()
                  : Stack(
                      alignment: Alignment.center,
                      children: [
                        CameraPreview(c),
                        if (_countdown > 0)
                          Text(
                            '$_countdown',
                            style: const TextStyle(fontSize: 120, fontWeight: FontWeight.w700, color: Colors.white),
                          ),
                        if (_rec) const Positioned(top: 12, child: Icon(Icons.fiber_manual_record, color: Colors.red)),
                      ],
                    ),
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 28),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: [
                _btn(
                  _timer == 0 ? Icons.timer_off_outlined : Icons.timer_outlined,
                  _timer == 0 ? 'Timer' : '${_timer}s',
                  () => _onWatch('timer'),
                ),
                _btn(
                  switch (_flash) {
                    FlashMode.auto => Icons.flash_auto,
                    FlashMode.always || FlashMode.torch => Icons.flash_on,
                    _ => Icons.flash_off,
                  },
                  'Flash',
                  () => _onWatch('flash'),
                ),
                GestureDetector(
                  onTap: _shutter,
                  child: Container(
                    width: 76,
                    height: 76,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      border: Border.all(color: Colors.white, width: 5),
                    ),
                    alignment: Alignment.center,
                    child: Container(
                      width: _video && _rec ? 30 : 56,
                      height: _video && _rec ? 30 : 56,
                      decoration: BoxDecoration(
                        color: _video ? Colors.red : Colors.white,
                        shape: _video && _rec ? BoxShape.rectangle : BoxShape.circle,
                        borderRadius: _video && _rec ? BorderRadius.circular(6) : null,
                      ),
                    ),
                  ),
                ),
                _btn(
                  _video ? Icons.videocam_rounded : Icons.photo_camera_rounded,
                  _video ? 'Video' : 'Photo',
                  () => _onWatch('mode'),
                ),
                _btn(Icons.cameraswitch_rounded, 'Flip', () => _onWatch('flip')),
              ],
            ),
          ),
        ],
      ),
    );
  }

  static Widget _btn(IconData i, String label, VoidCallback onTap) => InkWell(
    onTap: onTap,
    borderRadius: BorderRadius.circular(24),
    child: Padding(
      padding: const EdgeInsets.all(6),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(i, color: Colors.white),
          const SizedBox(height: 4),
          Text(label, style: const TextStyle(color: Colors.white70, fontSize: 11)),
        ],
      ),
    ),
  );
}
