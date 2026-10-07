import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Which phone notifications reach the watch, and call alerts.
class NotificationsScreen extends ConsumerStatefulWidget {
  const NotificationsScreen({super.key});

  @override
  ConsumerState<NotificationsScreen> createState() => _NotificationsScreenState();
}

class _AppItem {
  final String name;
  final IconData icon;
  final Color color;
  final String pkg;
  const _AppItem(this.name, this.icon, this.color, this.pkg);
}

class _NotificationsScreenState extends ConsumerState<NotificationsScreen> with WidgetsBindingObserver {
  bool _hasAccess = false;
  bool _waitingForAccess = false;

  static const _apps = [
    _AppItem('WhatsApp', Icons.chat_rounded, Color(0xFF25D366), 'com.whatsapp'),
    _AppItem('Telegram', Icons.send_rounded, Color(0xFF2AABEE), 'org.telegram.messenger'),
    _AppItem('Messages', Icons.sms_rounded, AppColors.accent2, 'com.google.android.apps.messaging'),
    _AppItem('Gmail', Icons.mail_rounded, Color(0xFFEA4335), 'com.google.android.gm'),
    _AppItem('Instagram', Icons.camera_alt_rounded, Color(0xFFE1306C), 'com.instagram.android'),
    _AppItem('Signal', Icons.lock_rounded, Color(0xFF3A76F0), 'org.thoughtcrime.securesms'),
    _AppItem('Discord', Icons.headset_mic_rounded, Color(0xFF5865F2), 'com.discord'),
    _AppItem('Slack', Icons.tag_rounded, Color(0xFFE01E5A), 'com.Slack'),
    _AppItem('Calendar', Icons.event_rounded, AppColors.warning, 'com.google.android.calendar'),
    _AppItem('X', Icons.alternate_email_rounded, AppColors.text, 'com.twitter.android'),
  ];

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _checkAccess();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  // Coming back from the system settings page: re-check access.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) _checkAccess();
  }

  Future<void> _checkAccess() async {
    final ok = await ref.read(nativeServiceProvider).isNotificationListenerEnabled();
    if (!mounted) return;
    setState(() => _hasAccess = ok);
    if (ok && _waitingForAccess) {
      _waitingForAccess = false;
      final s = ref.read(settingsProvider);
      await ref.read(settingsProvider.notifier).update(s.copyWith(forwardNotifications: true));
    }
  }

  Future<void> _grantAccess() async {
    _waitingForAccess = true;
    await ref.read(nativeServiceProvider).openNotificationListenerSettings();
  }

  Future<void> _setForward(bool on) async {
    if (on && !_hasAccess) {
      await _grantAccess();
      return;
    }
    if (on) await Permission.notification.request(); // for the "connected" status notification
    final s = ref.read(settingsProvider);
    await ref.read(settingsProvider.notifier).update(s.copyWith(forwardNotifications: on));
  }

  Future<void> _toggleApp(String pkg, bool on) async {
    final s = ref.read(settingsProvider);
    final set = s.notifPackages.toSet();
    if (on) {
      set.add(pkg);
    } else {
      set.remove(pkg);
    }
    await ref.read(settingsProvider.notifier).update(s.copyWith(notifPackages: set.toList()));
  }

  @override
  Widget build(BuildContext context) {
    final s = ref.watch(settingsProvider);
    final ble = ref.watch(bleProvider);
    final allApps = s.notifPackages.isEmpty;

    return Scaffold(
      appBar: AppBar(title: const Text('Notifications & calls')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          if (!_hasAccess)
            Panel(
              color: AppColors.warning.withValues(alpha: 0.12),
              child: Row(
                children: [
                  const IconChip(Icons.lock_open_rounded, color: AppColors.warning),
                  const SizedBox(width: 14),
                  const Expanded(
                    child: Text(
                      'Allow notification access so the app can read notifications, calls and what\'s playing.',
                      style: TextStyle(color: AppColors.text, fontSize: 13, height: 1.35),
                    ),
                  ),
                  const SizedBox(width: 8),
                  TextButton(onPressed: _grantAccess, child: const Text('Allow')),
                ],
              ),
            ),
          const SizedBox(height: 4),
          RowGroup(children: [
            ToggleRow(
              icon: Icons.notifications_active_outlined,
              title: 'Forward notifications',
              subtitle: 'Reply and dismiss from the watch',
              value: s.forwardNotifications && _hasAccess,
              onChanged: _setForward,
            ),
            ToggleRow(
              icon: Icons.call_rounded,
              color: AppColors.success,
              title: 'Call alerts',
              subtitle: 'See who is calling, answer or decline',
              value: s.callAlerts,
              onChanged: (v) => ref.read(settingsProvider.notifier).update(s.copyWith(callAlerts: v)),
            ),
          ]),
          const SectionLabel('Apps'),
          RowGroup(children: [
            ToggleRow(
              icon: Icons.apps_rounded,
              color: AppColors.accent2,
              title: 'All apps',
              subtitle: allApps ? 'Every notification goes to the watch' : 'Only the apps selected below',
              value: allApps,
              onChanged: (v) => ref.read(settingsProvider.notifier).update(
                  s.copyWith(notifPackages: v ? <String>[] : _apps.take(4).map((a) => a.pkg).toList())),
            ),
            if (!allApps)
              for (final a in _apps)
                ToggleRow(
                  icon: a.icon,
                  color: a.color,
                  title: a.name,
                  value: s.notifPackages.contains(a.pkg),
                  onChanged: (v) => _toggleApp(a.pkg, v),
                ),
          ]),
          const SectionLabel('Try it'),
          RowGroup(children: [
            NavRow(
              icon: Icons.send_rounded,
              title: 'Send a test notification',
              subtitle: ble.isConnected ? 'Shows up on the watch right away' : 'Connect the watch first',
              onTap: ble.isConnected
                  ? () async {
                      await ref.read(bleServiceProvider).sendLink({
                        't': 'ntf',
                        'id': DateTime.now().millisecondsSinceEpoch & 0x7fffffff,
                        'ap': 'AmoledWatch',
                        'ti': 'Hello from your phone',
                        'tx': 'Notifications are linked. Tap to open, swipe right to go back.',
                        'rp': 0,
                      });
                      if (context.mounted) showSnack(context, 'Sent');
                    }
                  : null,
            ),
          ]),
          const SizedBox(height: 12),
          const Text(
            'Ongoing notifications (music players, downloads, navigation) and duplicate updates are skipped. '
            'Do Not Disturb on the watch keeps them in its list without a popup.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}
