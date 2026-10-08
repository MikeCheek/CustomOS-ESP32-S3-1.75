import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/update_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'firmware_update_screen.dart';

/// Settings > Software updates: the app and the watch firmware against the
/// latest GitHub release. Also checked automatically once a day.
class UpdatesScreen extends ConsumerStatefulWidget {
  const UpdatesScreen({super.key});

  @override
  ConsumerState<UpdatesScreen> createState() => _UpdatesScreenState();
}

class _UpdatesScreenState extends ConsumerState<UpdatesScreen> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => ref.read(updateProvider.notifier).check(auto: true));
  }

  @override
  Widget build(BuildContext context) {
    final u = ref.watch(updateProvider);
    final ble = ref.watch(bleProvider);
    final watchFw = ref.watch(companionProvider).watchFirmware;
    final rel = u.latest;
    final notifier = ref.read(updateProvider.notifier);

    String status;
    if (u.checking) {
      status = 'Checking GitHub…';
    } else if (u.error != null) {
      status = u.error!;
    } else if (!u.checked) {
      status = 'Not checked yet';
    } else if (rel == null) {
      status = 'No release published yet';
    } else {
      status = 'Latest release: ${rel.version}';
    }

    return Scaffold(
      appBar: AppBar(title: const Text('Software updates')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          Panel(
            child: Row(children: [
              IconChip(Icons.cloud_sync_rounded, color: u.error != null ? AppColors.danger : AppColors.accent),
              const SizedBox(width: 14),
              Expanded(
                child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Text(status, style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                  if (u.lastCheck != null) ...[
                    const SizedBox(height: 2),
                    Text('Checked ${_ago(u.lastCheck!)}', style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
                  ],
                ]),
              ),
              if (u.checking)
                const SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2.5))
              else
                IconButton(
                  tooltip: 'Check now',
                  icon: const Icon(Icons.refresh_rounded, color: AppColors.text),
                  onPressed: notifier.check,
                ),
            ]),
          ),
          const SectionLabel('Companion app'),
          _UpdateCard(
            icon: Icons.phone_android_rounded,
            installed: u.appVersion ?? '—',
            latest: rel?.apkUrl != null ? rel!.version : null,
            available: u.appUpdate,
            progress: u.apkProgress,
            error: u.apkError,
            sizeBytes: rel?.apkSize ?? 0,
            actionLabel: 'Update app',
            onAction: () => notifier.installApp(),
            onCancel: notifier.cancelApp,
          ),
          const SectionLabel('Watch firmware'),
          _UpdateCard(
            icon: Icons.watch_rounded,
            installed: watchFw ?? (ble.isConnected ? 'unknown' : 'watch not connected'),
            latest: rel?.firmwareUrl != null ? rel!.version : null,
            available: u.firmwareUpdateFor(watchFw),
            sizeBytes: rel?.firmwareSize ?? 0,
            actionLabel: 'Update watch',
            note: ble.isConnected ? 'Installed over Bluetooth - keep the watch close.' : 'Connect the watch to update it.',
            onAction: ble.isConnected
                ? () => Navigator.push(
                    context, MaterialPageRoute(builder: (_) => const FirmwareUpdateScreen(installLatest: true)))
                : null,
          ),
          const SizedBox(height: 16),
          const Text(
            'The watch also checks for firmware updates by itself when it is on Wi-Fi '
            '(watch Settings > Software update).',
            style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.45),
          ),
        ],
      ),
    );
  }

  static String _ago(DateTime t) {
    final d = DateTime.now().difference(t);
    if (d.inMinutes < 1) return 'just now';
    if (d.inHours < 1) return '${d.inMinutes} min ago';
    if (d.inDays < 1) return '${d.inHours} h ago';
    return '${d.inDays} d ago';
  }
}

class _UpdateCard extends StatelessWidget {
  final IconData icon;
  final String installed;
  final String? latest;
  final bool available;
  final double? progress;
  final String? error;
  final int sizeBytes;
  final String actionLabel;
  final String? note;
  final VoidCallback? onAction;
  final VoidCallback? onCancel;

  const _UpdateCard({
    required this.icon,
    required this.installed,
    required this.latest,
    required this.available,
    required this.sizeBytes,
    required this.actionLabel,
    this.progress,
    this.error,
    this.note,
    this.onAction,
    this.onCancel,
  });

  @override
  Widget build(BuildContext context) {
    return Panel(
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          IconChip(icon, color: available ? AppColors.success : AppColors.accent2),
          const SizedBox(width: 14),
          Expanded(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(
                available ? 'Version $latest available' : (latest != null ? 'Up to date' : 'Installed'),
                style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
              ),
              const SizedBox(height: 2),
              Text(
                'Installed: $installed${available && sizeBytes > 0 ? ' · ${(sizeBytes / (1024 * 1024)).toStringAsFixed(1)} MB' : ''}',
                style: const TextStyle(color: AppColors.textDim, fontSize: 13),
              ),
            ]),
          ),
        ]),
        if (progress != null) ...[
          const SizedBox(height: 14),
          LinearProgressIndicator(value: progress! > 0 ? progress : null),
          const SizedBox(height: 8),
          Row(children: [
            Text('Downloading… ${(progress! * 100).round()}%',
                style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
            const Spacer(),
            if (onCancel != null) TextButton(onPressed: onCancel, child: const Text('Cancel')),
          ]),
        ] else if (available) ...[
          const SizedBox(height: 14),
          SizedBox(
            width: double.infinity,
            child: FilledButton.icon(
              icon: const Icon(Icons.system_update_rounded),
              label: Text(actionLabel),
              onPressed: onAction,
            ),
          ),
          if (note != null) ...[
            const SizedBox(height: 8),
            Text(note!, style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
          ],
        ],
        if (error != null) ...[
          const SizedBox(height: 10),
          Text(error!, style: const TextStyle(color: AppColors.danger, fontSize: 13)),
        ],
      ]),
    );
  }
}

/// Wraps the main tabs: checks for updates when the app opens (at most
/// daily) and asks once per version - "Later" snoozes it for a day.
class UpdatePrompter extends ConsumerStatefulWidget {
  final Widget child;
  const UpdatePrompter({super.key, required this.child});

  @override
  ConsumerState<UpdatePrompter> createState() => _UpdatePrompterState();
}

class _UpdatePrompterState extends ConsumerState<UpdatePrompter> with WidgetsBindingObserver {
  final _snoozed = <String, DateTime>{}; // "app:3.3.0" / "fw:3.3.0" -> when "Later" was tapped
  bool _dialogOpen = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    Future.delayed(const Duration(seconds: 3), () {
      if (mounted) ref.read(updateProvider.notifier).check(auto: true);
    });
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState s) {
    if (s == AppLifecycleState.resumed) ref.read(updateProvider.notifier).check(auto: true);
  }

  bool _isSnoozed(String key) {
    final t = _snoozed[key];
    return t != null && DateTime.now().difference(t) < UpdateNotifier.checkEvery;
  }

  Future<void> _maybePrompt() async {
    if (_dialogOpen || !mounted) return;
    final u = ref.read(updateProvider);
    final rel = u.latest;
    if (rel == null || u.apkProgress != null) return;
    final watchFw = ref.read(companionProvider).watchFirmware;
    final connected = ref.read(bleProvider).isConnected;

    if (u.appUpdate && !_isSnoozed('app:${rel.version}')) {
      _dialogOpen = true;
      final yes = await _ask('App update available',
          'AmoledWatch ${rel.version} is out (you have ${u.appVersion}). Download and install it now?');
      _dialogOpen = false;
      if (yes) {
        final ok = await ref.read(updateProvider.notifier).installApp();
        if (!ok && mounted) {
          final err = ref.read(updateProvider).apkError;
          if (err != null) showSnack(context, err);
        }
      } else {
        _snoozed['app:${rel.version}'] = DateTime.now();
      }
      return; // one question at a time; the watch one comes next time
    }
    if (connected && u.firmwareUpdateFor(watchFw) && !_isSnoozed('fw:${rel.version}')) {
      _dialogOpen = true;
      final yes = await _ask('Watch update available',
          'Firmware ${rel.version} is out (the watch has $watchFw). Download it and install it on the watch now?');
      _dialogOpen = false;
      if (!mounted) return;
      if (yes) {
        Navigator.push(context, MaterialPageRoute(builder: (_) => const FirmwareUpdateScreen(installLatest: true)));
      } else {
        _snoozed['fw:${rel.version}'] = DateTime.now();
      }
    }
  }

  Future<bool> _ask(String title, String body) async {
    final r = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text(title),
        content: Text(body),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Later')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Update')),
        ],
      ),
    );
    return r == true;
  }

  @override
  Widget build(BuildContext context) {
    // Re-evaluate when a check finishes or the watch reports its firmware.
    ref.listen(updateProvider.select((u) => u.latest?.version), (_, _) => _maybePrompt());
    ref.listen(companionProvider.select((c) => c.watchFirmware), (_, _) => _maybePrompt());
    return widget.child;
  }
}
