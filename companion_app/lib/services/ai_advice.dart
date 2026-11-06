import 'ai_profiler.dart';

/// What the run times say about this phone (AI models > Performance): the
/// model choices that would make it faster, from what was measured here.
List<String> aiAdvice(List<AiStats> stats) {
  final out = <String>[];
  String x(double v) => v.toStringAsFixed(v < 10 ? 1 : 0);

  // ---- transcription: seconds of work per second of audio ----
  final asr = [
    for (final s in stats)
      if (s.tasks.contains('Transcription') && s.realTimeFactor != null) s,
  ]..sort((a, b) => a.realTimeFactor!.compareTo(b.realTimeFactor!));
  if (asr.isNotEmpty) {
    final slow = asr.last, fast = asr.first;
    if (slow.realTimeFactor! > 1) {
      out.add(
        '${slow.model} is slower than real time here (${x(slow.realTimeFactor!)} s per second of audio): '
        'Moonshine tiny (English) or a smaller Whisper would be several times faster.',
      );
    }
    if (asr.length > 1 && fast.realTimeFactor! < slow.realTimeFactor!) {
      out.add(
        'Fastest transcription on this phone: ${fast.model}, '
        '${x(slow.realTimeFactor! / fast.realTimeFactor!)}x quicker than ${slow.model}.',
      );
    }
  }

  // ---- the language model ----
  for (final s in stats) {
    final load = s.byTask.where((t) => t.model == 'Load').firstOrNull;
    if (load == null) continue;
    final answers = [
      for (final t in s.byTask)
        if (t.model != 'Load' && t.good.isNotEmpty) t,
    ];
    final onCpu = load.runs.where((r) => r.ok).every((r) => r.where == 'CPU');
    if (load.good.isNotEmpty && onCpu) {
      out.add(
        '${s.model} has only run on the CPU. The GPU is usually several times faster at reading long '
        'prompts: set Speed to Auto or GPU (it falls back to the CPU if the GPU fails).',
      );
    }
    final slowest = answers.isEmpty ? null : answers.reduce((a, b) => a.avgMs > b.avgMs ? a : b);
    if (slowest != null && slowest.avgMs > 20000) {
      out.add(
        '${s.model} takes ${x(slowest.avgMs / 1000)} s on average for "${slowest.model}": '
        'a smaller language model would answer much sooner.',
      );
    }
  }

  // ---- what never works ----
  for (final s in stats) {
    if (s.count >= 2 && s.good.isEmpty) {
      out.add('${s.model} failed every time it ran (${s.count} runs): reinstall it or pick another in AI models.');
    }
  }
  return out;
}
