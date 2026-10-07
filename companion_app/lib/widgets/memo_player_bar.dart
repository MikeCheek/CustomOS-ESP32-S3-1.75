import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../services/memo_player.dart';
import '../theme/app_theme.dart';
import 'common.dart';

/// Play / pause, scrubber, -10 s / +10 s and speed for one memo.
class MemoPlayerBar extends ConsumerWidget {
  final String name;
  const MemoPlayerBar({super.key, required this.name});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final p = ref.watch(memoPlayerProvider);
    final n = ref.read(memoPlayerProvider.notifier);
    final mine = p.isMemo(name);
    final playing = mine && p.playing;
    return Panel(
      padding: const EdgeInsets.fromLTRB(10, 8, 14, 8),
      child: Column(
        children: [
          Row(
            children: [
              IconButton(
                iconSize: 44,
                color: AppColors.accent,
                onPressed: () => n.toggle(name),
                icon: Icon(playing ? Icons.pause_circle_filled_rounded : Icons.play_circle_fill_rounded),
              ),
              Expanded(
                child: SliderTheme(
                  data: SliderTheme.of(context).copyWith(
                    trackHeight: 3,
                    thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 6),
                    overlayShape: SliderComponentShape.noOverlay,
                  ),
                  child: Slider(
                    value: mine ? p.fraction : 0,
                    onChanged: mine ? (v) => n.seekFraction(v) : null,
                  ),
                ),
              ),
            ],
          ),
          Row(
            children: [
              const SizedBox(width: 14),
              Text(mine ? '${fmtMs(p.posMs)} / ${fmtMs(p.durMs)}' : 'Tap play to listen',
                  style: const TextStyle(color: AppColors.textDim, fontSize: 12, fontFeatures: [FontFeature.tabularFigures()])),
              const Spacer(),
              IconButton(
                visualDensity: VisualDensity.compact,
                tooltip: 'Back 10 s',
                onPressed: mine ? () => n.skip(-10000) : null,
                icon: const Icon(Icons.replay_10_rounded, size: 22),
              ),
              IconButton(
                visualDensity: VisualDensity.compact,
                tooltip: 'Forward 10 s',
                onPressed: mine ? () => n.skip(10000) : null,
                icon: const Icon(Icons.forward_10_rounded, size: 22),
              ),
              TextButton(
                onPressed: mine ? n.cycleSpeed : null,
                child: Text('${p.speed == p.speed.roundToDouble() ? p.speed.toStringAsFixed(0) : p.speed}×'),
              ),
            ],
          ),
          if (p.error != null && p.name == null)
            Padding(
              padding: const EdgeInsets.only(bottom: 6),
              child: Text(p.error!, style: const TextStyle(color: AppColors.danger, fontSize: 12)),
            ),
        ],
      ),
    );
  }
}

/// The transcript, with the sentence being heard highlighted while the memo
/// plays (estimated from the position: words are spread evenly over time).
class FollowAlongTranscript extends ConsumerWidget {
  final String name;
  final String text;
  const FollowAlongTranscript({super.key, required this.name, required this.text});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    const base = TextStyle(color: AppColors.text, fontSize: 14, height: 1.55);
    final p = ref.watch(memoPlayerProvider);
    if (!p.isMemo(name) || p.durMs <= 0 || (!p.playing && p.posMs == 0)) {
      return SelectableText(text, style: base);
    }
    final at = (p.fraction * text.length).floor().clamp(0, text.length - 1);
    // sentence around the position
    var start = at, end = at;
    bool stop(int i) => '.!?…\n'.contains(text[i]);
    while (start > 0 && !stop(start - 1)) {
      start--;
    }
    while (end < text.length && !stop(end)) {
      end++;
    }
    if (end < text.length) end++;
    return SelectableText.rich(TextSpan(style: base.copyWith(color: AppColors.textDim), children: [
      TextSpan(text: text.substring(0, start)),
      TextSpan(
          text: text.substring(start, end),
          style: TextStyle(color: AppColors.text, backgroundColor: AppColors.accent.withValues(alpha: 0.25))),
      TextSpan(text: text.substring(end)),
    ]));
  }
}
