import 'dart:async';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';

/// Playback state of the memo player (one memo at a time).
class PlayerState {
  final String? name;      // memo file name being played, null = nothing loaded
  final bool playing;
  final int posMs, durMs;
  final double speed;
  final String? error;
  const PlayerState({this.name, this.playing = false, this.posMs = 0, this.durMs = 0, this.speed = 1.0, this.error});

  double get fraction => durMs > 0 ? (posMs / durMs).clamp(0.0, 1.0) : 0.0;
  bool isMemo(String n) => name == n;
}

/// Plays downloaded voice memos on the phone through Android's MediaPlayer
/// (MemoPlayer.kt) - no extra Flutter package. Position is polled while
/// playing.
class MemoPlayerNotifier extends StateNotifier<PlayerState> {
  static const _ch = MethodChannel('com.amoledwatch.player');
  Timer? _poll;

  MemoPlayerNotifier() : super(const PlayerState());

  static Future<String> _path(String name) async {
    final dir = await getApplicationDocumentsDirectory();
    return '${dir.path}/recordings/$name';
  }

  void _apply(dynamic m, {String? name}) {
    if (m is! Map || !mounted) return;
    final playing = m['playing'] == true;
    state = PlayerState(
      name: m['path'] == null ? null : (name ?? state.name),
      playing: playing,
      posMs: (m['pos'] as num?)?.toInt() ?? 0,
      durMs: (m['dur'] as num?)?.toInt() ?? 0,
      speed: (m['speed'] as num?)?.toDouble() ?? state.speed,
    );
    if (playing) {
      _poll ??= Timer.periodic(const Duration(milliseconds: 200), (_) => _refresh());
    } else {
      _poll?.cancel();
      _poll = null;
    }
  }

  Future<void> _refresh() async {
    try {
      _apply(await _ch.invokeMethod('state'));
    } catch (_) {}
  }

  /// Play [name] from the start (or from [fromMs]); if it's the loaded memo,
  /// toggles pause/resume instead.
  Future<void> toggle(String name, {int fromMs = 0}) async {
    try {
      if (state.name == name) {
        _apply(await _ch.invokeMethod(state.playing ? 'pause' : 'resume'));
        return;
      }
      _apply(await _ch.invokeMethod('play', {'path': await _path(name), 'from': fromMs}), name: name);
    } on PlatformException catch (e) {
      if (mounted) state = PlayerState(error: 'Can\'t play this recording: ${e.message ?? e.code}');
    }
  }

  Future<void> seekFraction(double f) async {
    if (state.name == null || state.durMs <= 0) return;
    _apply(await _ch.invokeMethod('seek', {'ms': (f.clamp(0.0, 1.0) * state.durMs).round()}));
  }

  Future<void> skip(int deltaMs) async {
    if (state.name == null) return;
    _apply(await _ch.invokeMethod('seek', {'ms': (state.posMs + deltaMs).clamp(0, state.durMs)}));
  }

  Future<void> cycleSpeed() async {
    const speeds = [1.0, 1.25, 1.5, 2.0, 0.75];
    final i = speeds.indexOf(state.speed);
    final next = speeds[(i < 0 ? 0 : i + 1) % speeds.length];
    _apply(await _ch.invokeMethod('speed', {'v': next}));
  }

  Future<void> stop() async {
    try {
      _apply(await _ch.invokeMethod('stop'));
    } catch (_) {}
    if (mounted) state = PlayerState(speed: state.speed);
  }

  @override
  void dispose() {
    _poll?.cancel();
    super.dispose();
  }
}

final StateNotifierProvider<MemoPlayerNotifier, PlayerState> memoPlayerProvider =
    StateNotifierProvider<MemoPlayerNotifier, PlayerState>((ref) => MemoPlayerNotifier());

String fmtMs(int ms) {
  final s = ms ~/ 1000;
  return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
}
