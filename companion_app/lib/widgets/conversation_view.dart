import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/notes_provider.dart';
import '../services/ble_protocol.dart';
import '../services/memo_player.dart';
import '../services/speaker_merge.dart';
import '../theme/app_theme.dart';

const _voiceColors = [AppColors.accent2, AppColors.accent3, AppColors.warning, AppColors.success, AppColors.accent];

Color voiceColor(int speaker) => _voiceColors[speaker % _voiceColors.length];

String _clock(double s) {
  final t = s.floor();
  return '${t ~/ 60}:${(t % 60).toString().padLeft(2, '0')}';
}

/// A split memo as a conversation: who said what, the line being played
/// lit up. Tap a line to play from there, a name to rename that voice.
class ConversationView extends ConsumerWidget {
  final RecordingEntry entry;
  const ConversationView({super.key, required this.entry});

  Future<void> _rename(BuildContext context, WidgetRef ref, int speaker) async {
    final ctrl = TextEditingController(text: entry.speakerNames['$speaker'] ?? '');
    final name = await showDialog<String>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: AppColors.surfaceLight,
        title: Text('Who is ${speakerName(speaker, const {})}?'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            TextField(
              controller: ctrl,
              autofocus: true,
              textCapitalization: TextCapitalization.words,
              decoration: const InputDecoration(hintText: 'Name'),
              onSubmitted: (v) => Navigator.pop(ctx, v),
            ),
            if (entry.people.isNotEmpty) ...[
              const SizedBox(height: 12),
              Wrap(spacing: 8, runSpacing: 8, children: [
                for (final p in entry.people) ActionChip(label: Text(p), onPressed: () => Navigator.pop(ctx, p)),
              ]),
            ],
          ],
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, ''), child: const Text('Clear')),
          TextButton(onPressed: () => Navigator.pop(ctx, ctrl.text), child: const Text('Save')),
        ],
      ),
    );
    ctrl.dispose();
    if (name != null) await ref.read(notesProvider.notifier).renameSpeaker(entry.name, speaker, name);
  }

  Future<void> _playFrom(WidgetRef ref, double seconds) async {
    final player = ref.read(memoPlayerProvider.notifier);
    final p = ref.read(memoPlayerProvider);
    if (p.isMemo(entry.name) && p.durMs > 0) {
      await player.seekFraction(seconds * 1000 / p.durMs);
      if (!p.playing) await player.toggle(entry.name);
    } else {
      await player.toggle(entry.name, fromMs: (seconds * 1000).round());
    }
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final p = ref.watch(memoPlayerProvider);
    final at = p.isMemo(entry.name) && (p.playing || p.posMs > 0) ? p.posMs / 1000.0 : -1.0;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        for (final l in entry.speakers)
          InkWell(
            borderRadius: BorderRadius.circular(10),
            onTap: () => _playFrom(ref, l.start),
            child: Container(
              width: double.infinity,
              margin: const EdgeInsets.symmetric(vertical: 3),
              padding: const EdgeInsets.fromLTRB(10, 8, 10, 8),
              decoration: BoxDecoration(
                borderRadius: BorderRadius.circular(10),
                color: at >= l.start && at < l.end ? voiceColor(l.speaker).withValues(alpha: 0.12) : null,
                border: Border(left: BorderSide(color: voiceColor(l.speaker), width: 3)),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(children: [
                    GestureDetector(
                      onTap: () => _rename(context, ref, l.speaker),
                      child: Text(speakerName(l.speaker, entry.speakerNames),
                          style: TextStyle(color: voiceColor(l.speaker), fontSize: 13, fontWeight: FontWeight.w700)),
                    ),
                    const SizedBox(width: 8),
                    Text(_clock(l.start), style: const TextStyle(color: AppColors.textFaint, fontSize: 11)),
                  ]),
                  const SizedBox(height: 3),
                  Text(l.text, style: const TextStyle(color: AppColors.text, fontSize: 14, height: 1.45)),
                ],
              ),
            ),
          ),
      ],
    );
  }
}
