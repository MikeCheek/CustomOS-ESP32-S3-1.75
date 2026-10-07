import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'ble_service.dart';

/// Fast file transfers over Wi-Fi (firmware 2.7+).
///
/// Asked for over the Bluetooth link: the watch joins its saved Wi-Fi network
/// and runs a tiny HTTP server that answers only requests carrying a one-time
/// key. The phone has to be on the same network. See COMPANION_PROTOCOL.md,
/// "Wi-Fi transfers".
class WifiSession {
  final BleService _ble;
  final String host;
  final int port;
  final String key;
  final HttpClient _http = HttpClient()
    ..connectionTimeout = const Duration(seconds: 5)
    ..idleTimeout = const Duration(seconds: 10);

  static const _step = Duration(seconds: 15); // no progress for this long = stalled

  WifiSession._(this._ble, this.host, this.port, this.key);

  /// Starts the watch's server and checks the phone can reach it. Throws a
  /// readable message (no Wi-Fi set up on the watch, different networks...).
  static Future<WifiSession> open(BleService ble, {Duration timeout = const Duration(seconds: 28)}) async {
    if (!ble.hasPhoneLink) throw Exception('Watch not connected');
    final ready = Completer<Map<String, dynamic>>();
    final sub = ble.linkEvents.listen((e) {
      if (e['e'] != 'wifi' || ready.isCompleted) return;
      if (e['st'] == 'ready') {
        ready.complete(e);
      } else if (e['st'] == 'err') {
        ready.completeError(Exception(e['why'] as String? ?? 'Wi-Fi transfer failed'));
      }
    });
    Future<void> off() async {
      try {
        await ble.sendLink({'t': 'wifi', 'on': 0});
      } catch (_) {}
    }

    try {
      await ble.sendLink({'t': 'wifi', 'on': 1});
      Map<String, dynamic> r;
      try {
        r = await ready.future.timeout(timeout);
      } on TimeoutException {
        await off();
        throw Exception('The watch didn\'t join Wi-Fi in time');
      }
      final s = WifiSession._(ble, r['ip'] as String? ?? '', (r['port'] as num?)?.toInt() ?? 8080, r['k'] as String? ?? '');
      if (s.host.isEmpty || s.host == '0.0.0.0') {
        await off();
        throw Exception('The watch has no Wi-Fi address');
      }
      try {
        await s.list();
      } catch (_) {
        await s.close();
        throw Exception('Can\'t reach the watch over Wi-Fi - is the phone on the same network?');
      }
      return s;
    } finally {
      await sub.cancel();
    }
  }

  Uri _uri(String path, [Map<String, String> q = const {}]) =>
      Uri(scheme: 'http', host: host, port: port, path: path, queryParameters: {'k': key, ...q});

  Future<T> _t<T>(Future<T> f) => f.timeout(_step, onTimeout: () {
        _http.close(force: true);
        throw TimeoutException('The watch stopped answering over Wi-Fi');
      });

  /// [{name, size}] of the recordings on the watch.
  Future<List<Map<String, dynamic>>> list() async {
    final req = await _t(_http.getUrl(_uri('/list')));
    final res = await _t(req.close());
    if (res.statusCode != 200) throw HttpException('HTTP ${res.statusCode}');
    final body = await _t(res.transform(utf8.decoder).join());
    return (jsonDecode(body) as List).map((e) => Map<String, dynamic>.from(e as Map)).toList();
  }

  /// Downloads /Recordings/[name] to [dest]. [expected] = size from the
  /// watch's list (checked when the response has no length).
  Future<void> download(String name, File dest,
      {int expected = -1, void Function(int got, int total)? onProgress}) async {
    final req = await _t(_http.getUrl(_uri('/f', {'n': name})));
    final res = await _t(req.close());
    if (res.statusCode != 200) throw HttpException('HTTP ${res.statusCode} for $name');
    final total = res.contentLength >= 0 ? res.contentLength : expected;
    final tmp = File('${dest.path}.part');
    final sink = tmp.openWrite();
    var got = 0;
    try {
      await for (final chunk in res.timeout(_step)) {
        sink.add(chunk);
        got += chunk.length;
        onProgress?.call(got, total);
      }
      await sink.close();
    } catch (_) {
      await sink.close();
      if (await tmp.exists()) await tmp.delete();
      rethrow;
    }
    if (total >= 0 && got != total) {
      await tmp.delete();
      throw HttpException('Transfer of $name was cut short');
    }
    if (await dest.exists()) await dest.delete();
    await tmp.rename(dest.path);
  }

  /// Uploads [src] into the watch's /Recordings. Returns the name it was
  /// saved under (the watch never overwrites: name_2.wav ...).
  /// [toRoot]: any file into the card's root (firmware 2.8+, like a
  /// Bluetooth file transfer) instead of an audio file into /Recordings.
  Future<String> upload(String name, File src,
      {bool toRoot = false, void Function(int sent, int total)? onProgress}) async {
    final total = await src.length();
    final req = await _t(_http.postUrl(_uri(toRoot ? '/put' : '/up')));
    // Key and name as headers: the watch's server doesn't parse the query
    // string of a body upload.
    req.headers.set('X-Key', key);
    req.headers.set('X-Name', name);
    req.headers.contentType = ContentType.binary;
    req.contentLength = total;
    var sent = 0;
    await req.addStream(src.openRead().timeout(_step).map((c) {
      sent += c.length;
      onProgress?.call(sent, total);
      return c;
    }));
    final res = await _t(req.close());
    final body = await _t(res.transform(utf8.decoder).join());
    if (res.statusCode != 200) throw HttpException('The watch refused the file (HTTP ${res.statusCode})');
    return (jsonDecode(body) as Map)['name'] as String? ?? name;
  }

  /// Stops the server (the watch turns Wi-Fi back off if it was off).
  Future<void> close() async {
    _http.close(force: true);
    try {
      await _ble.sendLink({'t': 'wifi', 'on': 0});
    } catch (_) {}
  }
}
