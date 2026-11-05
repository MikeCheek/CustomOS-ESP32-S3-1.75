import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/watch_usage.dart';
import 'ble_provider.dart';

class UsageState {
  final WatchUsage? usage;
  final bool asking;

  /// The watch's battery from its status reports (every ~5 min while
  /// connected), kept for a week on the phone: (time, %, charging).
  final List<(DateTime, int, bool)> week;
  const UsageState({this.usage, this.asking = false, this.week = const []});
  UsageState copyWith({WatchUsage? usage, bool? asking, List<(DateTime, int, bool)>? week}) =>
      UsageState(usage: usage ?? this.usage, asking: asking ?? this.asking, week: week ?? this.week);
}

/// The Diagnostics' usage report: asks the watch ({"t":"usage"}), puts its
/// three parts together, and keeps a week of battery readings on the phone.
class UsageNotifier extends StateNotifier<UsageState> {
  static const _kWeek = 'usage_week';
  final Ref _ref;
  final _parts = <int, Map<String, dynamic>>{};
  UsageNotifier(this._ref) : super(const UsageState()) {
    _load();
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    final week = <(DateTime, int, bool)>[];
    for (final s in p.getStringList(_kWeek) ?? const <String>[]) {
      final f = s.split(',');
      if (f.length < 3) continue;
      final t = int.tryParse(f[0]), b = int.tryParse(f[1]);
      if (t != null && b != null) week.add((DateTime.fromMillisecondsSinceEpoch(t), b, f[2] == '1'));
    }
    if (mounted) state = state.copyWith(week: week);
  }

  Future<void> request() async {
    final ble = _ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    _parts.clear();
    state = state.copyWith(asking: true);
    await ble.sendLink({'t': 'usage'});
    // older firmware never answers
    Future.delayed(const Duration(seconds: 8), () {
      if (mounted && state.asking) state = state.copyWith(asking: false);
    });
  }

  void onPart(Map<String, dynamic> e) {
    final p = (e['p'] as num?)?.toInt();
    if (p == null) return;
    _parts[p] = e;
    final u = WatchUsage.fromParts(_parts, DateTime.now());
    if (u != null) state = state.copyWith(usage: u, asking: false);
  }

  /// A status report's battery: one point every 5 minutes at most, a week kept.
  Future<void> onReport(int battery, bool charging) async {
    if (battery < 0) return;
    final now = DateTime.now();
    final week = state.week;
    if (week.isNotEmpty && now.difference(week.last.$1) < const Duration(minutes: 4)) return;
    final cut = now.subtract(const Duration(days: 7));
    final next = [...week.where((w) => w.$1.isAfter(cut)), (now, battery, charging)];
    state = state.copyWith(week: next);
    final p = await SharedPreferences.getInstance();
    await p.setStringList(_kWeek, [for (final (t, b, c) in next) '${t.millisecondsSinceEpoch},$b,${c ? 1 : 0}']);
  }
}

final usageProvider = StateNotifierProvider<UsageNotifier, UsageState>((ref) => UsageNotifier(ref));
