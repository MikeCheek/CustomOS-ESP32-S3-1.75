import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// What the phone is playing, mirrored on the watch (Phone > Music) with
/// play/pause, skip and volume buttons that control the phone.
class MediaScreen extends ConsumerWidget {
  const MediaScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final link = ref.watch(companionProvider);
    final s = ref.watch(settingsProvider);
    final native = ref.read(nativeServiceProvider);
    final m = link.media;
    final hasMedia = m != null && link.mediaTitle.isNotEmpty;
    final vol = (m?['volume'] as num?)?.toInt() ?? 0;
    final vmax = (m?['volumeMax'] as num?)?.toInt() ?? 15;

    return Scaffold(
      appBar: AppBar(title: const Text('Music controls')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          Panel(
            padding: const EdgeInsets.all(24),
            gradient: const LinearGradient(
              begin: Alignment.topLeft,
              end: Alignment.bottomRight,
              colors: [Color(0xFF241638), Color(0xFF0C0C12)],
            ),
            child: Column(
              children: [
                Container(
                  width: 120,
                  height: 120,
                  decoration: BoxDecoration(
                    gradient: const LinearGradient(colors: [AppColors.accent, AppColors.accent3]),
                    borderRadius: BorderRadius.circular(28),
                  ),
                  child: const Icon(Icons.music_note_rounded, color: Colors.white, size: 52),
                ),
                const SizedBox(height: 20),
                Text(hasMedia ? link.mediaTitle : 'Nothing playing',
                    textAlign: TextAlign.center,
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: AppColors.text, fontSize: 20, fontWeight: FontWeight.w700)),
                const SizedBox(height: 4),
                Text(hasMedia ? link.mediaArtist : 'Start music in any app on your phone',
                    textAlign: TextAlign.center,
                    style: const TextStyle(color: AppColors.textDim, fontSize: 14)),
                const SizedBox(height: 22),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    IconButton(
                      iconSize: 34,
                      onPressed: hasMedia ? () => native.mediaCommand('prev') : null,
                      icon: const Icon(Icons.skip_previous_rounded, color: AppColors.text),
                    ),
                    const SizedBox(width: 16),
                    IconButton.filled(
                      iconSize: 40,
                      style: IconButton.styleFrom(backgroundColor: AppColors.text, fixedSize: const Size(72, 72)),
                      onPressed: hasMedia ? () => native.mediaCommand('toggle') : null,
                      icon: Icon(link.mediaPlaying ? Icons.pause_rounded : Icons.play_arrow_rounded, color: Colors.black),
                    ),
                    const SizedBox(width: 16),
                    IconButton(
                      iconSize: 34,
                      onPressed: hasMedia ? () => native.mediaCommand('next') : null,
                      icon: const Icon(Icons.skip_next_rounded, color: AppColors.text),
                    ),
                  ],
                ),
                if (m != null) ...[
                  const SizedBox(height: 12),
                  Row(
                    children: [
                      IconButton(
                        onPressed: () => native.mediaCommand('volDown'),
                        icon: const Icon(Icons.volume_down_rounded, color: AppColors.textDim),
                      ),
                      Expanded(
                        child: ClipRRect(
                          borderRadius: BorderRadius.circular(4),
                          child: LinearProgressIndicator(
                            value: vmax > 0 ? vol / vmax : 0,
                            minHeight: 6,
                            color: AppColors.text,
                          ),
                        ),
                      ),
                      IconButton(
                        onPressed: () => native.mediaCommand('volUp'),
                        icon: const Icon(Icons.volume_up_rounded, color: AppColors.textDim),
                      ),
                    ],
                  ),
                ],
              ],
            ),
          ),
          const SectionLabel('On the watch'),
          RowGroup(children: [
            ToggleRow(
              icon: Icons.watch_outlined,
              title: 'Show phone music on the watch',
              subtitle: 'Phone app › Music: play, pause, skip, volume',
              value: s.mediaToWatch,
              onChanged: (v) => ref.read(settingsProvider.notifier).update(s.copyWith(mediaToWatch: v)),
            ),
          ]),
          const SizedBox(height: 12),
          const Text(
            'Works with any app that shows media controls (Spotify, YouTube Music, podcasts…). '
            'Needs notification access, which you grant under Notifications & calls.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}
