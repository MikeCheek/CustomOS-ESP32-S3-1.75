import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'ble_provider.dart';

/// A gesture on the watch face (firmware/home_gestures.h).
class HomeSlot {
  final String key, label, hint;
  final IconData icon;
  const HomeSlot(this.key, this.label, this.hint, this.icon);
}

/// Something a gesture can do.
class HomeAction {
  final String key, label;
  final IconData icon;
  const HomeAction(this.key, this.label, this.icon);
}

class HomeStyle {
  final String key, label, about;
  final Map<String, String> actions;
  const HomeStyle(this.key, this.label, this.about, this.actions);
}

class TileInfo {
  final String key, label, about;
  final IconData icon;
  const TileInfo(this.key, this.label, this.about, this.icon);
}

const homeSlots = [
  HomeSlot('tap', 'Tap', 'Anywhere on the face', Icons.touch_app_rounded),
  HomeSlot('double', 'Double-tap', 'Two quick taps (a single tap then waits a moment)', Icons.ads_click_rounded),
  HomeSlot('hold', 'Touch and hold', 'Keep a finger down', Icons.back_hand_outlined),
  HomeSlot('up', 'Swipe up', 'From anywhere on the face', Icons.swipe_up_rounded),
  HomeSlot('down', 'Pull down', 'From the top of the face', Icons.swipe_down_rounded),
  HomeSlot('left', 'Swipe in from the right', 'Starting at the right edge', Icons.swipe_left_rounded),
  HomeSlot('right', 'Swipe in from the left', 'Starting at the left edge', Icons.swipe_right_rounded),
  HomeSlot('boot', 'BOOT button', 'A short press on a face (in apps it\'s back)', Icons.radio_button_checked_rounded),
];

const homeActions = [
  HomeAction('none', 'Nothing', Icons.block_rounded),
  HomeAction('apps', 'Apps', Icons.apps_rounded),
  HomeAction('notifs', 'Notifications', Icons.notifications_none_rounded),
  HomeAction('stack', 'Smart Stack', Icons.view_agenda_outlined),
  HomeAction('control', 'Control Center', Icons.tune_rounded),
  HomeAction('tiles', 'Tiles', Icons.view_carousel_outlined),
  HomeAction('faces', 'Faces', Icons.watch_outlined),
  HomeAction('nextface', 'Next face', Icons.arrow_forward_rounded),
  HomeAction('prevface', 'Previous face', Icons.arrow_back_rounded),
  HomeAction('assistant', 'Assistant', Icons.mic_none_rounded),
  HomeAction('music', 'Music', Icons.music_note_rounded),
  HomeAction('playpause', 'Play / pause', Icons.play_circle_outline_rounded),
  HomeAction('timer', 'Timer', Icons.timer_outlined),
  HomeAction('stopwatch', 'Stopwatch', Icons.av_timer_rounded),
  HomeAction('workout', 'Workout', Icons.directions_run_rounded),
  HomeAction('torch', 'Flashlight', Icons.flashlight_on_outlined),
  HomeAction('recorder', 'Voice note', Icons.keyboard_voice_outlined),
  HomeAction('life', 'Life', Icons.spa_outlined),
  HomeAction('activity', 'Activity', Icons.donut_large_rounded),
  HomeAction('weather', 'Weather', Icons.wb_sunny_outlined),
  HomeAction('calendar', 'Calendar', Icons.event_outlined),
  HomeAction('settings', 'Settings', Icons.settings_outlined),
  HomeAction('face', 'The face\'s own', Icons.photo_outlined),
  HomeAction('wallet', 'Wallet', Icons.qr_code_2_rounded),
];

HomeAction homeAction(String key) => homeActions.firstWhere((a) => a.key == key, orElse: () => homeActions.first);

/// What a slot can be set to: the face's own only for a tap, and touch and
/// hold never nothing (the faces must stay reachable).
List<HomeAction> actionsFor(String slot) => [
  for (final a in homeActions)
    if ((a.key != 'face' || slot == 'tap') && !(slot == 'hold' && a.key == 'none')) a,
];

// tap, double, hold, up, down, from the right, from the left, BOOT - as firmware/home_gestures.cpp
Map<String, String> _m(List<String> v) => {for (var i = 0; i < homeSlots.length; i++) homeSlots[i].key: v[i]};
final homeStyles = [
  HomeStyle(
    'standard',
    'Standard',
    'Tap for apps, Tiles from the right, notifications from the left',
    _m(['apps', 'playpause', 'faces', 'stack', 'control', 'tiles', 'notifs', 'control']),
  ),
  HomeStyle(
    'apple',
    'Apple-like',
    'Smart Stack up, notifications down, swipe from the edges to change face',
    _m(['apps', 'playpause', 'faces', 'stack', 'notifs', 'nextface', 'prevface', 'control']),
  ),
  HomeStyle(
    'wearos',
    'Wear OS-like',
    'Quick settings down, notifications up, Tiles to the side',
    _m(['apps', 'none', 'faces', 'notifs', 'control', 'tiles', 'assistant', 'apps']),
  ),
  HomeStyle(
    'galaxy',
    'Galaxy-like',
    'Apps up, quick panel down, Tiles and notifications to the sides',
    _m(['face', 'none', 'faces', 'apps', 'control', 'tiles', 'notifs', 'apps']),
  ),
  HomeStyle(
    'classic',
    'Classic',
    'How this watch worked before: apps from the right',
    _m(['face', 'none', 'faces', 'stack', 'control', 'apps', 'notifs', 'control']),
  ),
];

const allTiles = [
  TileInfo('activity', 'Activity', 'The rings, steps', Icons.donut_large_rounded),
  TileInfo('weather', 'Weather', 'Now, from the phone', Icons.wb_sunny_outlined),
  TileInfo('next', 'Up next', 'The next event', Icons.event_outlined),
  TileInfo('music', 'Music', 'What\'s playing, with buttons', Icons.music_note_rounded),
  TileInfo('timer', 'Timer', '1, 3, 5, 10 minutes in a tap', Icons.timer_outlined),
  TileInfo('life', 'Life', 'Today\'s score and what\'s next', Icons.spa_outlined),
  TileInfo('world', 'World clock', 'Your cities', Icons.public_rounded),
  TileInfo('alarm', 'Alarm', 'The next one', Icons.alarm_rounded),
  TileInfo('reminders', 'Reminders', 'The first ones', Icons.checklist_rounded),
  TileInfo('battery', 'Battery', 'Watch and phone', Icons.battery_5_bar_rounded),
];

/// The watch face's gestures and Tiles, as the watch reports them
/// ({"e":"home"}); changes go to the watch ({"t":"home"}) and show here
/// right away.
class HomeGestures {
  final Map<String, String> actions;
  final List<String> tiles;
  const HomeGestures({required this.actions, required this.tiles});

  static HomeGestures get defaults => HomeGestures(
    actions: homeStyles.first.actions,
    tiles: const ['activity', 'weather', 'next', 'music', 'timer', 'life', 'world'],
  );

  static HomeGestures fromWatch(Map<String, dynamic> e) {
    final a = <String, String>{...defaults.actions};
    final m = e['a'];
    if (m is Map) {
      for (final s in homeSlots) {
        final v = m[s.key];
        if (v is String && homeActions.any((x) => x.key == v)) a[s.key] = v;
      }
    }
    final t = e['tiles'];
    return HomeGestures(
      actions: a,
      tiles: t is List
          ? [
              for (final x in t)
                if (allTiles.any((k) => k.key == '$x')) '$x',
            ]
          : defaults.tiles,
    );
  }

  /// The style these match, or null (Custom).
  HomeStyle? get style {
    for (final st in homeStyles) {
      if (homeSlots.every((s) => st.actions[s.key] == actions[s.key])) return st;
    }
    return null;
  }

  String action(String slot) => actions[slot] ?? 'none';

  HomeGestures withAction(String slot, String key) => HomeGestures(actions: {...actions, slot: key}, tiles: tiles);
  HomeGestures withTiles(List<String> t) => HomeGestures(actions: actions, tiles: t);
}

class HomeNotifier extends StateNotifier<HomeGestures?> {
  final Ref _ref;
  HomeNotifier(this._ref) : super(null);

  void fromWatch(Map<String, dynamic> e) => state = HomeGestures.fromWatch(e);

  Future<void> _send(Map<String, dynamic> m) async {
    final ble = _ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    await ble.sendLink({'t': 'home', ...m});
  }

  Future<void> setAction(String slot, String key) async {
    if (!actionsFor(slot).any((a) => a.key == key)) return;
    state = (state ?? HomeGestures.defaults).withAction(slot, key);
    await _send({
      'a': {slot: key},
    });
  }

  Future<void> setStyle(HomeStyle st) async {
    state = HomeGestures(actions: st.actions, tiles: (state ?? HomeGestures.defaults).tiles);
    await _send({'style': st.key});
  }

  Future<void> setTiles(List<String> tiles) async {
    state = (state ?? HomeGestures.defaults).withTiles(tiles);
    await _send({'tiles': tiles});
  }
}

final homeProvider = StateNotifierProvider<HomeNotifier, HomeGestures?>((ref) => HomeNotifier(ref));
