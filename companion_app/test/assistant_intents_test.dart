import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/assistant_intents.dart';

void main() {
  final now = DateTime(2026, 11, 2, 9, 15);
  AssistantIntent p(String s) => parseAssistantIntent(s, now);

  test('timers', () {
    expect(p('Set a timer for 5 minutes').seconds, 300);
    expect(p('Set a timer for one hour and 30 minutes').seconds, 5400);
    expect(p('Start a 10 minute timer').seconds, 600);
    expect(p("Timer di mezz'ora").seconds, 1800);
    expect(p('Wake me in 20 minutes').kind, IntentKind.timer);
    expect(p('Cancel the timer').kind, IntentKind.cancelTimer);
    expect(p('Annulla il timer').kind, IntentKind.cancelTimer);
  });

  test('reminders and alarms', () {
    final r = p('Remind me to call mom at 6 pm');
    expect(r.kind, IntentKind.reminder);
    expect(r.text, 'Call mom');
    expect(r.at, DateTime(2026, 11, 2, 18, 0));
    expect(p('Remind me to take the pizza out in 20 minutes').at, DateTime(2026, 11, 2, 9, 35));
    expect(p('Ricordami domani alle 8:30 di chiamare Luca').text, 'Chiamare Luca');
    expect(p('Wake me up at 7').at, DateTime(2026, 11, 3, 7, 0));
    expect(p('Set an alarm for 6:45 am').at, DateTime(2026, 11, 3, 6, 45));
  });

  test('music, info, phone', () {
    expect(p('Next song').command, 'next');
    expect(p('Metti in pausa').command, 'pause');
    expect(p('Alza il volume').command, 'volUp');
    expect(p('Che tempo fa?').kind, IntentKind.weather);
    expect(p('What time is it?').kind, IntentKind.time);
    expect(p("What's on my calendar today?").kind, IntentKind.agenda);
    expect(p('Find my phone').kind, IntentKind.findPhone);
    expect(p('Disattiva non disturbare').on, false);
    expect(p('What did I say about the trip in my memos?').kind, IntentKind.memos);
    expect(p('Who wrote the Divine Comedy?').kind, IntentKind.none);
  });
}
