import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/today_brief.dart';

void main() {
  final now = DateTime(2026, 11, 6, 13, 0);

  test('counts what is left today and names the next event', () {
    final b = ruleBrief(
      now: now,
      events: [
        ('Standup', DateTime(2026, 11, 6, 9), false), // over
        ('Design review with the whole team', DateTime(2026, 11, 6, 15), false),
        ('Dentist', DateTime(2026, 11, 7, 10), false), // tomorrow
        ('Holiday', DateTime(2026, 11, 6), true),
      ],
      todos: ['Call Marco'],
    );
    expect(b.value, '3');
    expect(b.label, 'Design review with');
  });

  test('no events: the first to-do, then the memo, then a free day', () {
    expect(ruleBrief(now: now, events: [], todos: ['Call Marco']).label, 'Call Marco');
    expect(ruleBrief(now: now, events: [], todos: [], memoTitle: 'Trip budget').label, 'Trip budget');
    final free = ruleBrief(now: now, events: [], todos: []);
    expect((free.value, free.label), ('-', 'Free day'));
  });

  test('the model\'s words are cleaned, a rambling reply is dropped', () {
    expect(cleanHeadline('"Call Marco first."'), 'Call Marco first');
    expect(cleanHeadline('\n- Prep the review\n'), 'Prep the review');
    expect(cleanHeadline('You have a lot of things going on today and should really focus'), isNull);
    expect(cleanHeadline(null), isNull);
  });

  test('the prompt lists only what is left today', () {
    final p = briefPrompt(
      now: now,
      events: [('Standup', DateTime(2026, 11, 6, 9), false), ('Review', DateTime(2026, 11, 6, 15), false)],
      todos: ['Call Marco'],
      weather: '18°C, rain',
    );
    expect(p, contains('15:00: Review'));
    expect(p, isNot(contains('Standup')));
    expect(p, contains('to do: Call Marco'));
  });
}
