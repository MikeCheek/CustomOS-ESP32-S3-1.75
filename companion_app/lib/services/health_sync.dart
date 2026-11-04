import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:health/health.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// The watch's numbers into Health Connect (Android) or Apple Health (iOS):
/// steps and active energy as the day goes, each workout once, mindful
/// minutes (Apple Health only - Health Connect has no such type here),
/// weight and height when they change. Write-only: nothing is read.
///
/// The watch reports running totals for the day, so what's written is what
/// was added since the last sync, over that stretch of time - Health then
/// adds them up without counting anything twice.
class HealthSync {
  HealthSync._();
  static final instance = HealthSync._();

  static const _kOn = 'health_on';
  final _health = Health();
  bool _configured = false;

  static const types = [
    HealthDataType.STEPS,
    HealthDataType.ACTIVE_ENERGY_BURNED,
    HealthDataType.WORKOUT,
    HealthDataType.WEIGHT,
    HealthDataType.HEIGHT,
  ];
  static List<HealthDataType> get _types => [
    ...types,
    if (Platform.isIOS) HealthDataType.MINDFULNESS,
    if (Platform.isAndroid) HealthDataType.DISTANCE_DELTA,
  ];

  String get storeName => Platform.isIOS ? 'Apple Health' : 'Health Connect';

  Future<bool> enabled() async => (await SharedPreferences.getInstance()).getBool(_kOn) ?? false;

  Future<void> _configure() async {
    if (_configured) return;
    await _health.configure();
    _configured = true;
  }

  /// Android: null when Health Connect is ready, else what's missing.
  Future<String?> unavailable() async {
    if (!Platform.isAndroid) return null;
    await _configure();
    final st = await _health.getHealthConnectSdkStatus();
    if (st == HealthConnectSdkStatus.sdkAvailable) return null;
    if (st == HealthConnectSdkStatus.sdkUnavailableProviderUpdateRequired) return 'Health Connect needs an update';
    return 'Health Connect isn\'t installed';
  }

  Future<void> install() async {
    await _configure();
    await _health.installHealthConnect();
  }

  /// Asks for write access and turns syncing on. False when refused.
  Future<bool> enable() async {
    await _configure();
    if (await unavailable() != null) return false;
    final ok = await _health.requestAuthorization(
      _types,
      permissions: [for (final _ in _types) HealthDataAccess.WRITE],
    );
    final p = await SharedPreferences.getInstance();
    await p.setBool(_kOn, ok);
    return ok;
  }

  Future<void> disable() async => (await SharedPreferences.getInstance()).setBool(_kOn, false);

  Future<DateTime?> lastSync() async {
    final ms = (await SharedPreferences.getInstance()).getInt('health_last');
    return ms == null ? null : DateTime.fromMillisecondsSinceEpoch(ms);
  }

  Future<void> _stamp(SharedPreferences p) => p.setInt('health_last', DateTime.now().millisecondsSinceEpoch);

  static String _day(DateTime d) => '${d.year}-${d.month}-${d.day}';

  /// A running total for today ([key]: steps, kcal, mind) - writes what it grew by.
  Future<void> _delta(
    String key,
    num total,
    Future<bool> Function(double value, DateTime from, DateTime to) write,
  ) async {
    if (!await enabled()) return;
    final p = await SharedPreferences.getInstance();
    final now = DateTime.now();
    final today = _day(now);
    var last = p.getDouble('health_${key}_v') ?? 0;
    var from = DateTime.fromMillisecondsSinceEpoch(p.getInt('health_${key}_t') ?? 0);
    if (p.getString('health_${key}_d') != today) {
      // a new day: from midnight
      last = 0;
      from = DateTime(now.year, now.month, now.day);
    }
    if (total < last) {
      // the watch restarted and counts again from its own zero: start over from here
      await p.setDouble('health_${key}_v', total.toDouble());
      await p.setInt('health_${key}_t', now.millisecondsSinceEpoch);
      await p.setString('health_${key}_d', today);
      return;
    }
    final add = total - last;
    if (add <= 0) return;
    if (!from.isBefore(now)) from = now.subtract(const Duration(minutes: 1));
    try {
      await _configure();
      if (await write(add.toDouble(), from, now)) {
        await p.setDouble('health_${key}_v', total.toDouble());
        await p.setInt('health_${key}_t', now.millisecondsSinceEpoch);
        await p.setString('health_${key}_d', today);
        await _stamp(p);
      }
    } catch (e) {
      debugPrint('[Health] $key: $e');
    }
  }

  /// The watch's status report: today's steps and active calories.
  Future<void> onReport({required int steps, required int moveKcal}) async {
    if (steps >= 0) {
      await _delta(
        'steps',
        steps,
        (v, a, b) => _health.writeHealthData(value: v, type: HealthDataType.STEPS, startTime: a, endTime: b),
      );
    }
    if (moveKcal >= 0) {
      await _delta(
        'kcal',
        moveKcal,
        (v, a, b) =>
            _health.writeHealthData(value: v, type: HealthDataType.ACTIVE_ENERGY_BURNED, startTime: a, endTime: b),
      );
    }
  }

  /// Today's mindful minutes (the Life app's Breathe) - Apple Health only.
  Future<void> onMindful(int minutesToday) async {
    if (!Platform.isIOS) return;
    await _delta('mind', minutesToday, (v, a, b) {
      final start = b.subtract(Duration(minutes: v.round()));
      return _health.writeHealthData(
        value: v,
        type: HealthDataType.MINDFULNESS,
        startTime: start.isBefore(a) ? a : start,
        endTime: b,
      );
    });
  }

  static HealthWorkoutActivityType _activity(String type) {
    final t = type.toLowerCase();
    if (t.contains('cycl')) return HealthWorkoutActivityType.BIKING;
    if (t.contains('hik')) return HealthWorkoutActivityType.HIKING;
    if (t.contains('indoor run')) return HealthWorkoutActivityType.RUNNING_TREADMILL;
    if (t.contains('run')) return HealthWorkoutActivityType.RUNNING;
    if (t.contains('walk')) return HealthWorkoutActivityType.WALKING;
    if (t.contains('strength')) return HealthWorkoutActivityType.TRADITIONAL_STRENGTH_TRAINING;
    if (t.contains('hiit')) return HealthWorkoutActivityType.HIGH_INTENSITY_INTERVAL_TRAINING;
    return HealthWorkoutActivityType.OTHER;
  }

  /// A workout from the watch, written once (keyed by its start).
  Future<void> onWorkout({
    required String type,
    required DateTime start,
    required Duration duration,
    required int meters,
    required int kcal,
  }) async {
    if (!await enabled() || duration.inSeconds < 60) return;
    final p = await SharedPreferences.getInstance();
    final done = p.getStringList('health_workouts') ?? const <String>[];
    final key = start.toIso8601String();
    if (done.contains(key)) return;
    try {
      await _configure();
      final ok = await _health.writeWorkoutData(
        activityType: _activity(type),
        start: start,
        end: start.add(duration),
        totalEnergyBurned: kcal > 0 ? kcal : null,
        totalDistance: meters > 0 ? meters : null,
        title: type,
      );
      if (ok) {
        await p.setStringList('health_workouts', [key, ...done].take(200).toList());
        await _stamp(p);
      }
    } catch (e) {
      debugPrint('[Health] workout: $e');
    }
  }

  /// Weight and height, when they change.
  Future<void> onBody(int kg, int cm) async {
    if (!await enabled()) return;
    final p = await SharedPreferences.getInstance();
    final key = '$kg/$cm';
    if (p.getString('health_body') == key) return;
    try {
      await _configure();
      final now = DateTime.now();
      final a = await _health.writeHealthData(value: kg.toDouble(), type: HealthDataType.WEIGHT, startTime: now);
      final b = await _health.writeHealthData(value: cm / 100, type: HealthDataType.HEIGHT, startTime: now);
      if (a && b) {
        await p.setString('health_body', key);
        await _stamp(p);
      }
    } catch (e) {
      debugPrint('[Health] body: $e');
    }
  }
}
