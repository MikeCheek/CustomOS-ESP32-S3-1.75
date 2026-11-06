import 'dart:convert';
import 'dart:io';

import 'package:path_provider/path_provider.dart';

/// One model run: what ran, where, for how long, and how it ended.
class AiRun {
  final String model; // e.g. "Qwen3 0.6B", "Whisper base (whisper.cpp)"
  final String task; // e.g. "Summary", "Transcription", "Load"
  final String where; // e.g. "GPU", "CPU, 4 threads"
  final DateTime at;
  final int ms; // how long it took (0 for a crash: unknown)
  final double? input; // how much it was given...
  final String? unit; // ...in what: "chars", "s" (seconds of audio)
  final String? error; // null: it worked
  final bool crash; // the app stopped while it was running

  const AiRun({
    required this.model,
    required this.task,
    required this.where,
    required this.at,
    required this.ms,
    this.input,
    this.unit,
    this.error,
    this.crash = false,
  });

  bool get ok => error == null && !crash;

  Map<String, dynamic> toJson() => {
    'm': model,
    't': task,
    'w': where,
    'at': at.millisecondsSinceEpoch,
    'ms': ms,
    if (input != null) 'in': input,
    if (unit != null) 'u': unit,
    if (error != null) 'e': error,
    if (crash) 'c': 1,
  };

  static AiRun fromJson(Map<String, dynamic> j) => AiRun(
    model: j['m'] as String? ?? '?',
    task: j['t'] as String? ?? '?',
    where: j['w'] as String? ?? '?',
    at: DateTime.fromMillisecondsSinceEpoch((j['at'] as num?)?.toInt() ?? 0),
    ms: (j['ms'] as num?)?.toInt() ?? 0,
    input: (j['in'] as num?)?.toDouble(),
    unit: j['u'] as String?,
    error: j['e'] as String?,
    crash: j['c'] == 1,
  );
}

/// Numbers for one model, from its runs that worked.
class AiStats {
  final String model;
  final List<AiRun> runs; // oldest first, all of them (also the failed ones)
  const AiStats(this.model, this.runs);

  /// The runs that worked - not counting loading the model, a one-off that would skew the average.
  List<AiRun> get good {
    final g = runs.where((r) => r.ok && r.task != 'Load').toList();
    return g.isEmpty ? runs.where((r) => r.ok).toList() : g;
  }

  /// The same numbers for each task (Load, Memo summary, Transcription...).
  List<AiStats> get byTask {
    final m = <String, List<AiRun>>{};
    for (final r in runs) {
      m.putIfAbsent(r.task, () => []).add(r);
    }
    return [for (final e in m.entries) AiStats(e.key, e.value)];
  }

  int get count => runs.length;
  int get failures => runs.where((r) => !r.ok).length;
  int get avgMs {
    final g = good;
    return g.isEmpty ? 0 : g.fold<int>(0, (a, r) => a + r.ms) ~/ g.length;
  }

  int get minMs => good.isEmpty ? 0 : good.map((r) => r.ms).reduce((a, b) => a < b ? a : b);
  int get maxMs => good.isEmpty ? 0 : good.map((r) => r.ms).reduce((a, b) => a > b ? a : b);
  Set<String> get wheres => runs.map((r) => r.where).toSet();
  Set<String> get tasks => runs.map((r) => r.task).toSet();

  /// Seconds of work per second of audio (transcription, who said what) - under 1 is faster than real time.
  double? get realTimeFactor {
    final g = good.where((r) => r.unit == 's' && (r.input ?? 0) > 0).toList();
    if (g.isEmpty) return null;
    final work = g.fold<double>(0, (a, r) => a + r.ms / 1000);
    final audio = g.fold<double>(0, (a, r) => a + r.input!);
    return work / audio;
  }
}

/// Times every on-phone model run (AI models > Speed > Performance): the
/// language model (loading it too), transcription, who said what, the
/// voices, search by meaning, Look. Kept in a file (the last 800 runs).
/// Before each run a "running" note is written and removed after it: one
/// still there at the next start means the app died during that run - a
/// native crash isn't an exception - and it's listed as a crash.
class AiProfiler {
  AiProfiler._();
  static final instance = AiProfiler._();

  static const _max = 800;
  List<AiRun>? _runs;
  Future<void>? _loading;

  /// Tests: a folder instead of the app's support folder (and a fresh start).
  static Directory? debugDir;
  static AiProfiler fresh() => AiProfiler._();

  Future<File> _file(String name) async => File('${(debugDir ?? await getApplicationSupportDirectory()).path}/$name');

  Future<void> _ensure() => _loading ??= () async {
    final runs = <AiRun>[];
    try {
      final f = await _file('ai_profile.json');
      if (await f.exists()) {
        for (final j in jsonDecode(await f.readAsString()) as List) {
          runs.add(AiRun.fromJson(Map<String, dynamic>.from(j as Map)));
        }
      }
    } catch (_) {}
    // a run that never finished: the app stopped while it was going
    try {
      final p = await _file('ai_running.json');
      if (await p.exists()) {
        final j = Map<String, dynamic>.from(jsonDecode(await p.readAsString()) as Map);
        runs.add(
          AiRun(
            model: j['m'] as String? ?? '?',
            task: j['t'] as String? ?? '?',
            where: j['w'] as String? ?? '?',
            at: DateTime.fromMillisecondsSinceEpoch((j['at'] as num?)?.toInt() ?? 0),
            ms: 0,
            error: 'The app stopped while this was running: a crash, or the app was closed',
            crash: true,
          ),
        );
        await p.delete();
      }
    } catch (_) {}
    _runs = runs;
    await _save();
  }();

  Future<void> _save() async {
    final r = _runs;
    if (r == null) return;
    if (r.length > _max) r.removeRange(0, r.length - _max);
    try {
      await (await _file('ai_profile.json')).writeAsString(jsonEncode([for (final x in r) x.toJson()]));
    } catch (_) {}
  }

  /// At app start: turns a run left unfinished by a crash into a crash entry.
  Future<void> init() => _ensure();

  /// Runs [run] and records it. [where] is asked before and after the run
  /// (the language model only knows GPU or CPU once it has loaded).
  Future<T> track<T>({
    required String model,
    required String task,
    required String Function() where,
    required Future<T> Function() run,
    double? input,
    String? unit,
  }) async {
    await _ensure();
    final start = DateTime.now();
    File? note;
    try {
      note = await _file('ai_running.json');
      await note.writeAsString(
        jsonEncode({'m': model, 't': task, 'w': where(), 'at': start.millisecondsSinceEpoch}),
        flush: true,
      );
    } catch (_) {}
    final sw = Stopwatch()..start();
    try {
      final v = await run();
      _add(
        AiRun(
          model: model,
          task: task,
          where: where(),
          at: start,
          ms: sw.elapsedMilliseconds,
          input: input,
          unit: unit,
        ),
      );
      return v;
    } catch (e) {
      final msg = e.toString();
      _add(
        AiRun(
          model: model,
          task: task,
          where: where(),
          at: start,
          ms: sw.elapsedMilliseconds,
          input: input,
          unit: unit,
          error: msg.length > 300 ? '${msg.substring(0, 300)}...' : msg,
        ),
      );
      rethrow;
    } finally {
      try {
        if (note != null && await note.exists()) await note.delete();
      } catch (_) {}
    }
  }

  void _add(AiRun r) {
    _runs!.add(r);
    _save();
  }

  Future<List<AiRun>> runs() async {
    await _ensure();
    return List.unmodifiable(_runs!);
  }

  /// Per model, the slowest on average first.
  Future<List<AiStats>> stats() async {
    final by = <String, List<AiRun>>{};
    for (final r in await runs()) {
      by.putIfAbsent(r.model, () => []).add(r);
    }
    final out = [for (final e in by.entries) AiStats(e.key, e.value)];
    out.sort((a, b) => b.avgMs.compareTo(a.avgMs));
    return out;
  }

  Future<void> clear() async {
    await _ensure();
    _runs!.clear();
    await _save();
  }

  /// Seconds of 16 kHz 16-bit mono audio in a WAV file (for the speed per second of audio).
  static double wavSeconds(String path) {
    try {
      final n = File(path).lengthSync() - 44;
      return n > 0 ? n / 32000 : 0;
    } catch (_) {
      return 0;
    }
  }
}
