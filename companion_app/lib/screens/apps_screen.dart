import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../app.dart';
import 'life_screen.dart';
import '../providers/ble_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import '../widgets/emulator_card.dart';
import 'activity_screen.dart';
import 'ai_settings_screen.dart';
import 'assistant_screen.dart';
import 'calendar_screen.dart';
import 'contacts_screen.dart';
import 'controller_screen.dart';
import 'dnd_replies_screen.dart';
import 'files_screen.dart';
import 'look_screen.dart';
import 'media_screen.dart';
import 'memo_insights.dart';
import 'messages_screen.dart';
import 'notifications_screen.dart';
import 'piano_song_screen.dart';
import 'safety_screen.dart';
import 'sound_alerts_screen.dart';
import 'wake_word_screen.dart';
import 'watch_wifi_screen.dart';
import 'weather_screen.dart';
import '../widgets/platform_notice.dart';

/// One thing the app can do.
class AppFeature {
  final String title, subtitle;
  final IconData icon;
  final Color color;
  final Widget Function()? page;
  final int? tab; // opens a tab instead of a page
  final bool needsWatch;
  final List<String> keywords;
  const AppFeature(
    this.title,
    this.subtitle,
    this.icon,
    this.color, {
    this.page,
    this.tab,
    this.needsWatch = false,
    this.keywords = const [],
  });

  bool matches(String q) {
    final s = '$title $subtitle ${keywords.join(' ')}'.toLowerCase();
    return q.toLowerCase().split(RegExp(r'\s+')).where((w) => w.isNotEmpty).every(s.contains);
  }
}

class AppGroup {
  final String title;
  final List<AppFeature> items;
  const AppGroup(this.title, this.items);
}

/// Everything, grouped. Settings-only things (pairing, updates) stay in Settings.
final appGroups = <AppGroup>[
  AppGroup('Ask AI', [
    AppFeature(
      'Assistant',
      'Ask out loud, like on the watch',
      Icons.mic_rounded,
      AppColors.accent,
      page: () => const AssistantScreen(),
      keywords: ['voice', 'question', 'timer', 'reminder'],
    ),
    AppFeature(
      'Look',
      'What is it? Read or translate a photo',
      Icons.visibility_outlined,
      AppColors.success,
      page: () => const LookScreen(),
      keywords: ['camera', 'vision', 'ocr', 'translate'],
    ),
    AppFeature(
      'Messages',
      'Catch me up, translate, smart replies',
      Icons.forum_outlined,
      AppColors.accent2,
      page: () => const MessagesScreen(),
      keywords: ['notifications', 'digest', 'reply'],
    ),
    AppFeature(
      'Search memos',
      'Find and ask your voice memos',
      Icons.manage_search_rounded,
      AppColors.accent3,
      page: () => const MemoSearchScreen(),
      keywords: ['recordings', 'ask ai', 'meaning'],
    ),
    AppFeature(
      'To-dos',
      'Tasks found in your memos',
      Icons.task_alt_rounded,
      AppColors.success,
      page: () => const ActionsScreen(),
      keywords: ['actions', 'tasks'],
    ),
    AppFeature(
      'Sound alerts',
      'Doorbell, alarms, a baby crying - on the watch',
      Icons.hearing_rounded,
      AppColors.warning,
      page: () => const SoundAlertsScreen(),
      keywords: ['listen', 'deaf', 'smoke'],
    ),
    AppFeature(
      'AI models',
      'Transcription, summaries, voices, vision',
      Icons.auto_awesome_rounded,
      AppColors.accent2,
      page: () => const AiSettingsScreen(),
      keywords: ['whisper', 'gemma', 'download', 'llm', 'models'],
    ),
  ]),
  AppGroup('Watch', [
    AppFeature(
      'Faces & display',
      'Pick and style the face, slots, brightness, photo and designed faces',
      Icons.palette_outlined,
      AppColors.accent,
      tab: 3,
      keywords: ['watchface', 'design', 'photo', 'slots', 'complications', 'brightness', 'colour', 'color'],
    ),
    AppFeature(
      'Send files',
      'Music, photos and more to the SD card',
      Icons.folder_outlined,
      AppColors.warning,
      page: () => const FilesScreen(),
      needsWatch: true,
      keywords: ['transfer', 'sd'],
    ),
    AppFeature(
      'Contacts',
      'Who is on the watch',
      Icons.contacts_outlined,
      AppColors.accent2,
      page: () => const ContactsScreen(),
      needsWatch: true,
      keywords: ['people', 'call'],
    ),
    AppFeature(
      'Watch Wi-Fi',
      'Set up the watch\'s network',
      Icons.wifi_rounded,
      AppColors.success,
      page: () => const WatchWifiScreen(),
      needsWatch: true,
      keywords: ['network', 'internet'],
    ),
    AppFeature(
      'Wake word',
      'Hands-free, and offline commands',
      Icons.record_voice_over_rounded,
      AppColors.accent,
      page: () => const WakeWordScreen(),
      keywords: ['alexa', 'jarvis', 'voice', 'commands'],
    ),
  ]),
  AppGroup('Phone link', [
    AppFeature(
      'Notifications & calls',
      isIos ? 'Android only - see why' : 'What reaches the watch',
      Icons.notifications_active_outlined,
      AppColors.accent,
      page: () => const NotificationsScreen(),
      keywords: ['apps', 'forward'],
    ),
    AppFeature(
      'Music',
      isIos ? 'Android only - see why' : 'Control the phone\'s music from the watch',
      Icons.music_note_outlined,
      AppColors.accent3,
      page: () => const MediaScreen(),
      keywords: ['media', 'spotify', 'player'],
    ),
    AppFeature(
      'Weather',
      'Forecast on the watch',
      Icons.wb_cloudy_outlined,
      AppColors.warning,
      page: () => const WeatherScreen(),
      keywords: ['forecast', 'temperature'],
    ),
    AppFeature(
      'Calendar',
      'Agenda and reminders',
      Icons.calendar_month_rounded,
      AppColors.accent2,
      page: () => const CalendarScreen(),
      keywords: ['events', 'agenda'],
    ),
    AppFeature(
      'Do Not Disturb',
      isIos ? 'Bedtime on the watch' : 'Bedtime and quick replies',
      Icons.do_not_disturb_on_outlined,
      AppColors.accent2,
      page: () => const DndRepliesScreen(),
      keywords: ['dnd', 'quiet', 'replies', 'bedtime'],
    ),
  ]),
  AppGroup('Health & safety', [
    AppFeature(
      'Life',
      'Plan healthy days: water, stretch, breathe, train, meals, friends, sleep',
      Icons.spa_rounded,
      Color(0xFF2CCA70),
      page: () => const LifeScreen(),
      keywords: ['habits', 'water', 'meditation', 'mindfulness', 'stretch', 'meals', 'diet', 'social', 'wellbeing', 'health'],
    ),
    AppFeature(
      'Activity',
      'Rings, steps and workouts from the watch',
      Icons.directions_run_rounded,
      AppColors.success,
      page: () => const ActivityScreen(),
      keywords: ['steps', 'fitness'],
    ),
    AppFeature(
      'Safety',
      'Emergency contact for fall alerts',
      Icons.health_and_safety_outlined,
      AppColors.danger,
      page: () => const SafetyScreen(),
      keywords: ['fall', 'emergency', 'sms'],
    ),
  ]),
  AppGroup('Fun', [
    AppFeature(
      'Piano Tiles songs',
      'Hum a tune or pick a MIDI file',
      Icons.piano,
      AppColors.accent,
      page: () => const PianoSongScreen(),
      keywords: ['game', 'music', 'midi'],
    ),
    AppFeature(
      'Game controller',
      'The phone as a gamepad',
      Icons.sports_esports_outlined,
      AppColors.accent3,
      page: () => const ControllerScreen(),
      needsWatch: true,
      keywords: ['game', 'gamepad'],
    ),
  ]),
];

void openFeature(BuildContext context, AppFeature f) {
  if (f.tab != null) {
    TabSwitcher.open(context, f.tab!);
  } else if (f.page != null) {
    Navigator.push(context, MaterialPageRoute(builder: (_) => f.page!()));
  }
}

/// All of the app's features in one place, searchable.
class AppsScreen extends ConsumerStatefulWidget {
  const AppsScreen({super.key});

  @override
  ConsumerState<AppsScreen> createState() => _AppsScreenState();
}

class _AppsScreenState extends ConsumerState<AppsScreen> {
  final _search = TextEditingController();
  String _q = '';

  @override
  void dispose() {
    _search.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final connected = ref.watch(bleProvider).isConnected;
    final results = _q.isEmpty ? null : [for (final g in appGroups) ...g.items.where((f) => f.matches(_q))];

    return Scaffold(
      body: SafeArea(
        bottom: false,
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 16, 16, 32),
          children: [
            const Padding(
              padding: EdgeInsets.fromLTRB(4, 0, 4, 14),
              child: Text(
                'Apps',
                style: TextStyle(color: AppColors.text, fontSize: 28, fontWeight: FontWeight.w700, letterSpacing: -0.6),
              ),
            ),
            TextField(
              controller: _search,
              onChanged: (v) => setState(() => _q = v.trim()),
              decoration: InputDecoration(
                hintText: 'Search features',
                prefixIcon: const Icon(Icons.search_rounded),
                suffixIcon: _q.isEmpty
                    ? null
                    : IconButton(
                        icon: const Icon(Icons.close_rounded),
                        onPressed: () => setState(() {
                          _search.clear();
                          _q = '';
                        }),
                      ),
              ),
            ),
            if (results != null) ...[
              const SizedBox(height: 14),
              if (results.isEmpty)
                const Padding(
                  padding: EdgeInsets.all(24),
                  child: Text(
                    'Nothing matches. Pairing and updates are in Settings.',
                    textAlign: TextAlign.center,
                    style: TextStyle(color: AppColors.textDim),
                  ),
                )
              else
                RowGroup(children: [for (final f in results) _row(f, connected)]),
            ] else ...[
              const SizedBox(height: 16),
              const EmulatorCard(),
              for (final g in appGroups) ...[SectionLabel(g.title), _Grid(items: g.items, connected: connected)],
            ],
          ],
        ),
      ),
    );
  }

  Widget _row(AppFeature f, bool connected) {
    final off = f.needsWatch && !connected;
    return NavRow(
      icon: f.icon,
      color: f.color,
      title: f.title,
      subtitle: off ? 'Connect the watch (or turn on the emulator)' : f.subtitle,
      onTap: off ? null : () => openFeature(context, f),
    );
  }
}

class _Grid extends StatelessWidget {
  final List<AppFeature> items;
  final bool connected;
  const _Grid({required this.items, required this.connected});

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, c) {
        final cols = c.maxWidth > 560 ? 3 : 2;
        final w = (c.maxWidth - 10 * (cols - 1)) / cols;
        return Wrap(
          spacing: 10,
          runSpacing: 10,
          children: [
            for (final f in items)
              SizedBox(
                width: w,
                child: _Tile(f, enabled: !f.needsWatch || connected),
              ),
          ],
        );
      },
    );
  }
}

class _Tile extends StatelessWidget {
  final AppFeature f;
  final bool enabled;
  const _Tile(this.f, {required this.enabled});

  @override
  Widget build(BuildContext context) {
    return Opacity(
      opacity: enabled ? 1 : 0.45,
      child: Panel(
        padding: const EdgeInsets.all(14),
        onTap: enabled
            ? () => openFeature(context, f)
            : () => showSnack(context, 'Connect the watch first, or turn on the emulator'),
        child: SizedBox(
          height: 104,
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              IconChip(f.icon, color: f.color, size: 36),
              const Spacer(),
              Text(
                f.title,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(color: AppColors.text, fontSize: 14, fontWeight: FontWeight.w600),
              ),
              const SizedBox(height: 2),
              Text(
                f.subtitle,
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(color: AppColors.textDim, fontSize: 11.5, height: 1.3),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
