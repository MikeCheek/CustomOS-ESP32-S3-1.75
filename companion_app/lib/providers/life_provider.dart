import 'dart:convert';

import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';

import '../services/gemma_service.dart';
import '../services/life_plan.dart';
import 'ble_provider.dart';

typedef MealSet = (String, String, String);

class LifeState {
  final LifePlan plan;
  final LifeDay? today;
  final List<LifeDay> days; // before today, newest first
  final int shuffle; // today's meal ideas moved on this many times
  /// Today's ideas written by the on-phone LLM (null: the built-in list's).
  final MealSet? aiIdeas;
  final bool writing; // the LLM is writing them now

  const LifeState({
    this.plan = const LifePlan(),
    this.today,
    this.days = const [],
    this.shuffle = 0,
    this.aiIdeas,
    this.writing = false,
  });

  static const _keep = Object();
  LifeState copyWith({
    LifePlan? plan,
    LifeDay? today,
    List<LifeDay>? days,
    int? shuffle,
    Object? aiIdeas = _keep,
    bool? writing,
  }) => LifeState(
    plan: plan ?? this.plan,
    today: today ?? this.today,
    days: days ?? this.days,
    shuffle: shuffle ?? this.shuffle,
    aiIdeas: identical(aiIdeas, _keep) ? this.aiIdeas : aiIdeas as MealSet?,
    writing: writing ?? this.writing,
  );

  /// What the watch gets: the LLM's ideas when there are some, else the built-in list's.
  MealSet get meals => (plan.aiMeals ? aiIdeas : null) ?? MealIdeas.forDay(plan.diet, DateTime.now(), shuffle: shuffle);
  bool get mealsByAi => plan.aiMeals && aiIdeas != null;

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

/// The Life plan (kept on the phone, sent to the watch), today's meal ideas,
/// and the days the watch reports back ({"e":"life"}).
class LifeNotifier extends StateNotifier<LifeState> {
  static const _kPlan = 'life_plan', _kDays = 'life_days', _kAi = 'life_ai_meals', _kRecent = 'life_recent_meals';
  final Ref _ref;
  LifeNotifier(this._ref) : super(const LifeState()) {
    _load();
  }

  /// What a set of ideas was written for: the day, the food preferences and how many times "Others" was tapped.
  static String ideasKey(LifePlan p, int shuffle, [DateTime? now]) {
    final n = now ?? DateTime.now();
    return '${n.year}-${n.month}-${n.day}|${p.diet}|${p.foodNotes.trim()}|$shuffle';
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    final raw = p.getString(_kPlan);
    final plan = raw == null ? const LifePlan() : LifePlan.fromJson(jsonDecode(raw) as Map<String, dynamic>);
    final days = <LifeDay>[
      for (final s in p.getStringList(_kDays) ?? const <String>[]) ?LifeDay.fromRow(jsonDecode(s) as List),
    ];
    // today's LLM ideas, if they were written for today and these preferences
    MealSet? ai;
    var shuffle = 0;
    final c = p.getString(_kAi);
    if (c != null) {
      final j = jsonDecode(c) as Map<String, dynamic>;
      shuffle = (j['sh'] as num?)?.toInt() ?? 0;
      if (j['key'] == ideasKey(plan, shuffle)) {
        ai = (j['b'] as String, j['l'] as String, j['d'] as String);
      } else {
        shuffle = 0;
      }
    }
    if (mounted) state = state.copyWith(plan: plan, days: days, aiIdeas: ai, shuffle: shuffle);
  }

  Future<void> _save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString(_kPlan, jsonEncode(state.plan.toJson()));
    await p.setStringList(_kDays, [for (final d in state.days.take(120)) jsonEncode(d.toRow())]);
  }

  /// Asks the on-phone LLM for today's ideas, unless they're already written for
  /// today (or [fresh]: new ones). Without a model, or when its answer isn't
  /// usable, the built-in list's ideas stay.
  Future<void> _writeIdeas({bool fresh = false}) async {
    final plan = state.plan;
    if (!plan.aiMeals || state.writing) return;
    final key = ideasKey(plan, state.shuffle);
    final p = await SharedPreferences.getInstance();
    final c = p.getString(_kAi);
    if (!fresh && state.aiIdeas != null && c != null && (jsonDecode(c) as Map)['key'] == key) return;
    if (!await GemmaService.instance.hasModel()) return;
    if (!mounted) return;
    state = state.copyWith(writing: true);
    try {
      final recent = p.getStringList(_kRecent) ?? const <String>[];
      final now = DateTime.now();
      final training = plan.train && plan.trainDays & (1 << (now.weekday % 7)) != 0;
      final ideas = await GemmaService.instance
          .mealIdeas(
            diet: MealIdeas.diets[plan.diet] ?? plan.diet,
            notes: plan.foodNotes,
            avoid: recent,
            training: training,
          )
          .timeout(const Duration(seconds: 90), onTimeout: () => null);
      if (!mounted) return;
      if (ideas == null) {
        state = state.copyWith(writing: false);
        return;
      }
      final (b, l, d) = ideas;
      await p.setString(_kAi, jsonEncode({'key': key, 'sh': state.shuffle, 'b': b, 'l': l, 'd': d}));
      await p.setStringList(_kRecent, [b, l, d, ...recent].take(9).toList());
      state = state.copyWith(aiIdeas: ideas, writing: false);
    } catch (_) {
      if (mounted) state = state.copyWith(writing: false);
    }
  }

  Future<void> setPlan(LifePlan plan) async {
    final old = state.plan;
    final food = plan.diet != old.diet || plan.foodNotes.trim() != old.foodNotes.trim() || plan.aiMeals != old.aiMeals;
    state = state.copyWith(plan: plan, shuffle: food ? 0 : null, aiIdeas: food ? null : state.aiIdeas);
    await _save();
    await sendAll();
  }

  /// "Others": the LLM writes new ideas (or the built-in list moves on).
  Future<void> shuffleMeals() async {
    state = state.copyWith(shuffle: state.shuffle + 1, aiIdeas: null);
    await _sendMeals();
    if (state.plan.aiMeals) {
      await _writeIdeas(fresh: true);
      await _sendMeals();
    }
  }

  /// The plan and today's meal ideas - on connect and after a change. The
  /// ideas at hand go at once; the LLM's follow when written.
  Future<void> sendAll() async {
    final ble = _ref.read(bleServiceProvider);
    if (ble.hasPhoneLink) {
      await ble.sendLink(state.plan.toWatch());
      await _sendMeals();
    }
    final before = state.meals;
    await _writeIdeas();
    if (state.meals != before) await _sendMeals();
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
