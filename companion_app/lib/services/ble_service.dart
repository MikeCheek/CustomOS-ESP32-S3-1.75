import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'ble_protocol.dart';

class BleService {
  BluetoothDevice? _device;
  BluetoothCharacteristic? _batteryChar;
  BluetoothCharacteristic? _notifChar;
  BluetoothCharacteristic? _timeChar;
  BluetoothCharacteristic? _contactsChar;
  BluetoothCharacteristic? _mediaChar;
  BluetoothCharacteristic? _fileChar;
  BluetoothCharacteristic? _notesChar;
  BluetoothCharacteristic? _controllerChar;
  BluetoothCharacteristic? _linkChar;
  BluetoothCharacteristic? _otaChar;

  final _batteryController = StreamController<int>.broadcast();
  final _connectionController = StreamController<bool>.broadcast();
  final _notesController = StreamController<Map<String, dynamic>>.broadcast();
  final _linkController = StreamController<Map<String, dynamic>>.broadcast();
  final _otaController = StreamController<List<int>>.broadcast();
  StreamSubscription<BluetoothConnectionState>? _connStateSub;
  final List<StreamSubscription<List<int>>> _valueSubs = [];
  bool _ready = false;       // connected AND characteristics discovered
  /// Reconnect by itself when the watch drops (out of range, reboot).
  bool autoReconnect = true;
  bool _settingUp = false;

  /// Largest single write the watch accepts on this link (MTU - 3).
  int get _maxWrite {
    final mtu = _device?.mtuNow ?? 23;
    return (mtu - 3).clamp(20, 509);
  }

  Stream<int> get batteryStream => _batteryController.stream;

  /// true once connected and ready to talk, false on disconnect.
  Stream<bool> get connectionStream => _connectionController.stream;
  Stream<Map<String, dynamic>> get notesStream => _notesController.stream;

  /// Messages from the watch on the phone link (e.g. {"e":"med","a":"next"}).
  Stream<Map<String, dynamic>> get linkEvents => _linkController.stream;

  bool get isConnected => _ready && (_device?.isConnected ?? false);
  bool get hasPhoneLink => isConnected && _linkChar != null;
  /// The watch firmware supports updates over Bluetooth.
  bool get supportsOta => isConnected && _otaChar != null;

  /// Firmware version from the watch's hello (set by the companion link).
  String? firmwareVersion;

  /// Firmware 2.7+: windowed recording downloads and Wi-Fi transfers.
  bool get fastTransfers {
    final v = firmwareVersion;
    if (v == null) return false;
    final p = v.split('.').map((s) => int.tryParse(s) ?? 0).toList();
    while (p.length < 2) {
      p.add(0);
    }
    return p[0] > 2 || (p[0] == 2 && p[1] >= 7);
  }

  /// Firmware 2.8+: any file over Wi-Fi (POST /put).
  bool get wifiFiles {
    final v = firmwareVersion;
    if (v == null) return false;
    final p = v.split('.').map((s) => int.tryParse(s) ?? 0).toList();
    while (p.length < 2) {
      p.add(0);
    }
    return p[0] > 2 || (p[0] == 2 && p[1] >= 8);
  }

  /// Firmware 2.9+: the watch's Wi-Fi can be set up and checked from the
  /// app (link message "wcfg").
  bool get wifiSetup {
    final v = firmwareVersion;
    if (v == null) return false;
    final p = v.split('.').map((s) => int.tryParse(s) ?? 0).toList();
    while (p.length < 2) {
      p.add(0);
    }
    return p[0] > 2 || (p[0] == 2 && p[1] >= 9);
  }

  /// Short connection interval while moving data (Android); balanced after.
  Future<void> setTransferPriority(bool high) async {
    final d = _device;
    if (d == null || !Platform.isAndroid) return;
    try {
      await d.requestConnectionPriority(
          connectionPriorityRequest: high ? ConnectionPriority.high : ConnectionPriority.balanced);
    } catch (_) {}
  }
  BluetoothDevice? get device => _device;

  Future<void> startScan() async {
    await FlutterBluePlus.startScan(
      timeout: const Duration(seconds: 10),
    );
  }

  Future<void> stopScan() async {
    await FlutterBluePlus.stopScan();
  }

  Stream<List<ScanResult>> get scanResults => FlutterBluePlus.scanResults;

  /// Connects to [device]. With [auto] the phone keeps reconnecting by
  /// itself whenever the watch comes back in range (returns immediately;
  /// watch [connectionStream]). Without it, waits for the connection.
  Future<bool> connect(BluetoothDevice device, {bool auto = false}) async {
    if (_device != null && _device!.remoteId != device.remoteId) {
      await disconnect();
    }
    _device = device;
    await _connStateSub?.cancel();
    _connStateSub = device.connectionState.listen(_onConnectionState);
    try {
      if (auto) {
        await device.connect(autoConnect: true, mtu: null);
      } else {
        await device.connect(timeout: const Duration(seconds: 12));
      }
      return true;
    } catch (e) {
      print('[BLE] connect failed: $e');
      if (!auto) _connectionController.add(false);
      return false;
    }
  }

  Future<void> _onConnectionState(BluetoothConnectionState state) async {
    if (state == BluetoothConnectionState.connected) {
      if (_ready || _settingUp) return;
      _settingUp = true;
      final d = _device;
      try {
        await _setup();
        if (_device == null || !identical(_device, d)) return; // disconnected meanwhile
        _ready = true;
        _connectionController.add(true);
      } catch (e) {
        print('[BLE] setup failed: $e');
        await disconnect(); // not our watch, or it went away mid-setup
        _connectionController.add(false);
      } finally {
        _settingUp = false;
      }
    } else if (state == BluetoothConnectionState.disconnected) {
      final was = _ready;
      _ready = false;
      firmwareVersion = null;
      _clearChars();
      // The stream starts with the current state (disconnected) and a failed
      // attempt also ends here - only a working link that dropped counts.
      if (!was) return;
      _connectionController.add(false);
      // Not a user disconnect (that clears _device first): arm the
      // phone's own background reconnect to this watch.
      final d = _device;
      if (d != null && autoReconnect && !d.isAutoConnectEnabled) {
        try {
          await d.connect(autoConnect: true, mtu: null);
        } catch (e) {
          print('[BLE] auto-reconnect arm failed: $e');
        }
      }
    }
  }

  /// Runs on every (re)connection: MTU, service discovery, subscriptions.
  Future<void> _setup() async {
    final device = _device;
    if (device == null) return;
    try {
      await device.requestMtu(512);
    } catch (_) {}

    _clearChars();
    final services = await device.discoverServices();
    final watchServices = services.where((s) => s.uuid == BleProtocol.serviceUuid).toList();
    if (watchServices.isEmpty) throw StateError('Not an AmoledWatch');
    await _ensureSecure(device, watchServices.first);
    for (final service in watchServices) {
      for (final char in service.characteristics) {
        final u = char.uuid;
        if (u == BleProtocol.batteryUuid) {
          _batteryChar = char;
          try {
            await _notify(char);
          } catch (e) {
            print('[BLE] battery notify: $e');
          }
          _valueSubs.add(char.onValueReceived.listen((value) {
            if (value.isNotEmpty) _batteryController.add(value[0]);
          }));
          try {
            final v = await char.read();
            if (v.isNotEmpty && v[0] > 0) _batteryController.add(v[0]);
          } catch (_) {}
        } else if (u == BleProtocol.notifUuid) {
          _notifChar = char;
        } else if (u == BleProtocol.timeUuid) {
          _timeChar = char;
        } else if (u == BleProtocol.contactsUuid) {
          _contactsChar = char;
        } else if (u == BleProtocol.mediaUuid) {
          _mediaChar = char;
        } else if (u == BleProtocol.fileUuid) {
          _fileChar = char;
        } else if (u == BleProtocol.notesUuid) {
          _notesChar = char;
          await _notify(char);
          _valueSubs.add(char.onValueReceived.listen(_handleNotesNotification));
        } else if (u == BleProtocol.controllerUuid) {
          _controllerChar = char;
        } else if (u == BleProtocol.linkUuid) {
          _linkChar = char;
          await _notify(char);
          _valueSubs.add(char.onValueReceived.listen(_handleLinkNotification));
        } else if (u == BleProtocol.otaUuid) {
          _otaChar = char;
          await _notify(char);
          _valueSubs.add(char.onValueReceived.listen((v) {
            if (v.isNotEmpty) _otaController.add(v);
          }));
        }
      }
      break;
    }
    // Set the watch clock right away.
    try {
      await syncTime();
    } catch (_) {}
  }

  /// Subscribes, retrying once: right after bonding Android can still be
  /// busy with the read that started the pairing (one GATT op at a time).
  Future<void> _notify(BluetoothCharacteristic c) async {
    try {
      await c.setNotifyValue(true);
    } catch (_) {
      await Future.delayed(const Duration(milliseconds: 600));
      await c.setNotifyValue(true);
    }
  }

  /// Firmware 3.0 only accepts an encrypted link: the phone pairs once with
  /// the 6-digit code the watch shows (Android asks for it), then reconnects
  /// encrypted by itself. Older firmware reads fine without pairing, so
  /// nothing changes there. Throws if pairing was refused or failed.
  Future<void> _ensureSecure(BluetoothDevice device, BluetoothService svc) async {
    final bat = svc.characteristics.where((c) => c.uuid == BleProtocol.batteryUuid).firstOrNull;
    if (bat == null) return;
    if (!Platform.isAndroid) {
      // iOS pairs by itself when a read needs it (system prompt for the
      // code). If that was cancelled the read fails: don't sit "connected"
      // with every write refused.
      try {
        await bat.read();
      } catch (_) {
        throw Exception('Pairing needed: connect again, accept the request and type the code shown on the watch');
      }
      return;
    }
    final bonded = await device.bondState.first == BluetoothBondState.bonded;
    try {
      await bat.read();   // works: old firmware (open link) or a good existing bond
      return;
    } catch (_) {
      // "insufficient authentication/encryption": firmware 3.0, pair now
    }
    if (bonded) {
      // The phone has a bond the watch no longer knows ("Unpair all phones"
      // on the watch, or a reflash): drop it, then pair again.
      try {
        await device.removeBond();
        await Future.delayed(const Duration(milliseconds: 800));
      } catch (e) {
        print('[BLE] removing stale bond: $e');
      }
    }
    pairingNeeded = true;
    _pairingController.add(true);
    try {
      try {
        await device.createBond(timeout: 120);
      } catch (e) {
        // Android may already be pairing (it starts by itself after the
        // refused read): wait for that to finish instead.
        print('[BLE] createBond: $e');
        await device.bondState
            .firstWhere((s) => s == BluetoothBondState.bonded || s == BluetoothBondState.none)
            .timeout(const Duration(seconds: 120));
      }
      if (await device.bondState.first != BluetoothBondState.bonded) {
        throw Exception('Pairing was cancelled or the code was wrong');
      }
    } finally {
      pairingNeeded = false;
      _pairingController.add(false);
    }
  }

  /// True while the phone is pairing (the code is on the watch screen).
  bool pairingNeeded = false;
  final _pairingController = StreamController<bool>.broadcast();
  Stream<bool> get pairingEvents => _pairingController.stream;

  /// Unpairs this phone from the watch on the phone's side (Android), so a
  /// fresh pairing can happen. The watch's side: Settings > BLE Status.
  Future<void> removeBond() async {
    final d = _device;
    if (d == null || !Platform.isAndroid) return;
    try {
      await d.removeBond();
    } catch (e) {
      print('[BLE] removeBond: $e');
    }
  }

  /// User-initiated disconnect: also stops auto-reconnect.
  Future<void> disconnect() async {
    final d = _device;
    _device = null;
    await _connStateSub?.cancel();
    _connStateSub = null;
    final was = _ready;
    _ready = false;
    _clearChars();
    try {
      await d?.disconnect();
    } catch (_) {}
    if (was) _connectionController.add(false);
  }

  void _clearChars() {
    for (final s in _valueSubs) {
      s.cancel();
    }
    _valueSubs.clear();
    _batteryChar = null;
    _notifChar = null;
    _timeChar = null;
    _contactsChar = null;
    _mediaChar = null;
    _fileChar = null;
    _notesChar = null;
    _controllerChar = null;
    _linkChar = null;
    _otaChar = null;
    _notesReassembly.clear();
    _linkBuf = null;
  }

  // ---- Phone link ---------------------------------------------------------

  List<int>? _linkBuf;
  int _linkTotal = 0;
  Future<void> _linkQueue = Future.value();

  void _handleLinkNotification(List<int> value) {
    if (value.isEmpty) return;
    List<int>? complete;
    if (value[0] == 0x7B) {
      // '{' - a whole message
      complete = value;
    } else if (value[0] == 0x01 && value.length >= 3) {
      _linkTotal = value[1] | (value[2] << 8);
      _linkBuf = List<int>.from(value.sublist(3));
    } else if (value[0] == 0x02 && _linkBuf != null) {
      _linkBuf!.addAll(value.sublist(1));
    }
    final buf = _linkBuf;
    if (complete == null && buf != null && buf.length >= _linkTotal) {
      complete = buf.sublist(0, _linkTotal);
      _linkBuf = null;
    }
    if (complete == null) return;
    try {
      final msg = jsonDecode(utf8.decode(complete, allowMalformed: true));
      if (msg is Map<String, dynamic>) _linkController.add(msg);
    } catch (e) {
      print('[BLE] bad link message: $e');
    }
  }

  /// Sends one JSON message to the watch. Messages are queued so the
  /// chunks of two messages never interleave.
  Future<void> sendLink(Map<String, dynamic> msg) {
    final next = _linkQueue.then((_) async {
      final c = _linkChar;
      if (c == null || !isConnected) return;
      final data = Uint8List.fromList(utf8.encode(jsonEncode(msg)));
      try {
        if (data.length <= _maxWrite) {
          await c.write(data);
        } else {
          for (final p in BleProtocol.chunkPackets(data, 0x01, 0x02, _maxWrite)) {
            await c.write(p);
          }
        }
      } catch (e) {
        print('[BLE] link write failed: $e');
      }
    });
    _linkQueue = next.catchError((_) {});
    return next;
  }

  // ---- Firmware update ------------------------------------------------------

  /// Streams a firmware image to the watch (characteristic 000A). The
  /// watch writes it to its spare slot, verifies it and restarts into it.
  /// [onProgress] gets bytes confirmed written on the watch. Throws a
  /// readable message on failure. [cancel] returning true aborts.
  /// [signature]: 64-byte ECDSA signature from a *.signed.bin (fw 3.0+).
  Future<void> sendFirmware(Uint8List image,
      {Uint8List? signature, void Function(int written, int total)? onProgress, bool Function()? cancel}) async {
    final c = _otaChar;
    if (c == null || !isConnected) throw Exception('This watch firmware can\'t be updated over Bluetooth');
    final total = image.length;
    var acked = 0;
    int? beginStatus;
    int? endStatus;
    int? failCode;
    final events = _otaController.stream.listen((v) {
      final op = v[0];
      if (op == 0x81 && v.length >= 2) beginStatus = v[1];
      if (op == 0x82 && v.length >= 5) {
        acked = v[1] | (v[2] << 8) | (v[3] << 16) | (v[4] << 24);
        onProgress?.call(acked, total);
      }
      if (op == 0x84 && v.length >= 2) endStatus = v[1];
      if (op == 0x85 && v.length >= 2) failCode = v[1];
    });

    var started = false;
    Future<void> waitFor(bool Function() done, Duration timeout, String what) async {
      final end = DateTime.now().add(timeout);
      while (!done()) {
        if (failCode != null) throw Exception(_otaFailText(failCode!));
        if (started && cancel?.call() == true) throw Exception('Cancelled');
        if (!isConnected) throw Exception('The watch disconnected');
        if (DateTime.now().isAfter(end)) throw Exception('No answer from the watch ($what)');
        await Future.delayed(const Duration(milliseconds: 20));
      }
    }

    final noResponse = c.properties.writeWithoutResponse;
    Future<void> write(List<int> p) => c.write(p, withoutResponse: noResponse);

    try {
      final begin = Uint8List(5);
      begin[0] = 0x01;
      begin.buffer.asByteData().setUint32(1, total, Endian.little);
      await c.write(begin);
      await waitFor(() => beginStatus != null, const Duration(seconds: 15), 'start');
      switch (beginStatus) {
        case 0:
          break;
        case 2:
          throw Exception('The file is too big for the watch');
        case 3:
          throw Exception('Watch battery too low - charge it to 20% or plug it in');
        default:
          throw Exception('The watch couldn\'t start the update');
      }
      started = true;

      const window = 32 * 1024;
      final chunk = (_maxWrite - 5).clamp(1, 495);
      var off = 0;
      while (off < total) {
        if (cancel?.call() == true) throw Exception('Cancelled');
        if (off - acked >= window) {
          await waitFor(() => off - acked < window, const Duration(seconds: 20), 'flash write');
          continue;
        }
        if (failCode != null) throw Exception(_otaFailText(failCode!));
        final n = (total - off) < chunk ? (total - off) : chunk;
        final p = Uint8List(5 + n);
        p[0] = 0x02;
        p.buffer.asByteData().setUint32(1, off, Endian.little);
        p.setRange(5, 5 + n, image, off);
        await write(p);
        off += n;
      }
      if (signature != null && signature.length == 64) {
        await c.write([0x05, ...signature]);
      }
      await c.write([0x03]);
      await waitFor(() => endStatus != null, const Duration(seconds: 60), 'verification');
      if (endStatus == 10 || endStatus == 11) throw Exception(_otaFailText(endStatus!));
      if (endStatus != 0) throw Exception('The watch rejected the image (not a valid firmware)');
      onProgress?.call(total, total);
    } catch (e) {
      // Our side gave up (cancel, timeout): tell the watch so it leaves its
      // update screen now. Harmless if the watch is the one that failed.
      if (started && failCode == null && endStatus == null && isConnected) {
        try {
          await c.write([0x04]);
        } catch (_) {}
      }
      rethrow;
    } finally {
      await events.cancel();
    }
  }

  static String _otaFailText(int code) {
    switch (code) {
      case 11:
        return 'This is older than the firmware on the watch (downgrades only over USB when updates are signed)';
      case 10:
        return 'The watch only accepts updates signed with your key (see ota_pubkey.h)';
      case 2:
        return 'Data arrived out of order - try again';
      case 3:
        return 'The watch couldn\'t keep up - try again';
      case 4:
        return 'Cancelled on the watch';
      case 5:
        return 'The watch disconnected';
      case 6:
        return 'Timed out';
      case 7:
        return 'Writing to the watch\'s flash failed';
      default:
        return 'Update failed (code $code)';
    }
  }

  Future<void> syncTime() async {
    if (_timeChar == null) return;
    await _timeChar!.write(BleProtocol.encodeTime(DateTime.now()));
  }

  Future<void> sendNotification(String text) async {
    if (_notifChar == null) return;
    await _notifChar!.write(BleProtocol.encodeNotification(text));
  }

  /// Triggers the watch's full-screen "Find Me" response (pulsing
  /// display + repeating vibrate/beep, ~8s or until tapped away) -
  /// works even if the watch is asleep. Distinct from a normal
  /// notification: see BleProtocol.findMeSentinel for why a plain
  /// "find" notification isn't reliable enough for actually locating
  /// a misplaced watch.
  Future<void> findWatch() async {
    if (_notifChar == null) return;
    await _notifChar!.write(utf8.encode(BleProtocol.findMeSentinel));
  }

  /// Sends the contact list. It's usually bigger than one BLE write, so
  /// it goes as a 0x01 (start + total length) packet plus 0x02
  /// continuations; the watch swaps the list in once all bytes arrived.
  Future<void> sendContacts(List<ContactEntry> contacts) async {
    if (_contactsChar == null) return;
    final data = BleProtocol.encodeContacts(contacts);
    for (final p in BleProtocol.chunkPackets(data, 0x01, 0x02, _maxWrite)) {
      await _contactsChar!.write(p);
    }
  }

  Future<void> sendMediaState(MediaState media) async {
    if (_mediaChar == null) return;
    await _mediaChar!.write(BleProtocol.encodeMedia(media));
  }

  /// Sends the controller's current d-pad/button state. write-without-
  /// response (no await on a confirmation) since this fires on every
  /// UI change while the Controller screen is open - the firmware
  /// side just mirrors whatever the latest packet says, so an
  /// occasional dropped packet just means state holds until the next
  /// real change, not a stuck/missed input.
  Future<void> sendController({
    bool up = false, bool down = false, bool left = false, bool right = false,
    bool a = false, bool b = false, bool x = false, bool y = false,
  }) async {
    if (_controllerChar == null) return;
    await _controllerChar!.write(
      BleProtocol.encodeController(up: up, down: down, left: left, right: right, a: a, b: b, x: x, y: y),
    );
  }

  Future<void> sendWeather(WeatherData weather) async {
    if (_fileChar == null) return;
    await _fileChar!.write(BleProtocol.encodeWeather(weather));
  }

  Future<void> sendFitness(FitnessData fitness) async {
    if (_fileChar == null) return;
    await _fileChar!.write(BleProtocol.encodeFitness(fitness));
  }

  Future<bool> sendWatchface(WatchfaceLayout layout) async {
    if (_fileChar == null) {
      print('[BLE] sendWatchface: _fileChar is null');
      return false;
    }
    try {
      final jsonBytes = utf8.encode(jsonEncode(layout.toJson()));
      final maxPayload = _maxWrite;
      final firstChunkData = maxPayload - 3;
      final contChunkData = maxPayload - 1;
      final totalLen = jsonBytes.length;

      if (totalLen <= firstChunkData) {
        final packet = Uint8List(3 + totalLen);
        packet[0] = 0x01;
        packet[1] = totalLen & 0xFF;
        packet[2] = (totalLen >> 8) & 0xFF;
        packet.setRange(3, 3 + totalLen, jsonBytes);
        await _fileChar!.write(packet);
      } else {
        var offset = 0;
        var isFirst = true;
        while (offset < totalLen) {
          final chunkSize = isFirst ? firstChunkData : contChunkData;
          final end = (offset + chunkSize).clamp(0, totalLen);
          final chunkData = jsonBytes.sublist(offset, end);
          final packet = Uint8List(1 + (isFirst ? 2 : 0) + chunkData.length);
          packet[0] = 0x01;
          if (isFirst) {
            packet[1] = totalLen & 0xFF;
            packet[2] = (totalLen >> 8) & 0xFF;
            packet.setRange(3, 3 + chunkData.length, chunkData);
          } else {
            packet.setRange(1, 1 + chunkData.length, chunkData);
          }
          await _fileChar!.write(packet);
          offset = end;
          isFirst = false;
        }
      }

      print('[BLE] sendWatchface: success ($totalLen bytes)');
      return true;
    } catch (e, st) {
      print('[BLE] sendWatchface FAILED: $e');
      print('[BLE] $st');
      return false;
    }
  }

  /// Largest file the watch takes over Bluetooth (hal_ble.cpp
  /// BLE_FILE_MAX_LEN: 4 MB from firmware 2.8, 1 MB before). Bigger files
  /// go over Wi-Fi.
  int get maxFileSize => wifiFiles ? 4 * 1024 * 1024 : 1024 * 1024;

  Future<void> sendFile(String name, Uint8List data) async {
    if (_fileChar == null) return;
    final chunkSize = (_maxWrite - 8).clamp(12, 400);
    final nameBytes = BleProtocol.utf8Truncate(name, (_maxWrite - 8).clamp(1, 63));

    final header = Uint8List(8 + nameBytes.length);
    header.buffer.asByteData().setUint32(0, nameBytes.length, Endian.little);
    header.buffer.asByteData().setUint32(4, data.length, Endian.little);
    header.setRange(8, 8 + nameBytes.length, nameBytes);
    await _fileChar!.write(header);

    for (var offset = 0; offset < data.length; offset += chunkSize) {
      final end = (offset + chunkSize).clamp(0, data.length);
      final chunk = data.sublist(offset, end);
      final packet = Uint8List(8 + chunk.length);
      packet.buffer.asByteData().setUint32(0, chunk.length, Endian.little);
      packet.buffer.asByteData().setUint32(4, offset, Endian.little);
      packet.setRange(8, 8 + chunk.length, chunk);
      await _fileChar!.write(packet);
    }
  }

  /// [onProgress] gets the number of bytes sent so far.
  Future<void> sendFileWithProgress(String name, Uint8List data,
      {void Function(int)? onProgress}) async {
    if (_fileChar == null) return;
    if (data.length > maxFileSize) {
      throw Exception('Too big for Bluetooth (max ${maxFileSize ~/ (1024 * 1024)} MB) - needs Wi-Fi');
    }
    final chunkSize = (_maxWrite - 8).clamp(12, 400);
    final nameBytes = BleProtocol.utf8Truncate(name, (_maxWrite - 8).clamp(1, 63));

    final header = Uint8List(8 + nameBytes.length);
    header.buffer.asByteData().setUint32(0, nameBytes.length, Endian.little);
    header.buffer.asByteData().setUint32(4, data.length, Endian.little);
    header.setRange(8, 8 + nameBytes.length, nameBytes);
    await _fileChar!.write(header);

    // Firmware 2.7+ accepts write-without-response here: no round trip per
    // chunk (flutter_blue_plus still waits until each packet is queued, so
    // order is kept).
    final noResp = _fileChar!.properties.writeWithoutResponse;
    final totalChunks = (data.length / chunkSize).ceil();
    for (var i = 0; i < totalChunks; i++) {
      final offset = i * chunkSize;
      final end = (offset + chunkSize).clamp(0, data.length);
      final chunk = data.sublist(offset, end);
      final packet = Uint8List(8 + chunk.length);
      packet.buffer.asByteData().setUint32(0, chunk.length, Endian.little);
      packet.buffer.asByteData().setUint32(4, offset, Endian.little);
      packet.setRange(8, 8 + chunk.length, chunk);
      final fc = _fileChar;
      if (fc == null) throw Exception('Watch disconnected');
      await fc.write(packet, withoutResponse: noResp);
      onProgress?.call(end); // bytes sent so far
    }
  }

  /// Largest recording the watch takes over Bluetooth ("rec:" files).
  static const maxRecordingSize = 4 * 1024 * 1024;

  /// Adds an audio file to the watch's recordings over Bluetooth (the watch
  /// asks for confirmation). [name] = plain file name, .wav or .mp3.
  Future<void> sendRecording(String name, Uint8List data, {void Function(int)? onProgress}) async {
    if (_fileChar == null) throw Exception('Watch not connected');
    if (data.length > maxRecordingSize) throw Exception('Too big for Bluetooth (max 4 MB) - use Wi-Fi');
    final chunkSize = (_maxWrite - 8).clamp(12, 500);
    final full = 'rec:$name';
    final nameBytes = BleProtocol.utf8Truncate(full, (_maxWrite - 8).clamp(1, 63));
    if (nameBytes.length != utf8.encode(full).length) throw Exception('File name too long for this connection');
    final header = Uint8List(8 + nameBytes.length);
    header.buffer.asByteData().setUint32(0, nameBytes.length, Endian.little);
    header.buffer.asByteData().setUint32(4, data.length, Endian.little);
    header.setRange(8, 8 + nameBytes.length, nameBytes);
    await _fileChar!.write(header);
    final noResp = _fileChar!.properties.writeWithoutResponse;
    for (var offset = 0; offset < data.length; offset += chunkSize) {
      final end = (offset + chunkSize).clamp(0, data.length);
      final packet = Uint8List(8 + end - offset);
      packet.buffer.asByteData().setUint32(0, end - offset, Endian.little);
      packet.buffer.asByteData().setUint32(4, offset, Endian.little);
      packet.setRange(8, packet.length, data, offset);
      final fc = _fileChar;
      if (fc == null) throw Exception('Watch disconnected');
      await fc.write(packet, withoutResponse: noResp);
      onProgress?.call(end);
    }
  }

  Future<int> readBattery() async {
    if (_batteryChar == null) return -1;
    final value = await _batteryChar!.read();
    return value.isNotEmpty ? value[0] : -1;
  }

  // ---- Notes sync methods ----

  // Reassemble chunked notifications from the watch into complete data
  static final Map<String, dynamic> _notesReassembly = {};
  void _handleNotesNotification(List<int> value) {
    if (value.isEmpty) return;
    final cmd = value[0];
    print('[BLE] notes notification: cmd=0x${cmd.toRadixString(16).padLeft(2, '0')} len=${value.length}');

    if (cmd == 0x10) {
      // Header: 0x10 + totalChunks(2 LE) + totalLen(3 LE)
      if (value.length < 6) return;
      final totalChunks = value[1] | (value[2] << 8);
      final totalLen = value[3] | (value[4] << 8) | (value[5] << 16);
      _notesReassembly['fileList'] = {
        'totalChunks': totalChunks,
        'totalLen': totalLen,
        'chunks': <int, List<int>>{},
        'receivedChunks': 0,
      };
    } else if (cmd == 0x11) {
      final reassembly = _notesReassembly['fileList'] as Map<String, dynamic>?;
      if (reassembly == null) return;
      final chunks = reassembly['chunks'] as Map<int, List<int>>;

      // Chunk: 0x11 + chunkIdx(2 LE) + data
      if (value.length < 4) return;
      final chunkIdx = value[1] | (value[2] << 8);
      final data = value.sublist(3);

      if (!chunks.containsKey(chunkIdx)) {
        chunks[chunkIdx] = data;
        reassembly['receivedChunks'] = (reassembly['receivedChunks'] as int) + 1;
      }

      if (reassembly['receivedChunks'] == reassembly['totalChunks']) {
        final allData = <int>[];
        for (int i = 0; i < reassembly['totalChunks']; i++) {
          allData.addAll(chunks[i] ?? []);
        }
        final jsonStr = String.fromCharCodes(allData);
        _notesController.add({'type': 'fileList', 'data': jsonStr});
        _notesReassembly.remove('fileList');
      }
    } else if (cmd == 0x12) {
      // Chunk: 0x12 + chunkIdx(2 LE) + totalChunks(2 LE) + data
      if (value.length < 5) return;
      final chunkIdx = value[1] | (value[2] << 8);
      final totalChunks = value[3] | (value[4] << 8);
      final data = value.sublist(5);
      _notesController.add({
        'type': 'fileChunk',
        'chunkIdx': chunkIdx,
        'totalChunks': totalChunks,
        'data': Uint8List.fromList(data),
      });
    } else if (cmd == 0x13) {
      _notesController.add({'type': 'transferComplete'});
    }
  }

  /// Request file list from the watch
  Future<void> requestFileList() async {
    // Wait up to 5s for characteristics to be discovered after connect
    for (int i = 0; i < 50 && _notesChar == null; i++) {
      await Future.delayed(const Duration(milliseconds: 100));
    }
    if (_notesChar == null) {
      print('[BLE] requestFileList: _notesChar is null after waiting!');
      return;
    }
    print('[BLE] requestFileList: sending 0x01');
    await _notesChar!.write([0x01]);
  }

  /// Request file download from the watch
  Future<void> requestFileDownload(String filename) async {
    if (_notesChar == null) {
      print('[BLE] requestFileDownload: _notesChar is null!');
      throw Exception('Watch not connected');
    }
    print('[BLE] requestFileDownload: $filename');
    final nameBytes = Uint8List.fromList(filename.codeUnits);
    final packet = Uint8List(1 + nameBytes.length);
    packet[0] = 0x02;
    packet.setRange(1, 1 + nameBytes.length, nameBytes);
    await _notesChar!.write(packet);
  }

  /// Request file deletion on the watch
  Future<void> requestFileDelete(String filename) async {
    if (_notesChar == null) return;
    final nameBytes = Uint8List.fromList(filename.codeUnits);
    final packet = Uint8List(1 + nameBytes.length);
    packet[0] = 0x03;
    packet.setRange(1, 1 + nameBytes.length, nameBytes);
    await _notesChar!.write(packet);
  }

  /// Sends a transcript back to the watch, which saves it next to the
  /// recording as <name>.txt. Chunked (0x06 start + total, 0x07
  /// continuation) since transcripts are far longer than one write.
  Future<void> sendTranscript(String filename, String transcript) async {
    if (_notesChar == null) return;
    final data = BleProtocol.utf8Truncate('$filename\n$transcript', BleProtocol.maxTranscriptLen);
    for (final p in BleProtocol.chunkPackets(data, 0x06, 0x07, _maxWrite)) {
      await _notesChar!.write(p);
    }
  }

  /// Flow control for recording downloads: [n] more chunks (firmware 2.7+
  /// keeps a window in flight; older firmware sends one per request).
  Future<void> requestNextChunk([int n = 1]) async {
    if (_notesChar == null) return;
    await _notesChar!.write(Uint8List.fromList(n > 1 ? [0x05, n.clamp(1, 64)] : [0x05]));
  }

  void dispose() {
    _connStateSub?.cancel();
    _batteryController.close();
    _connectionController.close();
    _notesController.close();
    _linkController.close();
    _otaController.close();
    _pairingController.close();
  }
}
