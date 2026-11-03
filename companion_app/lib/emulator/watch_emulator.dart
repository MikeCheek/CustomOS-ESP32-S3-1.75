import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import '../services/ble_service.dart';

/// The watch emulator's native library (android/app/src/main/cpp): the
/// firmware's own screen code, built for the phone. One emulated watch per
/// app run; see tools/watch_sim/emu_main.cpp for what each call does.
class WatchEmulator implements VirtualWatch {
  WatchEmulator._(this._lib)
      : _init = _lib.lookupFunction<Int32 Function(Pointer<Utf8>), int Function(Pointer<Utf8>)>('emu_init'),
        _step = _lib.lookupFunction<Void Function(), void Function()>('emu_step'),
        _seq = _lib.lookupFunction<Uint32 Function(), int Function()>('emu_frame_seq'),
        _frame = _lib.lookupFunction<Void Function(Pointer<Uint8>), void Function(Pointer<Uint8>)>('emu_frame_rgba'),
        _touch = _lib.lookupFunction<Void Function(Int32, Int32, Int32), void Function(int, int, int)>('emu_touch'),
        _button = _lib.lookupFunction<Void Function(Int32, Int32), void Function(int, int)>('emu_button'),
        _connected = _lib.lookupFunction<Void Function(Int32), void Function(int)>('emu_set_connected'),
        _push = _lib.lookupFunction<Void Function(Pointer<Utf8>), void Function(Pointer<Utf8>)>('emu_link_push'),
        _pop = _lib.lookupFunction<Int32 Function(Pointer<Utf8>, Int32), int Function(Pointer<Utf8>, int)>('emu_link_pop'),
        _data = _lib.lookupFunction<Void Function(Int32, Pointer<Utf8>), void Function(int, Pointer<Utf8>)>('emu_set_data'),
        _accel = _lib.lookupFunction<Void Function(Float, Float, Float), void Function(double, double, double)>(
            'emu_set_accel'),
        _sounds = _lib.lookupFunction<Int32 Function(Pointer<Int32>, Int32), int Function(Pointer<Int32>, int)>(
            'emu_take_sounds'),
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
  final void Function(int) _connected;
  final void Function(Pointer<Utf8>) _push;
  final int Function(Pointer<Utf8>, int) _pop;
  final void Function(int, Pointer<Utf8>) _data;
  final void Function(double, double, double) _accel;
  final int Function(Pointer<Int32>, int) _sounds;
  Pointer<Int32>? _soundBuf;
  Pointer<Utf8>? _popBuf;
  int _popCap = 0;
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

  // ---- the phone link, in-process (phase 2) -------------------------------------------
  /// The app is (or stops being) the watch's phone.
  void setConnected(bool on) => _connected(on ? 1 : 0);

  @override
  void deliver(String json) {
    final p = json.toNativeUtf8();
    _push(p);
    malloc.free(p);
  }

  @override
  void deliverData(int kind, String json) {
    final p = json.toNativeUtf8();
    _data(kind, p);
    malloc.free(p);
  }

  // ---- sensors and sound (phase 3) ---------------------------------------------------
  /// The phone's accelerometer, in g.
  void setAccel(double x, double y, double z) => _accel(x, y, z);

  /// Tones the watch played since the last call, as (freq, ms, kind | volume << 8)
  /// triples - what EmuIo.kt plays.
  List<int> takeSounds() {
    const max = 64;
    _soundBuf ??= malloc<Int32>(max * 3);
    final n = _sounds(_soundBuf!, max);
    return n <= 0 ? const [] : _soundBuf!.asTypedList(n * 3).toList();
  }

  /// What the watch sent to its phone since the last call.
  List<Map<String, dynamic>> takeMessages() {
    final out = <Map<String, dynamic>>[];
    for (;;) {
      if (_popBuf == null) {
        _popCap = 4096;
        _popBuf = malloc<Uint8>(_popCap).cast<Utf8>();
      }
      final n = _pop(_popBuf!, _popCap);
      if (n == 0) break;
      if (n < 0) {
        // bigger than the buffer: grow it, the message stays queued
        malloc.free(_popBuf!);
        _popCap = -n;
        _popBuf = malloc<Uint8>(_popCap).cast<Utf8>();
        continue;
      }
      try {
        final m = jsonDecode(_popBuf!.toDartString(length: n));
        if (m is Map<String, dynamic>) out.add(m);
      } catch (_) {}
    }
    return out;
  }
}
