import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// App-wide preferences, persisted in SharedPreferences.
class AppSettings {
  final bool loaded;
  final bool autoConnect;
  final String? lastDeviceId;
  final String? lastDeviceName;
  final bool backgroundLink;
  final bool forwardNotifications;
  final List<String> notifPackages; // empty = all apps
  final bool callAlerts;
  final bool mediaToWatch;
  final bool weatherAuto;
  final bool fahrenheit;
  final bool onboarded;     // first-run setup finished (or skipped)
  final bool calendarToWatch;
  final int reminderMinutes; // 0 = no reminders on the watch
  final bool navigationToWatch;
  final bool wifiTransfers;   // recordings over Wi-Fi when the watch can
  final bool autoProcess;     // after a sync: transcribe + summarize new memos
  final bool dndSync;         // phone DND <-> watch DND
  final bool bedtimeOn;       // watch silent every night
  final int bedFrom, bedTo;   // minutes after midnight
  final List<String> quickReplies; // empty = the watch's built-in presets
  final List<String> compSlots;    // watchface complications, left to right

  const AppSettings({
    this.loaded = false,
    this.autoConnect = true,
    this.lastDeviceId,
    this.lastDeviceName,
    this.backgroundLink = true,
    this.forwardNotifications = false,
    this.notifPackages = const [],
    this.callAlerts = true,
    this.mediaToWatch = true,
    this.weatherAuto = true,
    this.fahrenheit = false,
    this.onboarded = false,
    this.calendarToWatch = true,
    this.reminderMinutes = 10,
    this.navigationToWatch = true,
    this.wifiTransfers = true,
    this.autoProcess = false,
    this.dndSync = true,
    this.bedtimeOn = false,
    this.bedFrom = 23 * 60,
    this.bedTo = 7 * 60,
    this.quickReplies = const [],
    this.compSlots = const ['steps', 'next_event', 'battery'],
  });

  AppSettings copyWith({
    bool? loaded,
    bool? autoConnect,
    String? lastDeviceId,
    String? lastDeviceName,
    bool? backgroundLink,
    bool? forwardNotifications,
    List<String>? notifPackages,
    bool? callAlerts,
    bool? mediaToWatch,
    bool? weatherAuto,
    bool? fahrenheit,
    bool? onboarded,
    bool? calendarToWatch,
    int? reminderMinutes,
    bool? navigationToWatch,
    bool? wifiTransfers,
    bool? autoProcess,
    bool? dndSync,
    bool? bedtimeOn,
    int? bedFrom,
    int? bedTo,
    List<String>? quickReplies,
    List<String>? compSlots,
  }) {
    return AppSettings(
      loaded: loaded ?? this.loaded,
      autoConnect: autoConnect ?? this.autoConnect,
      lastDeviceId: lastDeviceId ?? this.lastDeviceId,
      lastDeviceName: lastDeviceName ?? this.lastDeviceName,
      backgroundLink: backgroundLink ?? this.backgroundLink,
      forwardNotifications: forwardNotifications ?? this.forwardNotifications,
      notifPackages: notifPackages ?? this.notifPackages,
      callAlerts: callAlerts ?? this.callAlerts,
      mediaToWatch: mediaToWatch ?? this.mediaToWatch,
      weatherAuto: weatherAuto ?? this.weatherAuto,
      fahrenheit: fahrenheit ?? this.fahrenheit,
      onboarded: onboarded ?? this.onboarded,
      calendarToWatch: calendarToWatch ?? this.calendarToWatch,
      reminderMinutes: reminderMinutes ?? this.reminderMinutes,
      navigationToWatch: navigationToWatch ?? this.navigationToWatch,
      wifiTransfers: wifiTransfers ?? this.wifiTransfers,
      autoProcess: autoProcess ?? this.autoProcess,
      dndSync: dndSync ?? this.dndSync,
      bedtimeOn: bedtimeOn ?? this.bedtimeOn,
      bedFrom: bedFrom ?? this.bedFrom,
      bedTo: bedTo ?? this.bedTo,
      quickReplies: quickReplies ?? this.quickReplies,
      compSlots: compSlots ?? this.compSlots,
    );
  }
}

class SettingsNotifier extends StateNotifier<AppSettings> {
  SettingsNotifier() : super(const AppSettings()) {
    _load();
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    state = AppSettings(
      loaded: true,
      autoConnect: p.getBool('auto_connect') ?? true,
      lastDeviceId: p.getString('last_device_id'),
      lastDeviceName: p.getString('last_device_name'),
      backgroundLink: p.getBool('background_link') ?? true,
      forwardNotifications: p.getBool('forward_notifications') ?? false,
      notifPackages: p.getStringList('notif_packages') ?? const [],
      callAlerts: p.getBool('call_alerts') ?? true,
      mediaToWatch: p.getBool('media_to_watch') ?? true,
      weatherAuto: p.getBool('weather_auto') ?? true,
      fahrenheit: p.getBool('fahrenheit') ?? false,
      // People who already paired a watch before setup existed skip it.
      onboarded: p.getBool('onboarded') ?? (p.getString('last_device_id') != null),
      calendarToWatch: p.getBool('calendar_to_watch') ?? true,
      reminderMinutes: p.getInt('reminder_minutes') ?? 10,
      navigationToWatch: p.getBool('navigation_to_watch') ?? true,
      wifiTransfers: p.getBool('wifi_transfers') ?? true,
      autoProcess: p.getBool('auto_process') ?? false,
      dndSync: p.getBool('dnd_sync') ?? true,
      bedtimeOn: p.getBool('bedtime_on') ?? false,
      bedFrom: p.getInt('bed_from') ?? 23 * 60,
      bedTo: p.getInt('bed_to') ?? 7 * 60,
      quickReplies: p.getStringList('quick_replies') ?? const [],
      compSlots: p.getStringList('comp_slots') ?? const ['steps', 'next_event', 'battery'],
    );
  }

  Future<void> update(AppSettings next) async {
    state = next.copyWith(loaded: true);
    final p = await SharedPreferences.getInstance();
    await p.setBool('auto_connect', next.autoConnect);
    if (next.lastDeviceId != null) await p.setString('last_device_id', next.lastDeviceId!);
    if (next.lastDeviceName != null) await p.setString('last_device_name', next.lastDeviceName!);
    await p.setBool('background_link', next.backgroundLink);
    await p.setBool('forward_notifications', next.forwardNotifications);
    await p.setStringList('notif_packages', next.notifPackages);
    await p.setBool('call_alerts', next.callAlerts);
    await p.setBool('media_to_watch', next.mediaToWatch);
    await p.setBool('weather_auto', next.weatherAuto);
    await p.setBool('fahrenheit', next.fahrenheit);
    await p.setBool('onboarded', next.onboarded);
    await p.setBool('calendar_to_watch', next.calendarToWatch);
    await p.setInt('reminder_minutes', next.reminderMinutes);
    await p.setBool('navigation_to_watch', next.navigationToWatch);
    await p.setBool('wifi_transfers', next.wifiTransfers);
    await p.setBool('auto_process', next.autoProcess);
    await p.setBool('dnd_sync', next.dndSync);
    await p.setBool('bedtime_on', next.bedtimeOn);
    await p.setInt('bed_from', next.bedFrom);
    await p.setInt('bed_to', next.bedTo);
    await p.setStringList('quick_replies', next.quickReplies);
    await p.setStringList('comp_slots', next.compSlots);
  }

  /// Forget the paired watch (no more auto-reconnect to it).
  Future<void> forgetDevice() async {
    final p = await SharedPreferences.getInstance();
    await p.remove('last_device_id');
    await p.remove('last_device_name');
    state = AppSettings(
      loaded: true,
      autoConnect: state.autoConnect,
      backgroundLink: state.backgroundLink,
      forwardNotifications: state.forwardNotifications,
      notifPackages: state.notifPackages,
      callAlerts: state.callAlerts,
      mediaToWatch: state.mediaToWatch,
      weatherAuto: state.weatherAuto,
      fahrenheit: state.fahrenheit,
      onboarded: state.onboarded,
      calendarToWatch: state.calendarToWatch,
      reminderMinutes: state.reminderMinutes,
      navigationToWatch: state.navigationToWatch,
      wifiTransfers: state.wifiTransfers,
      autoProcess: state.autoProcess,
      dndSync: state.dndSync,
      bedtimeOn: state.bedtimeOn,
      bedFrom: state.bedFrom,
      bedTo: state.bedTo,
      quickReplies: state.quickReplies,
      compSlots: state.compSlots,
    );
  }
}

final settingsProvider = StateNotifierProvider<SettingsNotifier, AppSettings>((ref) => SettingsNotifier());
