import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/evening_recap.dart';

void main() {
  test('the day put together without the model', () {
    final s = recapRules(
      RecapInput(
        memoTitles: ['Trip budget', 'Call with Marco', 'Gym plan', 'Groceries'],
        openTodos: ['Book the hotel', 'Pay the gym'],
        tomorrowFirst: ('Standup', DateTime(2026, 11, 7, 9, 30), false),
      ),
    );
    expect(
      s,
      'Today you recorded 4 memos: Trip budget, Call with Marco, Gym plan and 1 more. '
      '2 to-dos are still open, first Book the hotel. Tomorrow starts with Standup at 09:30.',
    );
  });

  test('a quiet day', () {
    expect(recapRules(const RecapInput()), 'Nothing on the calendar tomorrow morning.');
    expect(recapRules(const RecapInput(memoTitles: ['Ideas'])), contains('No to-dos left.'));
  });

  test('due once a day, from the time set until midnight', () {
    final at = DateTime(2026, 11, 6, 20, 29);
    expect(recapDue(at, 20 * 60 + 30, 0), isFalse);
    expect(recapDue(at.add(const Duration(minutes: 1)), 20 * 60 + 30, 0), isTrue);
    expect(recapDue(DateTime(2026, 11, 6, 23, 50), 20 * 60 + 30, 20261106), isFalse);
  });

  test('the model\'s reply is cleaned, a too-short one dropped', () {
    expect(
      cleanRecap('**A calm day.** You recorded two memos and have one to-do left.'),
      'A calm day. You recorded two memos and have one to-do left.',
    );
    expect(cleanRecap('ok'), isNull);
  });
}
