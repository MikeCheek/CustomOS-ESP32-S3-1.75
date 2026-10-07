import 'package:permission_handler/permission_handler.dart';
import 'gemma_service.dart';
import 'native_service.dart';
import 'whisper_service.dart';

/// What the phone side is allowed / has installed. Shared by the first-run
/// setup and the Diagnostics page.
class SetupStatus {
  final bool bluetooth;          // nearby devices (scan + connect)
  final bool notificationAccess; // notification listener: forwarding, calls, media
  final bool postNotifications;  // the "connected" status notification
  final bool location;           // weather for the watch face
  final bool contacts;
  final bool calendar;
  final bool speechModel;        // whisper, for memos and voice replies
  final bool summaryModel;       // on-device LLM

  const SetupStatus({
    this.bluetooth = false,
    this.notificationAccess = false,
    this.postNotifications = false,
    this.location = false,
    this.contacts = false,
    this.calendar = false,
    this.speechModel = false,
    this.summaryModel = false,
  });

  static Future<SetupStatus> check(NativeService native) async {
    Future<bool> safe(Future<bool> Function() f) async {
      try {
        return await f();
      } catch (_) {
        return false;
      }
    }

    final w = WhisperService.instance;
    await w.loadPrefs();
    return SetupStatus(
      bluetooth: await safe(() async =>
          await Permission.bluetoothConnect.isGranted && await Permission.bluetoothScan.isGranted),
      notificationAccess: await safe(native.isNotificationListenerEnabled),
      postNotifications: await safe(() => Permission.notification.isGranted),
      location: await safe(() => Permission.locationWhenInUse.isGranted),
      contacts: await safe(() => Permission.contacts.isGranted),
      calendar: await safe(() => Permission.calendarFullAccess.isGranted),
      speechModel: await safe(() => w.isDownloaded()),
      summaryModel: await safe(() => GemmaService.instance.hasModel()),
    );
  }

  /// Asks for the Bluetooth permissions; true if granted.
  static Future<bool> requestBluetooth() async {
    final r = await [Permission.bluetoothScan, Permission.bluetoothConnect].request();
    return r.values.every((s) => s.isGranted || s.isLimited);
  }
}
