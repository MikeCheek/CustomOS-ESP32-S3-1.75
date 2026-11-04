import 'dart:convert';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/health_sync.dart';

/// One night as the watch measured it from wrist movement.
class SleepNight {
  final DateTime morning; // the date it ended
  final int startMin, asleepMin, stillMin, awakeMin;
  const SleepNight(this.morning, this.startMin, this.asleepMin, this.stillMin, this.awakeMin);

  /// When it started (the evening before when it began after noon).
  DateTime get start {
    final day = startMin >= 12 * 60 ? morning.subtract(const Duration(days: 1)) : morning;
    return DateTime(day.year, day.month, day.day, startMin ~/ 60, startMin % 60);
  }

  DateTime get end => start.add(Duration(minutes: asleepMin + awakeMin));

  List<int> toRow() => [
    morning.year * 10000 + morning.month * 100 + morning.day,
    startMin,
    asleepMin,
    stillMin,
    awakeMin,
  ];
  static SleepNight? fromRow(List r) {
    if (r.length < 5) return null;
    final ymd = (r[0] as num).toInt();
    if (ymd < 20000101) return null;
    return SleepNight(
      DateTime(ymd ~/ 10000, ymd ~/ 100 % 100, ymd % 100),
      (r[1] as num).toInt(),
      (r[2] as num).toInt(),
      (r[3] as num).toInt(),
      (r[4] as num).toInt(),
    );
  }
}

/// The nights the watch sends ({"e":"sleep"}, after each connect and each
/// morning), kept on the phone (120) and written to Health Connect / Apple
/// Health once each.
class SleepNotifier extends StateNotifier<List<SleepNight>> {
  static const _k = 'sleep_nights';
  SleepNotifier() : super(const []) {
    _load();
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    final l = <SleepNight>[];
    for (final s in p.getStringList(_k) ?? const <String>[]) {
      try {
        final n = SleepNight.fromRow(jsonDecode(s) as List);
        if (n != null) l.add(n);
      } catch (_) {}
    }
    if (mounted) state = l;
  }

  Future<void> onWatch(Map<String, dynamic> e) async {
    final rows = e['n'];
    if (rows is! List) return;
    final byDate = {for (final n in state) n.morning: n};
    final fresh = <SleepNight>[];
    for (final r in rows) {
      final n = r is List ? SleepNight.fromRow(r) : null;
      if (n == null) continue;
      if (byDate[n.morning]?.asleepMin != n.asleepMin) fresh.add(n);
      byDate[n.morning] = n;
    }
    state = byDate.values.toList()..sort((a, b) => b.morning.compareTo(a.morning));
    final p = await SharedPreferences.getInstance();
    await p.setStringList(_k, [for (final n in state.take(120)) jsonEncode(n.toRow())]);
    for (final n in fresh) {
      await HealthSync.instance.onSleep(start: n.start, end: n.end, asleepMin: n.asleepMin);
    }
  }
}

final sleepProvider = StateNotifierProvider<SleepNotifier, List<SleepNight>>((ref) => SleepNotifier());
