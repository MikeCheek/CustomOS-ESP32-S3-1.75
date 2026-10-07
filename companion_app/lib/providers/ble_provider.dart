import 'dart:async';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import '../services/ble_service.dart';
import '../services/ble_protocol.dart';
import '../services/native_service.dart';
import 'settings_provider.dart';

final bleServiceProvider = Provider<BleService>((ref) {
  final service = BleService();
  ref.onDispose(() => service.dispose());
  return service;
});

final nativeServiceProvider = Provider<NativeService>((ref) => NativeService());

class BleState {
  final bool isScanning;
  final bool isConnected;
  final bool isConnecting;  // a connection (or auto-reconnect) is pending
  final int batteryLevel;
  final String? deviceName;
  final List<ScanResult> scanResults;

  const BleState({
    this.isScanning = false,
    this.isConnected = false,
    this.isConnecting = false,
    this.batteryLevel = -1,
    this.deviceName,
    this.scanResults = const [],
  });

  BleState copyWith({
    bool? isScanning,
    bool? isConnected,
    bool? isConnecting,
    int? batteryLevel,
    String? deviceName,
    List<ScanResult>? scanResults,
  }) {
    return BleState(
      isScanning: isScanning ?? this.isScanning,
      isConnected: isConnected ?? this.isConnected,
      isConnecting: isConnecting ?? this.isConnecting,
      batteryLevel: batteryLevel ?? this.batteryLevel,
      deviceName: deviceName ?? this.deviceName,
      scanResults: scanResults ?? this.scanResults,
    );
  }
}

/// Connection to the watch: scanning, connecting, auto-reconnect to the
/// last watch, and the background link service while connected.
class BleNotifier extends StateNotifier<BleState> {
  final Ref _ref;
  BleService get _service => _ref.read(bleServiceProvider);
  NativeService get _native => _ref.read(nativeServiceProvider);
  final List<StreamSubscription> _subs = [];
  bool _autoStarted = false;

  BleNotifier(this._ref) : super(const BleState()) {
    _subs.add(_service.connectionStream.listen(_onConnection));
    _subs.add(_service.batteryStream.listen((level) {
      if (mounted) state = state.copyWith(batteryLevel: level);
    }));
    _subs.add(_service.scanResults.listen((results) {
      if (mounted) state = state.copyWith(scanResults: results);
    }));
    _subs.add(FlutterBluePlus.isScanning.listen((s) {
      if (mounted) state = state.copyWith(isScanning: s);
    }));

    // Reconnect to the last watch as soon as settings are loaded, and keep
    // the service's auto-reconnect in step with the setting.
    _ref.listen<AppSettings>(settingsProvider, (prev, next) {
      _service.autoReconnect = next.autoConnect;
      _maybeAutoConnect(next);
    }, fireImmediately: true);
  }

  void _maybeAutoConnect(AppSettings s) {
    if (_autoStarted || !s.loaded) return;
    final id = s.lastDeviceId;
    if (!s.autoConnect || id == null || id.isEmpty) return;
    if (_service.device != null) return; // already connecting / connected
    _autoStarted = true;
    state = state.copyWith(isConnecting: true, deviceName: s.lastDeviceName ?? BleProtocol.deviceName);
    _service.connect(BluetoothDevice.fromId(id), auto: true);
  }

  Future<void> _onConnection(bool connected) async {
    if (!mounted) return;
    if (connected) {
      final d = _service.device;
      final name = (d != null && d.platformName.isNotEmpty) ? d.platformName : (state.deviceName ?? BleProtocol.deviceName);
      state = state.copyWith(isConnected: true, isConnecting: false, deviceName: name);
      final settings = _ref.read(settingsProvider);
      if (d != null) {
        await _ref.read(settingsProvider.notifier).update(
            settings.copyWith(lastDeviceId: d.remoteId.str, lastDeviceName: name));
      }
      if (settings.backgroundLink) {
        await _native.startLinkService(title: '$name connected', text: 'Notifications, calls and music are linked');
      }
    } else {
      // Still pending if auto-reconnect is armed for this watch.
      state = BleState(
        isScanning: state.isScanning,
        isConnected: false,
        isConnecting: _service.device != null && _service.autoReconnect,
        batteryLevel: -1,
        deviceName: state.deviceName,
        scanResults: state.scanResults,
      );
    }
  }

  Future<void> startScan() async {
    if (mounted) state = state.copyWith(scanResults: []);
    try {
      await _service.startScan();
    } catch (e) {
      print('[BLE] scan failed: $e');
    }
  }

  Future<void> stopScan() async {
    try {
      await _service.stopScan();
    } catch (_) {}
  }

  Future<bool> connect(BluetoothDevice device) async {
    final name = device.advName.isNotEmpty
        ? device.advName
        : (device.platformName.isNotEmpty ? device.platformName : BleProtocol.deviceName);
    _autoStarted = true; // a manual choice wins over startup auto-connect
    state = state.copyWith(isConnecting: true, deviceName: name);
    final ok = await _service.connect(device);
    if (!ok && mounted) state = state.copyWith(isConnecting: false);
    return ok;
  }

  /// Disconnects and stops auto-reconnect until the next manual connect.
  Future<void> disconnect() async {
    _autoStarted = true; // don't auto-connect again until the next app start
    await _service.disconnect();
    await _native.stopLinkService();
    if (mounted) state = state.copyWith(isConnected: false, isConnecting: false, batteryLevel: -1);
  }

  Future<void> syncTime() => _service.syncTime();

  Future<bool> sendWatchface(WatchfaceLayout layout) => _service.sendWatchface(layout);

  @override
  void dispose() {
    for (final s in _subs) {
      s.cancel();
    }
    super.dispose();
  }
}

final bleProvider = StateNotifierProvider<BleNotifier, BleState>((ref) => BleNotifier(ref));
