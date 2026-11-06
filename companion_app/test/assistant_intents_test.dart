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

  test('a message to someone, in English and Italian', () {
    final a = p("Tell Marco I'm 10 minutes late");
    expect((a.kind, a.command, a.text), (IntentKind.message, 'Marco', "I'm 10 minutes late"));
    final b = p('Send a message to Giulia saying see you at the station');
    expect((b.kind, b.command, b.text), (IntentKind.message, 'Giulia', 'See you at the station'));
    final c = p("Di' a Luca che arrivo tardi");
    expect((c.kind, c.command, c.text, c.italian), (IntentKind.message, 'Luca', 'Arrivo tardi', true));
    final d = p('Scrivi a Sara che ci vediamo domani');
    expect((d.kind, d.command), (IntentKind.message, 'Sara'));
    expect(p('Tell me a joke').kind, IntentKind.none); // "me" is no one to write to
  });

  test('more in Italian, without the language model', () {
    expect(p('Imposta un timer di 10 minuti').seconds, 600);
    expect(p('Che tempo fa domani?').kind, IntentKind.weather);
    expect(p('Che ore sono?').kind, IntentKind.time);
    expect(p('Che giorno è oggi?').kind, IntentKind.date);
    expect(p('Metti la modalità silenziosa').kind, IntentKind.dnd);
    expect(p('Canzone successiva').command, 'next');
    expect(p('Alza il volume').command, 'volUp');
    expect(p('Quanta batteria ho?').kind, IntentKind.battery);
    expect(p('Dove è il mio telefono?').kind, IntentKind.findPhone);
    expect(p('Cosa ho oggi in calendario?').kind, IntentKind.agenda);
    expect(p('Ricordami di chiamare la mamma alle 18').kind, IntentKind.reminder);
    expect(p('Che ore sono?').italian, isTrue);
  });
}
