import 'dart:async';
import 'dart:typed_data';
import 'package:flutter/services.dart';

/// Dart side of android/.../PhoneBridge.kt.
class NativeService {
  static const _method = MethodChannel('com.amoledwatch.native');
  static const _notifStream = EventChannel('com.amoledwatch.native/notifications');
  static const _mediaStream = EventChannel('com.amoledwatch.native/media');
  static const _batteryStream = EventChannel('com.amoledwatch.native/battery');
  static const _navStream = EventChannel('com.amoledwatch.native/navigation');
  static const _calendarStream = EventChannel('com.amoledwatch.native/calendar');
  static const _widgetStream = EventChannel('com.amoledwatch.native/widget');
  static const _dndStream = EventChannel('com.amoledwatch.native/dnd');
  Stream<bool>? _dnd;

  Stream<Map<String, dynamic>>? _notifs;
  Stream<Map<String, dynamic>>? _media;
  Stream<Map<String, dynamic>>? _battery;
  Stream<Map<String, dynamic>>? _nav;
  Stream<String>? _calendar;
  Stream<String>? _widget;

  static Map<String, dynamic> _map(dynamic event) => Map<String, dynamic>.from(event as Map);

  Future<T?> _call<T>(String method, [Map<String, dynamic>? args]) async {
    try {
      return await _method.invokeMethod<T>(method, args);
    } catch (e) {
      return null;
    }
  }

  // ---- notifications ---------------------------------------------------------

  Future<bool> isNotificationListenerEnabled() async =>
      (await _call<bool>('isNotificationListenerEnabled')) ?? false;

  Future<void> openNotificationListenerSettings() => _call('openNotificationListenerSettings');

  /// [packages] empty = every app.
  Future<void> setAutoForward(bool enabled, {List<String> packages = const []}) =>
      _call('setAutoForward', {'enabled': enabled, 'packages': packages});

  Future<void> setCallAlerts(bool enabled) => _call('setCallAlerts', {'enabled': enabled});

  /// Events: {type: posted, id, package, app, title, text, isCall, ongoing, canReply}
  ///         {type: removed, id, isCall}
  Stream<Map<String, dynamic>> get notificationStream =>
      _notifs ??= _notifStream.receiveBroadcastStream().map(_map).asBroadcastStream();

  /// action: dismiss | reply | answer | decline
  Future<bool> notificationAction(int id, String action, {String? text}) async =>
      (await _call<bool>('notifAction', {'id': id, 'action': action, 'text': text})) ?? false;

  // ---- media -----------------------------------------------------------------------

  /// {title, artist, album, playing, position, duration, volume, volumeMax, app}
  Stream<Map<String, dynamic>> get mediaStream =>
      _media ??= _mediaStream.receiveBroadcastStream().map(_map).asBroadcastStream();

  /// toggle | play | pause | next | prev | volUp | volDown
  Future<bool> mediaCommand(String cmd) async => (await _call<bool>('mediaCommand', {'cmd': cmd})) ?? false;

  // Kept for compatibility with older callers.
  Future<void> startMediaListening() async {}
  Future<void> stopMediaListening() async {}

  // ---- phone ---------------------------------------------------------------------------

  /// {level, charging}
  Stream<Map<String, dynamic>> get batteryStream =>
      _battery ??= _batteryStream.receiveBroadcastStream().map(_map).asBroadcastStream();

  Future<void> findPhone(bool on) => _call('findPhone', {'on': on});

  /// {lat, lon} from the last known location, or null.
  Future<Map<String, double>?> lastLocation() async {
    final m = await _call<Map<dynamic, dynamic>>('getLocation');
    if (m == null) return null;
    final lat = m['lat'], lon = m['lon'];
    if (lat is! num || lon is! num) return null;
    return {'lat': lat.toDouble(), 'lon': lon.toDouble()};
  }

  /// Keeps the app (and the watch link) running in the background.
  Future<bool> startLinkService({String title = 'AmoledWatch connected', String text = 'Notifications and controls are linked'}) async =>
      (await _call<bool>('startLinkService', {'title': title, 'text': text})) ?? false;

  Future<void> stopLinkService() => _call('stopLinkService');

  // ---- contacts ------------------------------------------------------------------------

  /// Phone contacts with a number: [{name, phone, email}]. Caller must
  /// have READ_CONTACTS granted. Throws PlatformException otherwise.
  Future<List<Map<String, String>>> getContacts() async {
    final raw = await _method.invokeMethod<List<dynamic>>('getContacts') ?? const [];
    return raw
        .map((e) => Map<String, String>.from((e as Map).map((k, v) => MapEntry(k as String, (v ?? '') as String))))
        .toList();
  }

  // ---- navigation --------------------------------------------------------------------

  /// Turn-by-turn from navigation apps' notifications (needs notification access):
  /// {type: nav, app, dist, instr, extra, iconHash, icon?(Uint8List 48x48 1-bit)} / {type: navEnd}
  Stream<Map<String, dynamic>> get navigationStream =>
      _nav ??= _navStream.receiveBroadcastStream().map(_map).asBroadcastStream();

  Future<void> setNavigation(bool enabled) => _call('setNavigation', {'enabled': enabled});

  // ---- calendar ---------------------------------------------------------------------------

  /// Event instances from an hour ago to [hours] ahead:
  /// [{id, title, begin(ms UTC), end, allDay, location, color(0xRRGGBB)}]. Needs the calendar permission.
  Future<List<Map<String, dynamic>>> getEvents({int hours = 48}) async {
    final raw = await _method.invokeMethod<List<dynamic>>('getEvents', {'hours': hours}) ?? const [];
    return raw.map((e) => Map<String, dynamic>.from(e as Map)).toList();
  }

  /// Fires "changed" (debounced) when the phone's calendar changes.
  Stream<String> get calendarChanges =>
      _calendar ??= _calendarStream.receiveBroadcastStream().map((e) => e.toString()).asBroadcastStream();

  /// Start watching for calendar changes (after the permission was granted).
  Future<void> watchCalendar() => _call('watchCalendar');

  // ---- home-screen widget ----------------------------------------------------------------

  /// {name, connected, charging, battery, steps, goal, next}
  Future<void> updateWidget(Map<String, dynamic> data) => _call('updateWidget', data);

  // ---- Do Not Disturb ------------------------------------------------------------------

  /// {on, access}: whether the phone is in DND, and whether the app may change it.
  Future<({bool on, bool access})> getDnd() async {
    final m = await _call<Map<dynamic, dynamic>>('getDnd');
    return (on: m?['on'] == true, access: m?['access'] == true);
  }

  /// False if the app has no DND access (see [openDndAccess]).
  Future<bool> setDnd(bool on) async => await _call<bool>('setDnd', {'on': on}) ?? false;

  Future<void> openDndAccess() => _call('openDndAccess');

  /// The phone's DND switching on / off.
  Stream<bool> get dndChanges => _dnd ??= _dndStream.receiveBroadcastStream().map((e) => e == true).asBroadcastStream();

  /// Opens a web link in the browser. False if there's none to open it.
  Future<bool> openUrl(String url) async => (await _call<bool>('openUrl', {'url': url})) ?? false;

  /// An app's icon as a 40x40 JPEG for the watch, or null.
  Future<Uint8List?> appIconJpeg(String package) => _call<Uint8List>('appIconJpeg', {'package': package});

  /// The cover of what's playing as a 128x128 JPEG, if [hash] is still current.
  Future<Uint8List?> albumArtJpeg(int hash) => _call<Uint8List>('albumArtJpeg', {'hash': hash});

  // ---- app updates ------------------------------------------------------------------

  /// versionName of the installed app ("3.2.0").
  Future<String?> appVersion() => _call<String>('getAppVersion');

  /// Whether Android lets this app open the installer ("Install unknown apps").
  Future<bool> canInstallApks() async => (await _call<bool>('canInstallApks')) ?? false;

  Future<void> openInstallPermission() => _call('openInstallPermission');

  /// Opens the system installer for an .apk in the cache's updates/ folder.
  Future<bool> installApk(String path) async => (await _call<bool>('installApk', {'path': path})) ?? false;

  /// Taps on the widget's buttons ("find").
  Stream<String> get widgetActions =>
      _widget ??= _widgetStream.receiveBroadcastStream().map((e) => e.toString()).asBroadcastStream();
}
