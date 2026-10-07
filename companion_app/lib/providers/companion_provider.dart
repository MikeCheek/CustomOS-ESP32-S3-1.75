import 'dart:async';
import 'dart:convert';
import 'dart:collection';
import 'dart:io';
import 'dart:typed_data';
import 'package:path_provider/path_provider.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/ble_protocol.dart';
import '../services/ble_service.dart';
import '../services/native_service.dart';
import '../services/weather_service.dart';
import '../services/whisper_service.dart';
import 'ble_provider.dart';
import 'notes_provider.dart';
import 'settings_provider.dart';
import 'knowledge_provider.dart';

/// One calendar event instance, in local time.
class CalendarItem {
  final int id;
  final String title;
  final DateTime start, end;
  final bool allDay;
  final String location;
  final int color; // 0xRRGGBB

  const CalendarItem({
    required this.id,
    required this.title,
    required this.start,
    required this.end,
    required this.allDay,
    required this.location,
    required this.color,
  });

  factory CalendarItem.fromNative(Map<String, dynamic> m) {
    final allDay = m['allDay'] == true;
    final b = (m['begin'] as num?)?.toInt() ?? 0;
    final e = (m['end'] as num?)?.toInt() ?? b;
    DateTime conv(int ms) {
      if (!allDay) return DateTime.fromMillisecondsSinceEpoch(ms);
      // All-day events are stored at UTC midnight: keep the date, local.
      final u = DateTime.fromMillisecondsSinceEpoch(ms, isUtc: true);
      return DateTime(u.year, u.month, u.day);
    }

    return CalendarItem(
      id: (m['id'] as num?)?.toInt() ?? 0,
      title: (m['title'] as String? ?? '').trim(),
      start: conv(b),
      end: conv(e),
      allDay: allDay,
      location: (m['location'] as String? ?? '').trim(),
      color: (m['color'] as num?)?.toInt() ?? 0,
    );
  }

  /// Local date/time written as if it were UTC: what the watch's RTC counts in.
  static int localEpoch(DateTime d) =>
      DateTime.utc(d.year, d.month, d.day, d.hour, d.minute, d.second).millisecondsSinceEpoch ~/ 1000;
}

/// What the phone link knows right now.
class CompanionState {
  final bool linkActive;            // the watch speaks the phone-link protocol
  final int watchSteps;             // -1 = unknown
  final int watchBattery;           // -1 = unknown
  final bool watchCharging;
  final DateTime? watchReportAt;
  final Map<String, int> stepsHistory; // yyyy-mm-dd -> steps
  final int phoneBattery;
  final bool phoneCharging;
  final Map<String, dynamic>? media;   // last now-playing from the phone
  final WeatherSnapshot? weather;
  final String? weatherError;
  final bool weatherLoading;
  final bool findingPhone;
  final int forwardedToday;
  // From the watch's hello / diagnostics
  final String? watchFirmware;
  final String? watchResetReason;
  final int watchCrashId;           // 0 = no crash report on the watch
  final int seenCrashId;            // last crash report the user looked at
  final String? crashReport;        // fetched with requestDiagnostics()
  final String? watchSystem;
  final DateTime? diagnosticsAt;
  final String? dictation;          // voice reply being transcribed, for the UI
  final List<CalendarItem> agenda;  // next ~2 days, as sent to the watch
  final bool calendarAllowed;
  final Map<String, dynamic>? navigation; // current turn-by-turn step, null = not navigating

  const CompanionState({
    this.linkActive = false,
    this.watchSteps = -1,
    this.watchBattery = -1,
    this.watchCharging = false,
    this.watchReportAt,
    this.stepsHistory = const {},
    this.phoneBattery = -1,
    this.phoneCharging = false,
    this.media,
    this.weather,
    this.weatherError,
    this.weatherLoading = false,
    this.findingPhone = false,
    this.forwardedToday = 0,
    this.watchFirmware,
    this.watchResetReason,
    this.watchCrashId = 0,
    this.seenCrashId = 0,
    this.crashReport,
    this.watchSystem,
    this.diagnosticsAt,
    this.dictation,
    this.agenda = const [],
    this.calendarAllowed = false,
    this.navigation,
  });

  static const _keep = Object();

  CompanionState copyWith({
    bool? linkActive,
    int? watchSteps,
    int? watchBattery,
    bool? watchCharging,
    DateTime? watchReportAt,
    Map<String, int>? stepsHistory,
    int? phoneBattery,
    bool? phoneCharging,
    Object? media = _keep,
    WeatherSnapshot? weather,
    Object? weatherError = _keep,
    bool? weatherLoading,
    bool? findingPhone,
    int? forwardedToday,
    String? watchFirmware,
    String? watchResetReason,
    int? watchCrashId,
    int? seenCrashId,
    String? crashReport,
    String? watchSystem,
    DateTime? diagnosticsAt,
    Object? dictation = _keep,
    List<CalendarItem>? agenda,
    bool? calendarAllowed,
    Object? navigation = _keep,
  }) {
    return CompanionState(
      linkActive: linkActive ?? this.linkActive,
      watchSteps: watchSteps ?? this.watchSteps,
      watchBattery: watchBattery ?? this.watchBattery,
      watchCharging: watchCharging ?? this.watchCharging,
      watchReportAt: watchReportAt ?? this.watchReportAt,
      stepsHistory: stepsHistory ?? this.stepsHistory,
      phoneBattery: phoneBattery ?? this.phoneBattery,
      phoneCharging: phoneCharging ?? this.phoneCharging,
      media: identical(media, _keep) ? this.media : media as Map<String, dynamic>?,
      weather: weather ?? this.weather,
      weatherError: identical(weatherError, _keep) ? this.weatherError : weatherError as String?,
      weatherLoading: weatherLoading ?? this.weatherLoading,
      findingPhone: findingPhone ?? this.findingPhone,
      forwardedToday: forwardedToday ?? this.forwardedToday,
      watchFirmware: watchFirmware ?? this.watchFirmware,
      watchResetReason: watchResetReason ?? this.watchResetReason,
      watchCrashId: watchCrashId ?? this.watchCrashId,
      seenCrashId: seenCrashId ?? this.seenCrashId,
      crashReport: crashReport ?? this.crashReport,
      watchSystem: watchSystem ?? this.watchSystem,
      diagnosticsAt: diagnosticsAt ?? this.diagnosticsAt,
      dictation: identical(dictation, _keep) ? this.dictation : dictation as String?,
      agenda: agenda ?? this.agenda,
      calendarAllowed: calendarAllowed ?? this.calendarAllowed,
      navigation: identical(navigation, _keep) ? this.navigation : navigation as Map<String, dynamic>?,
    );
  }

  /// The next event that hasn't ended (timed events first).
  CalendarItem? get nextEvent {
    final now = DateTime.now();
    for (final e in agenda) {
      if (!e.allDay && e.end.isAfter(now)) return e;
    }
    for (final e in agenda) {
      if (e.allDay && e.end.isAfter(now)) return e;
    }
    return null;
  }

  /// The watch has a crash report the user hasn't opened yet.
  bool get hasNewCrash => watchCrashId != 0 && watchCrashId != seenCrashId;

  bool get mediaPlaying => media?['playing'] == true;
  String get mediaTitle => (media?['title'] as String?) ?? '';
  String get mediaArtist => (media?['artist'] as String?) ?? '';
}

/// The phone side of the companion link. Runs for the whole life of the
/// app process (also in the background, see LinkService.kt):
///  - phone notifications and calls -> watch, watch actions -> phone
///  - now playing -> watch, watch transport/volume buttons -> phone
///  - phone battery -> watch, watch steps/battery -> phone
///  - find my phone, weather for the watch face
class CompanionNotifier extends StateNotifier<CompanionState> {
  final Ref _ref;
  BleService get _ble => _ref.read(bleServiceProvider);
  NativeService get _native => _ref.read(nativeServiceProvider);
  AppSettings get _settings => _ref.read(settingsProvider);

  final List<StreamSubscription> _subs = [];
  Timer? _weatherTimer;
  Timer? _calendarTimer;
  int _stepGoal = 8000;
  String _lastWidget = '';
  // Maneuver icons the watch already has (it caches 12; we track 10).
  final LinkedHashSet<int> _navIcons = LinkedHashSet<int>();
  String _lastNotif = '';
  DateTime _lastNotifAt = DateTime.fromMillisecondsSinceEpoch(0);
  final Map<int, String> _callNames = {};

  CompanionNotifier(this._ref) : super(const CompanionState()) {
    _loadHistory();
    _subs.add(_ble.connectionStream.listen(_onConnection));
    _subs.add(_ble.linkEvents.listen(_onWatchEvent));
    _subs.add(_native.notificationStream.listen(_onPhoneNotification, onError: (_) {}));
    _subs.add(_native.mediaStream.listen(_onPhoneMedia, onError: (_) {}));
    _subs.add(_native.batteryStream.listen(_onPhoneBattery, onError: (_) {}));
    _subs.add(_native.navigationStream.listen(_onNavigation, onError: (_) {}));
    _subs.add(_native.calendarChanges.listen((_) => refreshCalendar(), onError: (_) {}));
    _subs.add(_native.widgetActions.listen(_onWidgetAction, onError: (_) {}));
    _subs.add(_native.dndChanges.listen(_onPhoneDnd, onError: (_) {}));
    _ref.listen(knowledgeProvider, (_, __) => _todosChanged());
    _ref.listen(doneActionsProvider, (_, __) => _todosChanged());
    _subs.add(_ble.batteryStream.listen((_) => _pushWidget()));
    _calendarTimer = Timer.periodic(const Duration(minutes: 15), (_) => refreshCalendar());
    Future.delayed(const Duration(seconds: 2), refreshCalendar);
    // A widget left at "Connected" by a process that died: correct it now.
    Future.delayed(const Duration(milliseconds: 500), _pushWidget);
    _ref.listen<AppSettings>(settingsProvider, (prev, next) => _applySettings(next), fireImmediately: true);
    _weatherTimer = Timer.periodic(const Duration(minutes: 30), (_) {
      if (_ble.isConnected && _settings.weatherAuto) refreshWeather();
    });
  }

  AppSettings? _lastSent;

  void _applySettings(AppSettings s) {
    if (!s.loaded) return;
    final prev = _lastSent;
    _lastSent = s;
    if (_ble.hasPhoneLink && prev != null) {
      if (prev.bedtimeOn != s.bedtimeOn || prev.bedFrom != s.bedFrom || prev.bedTo != s.bedTo) _sendBedtime();
      if (prev.quickReplies.join('\u0001') != s.quickReplies.join('\u0001')) _sendQuickReplies();
      if (prev.compSlots.join(',') != s.compSlots.join(',')) _sendComplications();
    }
    _native.setAutoForward(s.forwardNotifications, packages: s.notifPackages);
    _native.setCallAlerts(s.callAlerts);
    _native.setNavigation(s.navigationToWatch);
    if (!s.navigationToWatch && state.navigation != null && _ble.hasPhoneLink) {
      _ble.sendLink({'t': 'nav', 'st': 0});
    }
  }

  // ---- connection ----------------------------------------------------------

  Future<void> _onConnection(bool connected) async {
    if (!mounted) return;
    if (!connected) {
      state = state.copyWith(linkActive: false, findingPhone: false);
      _pushWidget();
      return;
    }
    _navIcons.clear(); // the watch may have restarted
    // Give the watch a moment to finish its own setup, then introduce us.
    await Future.delayed(const Duration(milliseconds: 600));
    if (!_ble.isConnected) return;
    await _ble.sendLink({'t': 'hi'});
    await _sendState();
    final w = state.weather;
    if (_settings.weatherAuto &&
        (w == null || DateTime.now().difference(w.fetchedAt) > const Duration(minutes: 30))) {
      await refreshWeather();
    } else if (w != null) {
      await _ble.sendWeather(w.toWatch());
    }
    await refreshCalendar();
    await _sendBedtime();
    await _sendQuickReplies();
    await _sendComplications();
    _lastTodo = '';
    await _sendTodos();
    if (_settings.dndSync) {
      final d = await _native.getDnd();
      await _ble.sendLink({'t': 'dnd', 'on': d.on ? 1 : 0});
    }
    final nav = state.navigation;
    if (nav != null && _settings.navigationToWatch) {
      await _sendNav(nav);
    } else {
      await _ble.sendLink({'t': 'nav', 'st': 0}); // a trip that ended while we were apart
    }
    _pushWidget();
  }

  // ---- Do Not Disturb + quick replies ------------------------------------------------

  bool _dndFromWatch = false;   // ignore the echo of a change the watch asked for

  Future<void> _onPhoneDnd(bool on) async {
    if (_dndFromWatch) return;
    if (!_settings.dndSync || !_ble.hasPhoneLink) return;
    await _ble.sendLink({'t': 'dnd', 'on': on ? 1 : 0});
  }

  Future<void> _onWatchDnd(bool on) async {
    if (!_settings.dndSync) return;
    _dndFromWatch = true;
    try {
      await _native.setDnd(on);   // false (ignored) without DND access
    } finally {
      Future.delayed(const Duration(seconds: 2), () => _dndFromWatch = false);
    }
  }

  // ---- watchface complications -----------------------------------------------------

  Future<void> _sendComplications() async {
    if (!_ble.hasPhoneLink) return;
    await _ble.sendLink({'t': 'comp', 's': _settings.compSlots.take(3).toList()});
  }

  String _lastTodo = '';
  Timer? _todoDebounce;

  /// Open to-dos from the memos, for the watch's "To-dos" complication.
  Future<void> _sendTodos() async {
    if (!_ble.hasPhoneLink) return;
    final done = _ref.read(doneActionsProvider);
    final open = _ref.read(knowledgeProvider).actions().where((a) => !done.contains(a.key)).toList();
    final top = open.isEmpty ? '' : open.first.text;
    final key = '${open.length}|$top';
    if (key == _lastTodo) return;
    _lastTodo = key;
    await _ble.sendLink({'t': 'todo', 'n': open.length, 'top': top.length > 36 ? top.substring(0, 36) : top});
  }

  void _todosChanged() {
    _todoDebounce?.cancel();
    _todoDebounce = Timer(const Duration(seconds: 2), _sendTodos);
  }

  Future<void> _sendBedtime() async {
    if (!_ble.hasPhoneLink) return;
    final s = _settings;
    await _ble.sendLink({'t': 'dnd', 'bed': s.bedtimeOn ? 1 : 0, 'from': s.bedFrom, 'to': s.bedTo});
  }

  Future<void> _sendQuickReplies() async {
    if (!_ble.hasPhoneLink) return;
    // Empty list = back to the watch's built-in presets.
    await _ble.sendLink({'t': 'qr', 'l': _settings.quickReplies.take(10).map((r) => r.length > 38 ? r.substring(0, 38) : r).toList()});
  }

  /// Phone battery + now playing, e.g. after (re)connecting or on request.
  Future<void> _sendState() async {
    if (state.phoneBattery >= 0) {
      await _ble.sendLink({'t': 'bat', 'l': state.phoneBattery, 'c': state.phoneCharging ? 1 : 0});
    }
    final m = state.media;
    if (m != null && _settings.mediaToWatch) await _sendMedia(m);
  }

  // ---- watch -> phone -------------------------------------------------------------

  Future<void> _onWatchEvent(Map<String, dynamic> e) async {
    final ev = e['e'] as String? ?? '';
    switch (ev) {
      case 'hi':
      case 'st':
        _onWatchReport(e);
        if (ev == 'hi') await _sendState();
        break;
      case 'req':
        await _sendState();
        break;
      case 'crash':
        if (!mounted) return;
        state = state.copyWith(
          crashReport: e['txt'] as String? ?? '',
          watchSystem: e['sys'] as String? ?? '',
          watchCrashId: (e['id'] as num?)?.toInt() ?? 0,
          diagnosticsAt: DateTime.now(),
        );
        break;
      case 'dict':
        await _transcribeReply((e['id'] as num?)?.toInt() ?? 0, (e['n'] as num?)?.toInt() ?? 0, e['f'] as String? ?? '');
        break;
      case 'med':
        await _native.mediaCommand(e['a'] as String? ?? '');
        break;
      case 'dnd':
        await _onWatchDnd((e['on'] ?? 0) == 1);
        break;
      case 'find':
        final on = (e['on'] ?? 0) == 1;
        await _native.findPhone(on);
        if (mounted) state = state.copyWith(findingPhone: on);
        if (on) {
          // The phone stops by itself after a minute; tell the watch then.
          Future.delayed(const Duration(seconds: 61), () async {
            if (mounted && state.findingPhone) {
              state = state.copyWith(findingPhone: false);
              await _ble.sendLink({'t': 'find', 'on': 0});
            }
          });
        }
        break;
      case 'call':
        final id = (e['id'] as num?)?.toInt() ?? 0;
        await _native.notificationAction(id, e['a'] == 'answer' ? 'answer' : 'decline');
        break;
      case 'ntf':
        final id = (e['id'] as num?)?.toInt() ?? 0;
        if (e['a'] == 'reply') {
          await _native.notificationAction(id, 'reply', text: e['tx'] as String? ?? '');
        } else {
          await _native.notificationAction(id, 'dismiss');
        }
        break;
    }
  }

  void _onWatchReport(Map<String, dynamic> e) {
    if (!mounted) return;
    final steps = (e['st'] as num?)?.toInt() ?? -1;
    final hist = Map<String, int>.from(state.stepsHistory);
    if (steps >= 0) {
      final key = _dayKey(DateTime.now());
      if ((hist[key] ?? 0) < steps) hist[key] = steps;
      // keep two weeks
      final keys = hist.keys.toList()..sort();
      while (keys.length > 14) {
        hist.remove(keys.removeAt(0));
      }
      _saveHistory(hist);
    }
    final fw = e['fw'] as String?;
    if (fw != null) _ble.firmwareVersion = fw;
    state = state.copyWith(
      linkActive: true,
      watchFirmware: fw,
      watchResetReason: e['rr'] as String?,
      watchCrashId: e.containsKey('cr') ? (e['cr'] as num?)?.toInt() ?? 0 : null,
      watchSteps: steps >= 0 ? steps : null,
      watchBattery: (e['bat'] as num?)?.toInt() ?? state.watchBattery,
      watchCharging: (e['chg'] ?? 0) == 1,
      watchReportAt: DateTime.now(),
      stepsHistory: hist,
    );
    _pushWidget();
  }

  /// Ask the watch for fresh steps/battery.
  Future<void> requestWatchReport() => _ble.sendLink({'t': 'req'});

  // ---- diagnostics ----------------------------------------------------------------

  /// Asks the watch for its crash report and status ([clear] = forget the
  /// crash report afterwards).
  Future<void> requestDiagnostics({bool clear = false}) async {
    if (clear && mounted) state = state.copyWith(crashReport: '', watchCrashId: 0);
    await _ble.sendLink(clear ? {'t': 'crash', 'clr': 1} : {'t': 'crash'});
  }

  Future<void> markCrashSeen() async {
    if (!mounted) return;
    state = state.copyWith(seenCrashId: state.watchCrashId);
    final p = await SharedPreferences.getInstance();
    await p.setInt('seen_crash_id', state.watchCrashId);
  }

  // ---- calendar ---------------------------------------------------------------------

  bool _calBusy = false, _calAgain = false;

  /// Reads the next two days from the phone's calendar and sends them to the
  /// watch (agenda screen + reminders there). One at a time: two transfers
  /// interleaving would mix their batches on the watch.
  Future<void> refreshCalendar() async {
    if (_calBusy) {
      _calAgain = true;
      return;
    }
    _calBusy = true;
    try {
      do {
        _calAgain = false;
        await _refreshCalendarOnce();
      } while (_calAgain && mounted);
    } finally {
      _calBusy = false;
    }
  }

  Future<void> _refreshCalendarOnce() async {
    if (!mounted) return;
    bool allowed;
    try {
      allowed = await Permission.calendarFullAccess.isGranted;
    } catch (_) {
      allowed = false;
    }
    if (!mounted) return;
    if (!allowed) {
      if (state.calendarAllowed) state = state.copyWith(calendarAllowed: false, agenda: const []);
      await _clearWatchAgenda();
      return;
    }
    await _native.watchCalendar(); // no-op once registered; covers a grant made in system settings
    List<CalendarItem> items;
    try {
      final raw = await _native.getEvents(hours: 48);
      final now = DateTime.now();
      items = raw.map(CalendarItem.fromNative).where((e) => e.end.isAfter(now)).take(24).toList();
    } catch (e) {
      debugPrintSafe('[calendar] read failed: $e');
      return;
    }
    if (!mounted) return;
    state = state.copyWith(agenda: items, calendarAllowed: true);
    _pushWidget();
    if (!_settings.calendarToWatch) {
      await _clearWatchAgenda();
    } else if (_ble.hasPhoneLink) {
      await _sendAgenda(items);
    }
  }

  /// Calendar off / not allowed: the watch forgets its agenda and reminders.
  Future<void> _clearWatchAgenda() async {
    if (_ble.hasPhoneLink) {
      await _ble.sendLink({'t': 'cal', 'r': 1, 'rm': 0, 'ev': <Map<String, dynamic>>[], 'done': 1, 'n': 0});
    }
  }

  /// Call after the calendar permission was granted.
  Future<void> calendarPermissionChanged() async {
    await _native.watchCalendar();
    await refreshCalendar();
  }

  Future<void> _sendAgenda(List<CalendarItem> items) async {
    final evs = [
      for (final e in items)
        {
          'id': e.id & 0x7fffffff,
          'ti': _clip(e.title, 44),
          's': CalendarItem.localEpoch(e.start),
          'e': CalendarItem.localEpoch(e.end),
          'ad': e.allDay ? 1 : 0,
          if (e.location.isNotEmpty) 'lo': _clip(e.location, 36),
          'c': e.color,
        }
    ];
    // Several messages if needed (the watch takes up to 1 KB per message).
    var batch = <Map<String, dynamic>>[];
    var first = true;
    Future<void> flush({bool done = false}) async {
      await _ble.sendLink({
        't': 'cal',
        if (first) 'r': 1,
        if (first) 'rm': _settings.reminderMinutes,
        'ev': batch,
        if (done) 'done': 1,
        if (done) 'n': evs.length, // the watch keeps its old agenda if a batch got lost
      });
      first = false;
      batch = [];
    }

    for (final e in evs) {
      batch.add(e);
      final size = utf8.encode(jsonEncode({'t': 'cal', 'r': 1, 'rm': 10, 'ev': batch, 'done': 1, 'n': 24})).length;
      if (size > 850 && batch.length > 1) {
        batch.removeLast();
        await flush();
        batch.add(e);
      }
    }
    await flush(done: true);
  }

  // ---- navigation ---------------------------------------------------------------------

  Future<void> _onNavigation(Map<String, dynamic> n) async {
    if (!mounted) return;
    if (n['type'] == 'navEnd') {
      state = state.copyWith(navigation: null);
      if (_ble.hasPhoneLink) await _ble.sendLink({'t': 'nav', 'st': 0});
      return;
    }
    state = state.copyWith(navigation: n);
    if (_settings.navigationToWatch && _ble.hasPhoneLink) await _sendNav(n);
  }

  Future<void> _sendNav(Map<String, dynamic> n) async {
    var hash = (n['iconHash'] as num?)?.toInt() ?? 0;
    final icon = n['icon'];
    if (hash != 0 && icon is Uint8List && icon.length == 48 * 48 ~/ 8) {
      if (!_navIcons.contains(hash)) {
        await _ble.sendLink({'t': 'navi', 'h': hash, 'w': 48, 'b': base64Encode(icon)});
        _navIcons.add(hash);
        while (_navIcons.length > 10) {
          _navIcons.remove(_navIcons.first);
        }
      }
    } else {
      hash = 0;
    }
    await _ble.sendLink({
      't': 'nav',
      'st': 1,
      'd': _clip(n['dist'] as String? ?? '', 20),
      'i': _clip(n['instr'] as String? ?? '', 150),
      'x': _clip(n['extra'] as String? ?? '', 60),
      'h': hash,
    });
  }

  // ---- home-screen widget ---------------------------------------------------------------

  Future<void> _onWidgetAction(String action) async {
    if (action == 'find' && _ble.isConnected) await findWatch();
  }

  /// Pushes the numbers the home-screen widget shows (only when they changed).
  Future<void> _pushWidget() async {
    if (!mounted) return;
    final ble = _ref.read(bleProvider);
    final next = state.nextEvent;
    String nextLabel = '';
    if (next != null) {
      final now = DateTime.now();
      final today = DateTime(now.year, now.month, now.day);
      final day = DateTime(next.start.year, next.start.month, next.start.day);
      final hm = '${next.start.hour.toString().padLeft(2, '0')}:${next.start.minute.toString().padLeft(2, '0')}';
      final tomorrow = DateTime(today.year, today.month, today.day + 1);
      final dayName = !day.isAfter(today)
          ? 'Today'
          : day == tomorrow
              ? 'Tomorrow'
              : const ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'][day.weekday - 1];
      final when = next.allDay
          ? dayName
          : (next.start.isBefore(now)
              ? 'Now'
              : dayName == 'Today'
                  ? hm
                  : '$dayName $hm');
      nextLabel = '$when · ${next.title}';
    }
    final data = <String, dynamic>{
      'name': ble.deviceName ?? _settings.lastDeviceName ?? 'AmoledWatch',
      'connected': ble.isConnected,
      'charging': state.watchCharging,
      'battery': ble.batteryLevel >= 0 ? ble.batteryLevel : (ble.isConnected ? state.watchBattery : -1),
      'steps': state.watchSteps,
      'goal': _stepGoal,
      'next': nextLabel,
    };
    final key = jsonEncode(data);
    if (key == _lastWidget) return;
    _lastWidget = key;
    await _native.updateWidget(data);
  }

  /// The activity screen changed the daily goal.
  void setStepGoal(int goal) {
    _stepGoal = goal;
    _pushWidget();
  }

  static void debugPrintSafe(String s) {
    // ignore: avoid_print
    print(s);
  }

  // ---- voice replies --------------------------------------------------------------

  /// The watch recorded a voice reply: fetch it, transcribe it here and send
  /// the text back for the user to confirm on the watch.
  Future<void> _transcribeReply(int id, int seq, String file) async {
    Future<void> fail(String why) => _ble.sendLink({'t': 'dict', 'id': id, 'n': seq, 'err': why});
    if (file.isEmpty) return fail('Nothing recorded');
    final whisper = WhisperService.instance;
    File? tmp;
    try {
      await whisper.loadPrefs();
      if (!await whisper.isDownloaded()) {
        return fail('Download the speech model in the companion app first');
      }
      if (mounted) state = state.copyWith(dictation: 'Receiving voice reply');
      // The watch waits 3 minutes in total; leave time for transcription.
      final bytes = await _ref.read(notesProvider.notifier).fetchTemp(file, timeout: const Duration(seconds: 100));
      final dir = await getTemporaryDirectory();
      tmp = File('${dir.path}/voice_reply_$id.wav');
      await tmp.writeAsBytes(bytes);
      if (mounted) state = state.copyWith(dictation: 'Transcribing voice reply');
      final text = (await whisper.transcribe(tmp.path)).replaceAll(RegExp(r'\s+'), ' ').trim();
      // whisper marks silence like "[BLANK_AUDIO]" / "(music)"
      final clean = text.replaceAll(RegExp(r'[\[(][^\])]*[\])]'), '').trim();
      if (clean.isEmpty) return fail('No words recognized - try again');
      await _ble.sendLink({'t': 'dict', 'id': id, 'n': seq, 'tx': _clip(clean, 300)});
    } catch (e) {
      await fail(_clip(e.toString().replaceFirst('Exception: ', ''), 90));
    } finally {
      if (mounted) state = state.copyWith(dictation: null);
      try {
        if (tmp != null && await tmp.exists()) await tmp.delete();
      } catch (_) {}
    }
  }

  // ---- phone -> watch -----------------------------------------------------------------

  Future<void> _onPhoneNotification(Map<String, dynamic> n) async {
    if (!_ble.isConnected) return;
    final type = n['type'] as String? ?? '';
    final id = (n['id'] as num?)?.toInt() ?? 0;
    final isCall = n['isCall'] == true;

    if (isCall) {
      if (!_settings.callAlerts || !_ble.hasPhoneLink) return;
      if (type == 'posted') {
        final name = (n['title'] as String? ?? '').trim();
        _callNames[id] = name;
        await _ble.sendLink({
          't': 'call',
          'st': n['callRinging'] == true ? 'ring' : 'active',
          'id': id,
          'n': _clip(name, 40),
        });
      } else if (type == 'removed') {
        _callNames.remove(id);
        await _ble.sendLink({'t': 'call', 'st': 'end', 'id': id});
      }
      return;
    }

    if (type == 'removed') {
      if (_ble.hasPhoneLink) await _ble.sendLink({'t': 'nrm', 'id': id});
      return;
    }
    if (!_settings.forwardNotifications) return;
    final app = n['app'] as String? ?? '';
    final title = n['title'] as String? ?? '';
    final text = n['text'] as String? ?? '';
    final key = '$app|$title|$text';
    final now = DateTime.now();
    if (key == _lastNotif && now.difference(_lastNotifAt) < const Duration(seconds: 10)) return;
    _lastNotif = key;
    _lastNotifAt = now;

    if (_ble.hasPhoneLink) {
      await _ble.sendLink({
        't': 'ntf',
        'id': id,
        'ap': _clip(app, 28),
        'ti': _clip(title, 44),
        'tx': _clip(text, 240),
        'rp': n['canReply'] == true ? 1 : 0,
      });
    } else {
      // Older watch firmware: plain text only.
      final body = title.isNotEmpty && text.isNotEmpty ? '$title: $text' : (title.isNotEmpty ? title : text);
      await _ble.sendNotification(app.isNotEmpty ? '[$app] $body' : body);
    }
    if (mounted) state = state.copyWith(forwardedToday: state.forwardedToday + 1);
  }

  Future<void> _onPhoneMedia(Map<String, dynamic> m) async {
    if (!mounted) return;
    // Nothing playing (or no notification access yet): nothing to mirror.
    final empty = (m['title'] as String? ?? '').isEmpty && m['playing'] != true;
    state = state.copyWith(media: empty ? null : m);
    if (empty) return;
    if (_ble.isConnected && _settings.mediaToWatch) await _sendMedia(m);
  }

  Future<void> _sendMedia(Map<String, dynamic> m) async {
    if (_ble.hasPhoneLink) {
      await _ble.sendLink({
        't': 'med',
        'ti': _clip(m['title'] as String? ?? '', 60),
        'ar': _clip(m['artist'] as String? ?? '', 44),
        'pl': m['playing'] == true ? 1 : 0,
        'po': (m['position'] as num?)?.toInt() ?? 0,
        'du': (m['duration'] as num?)?.toInt() ?? 0,
        'v': (m['volume'] as num?)?.toInt() ?? 0,
        'vm': (m['volumeMax'] as num?)?.toInt() ?? 15,
      });
    } else {
      await _ble.sendMediaState(MediaState(
        title: m['title'] as String? ?? '',
        artist: m['artist'] as String? ?? '',
        album: m['album'] as String? ?? '',
        playing: m['playing'] == true,
        position: (m['position'] as num?)?.toInt() ?? 0,
        duration: (m['duration'] as num?)?.toInt() ?? 0,
      ));
    }
  }

  Future<void> _onPhoneBattery(Map<String, dynamic> b) async {
    if (!mounted) return;
    final level = (b['level'] as num?)?.toInt() ?? -1;
    final charging = b['charging'] == true;
    state = state.copyWith(phoneBattery: level, phoneCharging: charging);
    if (_ble.hasPhoneLink) await _ble.sendLink({'t': 'bat', 'l': level, 'c': charging ? 1 : 0});
  }

  // ---- weather ---------------------------------------------------------------------------

  /// Fetches weather for the phone's location and sends it to the watch.
  /// [interactive] = may show the location permission prompt.
  Future<void> refreshWeather({bool interactive = false}) async {
    if (!mounted || state.weatherLoading) return;
    state = state.copyWith(weatherLoading: true, weatherError: null);
    try {
      if (interactive) {
        final st = await Permission.locationWhenInUse.request();
        if (!st.isGranted) throw Exception('Location permission is needed for local weather');
      }
      final loc = await _native.lastLocation();
      if (loc == null) {
        throw Exception(interactive
            ? 'No location yet - open Maps once, then retry'
            : 'Location unavailable');
      }
      final w = await WeatherService.fetch(loc['lat']!, loc['lon']!);
      if (!mounted) return;
      state = state.copyWith(weather: w, weatherLoading: false);
      if (_ble.isConnected) await _ble.sendWeather(w.toWatch());
    } catch (e) {
      if (!mounted) return;
      state = state.copyWith(weatherLoading: false, weatherError: e.toString().replaceFirst('Exception: ', ''));
    }
  }

  // ---- actions from the app UI -------------------------------------------------------

  Future<void> findWatch() => _ble.findWatch();

  Future<void> stopFindingPhone() async {
    await _native.findPhone(false);
    if (mounted) state = state.copyWith(findingPhone: false);
    await _ble.sendLink({'t': 'find', 'on': 0});
  }

  // ---- steps history ------------------------------------------------------------------

  static String _dayKey(DateTime d) =>
      '${d.year}-${d.month.toString().padLeft(2, '0')}-${d.day.toString().padLeft(2, '0')}';

  Future<void> _loadHistory() async {
    try {
      final p = await SharedPreferences.getInstance();
      final raw = p.getString('steps_history');
      if (!mounted) return;
      _stepGoal = p.getInt('step_goal') ?? 8000;
      state = state.copyWith(seenCrashId: p.getInt('seen_crash_id') ?? 0);
      if (raw == null) return;
      final m = (jsonDecode(raw) as Map<String, dynamic>).map((k, v) => MapEntry(k, (v as num).toInt()));
      state = state.copyWith(stepsHistory: m);
    } catch (_) {}
  }

  Future<void> _saveHistory(Map<String, int> h) async {
    final p = await SharedPreferences.getInstance();
    await p.setString('steps_history', jsonEncode(h));
  }

  static String _clip(String s, int n) => s.length <= n ? s : '${s.substring(0, n - 1)}…';

  @override
  void dispose() {
    for (final s in _subs) {
      s.cancel();
    }
    _weatherTimer?.cancel();
    _calendarTimer?.cancel();
    super.dispose();
  }
}

final companionProvider =
    StateNotifierProvider<CompanionNotifier, CompanionState>((ref) => CompanionNotifier(ref));
