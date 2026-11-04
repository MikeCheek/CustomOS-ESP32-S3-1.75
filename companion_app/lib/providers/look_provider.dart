import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'ble_provider.dart';

/// The watch's look, as it reports it ({"e":"look"}, firmware 3.5+): the face
/// it wears, the main face's colour and parts, and the display options.
/// Changes go to the watch ({"t":"look"}) and show here right away; the
/// watch's answer then confirms them.
class WatchLook {
  final int face; // index into [faces]
  final List<String> faces;
  final int accent; // [accents] index
  final bool seconds, eyes, date, pet;
  final bool weatherFx, alwaysOn, h24, menuList;
  final int textSize; // 0 default, 1 large, 2 larger
  final int brightness; // 8..255
  final List<String> slots;

  const WatchLook({
    this.face = 0,
    this.faces = const ['Eyes', 'Minimal', 'Custom', 'Photo'],
    this.accent = 0,
    this.seconds = true,
    this.eyes = true,
    this.date = true,
    this.pet = true,
    this.weatherFx = true,
    this.alwaysOn = false,
    this.h24 = true,
    this.menuList = false,
    this.textSize = 0,
    this.brightness = 200,
    this.slots = const ['steps', 'next_event', 'battery'],
  });

  /// The watch's palette for the main face (face_look.cpp), as ARGB.
  static const accents = [
    0xFF7B63FF, // violet
    0xFF2953FF, // blue
    0xFF1EEAEF, // cyan
    0xFF9BE82A, // green
    0xFFFFED00, // yellow
    0xFFFF8C00, // orange
    0xFFFA1452, // pink
    0xFFFFFFFF, // white
  ];

  static WatchLook fromWatch(Map<String, dynamic> e) {
    bool b(String k, bool d) => e[k] == null ? d : e[k] == 1;
    return WatchLook(
      face: (e['wf'] as num?)?.toInt() ?? 0,
      faces: (e['faces'] as List?)?.map((x) => '$x').toList() ?? const ['Eyes', 'Minimal', 'Custom', 'Photo'],
      accent: (e['acc'] as num?)?.toInt() ?? 0,
      seconds: b('sec', true),
      eyes: b('eyes', true),
      date: b('date', true),
      pet: b('pet', true),
      weatherFx: b('wx', true),
      alwaysOn: b('aod', false),
      h24: b('h24', true),
      menuList: b('ml', false),
      textSize: (e['ts'] as num?)?.toInt() ?? 0,
      brightness: (e['br'] as num?)?.toInt() ?? 200,
      slots: (e['comp'] as List?)?.map((x) => '$x').toList() ?? const ['steps', 'next_event', 'battery'],
    );
  }

  /// This look with one watch key changed (the keys of {"t":"look"}).
  WatchLook withKey(String k, Object v) {
    int i() => (v as num).toInt();
    bool f() => v == 1 || v == true;
    return WatchLook(
      face: k == 'wf' ? i() : face,
      faces: faces,
      accent: k == 'acc' ? i() : accent,
      seconds: k == 'sec' ? f() : seconds,
      eyes: k == 'eyes' ? f() : eyes,
      date: k == 'date' ? f() : date,
      pet: k == 'pet' ? f() : pet,
      weatherFx: k == 'wx' ? f() : weatherFx,
      alwaysOn: k == 'aod' ? f() : alwaysOn,
      h24: k == 'h24' ? f() : h24,
      menuList: k == 'ml' ? f() : menuList,
      textSize: k == 'ts' ? i() : textSize,
      brightness: k == 'br' ? i() : brightness,
      slots: slots,
    );
  }
}

class LookNotifier extends StateNotifier<WatchLook?> {
  final Ref _ref;
  LookNotifier(this._ref) : super(null);

  void fromWatch(Map<String, dynamic> e) => state = WatchLook.fromWatch(e);

  /// Sets one thing on the watch: 'wf' (face), 'acc', 'sec', 'eyes', 'date', 'pet', 'wx', 'aod', 'h24', 'ml', 'ts', 'br'.
  Future<void> set(String key, Object value) async {
    final cur = state ?? const WatchLook();
    state = cur.withKey(key, value);
    final ble = _ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    await ble.sendLink({'t': 'look', key: value is bool ? (value ? 1 : 0) : value});
  }
}

final lookProvider = StateNotifierProvider<LookNotifier, WatchLook?>((ref) => LookNotifier(ref));
