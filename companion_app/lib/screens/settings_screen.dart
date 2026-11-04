import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/settings_provider.dart';
import '../providers/update_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'ai_settings_screen.dart';
import 'calendar_screen.dart';
import 'dnd_replies_screen.dart';
import '../app.dart';
import 'diagnostics_screen.dart';
import 'firmware_update_screen.dart';
import 'updates_screen.dart';
import 'onboarding_screen.dart';
import 'media_screen.dart';
import 'notifications_screen.dart';
import 'scan_screen.dart';
import 'weather_screen.dart';
import 'safety_screen.dart';
import 'wake_word_screen.dart';
import '../widgets/emulator_card.dart';
import '../widgets/platform_notice.dart';

class SettingsScreen extends ConsumerWidget {
  const SettingsScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final s = ref.watch(settingsProvider);
    final ble = ref.watch(bleProvider);
    final setter = ref.read(settingsProvider.notifier);
    void push(Widget w) => Navigator.push(context, MaterialPageRoute(builder: (_) => w));
    final upd = ref.watch(updateProvider);
    final watchFw = ref.watch(companionProvider).watchFirmware;
    final updates = (upd.appUpdate ? 1 : 0) + (upd.firmwareUpdateFor(watchFw) ? 1 : 0);

    return Scaffold(
      appBar: AppBar(title: const Text('Settings')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 32),
        children: [
          const SectionLabel('Watch'),
          RowGroup(
            children: [
              NavRow(
                icon: Icons.add_link_rounded,
                color: AppColors.success,
                title: 'Pair a watch',
                subtitle: s.lastDeviceName != null ? 'Current: ${s.lastDeviceName}' : 'Scan for AmoledWatch nearby',
                onTap: () => push(const ScanScreen()),
              ),
              if (s.lastDeviceId != null)
                NavRow(
                  icon: Icons.link_off_rounded,
                  color: AppColors.danger,
                  title: 'Forget this watch',
                  subtitle: 'Disconnects and stops reconnecting',
                  onTap: () async {
                    final ok = await showDialog<bool>(
                      context: context,
                      builder: (ctx) => AlertDialog(
                        title: const Text('Forget watch?'),
                        content: const Text('The app will stop reconnecting to it. You can pair again any time.'),
                        actions: [
                          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
                          TextButton(
                            onPressed: () => Navigator.pop(ctx, true),
                            child: const Text('Forget', style: TextStyle(color: AppColors.danger)),
                          ),
                        ],
                      ),
                    );
                    if (ok != true) return;
                    // Unpair on the phone too (firmware 3.0 pairs with a code).
                    await ref.read(bleServiceProvider).removeBond();
                    await ref.read(bleProvider.notifier).disconnect();
                    await setter.forgetDevice();
                  },
                ),
              ToggleRow(
                icon: Icons.bluetooth_connected_rounded,
                title: 'Reconnect automatically',
                subtitle: 'Whenever the watch is back in range',
                value: s.autoConnect,
                onChanged: (v) => setter.update(s.copyWith(autoConnect: v)),
              ),
              ToggleRow(
                icon: Icons.sync_lock_rounded,
                color: AppColors.accent2,
                title: 'Stay connected in the background',
                subtitle: 'Keeps notifications and controls working with the app closed',
                value: s.backgroundLink,
                onChanged: (v) async {
                  await setter.update(s.copyWith(backgroundLink: v));
                  final native = ref.read(nativeServiceProvider);
                  if (v) {
                    await Permission.notification.request();
                    if (ref.read(bleProvider).isConnected) await native.startLinkService();
                  } else {
                    await native.stopLinkService();
                  }
                },
              ),
            ],
          ),
          const SectionLabel('Watch emulator'),
          const EmulatorCard(),
          const SectionLabel('Phone link'),
          RowGroup(
            children: [
              NavRow(
                icon: Icons.notifications_active_outlined,
                title: 'Notifications & calls',
                subtitle: isIos
                    ? 'Android only - see why'
                    : (s.forwardNotifications ? 'Forwarding to the watch' : 'Off'),
                onTap: () => push(const NotificationsScreen()),
              ),
              NavRow(
                icon: Icons.music_note_outlined,
                color: AppColors.accent3,
                title: 'Music controls',
                subtitle: isIos ? 'Android only - see why' : (s.mediaToWatch ? 'On' : 'Off'),
                onTap: () => push(const MediaScreen()),
              ),
              NavRow(
                icon: Icons.wb_cloudy_outlined,
                color: AppColors.warning,
                title: 'Weather',
                subtitle: s.weatherAuto ? 'Automatic, from your location' : 'Manual',
                onTap: () => push(const WeatherScreen()),
              ),
              NavRow(
                icon: Icons.calendar_month_rounded,
                color: AppColors.accent2,
                title: 'Calendar',
                subtitle: s.calendarToWatch
                    ? (s.reminderMinutes > 0
                          ? 'Agenda, reminders ${s.reminderMinutes} min before'
                          : 'Agenda, no reminders')
                    : 'Off',
                onTap: () => push(const CalendarScreen()),
              ),
              // reads Google Maps' notifications: Android only
              if (!isIos)
                ToggleRow(
                  icon: Icons.navigation_rounded,
                  color: AppColors.success,
                  title: 'Navigation',
                  subtitle: 'Google Maps directions on the watch',
                  value: s.navigationToWatch,
                  onChanged: (v) => setter.update(s.copyWith(navigationToWatch: v)),
                ),
              NavRow(
                icon: Icons.do_not_disturb_on_outlined,
                color: AppColors.accent2,
                title: 'Do Not Disturb & quick replies',
                subtitle: [
                  s.dndSync ? 'DND synced' : 'DND not synced',
                  if (s.bedtimeOn) 'bedtime on',
                  s.quickReplies.isEmpty ? 'default replies' : '${s.quickReplies.length} replies',
                ].join(' · '),
                onTap: () => push(const DndRepliesScreen()),
              ),
              NavRow(
                icon: Icons.palette_outlined,
                color: AppColors.accent3,
                title: 'Faces & display',
                subtitle: 'The face, its colour and slots, brightness, text size - in the Faces tab',
                onTap: () => TabSwitcher.open(context, TabSwitcher.faces),
              ),
            ],
          ),
          const SectionLabel('AI & voice memos'),
          RowGroup(
            children: [
              NavRow(
                icon: Icons.auto_awesome_rounded,
                color: AppColors.accent2,
                title: 'AI models',
                subtitle: 'Transcription, summaries, voices, vision - all on the phone',
                onTap: () => push(const AiSettingsScreen()),
              ),
              NavRow(
                icon: Icons.record_voice_over_rounded,
                color: AppColors.accent,
                title: 'Wake word & offline commands',
                subtitle: 'Installed on the watch\'s SD card',
                onTap: () => push(const WakeWordScreen()),
              ),
              ToggleRow(
                icon: Icons.wifi_rounded,
                color: AppColors.success,
                title: 'Fast memo transfers over Wi-Fi',
                subtitle: 'For big recordings: the watch and the phone on the same network',
                value: s.wifiTransfers,
                onChanged: (v) => setter.update(s.copyWith(wifiTransfers: v)),
              ),
            ],
          ),
          const SectionLabel('Safety'),
          RowGroup(
            children: [
              NavRow(
                icon: Icons.health_and_safety_outlined,
                color: AppColors.danger,
                title: 'Fall alerts',
                subtitle: 'Who gets a text if you fall',
                onTap: () => push(const SafetyScreen()),
              ),
            ],
          ),
          const SectionLabel('Software'),
          RowGroup(
            children: [
              NavRow(
                icon: Icons.cloud_download_rounded,
                color: updates > 0 ? AppColors.success : AppColors.accent,
                title: 'Software updates',
                subtitle: updates > 0
                    ? '${updates} update${updates > 1 ? 's' : ''} available'
                    : 'App and watch firmware from GitHub',
                trailing: updates > 0 ? StatusPill('$updates new', color: AppColors.success) : null,
                onTap: () => push(const UpdatesScreen()),
              ),
              NavRow(
                icon: Icons.system_update_rounded,
                color: AppColors.accent2,
                title: 'Install watch firmware',
                subtitle: 'From GitHub or a .bin file, over Bluetooth',
                onTap: () => push(const FirmwareUpdateScreen()),
              ),
              NavRow(
                icon: Icons.monitor_heart_outlined,
                color: AppColors.success,
                title: 'Diagnostics',
                subtitle: 'Crash reports, firmware, permissions',
                onTap: () => push(const DiagnosticsScreen()),
              ),
              NavRow(
                icon: Icons.flag_outlined,
                color: AppColors.warning,
                title: 'Setup guide',
                subtitle: 'Pairing and permissions, step by step',
                onTap: () => push(const OnboardingScreen(fromSettings: true)),
              ),
            ],
          ),
          const SectionLabel('About'),
          Panel(
            child: Column(
              children: [
                Padding(
                  padding: const EdgeInsets.only(bottom: 8),
                  child: Row(
                    children: [
                      Image.asset('assets/logo.png', width: 44, height: 44),
                      const SizedBox(width: 12),
                      const Text(
                        'AmoledWatch',
                        style: TextStyle(color: AppColors.text, fontSize: 17, fontWeight: FontWeight.w700),
                      ),
                    ],
                  ),
                ),
                _InfoRow('App', upd.appVersion ?? '—'),
                _InfoRow('Watch firmware', watchFw ?? '—'),
                _InfoRow('Watch', ble.deviceName ?? s.lastDeviceName ?? '—'),
                _InfoRow(
                  'Status',
                  ble.isConnected ? 'Connected' : (ble.isConnecting ? 'Reconnecting' : 'Not connected'),
                ),
                _InfoRow('Watch battery', ble.batteryLevel >= 0 ? '${ble.batteryLevel}%' : '—'),
              ],
            ),
          ),
          const Padding(
            padding: EdgeInsets.fromLTRB(4, 10, 4, 0),
            child: Text(
              'Tip: add the AmoledWatch widget to your home screen for battery, steps, the next event and Find my watch.',
              style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.4),
            ),
          ),
          const SectionLabel('Author'),
          const _AuthorCard(),
        ],
      ),
    );
  }
}

/// Who made the app, with links to support them and to their work.
class _AuthorCard extends ConsumerWidget {
  const _AuthorCard();

  static const _koFi = 'https://ko-fi.com/michelepulvirenti';
  static const _portfolio = 'https://michelepulvirenti.vercel.app/';

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    Future<void> open(String url) async {
      final ok = await ref.read(nativeServiceProvider).openUrl(url);
      if (!ok && context.mounted) showSnack(context, 'No browser to open $url');
    }

    return RowGroup(
      children: [
        const Padding(
          padding: EdgeInsets.fromLTRB(14, 14, 14, 10),
          child: Row(
            children: [
              CircleAvatar(
                radius: 22,
                backgroundColor: AppColors.surfaceHigh,
                child: Text(
                  'MP',
                  style: TextStyle(color: AppColors.text, fontWeight: FontWeight.w700),
                ),
              ),
              SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      'Michele Pulvirenti',
                      style: TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w700),
                    ),
                    SizedBox(height: 2),
                    Text('Made AmoledWatch OS and this app', style: TextStyle(color: AppColors.textDim, fontSize: 13)),
                  ],
                ),
              ),
            ],
          ),
        ),
        NavRow(
          icon: Icons.coffee_rounded,
          color: AppColors.warning,
          title: 'Buy me a coffee',
          subtitle: 'Support the project on Ko-fi',
          trailing: const Icon(Icons.open_in_new_rounded, color: AppColors.textFaint, size: 20),
          onTap: () => open(_koFi),
        ),
        NavRow(
          icon: Icons.language_rounded,
          color: AppColors.accent2,
          title: 'Portfolio',
          subtitle: 'michelepulvirenti.vercel.app',
          trailing: const Icon(Icons.open_in_new_rounded, color: AppColors.textFaint, size: 20),
          onTap: () => open(_portfolio),
        ),
      ],
    );
  }
}

class _InfoRow extends StatelessWidget {
  final String label;
  final String value;
  const _InfoRow(this.label, this.value);

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 7),
      child: Row(
        children: [
          Expanded(
            child: Text(label, style: const TextStyle(color: AppColors.textDim, fontSize: 14)),
          ),
          Text(
            value,
            style: const TextStyle(color: AppColors.text, fontSize: 14, fontWeight: FontWeight.w500),
          ),
        ],
      ),
    );
  }
}
