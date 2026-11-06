import 'package:flutter/material.dart';

import '../services/ai_advice.dart';
import '../services/ai_profiler.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Every on-phone model's run times (AI models > Speed > Performance): per
/// model the average, the fastest and the slowest, a chart of the recent
/// runs, where it ran (GPU, or CPU and threads), and its errors and crashes.
class AiProfileScreen extends StatefulWidget {
  const AiProfileScreen({super.key});

  @override
  State<AiProfileScreen> createState() => _AiProfileScreenState();
}

class _AiProfileScreenState extends State<AiProfileScreen> {
  List<AiStats>? _stats;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final s = await AiProfiler.instance.stats();
    if (mounted) setState(() => _stats = s);
  }

  Future<void> _clear() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Clear the run times?'),
        content: const Text('Every model\'s timings, errors and crashes are forgotten.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(c, true), child: const Text('Clear')),
        ],
      ),
    );
    if (ok != true) return;
    await AiProfiler.instance.clear();
    await _load();
  }

  @override
  Widget build(BuildContext context) {
    final s = _stats;
    return Scaffold(
      appBar: AppBar(
        title: const Text('AI performance'),
        actions: [
          IconButton(tooltip: 'Refresh', icon: const Icon(Icons.refresh_rounded), onPressed: _load),
          if (s != null && s.isNotEmpty)
            IconButton(tooltip: 'Clear', icon: const Icon(Icons.delete_outline_rounded), onPressed: _clear),
        ],
      ),
      body: s == null
          ? const Center(child: CircularProgressIndicator())
          : s.isEmpty
          ? const Padding(
              padding: EdgeInsets.all(32),
              child: Center(
                child: Text(
                  'Nothing has run yet. Transcribe a memo, ask the assistant or use Look - each model\'s '
                  'run times show up here.',
                  textAlign: TextAlign.center,
                  style: TextStyle(color: AppColors.textDim, height: 1.4),
                ),
              ),
            )
          : RefreshIndicator(
              onRefresh: _load,
              child: ListView(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
                children: [
                  const Text(
                    'How long each model took on this phone. Average, fastest and slowest count the runs that '
                    'worked, without loading the model (it has its own line); red bars are failed runs and crashes.',
                    style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
                  ),
                  const SizedBox(height: 12),
                  if (aiAdvice(s).isNotEmpty) _Advice(aiAdvice(s)),
                  for (final m in s) _ModelCard(m),
                ],
              ),
            ),
    );
  }
}

String _dur(int ms) {
  if (ms < 1000) return '$ms ms';
  if (ms < 60000) return '${(ms / 1000).toStringAsFixed(ms < 10000 ? 1 : 0)} s';
  return '${ms ~/ 60000} min ${(ms % 60000) ~/ 1000} s';
}

String _when(DateTime t) {
  final d = DateTime.now().difference(t);
  if (d.inMinutes < 1) return 'just now';
  if (d.inHours < 1) return '${d.inMinutes} min ago';
  if (d.inDays < 1) return '${d.inHours} h ago';
  return '${t.day}/${t.month} ${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
}

class _ModelCard extends StatelessWidget {
  final AiStats s;
  const _ModelCard(this.s);

  @override
  Widget build(BuildContext context) {
    final rtf = s.realTimeFactor;
    final bad = s.runs.where((r) => !r.ok).toList().reversed.take(5).toList();
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Panel(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              s.model,
              style: const TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w600),
            ),
            const SizedBox(height: 4),
            Wrap(
              spacing: 6,
              runSpacing: 4,
              children: [
                for (final w in s.wheres) _Chip(w, w.startsWith('GPU') ? AppColors.success : AppColors.accent2),
                for (final t in s.tasks) _Chip(t, AppColors.textDim),
              ],
            ),
            const SizedBox(height: 10),
            Row(
              children: [
                _Num('Average', s.good.isEmpty ? '-' : _dur(s.avgMs)),
                _Num('Fastest', s.good.isEmpty ? '-' : _dur(s.minMs)),
                _Num('Slowest', s.good.isEmpty ? '-' : _dur(s.maxMs)),
                _Num('Runs', '${s.count}${s.failures > 0 ? ' (${s.failures} failed)' : ''}'),
              ],
            ),
            if (s.tasks.length > 1) ...[
              const SizedBox(height: 8),
              for (final t in s.byTask)
                Padding(
                  padding: const EdgeInsets.only(top: 2),
                  child: Text(
                    t.good.isEmpty
                        ? '${t.model}: ${t.count} run${t.count == 1 ? '' : 's'}, all failed'
                        : '${t.model}: ${_dur(t.avgMs)} average, ${_dur(t.minMs)}-${_dur(t.maxMs)} '
                              '(${t.count} run${t.count == 1 ? '' : 's'})',
                    style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                  ),
                ),
            ],
            if (rtf != null)
              Padding(
                padding: const EdgeInsets.only(top: 6),
                child: Text(
                  '${rtf.toStringAsFixed(rtf < 0.1 ? 3 : 2)} s of work per second of audio '
                  '(${rtf < 1 ? '${(1 / rtf).toStringAsFixed(1)}x faster than real time' : 'slower than real time'})',
                  style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                ),
              ),
            const SizedBox(height: 10),
            SizedBox(
              height: 90,
              child: CustomPaint(painter: _Bars(s), size: Size.infinite),
            ),
            const SizedBox(height: 4),
            Text(
              'Last ${s.runs.length > 40 ? 40 : s.runs.length} runs, oldest on the left - last ${_when(s.runs.last.at)}',
              style: const TextStyle(color: AppColors.textDim, fontSize: 11),
            ),
            if (bad.isNotEmpty) ...[
              const SizedBox(height: 10),
              for (final r in bad)
                Padding(
                  padding: const EdgeInsets.only(top: 4),
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Icon(
                        r.crash ? Icons.dangerous_rounded : Icons.error_outline_rounded,
                        size: 16,
                        color: AppColors.danger,
                      ),
                      const SizedBox(width: 6),
                      Expanded(
                        child: Text(
                          '${r.crash ? 'Crash' : 'Failed'} - ${r.task} on ${r.where}, ${_when(r.at)}: ${r.error}',
                          style: const TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.3),
                        ),
                      ),
                    ],
                  ),
                ),
            ],
          ],
        ),
      ),
    );
  }
}

/// What the numbers suggest for this phone (ai_advice.dart).
class _Advice extends StatelessWidget {
  final List<String> tips;
  const _Advice(this.tips);

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.only(bottom: 12),
    child: Panel(
      color: AppColors.accent.withValues(alpha: 0.10),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Row(
            children: [
              Icon(Icons.tips_and_updates_outlined, size: 18, color: AppColors.accent),
              SizedBox(width: 8),
              Text(
                'For this phone',
                style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
              ),
            ],
          ),
          for (final t in tips)
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Text(t, style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4)),
            ),
        ],
      ),
    ),
  );
}

class _Chip extends StatelessWidget {
  final String text;
  final Color color;
  const _Chip(this.text, this.color);

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
    decoration: BoxDecoration(color: color.withValues(alpha: 0.14), borderRadius: BorderRadius.circular(10)),
    child: Text(
      text,
      style: TextStyle(color: color, fontSize: 11, fontWeight: FontWeight.w600),
    ),
  );
}

class _Num extends StatelessWidget {
  final String label, value;
  const _Num(this.label, this.value);

  @override
  Widget build(BuildContext context) => Expanded(
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(label, style: const TextStyle(color: AppColors.textDim, fontSize: 11)),
        const SizedBox(height: 2),
        Text(
          value,
          style: const TextStyle(color: AppColors.text, fontSize: 14, fontWeight: FontWeight.w600),
        ),
      ],
    ),
  );
}

/// The recent runs as bars (time on the height), the average as a dashed line.
class _Bars extends CustomPainter {
  final AiStats s;
  _Bars(this.s);

  @override
  void paint(Canvas c, Size size) {
    final runs = s.runs.length > 40 ? s.runs.sublist(s.runs.length - 40) : s.runs;
    if (runs.isEmpty) return;
    final top = runs.fold<int>(1, (a, r) => r.ms > a ? r.ms : a).toDouble();
    final slot = size.width / runs.length;
    final w = (slot * 0.7).clamp(2.0, 18.0);
    for (var i = 0; i < runs.length; i++) {
      final r = runs[i];
      // a failed run or a crash: a thin red mark the full height, whatever its time
      final h = r.ok ? (r.ms / top) * (size.height - 2) : size.height;
      final x = i * slot + (slot - w) / 2;
      final p = Paint()
        ..color = !r.ok
            ? AppColors.danger
            : (r.where.startsWith('GPU') ? AppColors.success : AppColors.accent).withValues(alpha: 0.85);
      c.drawRRect(
        RRect.fromRectAndRadius(Rect.fromLTWH(x, size.height - h, r.ok ? w : w * 0.5 + 1, h), const Radius.circular(2)),
        p,
      );
    }
    if (s.good.isNotEmpty) {
      final y = size.height - (s.avgMs / top) * (size.height - 2);
      final dash = Paint()
        ..color = AppColors.text.withValues(alpha: 0.6)
        ..strokeWidth = 1;
      for (double x = 0; x < size.width; x += 8) {
        c.drawLine(Offset(x, y), Offset((x + 4).clamp(0, size.width), y), dash);
      }
    }
  }

  @override
  bool shouldRepaint(covariant _Bars old) => old.s != s;
}
