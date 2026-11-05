import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/widgets.dart';

import 'native_service.dart';

/// Between the watch's camera remote (firmware/app_camera.h) and a camera:
///  - the app's own camera screen: the watch's buttons ({"e":"cam","a":...})
///    reach it while it's open; "open" from the watch opens it;
///  - another camera app (Android, NativeCamera.kt): when one opens, the
///    phone asks "use the watch?"; say yes and the watch shows the screen
///    and presses that app's buttons (shutter, flip, zoom, a self-timer here).
class CameraRemote {
  CameraRemote._();
  static final instance = CameraRemote._();

  /// The camera screen is showing (it owns the camera then).
  bool screenOpen = false;

  /// Another app has the camera (Android tells): the watch was told.
  bool externalOpen = false;

  /// The watch runs another camera app (NativeCamera), with its view or not.
  bool nativeOn = false, nativeView = false;
  int _timer = 0;
  bool _busy = false, _sending = false;
  int _frame = 1;

  /// Set by the camera screen while it's open.
  void Function(String action)? onAction;

  /// Opens the camera screen (set by the app, through its navigator).
  void Function()? openScreen;

  /// To the watch: a {"t":"cam",...} message, and a live-view JPEG (set by the companion).
  Future<void> Function(Map<String, dynamic> m)? send;
  Future<void> Function(int hash, Uint8List jpeg)? sendView;

  final _native = NativeService();

  Future<void> _state() async {
    await send?.call(
      nativeOn
          ? {'on': 1, 'nat': 1, 'tm': _timer, 'pv': nativeView ? 1 : 0}
          : (externalOpen ? {'on': 1, 'ext': 1} : {'on': 0}),
    );
  }

  /// Another app opened the camera (or closed it).
  Future<void> onCameraInUse(bool on) async {
    if (screenOpen) return;
    if (on == externalOpen) return;
    externalOpen = on;
    if (on) {
      await _state();
      await _native.camPrompt();
    } else {
      final was = nativeOn;
      await _native.camNativeStop(); // its state event turns nativeOn off
      if (!was) await _state();
    }
  }

  /// From NativeCamera: the mode starting / stopping, and frames of the view.
  void onNative(Map<String, dynamic> e) {
    if (e['k'] == 'frame') {
      final j = e['jpeg'];
      if (!nativeOn || _sending || j is! Uint8List || sendView == null) return;
      _sending = true;
      _frame = (_frame % 0x7FFFFFF0) + 1;
      sendView!(_frame, j).whenComplete(() => _sending = false);
      return;
    }
    final on = e['on'] == true, view = e['view'] == true;
    if (on == nativeOn && view == nativeView) return;
    nativeOn = on;
    nativeView = view;
    if (on) externalOpen = true;
    _state();
  }

  void onWatch(Map<String, dynamic> e) {
    final a = e['a'] as String? ?? '';
    if (nativeOn) {
      _nativeAction(a);
      return;
    }
    if (a == 'native' || (a == 'open' && externalOpen && Platform.isAndroid)) {
      // the watch asks to run the camera app that's open: the buttons need the
      // accessibility service; the view needs a tap on the phone (the notification)
      _startNative();
      return;
    }
    if (a == 'open') {
      if (screenOpen) return;
      // in the background, Android won't let the app come forward by itself:
      // a notification to tap does it
      final front = WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed;
      if (!front && Platform.isAndroid) _native.cameraNotify();
      openScreen?.call(); // there when the app comes up
      return;
    }
    onAction?.call(a);
  }

  Future<void> _startNative() async {
    // the buttons need the Camera Helper; the view needs a tap on the phone (the notification)
    await _native.camNativeStart();
    if (!await _native.camAccessOn()) await send?.call({'msg': 'Tap the notification on the phone'});
    await _native.camPrompt(force: true);
  }

  /// Why a button didn't work: the helper isn't there / isn't on, or [otherwise].
  Future<String> _why(String otherwise) async {
    if (!await _native.camHelperInstalled()) return 'Needs the Camera Helper app';
    if (!await _native.camAccessOn()) return 'Turn the Camera Helper on';
    return otherwise;
  }

  Future<void> _nativeAction(String a) async {
    switch (a) {
      case 'shot':
        if (_busy) return;
        _busy = true;
        try {
          for (var s = _timer; s > 0; s--) {
            await send?.call({'cd': s});
            await Future.delayed(const Duration(seconds: 1));
          }
          final ok = await _native.camNativeAction('shot');
          await send?.call(ok ? {'ok': 1} : {'cd': 0, 'msg': await _why('Couldn\'t press the shutter')});
        } finally {
          _busy = false;
        }
      case 'timer':
        _timer = _timer == 0 ? 3 : (_timer == 3 ? 10 : 0);
        await _state();
      case 'flip' || 'zoom+' || 'zoom-':
        if (!await _native.camNativeAction(a)) {
          await send?.call({'msg': await _why(a == 'flip' ? 'No flip button found' : 'Couldn\'t zoom')});
        }
      case 'view':
        await _native.camPrompt(force: true);
      case 'stop':
        await _native.camNativeStop();
    }
  }
}
