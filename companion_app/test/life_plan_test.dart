import 'dart:convert';

import 'package:amoled_companion/services/gemma_service.dart';
import 'package:amoled_companion/services/life_plan.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('the plan goes to the watch in its own keys and comes back the same', () {
    final p = const LifePlan().edit((j) {
      j['w']['g'] = 10;
      j['so']['on'] = 1;
      j['so']['who'] = 'Mum';
      j['diet'] = 'vegan';
    });
    final w = p.toWatch();
    expect(w['t'], 'life');
    expect(w.containsKey('diet'), isFalse);   // phone-only
    expect((w['w'] as Map)['g'], 10);
    expect(utf8.encode(jsonEncode(w)).length, lessThan(900));   // one link message
    final back = LifePlan.fromJson(jsonDecode(jsonEncode(p.toJson())) as Map<String, dynamic>);
    expect(back.waterGoal, 10);
    expect(back.socialWho, 'Mum');
    expect(back.diet, 'vegan');
  });

  test('meal ideas fit the watch, follow the diet and change by day', () {
    for (final diet in MealIdeas.diets.keys) {
      for (var d = 0; d < 14; d++) {
        final (b, l, dn) = MealIdeas.forDay(diet, DateTime(2026, 11, 1).add(Duration(days: d)));
        for (final x in [b, l, dn]) {
          expect(x.length, lessThan(70));
        }
      }
    }
    final a = MealIdeas.forDay('vegan', DateTime(2026, 11, 4));
    final b = MealIdeas.forDay('vegan', DateTime(2026, 11, 5));
    expect(a, isNot(b));
    expect(MealIdeas.forDay('vegan', DateTime(2026, 11, 4), shuffle: 1), isNot(a));
  });

  test('a day from the watch', () {
    final d = LifeDay.fromWatch({'e': 'life', 'd': 20261104, 'w': 3, 'st': 1, 'mi': 2, 'tr': 12, 'me': 2, 'so': 0, 'wd': 0, 'sc': 42})!;
    expect(d.date, DateTime(2026, 11, 4));
    expect(d.score, 42);
    expect(LifeDay.fromRow(d.toRow())!.water, 3);
  });

  test('the local LLM\'s meal ideas: parsed from messy answers, fitted to the watch', () {
    const out = 'Sure! Here are some ideas:\n**Breakfast:** Porridge with apples, cinnamon and walnuts.\n'
        '- Lunch - Warm farro salad with roasted squash, rocket and feta cheese and a lemony olive oil dressing\n'
        'Dinner: Baked trout with potatoes and green beans';
    final r = GemmaService.parseMealIdeas(out)!;
    expect(r.$1, 'Porridge with apples, cinnamon and walnuts');
    expect(r.$2.startsWith('Warm farro salad'), isTrue);
    expect(r.$2.length, lessThanOrEqualTo(66));
    expect(r.$3, 'Baked trout with potatoes and green beans');
    expect(GemmaService.parseMealIdeas('Breakfast: eggs\nLunch: soup'), isNull);   // dinner missing
  });
}
