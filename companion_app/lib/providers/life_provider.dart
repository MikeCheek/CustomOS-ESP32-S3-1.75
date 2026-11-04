import 'dart:convert';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/life_plan.dart';
import 'ble_provider.dart';

class LifeState {
  final LifePlan plan;
  final LifeDay? today;
  final List<LifeDay> days; // before today, newest first
  final int shuffle; // today's meal ideas moved on this many times
  const LifeState({this.plan = const LifePlan(), this.today, this.days = const [], this.shuffle = 0});

  LifeState copyWith({LifePlan? plan, LifeDay? today, List<LifeDay>? days, int? shuffle}) => LifeState(
    plan: plan ?? this.plan,
    today: today ?? this.today,
    days: days ?? this.days,
    shuffle: shuffle ?? this.shuffle,
  );

  (String, String, String) get meals => MealIdeas.forDay(plan.diet, DateTime.now(), shuffle: shuffle);

  /// Days in a row with a score of 60% or more (today counts once it's there).
  int get streak {
    var n = 0;
    if ((today?.score ?? 0) >= 60) n++;
    var expect = DateTime.now().subtract(const Duration(days: 1));
    for (final d in days) {
      if (d.date.year != expect.year || d.date.month != expect.month || d.date.day != expect.day) break;
      if (d.score < 60) break;
      n++;
      expect = expect.subtract(const Duration(days: 1));
    }
    return n;
  }
}

/// The Life plan (kept on the phone, sent to the watch) and the days the
/// watch reports back ({"e":"life"}).
class LifeNotifier extends StateNotifier<LifeState> {
  static const _kPlan = 'life_plan', _kDays = 'life_days';
  final Ref _ref;
  LifeNotifier(this._ref) : super(const LifeState()) {
    _load();
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    final raw = p.getString(_kPlan);
    final plan = raw == null ? const LifePlan() : LifePlan.fromJson(jsonDecode(raw) as Map<String, dynamic>);
    final days = <LifeDay>[
      for (final s in p.getStringList(_kDays) ?? const <String>[]) ?LifeDay.fromRow(jsonDecode(s) as List),
    ];
    if (mounted) state = state.copyWith(plan: plan, days: days);
  }

  Future<void> _save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString(_kPlan, jsonEncode(state.plan.toJson()));
    await p.setStringList(_kDays, [for (final d in state.days.take(120)) jsonEncode(d.toRow())]);
  }

  Future<void> setPlan(LifePlan plan) async {
    final dietChanged = plan.diet != state.plan.diet;
    state = state.copyWith(plan: plan, shuffle: dietChanged ? 0 : null);
    await _save();
    await sendAll();
  }

  Future<void> shuffleMeals() async {
    state = state.copyWith(shuffle: state.shuffle + 1);
    await _sendMeals();
  }

  /// The plan and today's meal ideas - on connect and after a change.
  Future<void> sendAll() async {
    final ble = _ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    await ble.sendLink(state.plan.toWatch());
    await _sendMeals();
  }

  Future<void> _sendMeals() async {
    final ble = _ref.read(bleServiceProvider);
    if (!ble.hasPhoneLink) return;
    final (b, l, d) = state.meals;
    await ble.sendLink({'t': 'meals', 'b': b, 'l': l, 'd': d});
  }

  void onWatch(Map<String, dynamic> e) {
    final today = LifeDay.fromWatch(e);
    var days = state.days;
    final h = e['h'];
    if (h is List) {
      final byDate = {for (final d in days) d.date: d};
      for (final r in h) {
        final d = r is List ? LifeDay.fromRow(r) : null;
        if (d != null) byDate[d.date] = d;
      }
      if (today != null) byDate.remove(today.date);
      days = byDate.values.toList()..sort((a, b) => b.date.compareTo(a.date));
    }
    state = state.copyWith(today: today, days: days);
    _save();
  }
}

final lifeProvider = StateNotifierProvider<LifeNotifier, LifeState>((ref) => LifeNotifier(ref));
