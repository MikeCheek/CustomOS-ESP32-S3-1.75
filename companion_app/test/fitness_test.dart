import 'package:amoled_companion/providers/companion_provider.dart';
import 'package:amoled_companion/providers/fitness_provider.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUp(() => SharedPreferences.setMockInitialValues({}));

  test('a workout from the watch: local start time, pace', () {
    // 2026-10-08 07:30:00 local, counted as if UTC
    final st = DateTime.utc(2026, 10, 8, 7, 30).millisecondsSinceEpoch ~/ 1000;
    final w = Workout.fromWatch({'e': 'wo', 'ty': 'Outdoor Run', 'st': st, 'du': 1800, 'm': 5000, 'kc': 310, 'stp': 4800, 'gps': 1});
    expect(w.start, DateTime(2026, 10, 8, 7, 30));
    expect(w.gps, isTrue);
    expect(w.pace, const Duration(minutes: 6));
    expect(Workout.fromJson(w.toJson())!.meters, 5000);
  });

  test('the status report: today, goals and the last days', () async {
    final n = FitnessNotifier();
    await Future<void>.delayed(Duration.zero);
    n.onReport({
      'e': 'hi', 'mv': 120, 'ex': 18, 'sd': 4, 'st': 5000, 'gm': 400, 'ge': 30, 'gs': 12,
      'fh': [
        [20261007, 350, 41, 11, 9000],
        [20261006, 200, 12, 8, 6000],
      ],
    });
    expect(n.state.today!.moveKcal, 120);
    expect(n.state.moveGoal, 400);
    expect(n.state.days.length, 2);
    expect(n.state.days.first.date, DateTime(2026, 10, 7));
    await n.addWorkout({'ty': 'Cycling', 'st': 1000, 'du': 600});
    await n.addWorkout({'ty': 'Cycling', 'st': 1000, 'du': 600});   // the watch sent it again
    expect(n.state.workouts.length, 1);
  });

  test('to-do keys are short and stable', () {
    final a = CompanionNotifier.todoKey('memo_1.wav|Call Marco');
    expect(a, CompanionNotifier.todoKey('memo_1.wav|Call Marco'));
    expect(a.length, 8);
    expect(a, isNot(CompanionNotifier.todoKey('memo_1.wav|Call Marta')));
  });
}
