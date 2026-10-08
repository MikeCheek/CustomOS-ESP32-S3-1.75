import 'package:flutter/services.dart';

/// Records a voice memo with the phone's microphone (MemoRecorder.kt):
/// 16 kHz mono WAV, the same format the watch records.
class PhoneRecorder {
  static const _ch = MethodChannel('com.amoledwatch.recorder');

  /// Starts recording to [path]. False with [lastError] set if it couldn't.
  static Future<bool> start(String path) async => (await _ch.invokeMethod<bool>('start', {'path': path})) ?? false;

  static Future<void> pause() => _ch.invokeMethod('pause');
  static Future<void> resume() => _ch.invokeMethod('resume');

  /// Peak level 0-100 since the last call, recorded length, and any error.
  static Future<({int level, int ms, bool running, String? error})> poll() async {
    final m = await _ch.invokeMethod<Map<dynamic, dynamic>>('level') ?? const {};
    return (
      level: (m['level'] as num?)?.toInt() ?? 0,
      ms: (m['ms'] as num?)?.toInt() ?? 0,
      running: m['running'] == true,
      error: m['error'] as String?,
    );
  }

  /// Finishes the file; returns its length in ms.
  static Future<int> stop() async {
    final m = await _ch.invokeMethod<Map<dynamic, dynamic>>('stop') ?? const {};
    final err = m['error'] as String?;
    if (err != null) throw Exception(err);
    return (m['ms'] as num?)?.toInt() ?? 0;
  }

  /// Stops and deletes the file.
  static Future<void> cancel() => _ch.invokeMethod('cancel');
}
