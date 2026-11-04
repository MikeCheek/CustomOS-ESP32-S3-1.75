import 'dart:convert';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'ble_provider.dart';

/// One day's activity rings, as the watch counted them (fitness.h).
class RingDay {
  final DateTime date;
  final int moveKcal, exerciseMin, standHours, steps;
  const RingDay(this.date, this.moveKcal, this.exerciseMin, this.standHours, this.steps);

  Map<String, dynamic> toJson() => {
    'd': date.toIso8601String().substring(0, 10),
    'mv': moveKcal,
    'ex': exerciseMin,
    'sd': standHours,
    'st': steps,
  };
  static RingDay? fromJson(Map<String, dynamic> j) {
    final d = DateTime.tryParse(j['d'] as String? ?? '');
    if (d == null) return null;
    return RingDay(
      d,
      (j['mv'] as num?)?.toInt() ?? 0,
      (j['ex'] as num?)?.toInt() ?? 0,
      (j['sd'] as num?)?.toInt() ?? 0,
      (j['st'] as num?)?.toInt() ?? 0,
    );
  }
}

/// A workout recorded on the watch (app_workout.h).
class Workout {
  final String type;
  final DateTime start; // local time
  final Duration duration;
  final int meters, kcal, steps;
  final bool gps;
  const Workout(this.type, this.start, this.duration, this.meters, this.kcal, this.steps, this.gps);

  /// From the watch's {"e":"wo",...}: "st" is local time counted as if UTC.
  static Workout fromWatch(Map<String, dynamic> e) {
    final st = (e['st'] as num?)?.toInt() ?? 0;
    final u = DateTime.fromMillisecondsSinceEpoch(st * 1000, isUtc: true);
    return Workout(
      e['ty'] as String? ?? 'Workout',
      DateTime(u.year, u.month, u.day, u.hour, u.minute, u.second),
      Duration(seconds: (e['du'] as num?)?.toInt() ?? 0),
      (e['m'] as num?)?.toInt() ?? 0,
      (e['kc'] as num?)?.toInt() ?? 0,
      (e['stp'] as num?)?.toInt() ?? 0,
      e['gps'] == 1,
    );
  }

  Map<String, dynamic> toJson() => {
    'ty': type,
    's': start.toIso8601String(),
    'du': duration.inSeconds,
    'm': meters,
    'kc': kcal,
    'stp': steps,
    'gps': gps,
  };
  static Workout? fromJson(Map<String, dynamic> j) {
    final s = DateTime.tryParse(j['s'] as String? ?? '');
    if (s == null) return null;
    return Workout(
      j['ty'] as String? ?? 'Workout',
      s,
      Duration(seconds: (j['du'] as num?)?.toInt() ?? 0),
      (j['m'] as num?)?.toInt() ?? 0,
      (j['kc'] as num?)?.toInt() ?? 0,
      (j['stp'] as num?)?.toInt() ?? 0,
      j['gps'] == true,
    );
  }

  /// min per km, null without a distance
  Duration? get pace => meters < 50 ? null : Duration(seconds: (duration.inSeconds / (meters / 1000)).round());
}

class FitnessState {
  final RingDay? today;
  final int moveGoal, exerciseGoal, standGoal;
  final List<RingDay> days; // before today, newest first
  final List<Workout> workouts; // newest first
  final int weightKg, heightCm; // for calories and the stride, kept on the watch
  const FitnessState({
    this.today,
    this.moveGoal = 300,
    this.exerciseGoal = 30,
    this.standGoal = 10,
    this.days = const [],
    this.workouts = const [],
    this.weightKg = 70,
    this.heightCm = 175,
  });

  FitnessState copyWith({
    RingDay? today,
    int? moveGoal,
    int? exerciseGoal,
    int? standGoal,
    List<RingDay>? days,
    List<Workout>? workouts,
    int? weightKg,
    int? heightCm,
  }) => FitnessState(
    today: today ?? this.today,
    moveGoal: moveGoal ?? this.moveGoal,
    exerciseGoal: exerciseGoal ?? this.exerciseGoal,
    standGoal: standGoal ?? this.standGoal,
    days: days ?? this.days,
    workouts: workouts ?? this.workouts,
    weightKg: weightKg ?? this.weightKg,
    heightCm: heightCm ?? this.heightCm,
  );
}

/// The watch's rings and workouts, kept on the phone (the watch only keeps a week).
class FitnessNotifier extends StateNotifier<FitnessState> {
  static const _kDays = 'fit_days', _kWorkouts = 'fit_workouts', _kGoals = 'fit_goals';
  final Ref _ref;
  FitnessNotifier(this._ref) : super(const FitnessState()) {
    _load();
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    final days = <RingDay>[
      for (final s in p.getStringList(_kDays) ?? const <String>[])
        ?RingDay.fromJson(jsonDecode(s) as Map<String, dynamic>),
    ];
    final wos = <Workout>[
      for (final s in p.getStringList(_kWorkouts) ?? const <String>[])
        ?Workout.fromJson(jsonDecode(s) as Map<String, dynamic>),
    ];
    final g = (p.getStringList(_kGoals) ?? const []).map(int.tryParse).toList();
    if (!mounted) return;
    state = state.copyWith(
      days: days,
      workouts: wos,
      moveGoal: g.isNotEmpty ? g[0] : null,
      exerciseGoal: g.length > 1 ? g[1] : null,
      standGoal: g.length > 2 ? g[2] : null,
    );
  }

  Future<void> _save() async {
    final p = await SharedPreferences.getInstance();
    await p.setStringList(_kDays, [for (final d in state.days.take(90)) jsonEncode(d.toJson())]);
    await p.setStringList(_kWorkouts, [for (final w in state.workouts.take(200)) jsonEncode(w.toJson())]);
    await p.setStringList(_kGoals, ['${state.moveGoal}', '${state.exerciseGoal}', '${state.standGoal}']);
  }

  /// The watch's status report: today's rings, goals, and (on "hi") its last days.
  void onReport(Map<String, dynamic> e) {
    if (!e.containsKey('mv')) return; // older firmware
    final now = DateTime.now();
    final today = RingDay(
      DateTime(now.year, now.month, now.day),
      (e['mv'] as num?)?.toInt() ?? 0,
      (e['ex'] as num?)?.toInt() ?? 0,
      (e['sd'] as num?)?.toInt() ?? 0,
      (e['st'] as num?)?.toInt() ?? 0,
    );
    var days = state.days;
    final fh = e['fh'];
    if (fh is List) {
      final byDate = {for (final d in days) d.date: d};
      for (final row in fh) {
        if (row is! List || row.length < 5) continue;
        final ymd = (row[0] as num).toInt();
        final date = DateTime(ymd ~/ 10000, ymd ~/ 100 % 100, ymd % 100);
        byDate[date] = RingDay(
          date,
          (row[1] as num).toInt(),
          (row[2] as num).toInt(),
          (row[3] as num).toInt(),
          (row[4] as num).toInt(),
        );
      }
      byDate.remove(today.date);
      days = byDate.values.toList()..sort((a, b) => b.date.compareTo(a.date));
    }
    state = state.copyWith(
      today: today,
      days: days,
      moveGoal: (e['gm'] as num?)?.toInt(),
      exerciseGoal: (e['ge'] as num?)?.toInt(),
      standGoal: (e['gs'] as num?)?.toInt(),
      weightKg: (e['kg'] as num?)?.toInt(),
      heightCm: (e['cm'] as num?)?.toInt(),
    );
    _save();
  }

  Future<void> addWorkout(Map<String, dynamic> e) async {
    final w = Workout.fromWatch(e);
    if (state.workouts.any((o) => o.start == w.start)) return; // sent again before our ack arrived
    state = state.copyWith(workouts: [w, ...state.workouts]..sort((a, b) => b.start.compareTo(a.start)));
    await _save();
  }

  /// Weight and height to the watch ({"t":"body"}); it answers with a report.
  Future<void> setBody(int kg, int cm) async {
    state = state.copyWith(weightKg: kg.clamp(30, 250), heightCm: cm.clamp(100, 230));
    final ble = _ref.read(bleServiceProvider);
    if (ble.hasPhoneLink) await ble.sendLink({'t': 'body', 'kg': state.weightKg, 'cm': state.heightCm});
  }

  Future<void> deleteWorkout(Workout w) async {
    state = state.copyWith(workouts: state.workouts.where((o) => o.start != w.start).toList());
    await _save();
  }
}

final fitnessProvider = StateNotifierProvider<FitnessNotifier, FitnessState>((ref) => FitnessNotifier(ref));
