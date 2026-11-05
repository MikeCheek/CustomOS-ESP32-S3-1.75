import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:path_provider/path_provider.dart';

import '../services/ble_service.dart';
import 'ble_provider.dart';

/// The photo faces on the watch's SD card (firmware app_photo_watchface.h),
/// as the watch last reported them ({"e":"pf"}).
class PhotoFacesState {
  final List<String> names;
  final String current;
  final int rotate; // 0 tap only, 1 every hour, 2 every day
  final int max;
  final bool known; // the watch has answered since the app started
  const PhotoFacesState({this.names = const [], this.current = '', this.rotate = 0, this.max = 20, this.known = false});
}

class PhotoFacesNotifier extends StateNotifier<PhotoFacesState> {
  PhotoFacesNotifier(this._ble) : super(const PhotoFacesState()) {
    _sub = _ble.linkEvents.listen((e) {
      if (e['e'] != 'pf') return;
      state = PhotoFacesState(
        names: [for (final n in (e['l'] as List? ?? const [])) '$n'],
        current: e['cur'] as String? ?? '',
        rotate: (e['rot'] as num?)?.toInt() ?? 0,
        max: (e['max'] as num?)?.toInt() ?? 20,
        known: true,
      );
    });
  }

  final BleService _ble;
  StreamSubscription<Map<String, dynamic>>? _sub;

  Future<void> refresh() => _ble.sendLink({'t': 'pf', 'a': 'list'});
  Future<void> show(String name) => _ble.sendLink({'t': 'pf', 'a': 'show', 'n': name});
  Future<void> setRotate(int m) => _ble.sendLink({'t': 'pf', 'a': 'rot', 'm': m});

  Future<void> delete(String name) async {
    await _ble.sendLink({'t': 'pf', 'a': 'del', 'n': name});
    final f = await thumbFile(name);
    if (await f.exists()) await f.delete();
  }

  /// A new name for a photo: "p" and the date and time, so they sort by age.
  static String newName() {
    final t = DateTime.now();
    String two(int v) => v.toString().padLeft(2, '0');
    return 'p${t.year}${two(t.month)}${two(t.day)}${two(t.hour)}${two(t.minute)}${two(t.second)}';
  }

  /// The preview kept on the phone for each photo (the watch has no thumbnails).
  static Future<File> thumbFile(String name) async {
    final d = await getApplicationSupportDirectory();
    return File('${d.path}/photo_faces/$name.png');
  }

  static Future<void> saveThumb(String name, Uint8List png) async {
    final f = await thumbFile(name);
    await f.parent.create(recursive: true);
    await f.writeAsBytes(png);
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }
}

final photoFacesProvider = StateNotifierProvider<PhotoFacesNotifier, PhotoFacesState>(
  (ref) => PhotoFacesNotifier(ref.read(bleServiceProvider)),
);
