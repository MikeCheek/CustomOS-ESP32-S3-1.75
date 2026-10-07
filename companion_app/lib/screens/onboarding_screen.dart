import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/settings_provider.dart';
import '../services/setup_checks.dart';
import '../services/whisper_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'scan_screen.dart';

/// First-run setup: pair the watch, then grant what each feature needs, one
/// step at a time with a sentence on why. Every step can be skipped and
/// done later from Settings (and Diagnostics shows what's still missing).
class OnboardingScreen extends ConsumerStatefulWidget {
  /// Opened again from Settings: pops when finished instead of
  /// handing over to the main screen.
  final bool fromSettings;
  const OnboardingScreen({super.key, this.fromSettings = false});

  @override
  ConsumerState<OnboardingScreen> createState() => _OnboardingScreenState();
}

class _OnboardingScreenState extends ConsumerState<OnboardingScreen> with WidgetsBindingObserver {
  final _pages = PageController();
  int _page = 0;
  SetupStatus _s = const SetupStatus();
  bool _waitingListener = false;
  double? _speechProgress;
  String? _speechError;

  static const _count = 6;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _refresh();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _pages.dispose();
    super.dispose();
  }

  // Back from a system settings page (notification access): re-check.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) _refresh();
  }

  Future<void> _refresh() async {
    final s = await SetupStatus.check(ref.read(nativeServiceProvider));
    if (!mounted) return;
    setState(() => _s = s);
    if (_waitingListener && s.notificationAccess) {
      _waitingListener = false;
      final st = ref.read(settingsProvider);
      await ref.read(settingsProvider.notifier).update(st.copyWith(forwardNotifications: true));
    }
  }

  void _next() {
    if (_page < _count - 1) {
      _pages.nextPage(duration: const Duration(milliseconds: 320), curve: Curves.easeOutCubic);
    } else {
      _finish();
    }
  }

  Future<void> _finish() async {
    final st = ref.read(settingsProvider);
    await ref.read(settingsProvider.notifier).update(st.copyWith(onboarded: true));
    if (widget.fromSettings && mounted) Navigator.pop(context);
  }

  // ---- step actions --------------------------------------------------------------

  Future<void> _pair() async {
    final ok = await SetupStatus.requestBluetooth();
    await _refresh();
    if (!ok) {
      if (mounted) showSnack(context, 'Bluetooth permission is needed to find the watch');
      if (await Permission.bluetoothConnect.isPermanentlyDenied) await openAppSettings();
      return;
    }
    if (!mounted) return;
    await Navigator.push(context, MaterialPageRoute(builder: (_) => const ScanScreen()));
  }

  Future<void> _notificationAccess() async {
    _waitingListener = true;
    await ref.read(nativeServiceProvider).openNotificationListenerSettings();
  }

  Future<void> _downloadSpeech() async {
    setState(() {
      _speechProgress = 0;
      _speechError = null;
    });
    try {
      await WhisperService.instance.download(onProgress: (p) {
        if (mounted) setState(() => _speechProgress = p);
      });
    } catch (e) {
      _speechError = e.toString().replaceFirst('Exception: ', '');
    }
    if (!mounted) return;
    setState(() => _speechProgress = null);
    await _refresh();
  }

  // ---- UI ---------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    final ble = ref.watch(bleProvider);
    final settings = ref.watch(settingsProvider);

    final pages = <Widget>[
      _Step(
        icon: Icons.watch_rounded,
        color: AppColors.accent,
        art: Image.asset('assets/logo.png', width: 112, height: 112),
        title: 'Your watch, linked to your phone',
        body: 'Notifications with replies, calls, music controls, weather, voice memos with transcripts, '
            'find my phone and firmware updates - all over Bluetooth, and nothing leaves your phone.\n\n'
            'A few quick steps and you\'re set.',
      ),
      _Step(
        icon: Icons.bluetooth_searching_rounded,
        color: AppColors.accent2,
        title: 'Pair the watch',
        body: 'Turn Bluetooth on on the watch (Settings › Bluetooth), keep it close, then pick "AmoledWatch". '
            'If asked for a code, it\'s shown on the watch\'s Bluetooth status screen.',
        done: ble.isConnected,
        doneText: 'Connected to ${ble.deviceName ?? 'AmoledWatch'}',
        actions: [
          _Action(ble.isConnected ? 'Pair a different watch' : 'Find my watch', _pair, primary: !ble.isConnected),
        ],
      ),
      _Step(
        icon: Icons.notifications_active_rounded,
        color: AppColors.accent3,
        title: 'Notifications and calls',
        body: 'To show your phone\'s notifications on the watch - and reply, dismiss, answer calls and '
            'control music from it - allow notification access for AmoledWatch on the next screen.',
        done: _s.notificationAccess,
        doneText: settings.forwardNotifications ? 'Notifications go to the watch' : 'Access allowed',
        actions: [
          if (!_s.notificationAccess) _Action('Allow notification access', _notificationAccess, primary: true),
        ],
      ),
      _Step(
        icon: Icons.sync_lock_rounded,
        color: AppColors.success,
        title: 'Stay connected',
        body: 'The app keeps the watch linked while it\'s closed, with a small status notification. '
            'Weather uses your approximate location; your calendar gives the watch an agenda and reminders.',
        checklist: [
          _CheckItem('Status notification', _s.postNotifications, () async {
            await Permission.notification.request();
            final st = ref.read(settingsProvider);
            await ref.read(settingsProvider.notifier).update(st.copyWith(backgroundLink: true));
            if (ref.read(bleProvider).isConnected) await ref.read(nativeServiceProvider).startLinkService();
            _refresh();
          }),
          _CheckItem('Location for weather', _s.location, () async {
            await Permission.locationWhenInUse.request();
            _refresh();
          }),
          _CheckItem('Calendar: agenda and reminders', _s.calendar, () async {
            await Permission.calendarFullAccess.request();
            await ref.read(companionProvider.notifier).calendarPermissionChanged();
            _refresh();
          }),
          _CheckItem('Contacts on the watch (optional)', _s.contacts, () async {
            await Permission.contacts.request();
            _refresh();
          }),
        ],
      ),
      _Step(
        icon: Icons.record_voice_over_rounded,
        color: AppColors.warning,
        title: 'Voice memos & voice replies',
        body: 'Recordings from the watch are transcribed here on the phone, and you can dictate replies to '
            'messages on the watch. This needs a one-time speech model download (about 140 MB - Wi-Fi recommended).',
        done: _s.speechModel,
        doneText: 'Speech model installed',
        progress: _speechProgress,
        error: _speechError,
        actions: [
          if (!_s.speechModel && _speechProgress == null) _Action('Download speech model', _downloadSpeech, primary: true),
        ],
      ),
      _Step(
        icon: Icons.check_circle_rounded,
        color: AppColors.success,
        title: 'All set',
        body: 'You can change any of this later in Settings. Settings › Diagnostics shows what\'s still missing '
            'and lets you update the watch firmware.',
      ),
    ];

    return Scaffold(
      body: SafeArea(
        child: Column(children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 12, 8, 0),
            child: Row(children: [
              for (var i = 0; i < _count; i++)
                AnimatedContainer(
                  duration: const Duration(milliseconds: 250),
                  margin: const EdgeInsets.only(right: 6),
                  width: i == _page ? 22 : 8,
                  height: 8,
                  decoration: BoxDecoration(
                    color: i <= _page ? AppColors.accent : AppColors.surfaceHigh,
                    borderRadius: BorderRadius.circular(4),
                  ),
                ),
              const Spacer(),
              if (_page < _count - 1) TextButton(onPressed: _finish, child: const Text('Skip setup')),
            ]),
          ),
          Expanded(
            child: PageView(
              controller: _pages,
              onPageChanged: (i) {
                setState(() => _page = i);
                _refresh();
              },
              children: pages,
            ),
          ),
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 0, 20, 16),
            child: Row(children: [
              if (_page > 0)
                TextButton(
                  onPressed: () => _pages.previousPage(duration: const Duration(milliseconds: 300), curve: Curves.easeOut),
                  child: const Text('Back'),
                ),
              const Spacer(),
              FilledButton(
                onPressed: _next,
                child: Text(_page == 0 ? 'Get started' : _page == _count - 1 ? 'Open the app' : 'Continue'),
              ),
            ]),
          ),
        ]),
      ),
    );
  }
}

class _Action {
  final String label;
  final VoidCallback onTap;
  final bool primary;
  const _Action(this.label, this.onTap, {this.primary = false});
}

class _CheckItem {
  final String label;
  final bool ok;
  final VoidCallback onTap;
  const _CheckItem(this.label, this.ok, this.onTap);
}

class _Step extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String title;
  final String body;
  final bool done;
  final String? doneText;
  final List<_Action> actions;
  final List<_CheckItem> checklist;
  final double? progress;
  final String? error;
  final Widget? art;   // replaces the icon tile (welcome page: the app logo)

  const _Step({
    required this.icon,
    required this.color,
    required this.title,
    required this.body,
    this.done = false,
    this.doneText,
    this.actions = const [],
    this.checklist = const [],
    this.progress,
    this.error,
    this.art,
  });

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.fromLTRB(24, 40, 24, 24),
      children: [
        Align(
          alignment: Alignment.centerLeft,
          child: art ??
              Container(
                width: 76,
                height: 76,
                decoration: BoxDecoration(color: color.withValues(alpha: 0.16), borderRadius: BorderRadius.circular(24)),
                child: Icon(icon, color: color, size: 40),
              ),
        ),
        const SizedBox(height: 28),
        Text(title, style: const TextStyle(color: AppColors.text, fontSize: 26, fontWeight: FontWeight.w700, height: 1.2)),
        const SizedBox(height: 14),
        Text(body, style: const TextStyle(color: AppColors.textDim, fontSize: 15, height: 1.5)),
        const SizedBox(height: 26),
        if (done && doneText != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 14),
            child: Align(alignment: Alignment.centerLeft, child: StatusPill(doneText!)),
          ),
        if (progress != null) ...[
          LinearProgressIndicator(value: progress! > 0 ? progress : null, minHeight: 6, borderRadius: BorderRadius.circular(3)),
          const SizedBox(height: 8),
          Text('Downloading… ${(progress! * 100).round()}%', style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
          const SizedBox(height: 14),
        ],
        if (error != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 14),
            child: Text(error!, style: const TextStyle(color: AppColors.danger, fontSize: 13)),
          ),
        if (checklist.isNotEmpty)
          RowGroup(children: [
            for (final c in checklist)
              NavRow(
                icon: c.ok ? Icons.check_circle_rounded : Icons.radio_button_unchecked_rounded,
                color: c.ok ? AppColors.success : AppColors.textDim,
                title: c.label,
                subtitle: c.ok ? 'Allowed' : 'Tap to allow',
                onTap: c.onTap,
                trailing: const SizedBox.shrink(),
              ),
          ]),
        for (final a in actions)
          Padding(
            padding: const EdgeInsets.only(bottom: 10),
            child: SizedBox(
              width: double.infinity,
              child: a.primary
                  ? FilledButton(onPressed: a.onTap, child: Text(a.label))
                  : OutlinedButton(onPressed: a.onTap, child: Text(a.label)),
            ),
          ),
      ],
    );
  }
}
