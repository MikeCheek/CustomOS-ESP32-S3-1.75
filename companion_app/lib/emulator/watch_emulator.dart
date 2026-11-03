import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

/// The watch emulator's native library (android/app/src/main/cpp): the
/// firmware's own screen code, built for the phone. One emulated watch per
/// app run; see tools/watch_sim/emu_main.cpp for what each call does.
class WatchEmulator {
  WatchEmulator._(this._lib)
      : _init = _lib.lookupFunction<Int32 Function(Pointer<Utf8>), int Function(Pointer<Utf8>)>('emu_init'),
        _step = _lib.lookupFunction<Void Function(), void Function()>('emu_step'),
        _seq = _lib.lookupFunction<Uint32 Function(), int Function()>('emu_frame_seq'),
        _frame = _lib.lookupFunction<Void Function(Pointer<Uint8>), void Function(Pointer<Uint8>)>('emu_frame_rgba'),
        _touch = _lib.lookupFunction<Void Function(Int32, Int32, Int32), void Function(int, int, int)>('emu_touch'),
        _button = _lib.lookupFunction<Void Function(Int32, Int32), void Function(int, int)>('emu_button'),
        width = _lib.lookupFunction<Int32 Function(), int Function()>('emu_width')(),
        height = _lib.lookupFunction<Int32 Function(), int Function()>('emu_height')();

  static WatchEmulator? _instance;

  /// Null where there's no native library (anything but Android for now).
  static WatchEmulator? get instance {
    if (_instance != null) return _instance;
    if (!Platform.isAndroid) return null;
    try {
      return _instance = WatchEmulator._(DynamicLibrary.open('libwatchemu.so'));
    } catch (_) {
      return null;
    }
  }

  final DynamicLibrary _lib;
  final int Function(Pointer<Utf8>) _init;
  final void Function() _step;
  final int Function() _seq;
  final void Function(Pointer<Uint8>) _frame;
  final void Function(int, int, int) _touch;
  final void Function(int, int) _button;
  final int width, height;
  Pointer<Uint8>? _buf;
  bool _started = false;

  void start(String dir) {
    if (_started) return;
    _started = true;
    final p = dir.toNativeUtf8();
    _init(p);
    malloc.free(p);
  }

  /// Runs the watch's loop once.
  void step() => _step();

  /// Changes whenever the watch drew a new frame.
  int get frameSeq => _seq();

  /// The current frame, RGBA, width x height (a copy).
  Uint8List frame() {
    final n = width * height * 4;
    _buf ??= malloc<Uint8>(n);
    _frame(_buf!);
    return Uint8List.fromList(_buf!.asTypedList(n));
  }

  /// [down]: the finger is on the screen (moves too); false lifts it.
  void touch(int x, int y, bool down) => _touch(x, y, down ? 1 : 0);

  static const boot = 0, power = 1;
  static const shortPress = 1, doublePress = 2, longPress = 3;
  void button(int which, int event) => _button(which, event);
}
