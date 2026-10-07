import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../app.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'activity_screen.dart';
import 'contacts_screen.dart';
import 'calendar_screen.dart';
import 'controller_screen.dart';
import 'diagnostics_screen.dart';
import 'files_screen.dart';
import 'media_screen.dart';
import 'notifications_screen.dart';
import 'scan_screen.dart';
import 'weather_screen.dart';
import 'watch_wifi_screen.dart';
import 'memo_insights.dart';
import '../providers/notes_provider.dart';
import '../providers/knowledge_provider.dart';

/// Dashboard: the watch, what's linked, today's numbers, shortcuts.
class HomeScreen extends ConsumerWidget {
  const HomeScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final ble = ref.watch(bleProvider);
    final link = ref.watch(companionProvider);

    return Scaffold(
      body: SafeArea(
        bottom: false,
        child: CustomScrollView(
          slivers: [
            SliverToBoxAdapter(child: _Header(ble: ble)),
            SliverPadding(
              padding: const EdgeInsets.fromLTRB(16, 4, 16, 32),
              sliver: SliverList.list(
                children: [
                  _WatchCard(ble: ble, link: link),
                  if (link.hasNewCrash && ble.isConnected) ...[
                    const SizedBox(height: 12),
                    _Banner(
                      icon: Icons.bug_report_rounded,
                      color: AppColors.warning,
                      title: 'The watch restarted after a crash',
                      subtitle: 'Tap to see the crash report',
                      onTap: () => _push(context, const DiagnosticsScreen()),
                    ),
                  ],
                  if (link.navigation != null) ...[
                    const SizedBox(height: 12),
                    _Banner(
                      icon: Icons.navigation_rounded,
                      color: AppColors.success,
                      title: [
                        link.navigation!['dist'] as String? ?? '',
                        link.navigation!['instr'] as String? ?? '',
                      ].where((x) => x.isNotEmpty).join(' · '),
                      subtitle: ref.watch(settingsProvider).navigationToWatch
                          ? 'Directions are on the watch'
                          : 'Navigation to the watch is off',
                    ),
                  ],
                  if (link.dictation != null) ...[
                    const SizedBox(height: 12),
                    _Banner(
                      icon: Icons.record_voice_over_rounded,
                      color: AppColors.accent2,
                      title: link.dictation!,
                      subtitle: 'The text goes back to the watch to confirm',
                      busy: true,
                    ),
                  ],
                  const _MemoBrief(),
                  if (ble.isConnected) ...[
                    const SizedBox(height: 12),
                    const _QuickActions(),
                    const SectionLabel('Today'),
                    _TodayRow(link: link),
                    if (link.media != null && link.mediaTitle.isNotEmpty) ...[
                      const SectionLabel('Playing on your phone'),
                      _NowPlayingCard(link: link),
                    ],
                  ],
                  const SectionLabel('Phone link'),
                  _LinkGroup(connected: ble.isConnected),
                  const SectionLabel('More'),
                  RowGroup(children: [
                    NavRow(
                      icon: Icons.graphic_eq_rounded,
                      color: AppColors.accent3,
                      title: 'Voice memos',
                      subtitle: 'Recordings, transcripts and summaries',
                      onTap: () => TabSwitcher.open(context, 1),
                    ),
                    NavRow(
                      icon: Icons.contacts_outlined,
                      color: AppColors.accent2,
                      title: 'Contacts',
                      subtitle: 'Choose who is on the watch',
                      onTap: ble.isConnected ? () => _push(context, const ContactsScreen()) : null,
                    ),
                    NavRow(
                      icon: Icons.wifi_rounded,
                      color: AppColors.success,
                      title: 'Watch Wi-Fi',
                      subtitle: 'Set up and check the watch\'s network',
                      onTap: ble.isConnected ? () => _push(context, const WatchWifiScreen()) : null,
                    ),
                    NavRow(
                      icon: Icons.folder_outlined,
                      color: AppColors.warning,
                      title: 'Send files',
                      subtitle: 'Saved to the watch SD card',
                      onTap: ble.isConnected ? () => _push(context, const FilesScreen()) : null,
                    ),
                    NavRow(
                      icon: Icons.sports_esports_outlined,
                      color: AppColors.accent3,
                      title: 'Game controller',
                      subtitle: 'Use the phone as a gamepad',
                      onTap: ble.isConnected ? () => _push(context, const ControllerScreen()) : null,
                    ),
                  ]),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

void _push(BuildContext context, Widget page) =>
    Navigator.push(context, MaterialPageRoute(builder: (_) => page));

String _greeting() {
  final h = DateTime.now().hour;
  if (h < 5) return 'Good night';
  if (h < 12) return 'Good morning';
  if (h < 18) return 'Good afternoon';
  return 'Good evening';
}

class _Header extends ConsumerWidget {
  final BleState ble;
  const _Header({required this.ble});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 16, 8, 12),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(_greeting(), style: const TextStyle(color: AppColors.textDim, fontSize: 14)),
                const SizedBox(height: 2),
                const Text('AmoledWatch',
                    style: TextStyle(color: AppColors.text, fontSize: 28, fontWeight: FontWeight.w700, letterSpacing: -0.6)),
              ],
            ),
          ),
          PopupMenuButton<String>(
            icon: const Icon(Icons.more_horiz_rounded, color: AppColors.textDim),
            color: AppColors.surfaceLight,
            onSelected: (v) async {
              if (v == 'scan') _push(context, const ScanScreen());
              if (v == 'disconnect') await ref.read(bleProvider.notifier).disconnect();
            },
            itemBuilder: (_) => [
              const PopupMenuItem(value: 'scan', child: Text('Pair another watch')),
              if (ble.isConnected || ble.isConnecting)
                const PopupMenuItem(value: 'disconnect', child: Text('Disconnect')),
            ],
          ),
        ],
      ),
    );
  }
}

// ---- watch card ------------------------------------------------------------------

class _WatchCard extends ConsumerStatefulWidget {
  final BleState ble;
  final CompanionState link;
  const _WatchCard({required this.ble, required this.link});

  @override
  ConsumerState<_WatchCard> createState() => _WatchCardState();
}

class _WatchCardState extends ConsumerState<_WatchCard> {
  Timer? _clock;
  bool _searching = false;

  @override
  void initState() {
    super.initState();
    _clock = Timer.periodic(const Duration(seconds: 20), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _clock?.cancel();
    super.dispose();
  }

  Future<void> _quickConnect() async {
    setState(() => _searching = true);
    final notifier = ref.read(bleProvider.notifier);
    await notifier.startScan();
    await Future.delayed(const Duration(seconds: 4));
    await notifier.stopScan();
    final results = ref.read(bleProvider).scanResults;
    final match = results.where((r) {
      final n = r.advertisementData.advName.toLowerCase();
      return n.contains('amoled') || n.contains('watch');
    }).toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
    if (!mounted) return;
    if (match.isEmpty) {
      setState(() => _searching = false);
      showSnack(context, 'No watch found nearby. Is Bluetooth on in the watch settings?');
      return;
    }
    final ok = await notifier.connect(match.first.device);
    if (!mounted) return;
    setState(() => _searching = false);
    if (!ok) showSnack(context, 'Could not connect. Try again closer to the watch.');
  }

  @override
  Widget build(BuildContext context) {
    final ble = widget.ble;
    final link = widget.link;
    final connected = ble.isConnected;
    final bat = ble.batteryLevel >= 0 ? ble.batteryLevel : link.watchBattery;
    final now = TimeOfDay.now();
    final time = '${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}';

    String status;
    Color statusColor;
    if (connected) {
      status = link.linkActive ? 'Connected · linked' : 'Connected';
      statusColor = AppColors.success;
    } else if (ble.isConnecting || _searching) {
      status = _searching ? 'Searching…' : 'Reconnecting…';
      statusColor = AppColors.warning;
    } else {
      status = 'Not connected';
      statusColor = AppColors.textDim;
    }

    return Panel(
      padding: const EdgeInsets.all(20),
      gradient: const LinearGradient(
        begin: Alignment.topLeft,
        end: Alignment.bottomRight,
        colors: [Color(0xFF17142B), Color(0xFF0B0B10)],
      ),
      child: Row(
        children: [
          Ring(
            value: connected && bat >= 0 ? bat / 100 : 0,
            size: 124,
            stroke: 9,
            color: batteryColor(bat),
            child: Container(
              width: 92,
              height: 92,
              decoration: const BoxDecoration(color: Colors.black, shape: BoxShape.circle),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Text(time,
                      style: TextStyle(
                        color: connected ? AppColors.text : AppColors.textFaint,
                        fontSize: 22,
                        fontWeight: FontWeight.w700,
                        fontFeatures: const [FontFeature.tabularFigures()],
                      )),
                  if (connected && bat >= 0)
                    Text('$bat%', style: TextStyle(color: batteryColor(bat), fontSize: 12, fontWeight: FontWeight.w600)),
                ],
              ),
            ),
          ),
          const SizedBox(width: 18),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(ble.deviceName ?? ref.read(settingsProvider).lastDeviceName ?? 'Your watch',
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: AppColors.text, fontSize: 19, fontWeight: FontWeight.w700)),
                const SizedBox(height: 8),
                StatusPill(status, color: statusColor),
                const SizedBox(height: 14),
                if (!connected && !ble.isConnecting)
                  FilledButton.icon(
                    onPressed: _searching ? null : _quickConnect,
                    icon: _searching
                        ? const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2, color: Colors.white))
                        : const Icon(Icons.bluetooth_searching_rounded, size: 18),
                    label: Text(_searching ? 'Searching' : 'Connect'),
                  )
                else if (connected && link.watchCharging)
                  const Text('Charging', style: TextStyle(color: AppColors.success, fontSize: 13))
                else if (!connected)
                  const Text('Will connect when the watch is in range',
                      style: TextStyle(color: AppColors.textDim, fontSize: 13)),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

// ---- quick actions -----------------------------------------------------------------

class _QuickActions extends ConsumerWidget {
  const _QuickActions();

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final link = ref.watch(companionProvider);
    return Row(
      children: [
        _QuickButton(
          icon: Icons.vibration_rounded,
          label: 'Find watch',
          onTap: () async {
            await ref.read(companionProvider.notifier).findWatch();
            if (context.mounted) showSnack(context, 'Your watch is buzzing');
          },
        ),
        const SizedBox(width: 10),
        _QuickButton(
          icon: Icons.schedule_rounded,
          label: 'Sync time',
          onTap: () async {
            await ref.read(bleProvider.notifier).syncTime();
            if (context.mounted) showSnack(context, 'Watch clock synced');
          },
        ),
        const SizedBox(width: 10),
        _QuickButton(
          icon: link.findingPhone ? Icons.volume_off_rounded : Icons.cloud_sync_rounded,
          label: link.findingPhone ? 'Stop ringing' : 'Weather',
          highlight: link.findingPhone,
          onTap: () async {
            if (link.findingPhone) {
              await ref.read(companionProvider.notifier).stopFindingPhone();
            } else {
              await ref.read(companionProvider.notifier).refreshWeather(interactive: true);
              final err = ref.read(companionProvider).weatherError;
              if (context.mounted) showSnack(context, err ?? 'Weather sent to the watch');
            }
          },
        ),
      ],
    );
  }
}

class _QuickButton extends StatelessWidget {
  final IconData icon;
  final String label;
  final VoidCallback onTap;
  final bool highlight;
  const _QuickButton({required this.icon, required this.label, required this.onTap, this.highlight = false});

  @override
  Widget build(BuildContext context) {
    return Expanded(
      child: Panel(
        onTap: onTap,
        color: highlight ? AppColors.danger.withValues(alpha: 0.18) : null,
        padding: const EdgeInsets.symmetric(vertical: 16),
        child: Column(
          children: [
            Icon(icon, color: highlight ? AppColors.danger : AppColors.text, size: 24),
            const SizedBox(height: 8),
            Text(label, style: const TextStyle(color: AppColors.textDim, fontSize: 12, fontWeight: FontWeight.w500)),
          ],
        ),
      ),
    );
  }
}

// ---- today -------------------------------------------------------------------------------

class _TodayRow extends ConsumerWidget {
  final CompanionState link;
  const _TodayRow({required this.link});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final w = link.weather;
    final f = ref.watch(settingsProvider).fahrenheit;
    String temp = '--';
    if (w != null) {
      final t = f ? w.tempC * 9 / 5 + 32 : w.tempC;
      temp = '${t.round()}°';
    }
    return Row(
      children: [
        Expanded(
          child: StatTile(
            icon: Icons.directions_walk_rounded,
            color: AppColors.success,
            value: link.watchSteps >= 0 ? _thousands(link.watchSteps) : '--',
            label: 'Steps',
            onTap: () => _push(context, const ActivityScreen()),
          ),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: StatTile(
            icon: Icons.wb_sunny_outlined,
            color: AppColors.warning,
            value: temp,
            label: w?.condition ?? 'Weather',
            onTap: () => _push(context, const WeatherScreen()),
          ),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: StatTile(
            icon: link.phoneCharging ? Icons.battery_charging_full_rounded : Icons.smartphone_rounded,
            color: batteryColor(link.phoneBattery),
            value: link.phoneBattery >= 0 ? '${link.phoneBattery}%' : '--',
            label: 'Phone',
          ),
        ),
      ],
    );
  }
}

String _thousands(int n) {
  final s = n.toString();
  final b = StringBuffer();
  for (var i = 0; i < s.length; i++) {
    if (i > 0 && (s.length - i) % 3 == 0) b.write(',');
    b.write(s[i]);
  }
  return b.toString();
}

// ---- now playing --------------------------------------------------------------------------

class _NowPlayingCard extends ConsumerWidget {
  final CompanionState link;
  const _NowPlayingCard({required this.link});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final native = ref.read(nativeServiceProvider);
    return Panel(
      onTap: () => _push(context, const MediaScreen()),
      child: Row(
        children: [
          Container(
            width: 52,
            height: 52,
            decoration: BoxDecoration(
              gradient: const LinearGradient(colors: [AppColors.accent, AppColors.accent3]),
              borderRadius: BorderRadius.circular(14),
            ),
            child: const Icon(Icons.music_note_rounded, color: Colors.white),
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(link.mediaTitle, maxLines: 1, overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                const SizedBox(height: 2),
                Text(link.mediaArtist.isEmpty ? 'Also on your watch' : link.mediaArtist,
                    maxLines: 1, overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
              ],
            ),
          ),
          IconButton(
            onPressed: () => native.mediaCommand('prev'),
            icon: const Icon(Icons.skip_previous_rounded, color: AppColors.text),
          ),
          IconButton.filled(
            onPressed: () => native.mediaCommand('toggle'),
            style: IconButton.styleFrom(backgroundColor: AppColors.text),
            icon: Icon(link.mediaPlaying ? Icons.pause_rounded : Icons.play_arrow_rounded, color: Colors.black),
          ),
          IconButton(
            onPressed: () => native.mediaCommand('next'),
            icon: const Icon(Icons.skip_next_rounded, color: AppColors.text),
          ),
        ],
      ),
    );
  }
}

// ---- phone link -----------------------------------------------------------------------

class _LinkGroup extends ConsumerWidget {
  final bool connected;
  const _LinkGroup({required this.connected});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final s = ref.watch(settingsProvider);
    final link = ref.watch(companionProvider);
    return RowGroup(children: [
      NavRow(
        icon: Icons.notifications_active_outlined,
        color: AppColors.accent,
        title: 'Notifications & calls',
        subtitle: s.forwardNotifications
            ? (link.forwardedToday > 0 ? '${link.forwardedToday} sent to the watch today' : 'Forwarding to the watch')
            : 'Off - tap to set up',
        onTap: () => _push(context, const NotificationsScreen()),
      ),
      NavRow(
        icon: Icons.music_note_outlined,
        color: AppColors.accent3,
        title: 'Music controls',
        subtitle: s.mediaToWatch ? 'Control phone music from the watch' : 'Off',
        onTap: () => _push(context, const MediaScreen()),
      ),
      NavRow(
        icon: Icons.wb_cloudy_outlined,
        color: AppColors.warning,
        title: 'Weather',
        subtitle: link.weather != null
            ? '${link.weather!.condition} · updated ${_ago(link.weather!.fetchedAt)}'
            : (s.weatherAuto ? 'Automatic, from your location' : 'Manual'),
        onTap: () => _push(context, const WeatherScreen()),
      ),
      NavRow(
        icon: Icons.calendar_month_rounded,
        color: AppColors.accent2,
        title: 'Calendar',
        subtitle: !link.calendarAllowed
            ? 'Agenda and reminders on the watch - tap to set up'
            : link.nextEvent != null
                ? 'Next: ${link.nextEvent!.title}'
                : 'Nothing in the next two days',
        onTap: () => _push(context, const CalendarScreen()),
      ),
      ToggleRow(
        icon: Icons.navigation_rounded,
        color: AppColors.success,
        title: 'Navigation',
        subtitle: 'Google Maps directions on the watch',
        value: s.navigationToWatch,
        onChanged: (v) => ref.read(settingsProvider.notifier).update(s.copyWith(navigationToWatch: v)),
      ),
      NavRow(
        icon: Icons.directions_run_rounded,
        color: AppColors.success,
        title: 'Activity',
        subtitle: 'Steps counted by the watch',
        onTap: () => _push(context, const ActivityScreen()),
      ),
    ]);
  }
}

String _ago(DateTime t) {
  final d = DateTime.now().difference(t);
  if (d.inMinutes < 1) return 'just now';
  if (d.inMinutes < 60) return '${d.inMinutes} min ago';
  if (d.inHours < 24) return '${d.inHours} h ago';
  return '${d.inDays} d ago';
}

class _Banner extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String title;
  final String subtitle;
  final VoidCallback? onTap;
  final bool busy;
  const _Banner({
    required this.icon,
    required this.color,
    required this.title,
    required this.subtitle,
    this.onTap,
    this.busy = false,
  });

  @override
  Widget build(BuildContext context) {
    return Panel(
      onTap: onTap,
      color: color.withValues(alpha: 0.08),
      padding: const EdgeInsets.all(14),
      child: Row(children: [
        IconChip(icon, color: color),
        const SizedBox(width: 14),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title, style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
            const SizedBox(height: 2),
            Text(subtitle, style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
          ]),
        ),
        if (busy)
          const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
        else if (onTap != null)
          const Icon(Icons.chevron_right_rounded, color: AppColors.textFaint),
      ]),
    );
  }
}

// ---- memos: urgent + to-dos -----------------------------------------------------

/// What needs attention from the voice memos: important recent memos, open
/// to-dos (tick them off here), and memos still waiting to be processed.
class _MemoBrief extends ConsumerWidget {
  const _MemoBrief();

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final kb = ref.watch(knowledgeProvider);
    final done = ref.watch(doneActionsProvider);
    final notes = ref.watch(notesProvider);
    if (kb.isEmpty) return const SizedBox.shrink();

    final now = DateTime.now();
    final urgent = kb.entries
        .where((e) => e.importance >= 4 && e.summary.isNotEmpty &&
            (e.recordedAt == null || now.difference(e.recordedAt!).inDays <= 14))
        .toList()
      ..sort((a, b) {
        final c = b.importance.compareTo(a.importance);
        return c != 0 ? c : (b.recordedAt ?? DateTime(2000)).compareTo(a.recordedAt ?? DateTime(2000));
      });
    final todos = kb.actions().where((a) => !done.contains(a.key)).toList();
    final pending = notes.recordings
        .where((r) => r.transcript.isEmpty && !r.noSpeech && !r.name.toLowerCase().endsWith('.mp3'))
        .length;
    final today = kb.entries
        .where((e) => e.recordedAt != null && DateUtils.isSameDay(e.recordedAt, now))
        .length;
    if (urgent.isEmpty && todos.isEmpty && pending == 0 && today == 0 && !notes.pipelineRunning) {
      return const SizedBox.shrink();
    }

    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        SectionLabel('Memos',
            trailing: IconButton(
              visualDensity: VisualDensity.compact,
              tooltip: 'Search & ask your memos',
              icon: const Icon(Icons.search_rounded, size: 20, color: AppColors.textDim),
              onPressed: () => _push(context, const MemoSearchScreen()),
            )),
        Row(children: [
          Expanded(
            child: _MiniStat(
              icon: Icons.priority_high_rounded,
              color: AppColors.danger,
              value: '${urgent.length}',
              label: 'urgent',
              onTap: urgent.isEmpty
                  ? null
                  : () => _push(context, MemoListScreen(title: 'Urgent memos', items: urgent)),
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: _MiniStat(
              icon: Icons.task_alt_rounded,
              color: AppColors.success,
              value: '${todos.length}',
              label: 'to-dos',
              onTap: () => _push(context, const ActionsScreen()),
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: _MiniStat(
              icon: Icons.mic_rounded,
              color: AppColors.accent2,
              value: '$today',
              label: 'today',
              onTap: () => TabSwitcher.open(context, 1),
            ),
          ),
        ]),
        if (notes.pipelineRunning || pending > 0) ...[
          const SizedBox(height: 10),
          Panel(
            padding: const EdgeInsets.fromLTRB(14, 10, 10, 10),
            child: Row(children: [
              notes.pipelineRunning
                  ? const SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.bolt_rounded, color: AppColors.accent),
              const SizedBox(width: 12),
              Expanded(
                child: Text(
                  notes.pipelineRunning
                      ? notes.pipelineStage
                      : '$pending memo${pending == 1 ? '' : 's'} not transcribed yet',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: AppColors.text, fontSize: 14),
                ),
              ),
              if (!notes.pipelineRunning)
                TextButton(
                  onPressed: notes.processingTranscript ? null : () => ref.read(notesProvider.notifier).runPipeline(),
                  child: const Text('Process'),
                ),
            ]),
          ),
        ],
        if (urgent.isNotEmpty) ...[
          const SizedBox(height: 10),
          for (final e in urgent.take(3))
            MemoTile(
              e,
              trailing: Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                decoration: BoxDecoration(
                  color: importanceColor(e.importance).withValues(alpha: 0.16),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Text(e.importance >= 5 ? 'Critical' : 'High',
                    style: TextStyle(color: importanceColor(e.importance), fontSize: 11, fontWeight: FontWeight.w700)),
              ),
              below: Text(e.summary, maxLines: 2, overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.35)),
            ),
        ],
        if (todos.isNotEmpty) ...[
          const SizedBox(height: 10),
          Panel(
            padding: const EdgeInsets.fromLTRB(0, 6, 0, 4),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                const Padding(
                  padding: EdgeInsets.fromLTRB(16, 6, 16, 0),
                  child: Text('To-do', style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w700)),
                ),
                for (final a in todos.take(4)) ActionRow(a),
                if (todos.length > 4)
                  Align(
                    alignment: Alignment.centerRight,
                    child: TextButton(
                      onPressed: () => _push(context, const ActionsScreen()),
                      child: Text('See all ${todos.length}'),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ],
    );
  }
}

class _MiniStat extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String value, label;
  final VoidCallback? onTap;
  const _MiniStat({required this.icon, required this.color, required this.value, required this.label, this.onTap});

  @override
  Widget build(BuildContext context) => Panel(
        onTap: onTap,
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
        child: Row(children: [
          Icon(icon, color: color, size: 20),
          const SizedBox(width: 8),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(value, style: const TextStyle(color: AppColors.text, fontSize: 18, fontWeight: FontWeight.w700)),
              Text(label, maxLines: 1, overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: AppColors.textDim, fontSize: 11)),
            ]),
          ),
        ]),
      );
}
