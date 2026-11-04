import 'dart:async';
import 'dart:io';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../providers/ble_provider.dart';
import '../services/memo_player.dart';
import '../services/native_service.dart';
import '../services/phone_recorder.dart';
import 'watch_emulator.dart';

/// Runs the emulated watch, with or without its screen open: steps the
/// firmware, links it to the app as the watch (BleService.attachVirtual)
/// whenever no real watch is connected, and serves its accelerometer,
/// sound, microphone and playback. With "Keep it as my watch" on it starts
/// with the app and keeps running in the background like a real watch.
class EmulatorController {
  EmulatorController(this._ref);
  final Ref _ref;

  static const _sound = MethodChannel('com.amoledwatch.emu_sound');
  static const _accel = EventChannel('com.amoledwatch.emu_accel');
  static const _keepKey = 'emu_keep_running';

  final WatchEmulator? emu = WatchEmulator.instance;
  bool get available => emu != null;
  bool get running => _timer != null;

  /// The emulated watch is the one the app talks to right now.
  bool get linked => _ref.read(bleServiceProvider).isVirtual;

  Timer? _timer;
  bool _visible = false;
  StreamSubscription<dynamic>? _accelSub;
  bool _recording = false, _playing = false;
  Timer? _micPoll;
  Completer<void>? _recDone;
  bool _wasLinked = false;
  final _changes = StreamController<void>.broadcast();
  Stream<void> get changes => _changes.stream;

  static Future<bool> keepRunning() async => (await SharedPreferences.getInstance()).getBool(_keepKey) ?? false;
  Future<void> setKeepRunning(bool on) async {
    await (await SharedPreferences.getInstance()).setBool(_keepKey, on);
    _changes.add(null);
  }

  /// App start: back on if it was kept as the watch.
  Future<void> autoStart() async {
    if (available && await keepRunning()) await start();
  }

  Future<void> start() async {
    final e = emu;
    if (e == null || running) return;
    final d = await getApplicationSupportDirectory();
    e.start('${d.path}/watch_emulator');
    e.onSay = _play;
    _wasLinked = _ref.read(bleServiceProvider).attachVirtual(e);
    _accelSub = _accel.receiveBroadcastStream().listen((v) {
      if (v is List && v.length >= 3) {
        e.setAccel((v[0] as num).toDouble(), (v[1] as num).toDouble(), (v[2] as num).toDouble());
      }
    }, onError: (_) {});
    _ref.listen<PlayerState>(memoPlayerProvider, (prev, next) {
      if (_playing && prev?.name == '__emu' && (next.name != '__emu' || !next.playing)) {
        _playing = false;
        e.setPlaying(false);
      }
    });
    _schedule();
    _changes.add(null);
  }

  /// Stops it (unless it's kept as the watch and only the screen closed).
  Future<void> stop() async {
    final e = emu;
    if (e == null || !running) return;
    _timer?.cancel();
    _timer = null;
    e.setConnected(false);
    _ref.read(bleServiceProvider).detachVirtual(e);
    await _accelSub?.cancel();
    _accelSub = null;
    await _stopRec();
    _sound.invokeMethod('stop').catchError((_) => null);
    _changes.add(null);
  }

  /// The screen shows it: full speed; otherwise 10 steps a second are
  /// plenty for the link, timers and sensors, and spare the battery.
  set visible(bool v) {
    _visible = v;
    if (running) _schedule();
  }

  void _schedule() {
    _timer?.cancel();
    _timer = Timer.periodic(Duration(milliseconds: _visible || _floating ? 16 : 100), (_) => _tick());
  }

  // ---- floating over other apps (Android: a round watch you move, resize and tuck away) ----
  bool _floating = false;
  StreamSubscription<void>? _closedSub;
  bool get floating => _floating;

  /// Shows the watch over every app. False when Android still needs the
  /// "Display over other apps" permission (its settings page opens then).
  Future<bool> float(bool on) async {
    if (!Platform.isAndroid) return false;
    final n = NativeService();
    if (!on) {
      await n.overlayHide();
      _floating = false;
      if (running) _schedule();
      _changes.add(null);
      return true;
    }
    if (!await n.overlayCan()) {
      await n.overlayAsk();
      return false;
    }
    if (!running) await start();
    _closedSub ??= n.overlayClosed.listen((_) {
      _floating = false;
      if (running) _schedule();
      _changes.add(null);
    });
    _floating = await n.overlayShow();
    if (running) _schedule();
    _changes.add(null);
    return _floating;
  }

  void _tick() {
    final e = emu!;
    final ble = _ref.read(bleServiceProvider);
    // Linked whenever no real watch is connected; a real one stays the watch.
    if (!ble.isVirtual && !ble.isConnected) ble.attachVirtual(e);
    final linked = ble.isVirtual;
    if (linked && !_wasLinked) ble.reannounceVirtual();
    if (linked != _wasLinked) _changes.add(null);
    _wasLinked = linked;
    e.setConnected(linked);
    e.step();
    final msgs = e.takeMessages();
    if (linked) {
      for (final m in msgs) {
        ble.virtualIncoming(m);
      }
    }
    for (final ev in e.takeEvents()) {
      _onEvent(ev);
    }
    final tones = e.takeSounds();
    if (tones.isNotEmpty) _sound.invokeMethod('tones', tones).catchError((_) => null);
  }

  Future<void> _onEvent(Map<String, dynamic> ev) async {
    switch (ev['ev']) {
      case 'rec':
        if (ev['on'] == 1) {
          await _startRec(ev['path'] as String? ?? '');
        } else {
          await _stopRec();
        }
        break;
      case 'play':
        await _play(ev['path'] as String? ?? '');
        break;
      case 'stop':
        if (_playing) await _ref.read(memoPlayerProvider.notifier).stop();
        _playing = false;
        emu?.setPlaying(false);
        break;
    }
  }

  Future<void> _startRec(String path) async {
    final e = emu!;
    if (_recording || path.isEmpty) return;
    final done = Completer<void>();
    _recDone = done;
    e.recordingDone = done.future;
    if (!(await Permission.microphone.request()).isGranted || !await PhoneRecorder.start(path)) {
      done.complete();
      return;
    }
    _recording = true;
    _micPoll = Timer.periodic(const Duration(milliseconds: 80), (_) async {
      e.setMicLevel((await PhoneRecorder.poll()).level);
    });
  }

  Future<void> _stopRec() async {
    _micPoll?.cancel();
    _micPoll = null;
    emu?.setMicLevel(0);
    if (_recording) {
      _recording = false;
      try {
        await PhoneRecorder.stop();
      } catch (_) {}
    }
    final d = _recDone;
    _recDone = null;
    if (d != null && !d.isCompleted) d.complete();
  }

  Future<void> _play(String path) async {
    if (path.isEmpty) return;
    _playing = true;
    await _ref.read(memoPlayerProvider.notifier).playPath(path, name: '__emu');
  }
}

final emulatorProvider = Provider<EmulatorController>((ref) => EmulatorController(ref));
