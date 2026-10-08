import 'dart:io';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/firmware_image.dart';
import '../services/update_service.dart';
import 'ble_provider.dart';

/// What the latest GitHub release offers, compared with what's installed.
class UpdateState {
  final String? appVersion; // installed app (versionName)
  final ReleaseInfo? latest;
  final bool checked; // a check finished (latest == null then means "no releases")
  final bool checking;
  final String? error;
  final DateTime? lastCheck;
  final double? apkProgress; // 0..1 while the app update downloads
  final String? apkError;

  const UpdateState({
    this.appVersion,
    this.latest,
    this.checked = false,
    this.checking = false,
    this.error,
    this.lastCheck,
    this.apkProgress,
    this.apkError,
  });

  bool get appUpdate =>
      latest?.apkUrl != null && appVersion != null && FirmwareImage.compareVersions(latest!.version, appVersion!) > 0;

  /// True when the release has firmware newer than [watchFirmware].
  bool firmwareUpdateFor(String? watchFirmware) =>
      latest?.firmwareUrl != null &&
      watchFirmware != null &&
      FirmwareImage.compareVersions(latest!.version, watchFirmware) > 0;

  UpdateState copyWith({
    String? appVersion,
    ReleaseInfo? latest,
    bool clearLatest = false,
    bool? checked,
    bool? checking,
    String? error,
    bool clearError = false,
    DateTime? lastCheck,
    double? apkProgress,
    bool clearApkProgress = false,
    String? apkError,
    bool clearApkError = false,
  }) {
    return UpdateState(
      appVersion: appVersion ?? this.appVersion,
      latest: clearLatest ? null : (latest ?? this.latest),
      checked: checked ?? this.checked,
      checking: checking ?? this.checking,
      error: clearError ? null : (error ?? this.error),
      lastCheck: lastCheck ?? this.lastCheck,
      apkProgress: clearApkProgress ? null : (apkProgress ?? this.apkProgress),
      apkError: clearApkError ? null : (apkError ?? this.apkError),
    );
  }
}

class UpdateNotifier extends StateNotifier<UpdateState> {
  final Ref _ref;
  static const _lastCheckKey = 'update_last_check';
  static const checkEvery = Duration(hours: 24);
  bool _cancelApk = false;

  UpdateNotifier(this._ref) : super(const UpdateState()) {
    _init();
  }

  Future<void> _init() async {
    final v = await _ref.read(nativeServiceProvider).appVersion();
    final prefs = await SharedPreferences.getInstance();
    final ms = prefs.getInt(_lastCheckKey);
    state = state.copyWith(
      appVersion: v,
      lastCheck: ms != null ? DateTime.fromMillisecondsSinceEpoch(ms) : null,
    );
  }

  /// Checks GitHub now. [auto]: only if the last check is a day old, and
  /// errors stay quiet.
  Future<void> check({bool auto = false}) async {
    if (state.checking) return;
    if (auto) {
      final last = state.lastCheck;
      // Already checked this session, less than a day ago.
      if (state.checked && last != null && DateTime.now().difference(last) < checkEvery) return;
    }
    state = state.copyWith(checking: true, clearError: true);
    try {
      if (state.appVersion == null) await _init();
      final latest = await UpdateService.fetchLatest();
      final now = DateTime.now();
      (await SharedPreferences.getInstance()).setInt(_lastCheckKey, now.millisecondsSinceEpoch);
      state = state.copyWith(latest: latest, clearLatest: latest == null, checked: true, checking: false, lastCheck: now);
    } catch (e) {
      state = state.copyWith(checking: false, error: auto ? null : _message(e));
    }
  }

  /// Downloads the release APK and opens Android's installer. Returns false
  /// (and sets apkError) if it couldn't.
  Future<bool> installApp() async {
    final rel = state.latest;
    if (rel?.apkUrl == null || state.apkProgress != null) return false;
    final native = _ref.read(nativeServiceProvider);
    if (!await native.canInstallApks()) {
      state = state.copyWith(apkError: 'Allow "Install unknown apps" for AmoledWatch, then tap Update again.');
      await native.openInstallPermission();
      return false;
    }
    _cancelApk = false;
    state = state.copyWith(apkProgress: 0, clearApkError: true);
    try {
      final bytes = await UpdateService.download(
        rel!.apkUrl!,
        onProgress: (r, t) {
          final total = t > 0 ? t : rel.apkSize;
          if (total > 0) state = state.copyWith(apkProgress: (r / total).clamp(0.0, 1.0));
        },
        cancel: () => _cancelApk,
      );
      final dir = Directory('${(await getTemporaryDirectory()).path}/updates');
      if (dir.existsSync()) dir.deleteSync(recursive: true);
      dir.createSync(recursive: true);
      final file = File('${dir.path}/AmoledWatch-${rel.version}.apk');
      await file.writeAsBytes(bytes, flush: true);
      state = state.copyWith(clearApkProgress: true);
      if (!await native.installApk(file.path)) {
        state = state.copyWith(apkError: 'Could not open the installer');
        return false;
      }
      return true;
    } catch (e) {
      state = state.copyWith(clearApkProgress: true, apkError: _message(e));
      return false;
    }
  }

  void cancelApp() => _cancelApk = true;

  static String _message(Object e) {
    if (e is HttpException) return e.message;
    return e.toString().replaceFirst('Exception: ', '');
  }
}

final updateProvider = StateNotifierProvider<UpdateNotifier, UpdateState>((ref) => UpdateNotifier(ref));
