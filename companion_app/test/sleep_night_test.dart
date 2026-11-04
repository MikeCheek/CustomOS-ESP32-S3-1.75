import 'package:amoled_companion/providers/sleep_provider.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('a night that began in the evening started the day before its morning', () {
    final n = SleepNight.fromRow([20261104, 23 * 60 + 12, 366, 209, 7])!;
    expect(n.start, DateTime(2026, 11, 3, 23, 12));
    expect(n.end, DateTime(2026, 11, 4, 5, 25));
  });

  test('one after midnight starts on its morning', () {
    final n = SleepNight.fromRow([20261104, 45, 400, 200, 20])!;
    expect(n.start, DateTime(2026, 11, 4, 0, 45));
    expect(n.toRow(), [20261104, 45, 400, 200, 20]);
  });
}
