import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/settings_provider.dart';
import '../services/setup_checks.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'ai_settings_screen.dart';
import 'firmware_update_screen.dart';
import 'notifications_screen.dart';

/// Everything needed to tell why something doesn't work: the watch's
/// firmware, last reset and crash report, the link, and the phone-side
/// permissions and models.
class DiagnosticsScreen extends ConsumerStatefulWidget {
  const DiagnosticsScreen({super.key});

  @override
  ConsumerState<DiagnosticsScreen> createState() => _DiagnosticsScreenState();
}

class _DiagnosticsScreenState extends ConsumerState<DiagnosticsScreen> with WidgetsBindingObserver {
  SetupStatus? _setup;
  bool _loading = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    WidgetsBinding.instance.addPostFrameCallback((_) => _refresh());
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) _refresh();
  }

  Future<void> _refresh() async {
    if (_loading) return;
    setState(() => _loading = true);
    try {
      final link = ref.read(companionProvider.notifier);
      if (ref.read(bleProvider).isConnected) {
        await link.requestDiagnostics();
      }
      final s = await SetupStatus.check(ref.read(nativeServiceProvider));
      if (mounted) setState(() => _setup = s);
    } catch (_) {
    } finally {
      if (mounted) setState(() => _loading = false);
    }
    if (!mounted) return;
    // Opening this page counts as having seen the crash report.
    await Future.delayed(const Duration(milliseconds: 800));
    if (mounted) await ref.read(companionProvider.notifier).markCrashSeen();
  }

  void _push(Widget w) => Navigator.push(context, MaterialPageRoute(builder: (_) => w));

  @override
  Widget build(BuildContext context) {
    final ble = ref.watch(bleProvider);
    final link = ref.watch(companionProvider);
    final settings = ref.watch(settingsProvider);
    final crash = link.crashReport ?? '';
    final s = _setup;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Diagnostics'),
        actions: [
          IconButton(
            icon: _loading
                ? const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.refresh_rounded),
            onPressed: _loading ? null : _refresh,
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 32),
        children: [
          const SectionLabel('Watch'),
          Panel(
            child: Column(children: [
              _Info('Connection', ble.isConnected ? (link.linkActive ? 'Connected, phone link on' : 'Connected') : 'Not connected',
                  color: ble.isConnected ? AppColors.success : AppColors.danger),
              _Info('Firmware', link.watchFirmware ?? (ble.isConnected ? 'older than 2.5' : '—')),
              _Info('Last restart', link.watchResetReason ?? '—',
                  color: _isCrashReason(link.watchResetReason) ? AppColors.warning : null),
              _Info('Battery', ble.batteryLevel >= 0 ? '${ble.batteryLevel}%' : '—'),
            ]),
          ),
          if ((link.watchSystem ?? '').isNotEmpty) ...[
            const SizedBox(height: 10),
            _MonoBlock(text: link.watchSystem!),
          ],
          SectionLabel('Last crash',
              trailing: crash.isNotEmpty
                  ? Row(mainAxisSize: MainAxisSize.min, children: [
                      TextButton.icon(
                        icon: const Icon(Icons.copy_rounded, size: 16),
                        label: const Text('Copy'),
                        onPressed: () async {
                          await Clipboard.setData(ClipboardData(text: '$crash\n\n${link.watchSystem ?? ''}'));
                          if (context.mounted) showSnack(context, 'Crash report copied');
                        },
                      ),
                      TextButton(
                        onPressed: () => ref.read(companionProvider.notifier).requestDiagnostics(clear: true),
                        child: const Text('Clear', style: TextStyle(color: AppColors.danger)),
                      ),
                    ])
                  : null),
          if (crash.isNotEmpty) ...[
            _MonoBlock(text: crash, color: AppColors.warning),
            const Padding(
              padding: EdgeInsets.fromLTRB(4, 10, 4, 0),
              child: Text(
                'Decode the backtrace on your computer with:\n'
                'xtensa-esp32s3-elf-addr2line -pfiaC -e AmoledSmartWatchOS.ino.elf <addresses>',
                style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.4),
              ),
            ),
          ] else
            Panel(
              child: Row(children: [
                const IconChip(Icons.verified_rounded, color: AppColors.success),
                const SizedBox(width: 14),
                Expanded(
                  child: Text(
                    !ble.isConnected
                        ? 'Connect the watch to read its crash log'
                        : link.diagnosticsAt == null
                            ? (link.linkActive ? 'Asking the watch…' : 'Update the watch firmware to read crash reports')
                            : 'No crashes recorded',
                    style: const TextStyle(color: AppColors.text, fontSize: 14),
                  ),
                ),
              ]),
            ),
          const SectionLabel('Firmware'),
          RowGroup(children: [
            NavRow(
              icon: Icons.system_update_rounded,
              color: AppColors.accent2,
              title: 'Update watch firmware',
              subtitle: 'Install a .bin over Bluetooth',
              onTap: () => _push(const FirmwareUpdateScreen()),
            ),
          ]),
          const SectionLabel('This phone'),
          if (s == null)
            const Panel(child: Center(child: CircularProgressIndicator()))
          else
            RowGroup(children: [
              _Check(
                icon: Icons.bluetooth_rounded,
                title: 'Nearby devices',
                ok: s.bluetooth,
                okText: 'Allowed',
                fix: () async {
                  await SetupStatus.requestBluetooth();
                  _refresh();
                },
              ),
              _Check(
                icon: Icons.notifications_active_outlined,
                title: 'Notification access',
                ok: s.notificationAccess,
                okText: settings.forwardNotifications ? 'Forwarding on' : 'Allowed, forwarding off',
                fix: () => _push(const NotificationsScreen()),
              ),
              _Check(
                icon: Icons.sync_lock_rounded,
                title: 'Status notification',
                ok: s.postNotifications || !settings.backgroundLink,
                okText: settings.backgroundLink ? 'Allowed' : 'Background link off',
                fix: () async {
                  await Permission.notification.request();
                  _refresh();
                },
              ),
              _Check(
                icon: Icons.place_outlined,
                title: 'Location (weather)',
                ok: s.location || !settings.weatherAuto,
                okText: settings.weatherAuto ? 'Allowed' : 'Weather off',
                fix: () async {
                  await Permission.locationWhenInUse.request();
                  _refresh();
                },
              ),
              _Check(
                icon: Icons.calendar_month_rounded,
                title: 'Calendar',
                ok: s.calendar || !settings.calendarToWatch,
                okText: settings.calendarToWatch ? 'Allowed' : 'Calendar off',
                fix: () async {
                  await Permission.calendarFullAccess.request();
                  await ref.read(companionProvider.notifier).calendarPermissionChanged();
                  _refresh();
                },
              ),
              _Check(
                icon: Icons.contacts_outlined,
                title: 'Contacts',
                ok: s.contacts,
                okText: 'Allowed',
                optional: true,
                fix: () async {
                  await Permission.contacts.request();
                  _refresh();
                },
              ),
              _Check(
                icon: Icons.record_voice_over_outlined,
                title: 'Speech model',
                ok: s.speechModel,
                okText: 'Installed - memos and voice replies',
                failText: 'Needed for transcripts and voice replies',
                fix: () => _push(const AiSettingsScreen()),
              ),
              _Check(
                icon: Icons.auto_awesome_rounded,
                title: 'Summary model',
                ok: s.summaryModel,
                okText: 'Installed',
                optional: true,
                fix: () => _push(const AiSettingsScreen()),
              ),
            ]),
        ],
      ),
    );
  }

  static bool _isCrashReason(String? r) =>
      r != null && (r.contains('crash') || r.contains('watchdog') || r.contains('brownout') || r.contains('lockup'));
}

class _Info extends StatelessWidget {
  final String label, value;
  final Color? color;
  const _Info(this.label, this.value, {this.color});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 7),
      child: Row(children: [
        Expanded(child: Text(label, style: const TextStyle(color: AppColors.textDim, fontSize: 14))),
        Flexible(
          child: Text(value,
              textAlign: TextAlign.right,
              style: TextStyle(color: color ?? AppColors.text, fontSize: 14, fontWeight: FontWeight.w500)),
        ),
      ]),
    );
  }
}

class _MonoBlock extends StatelessWidget {
  final String text;
  final Color? color;
  const _MonoBlock({required this.text, this.color});

  @override
  Widget build(BuildContext context) {
    return Panel(
      padding: const EdgeInsets.all(14),
      child: SelectableText(
        text.trim(),
        style: TextStyle(
          color: color ?? AppColors.textDim,
          fontFamily: 'monospace',
          fontSize: 12,
          height: 1.45,
        ),
      ),
    );
  }
}

class _Check extends StatelessWidget {
  final IconData icon;
  final String title;
  final bool ok;
  final bool optional;
  final String okText;
  final String? failText;
  final VoidCallback fix;

  const _Check({
    required this.icon,
    required this.title,
    required this.ok,
    required this.okText,
    required this.fix,
    this.failText,
    this.optional = false,
  });

  @override
  Widget build(BuildContext context) {
    final color = ok ? AppColors.success : (optional ? AppColors.textDim : AppColors.warning);
    return NavRow(
      icon: icon,
      color: color,
      title: title,
      subtitle: ok ? okText : (failText ?? (optional ? 'Optional - not set up' : 'Not set up - tap to fix')),
      onTap: fix,
      trailing: ok
          ? const Icon(Icons.check_circle_rounded, color: AppColors.success)
          : const Icon(Icons.chevron_right_rounded, color: AppColors.textFaint),
    );
  }
}
