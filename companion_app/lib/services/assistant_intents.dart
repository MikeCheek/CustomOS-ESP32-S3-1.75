/// Understands the everyday things people ask a watch - timers, reminders,
/// alarms, music, weather, the time, the agenda... - with plain rules, so
/// they work instantly and without a language model. English and Italian.
/// Anything it doesn't recognise goes to the on-device LLM (see
/// GemmaService.assistantRoute), which can map it to the same actions or
/// just answer.
///
/// Pure Dart: no Flutter, so it's testable on its own
/// (test/assistant_intents_test.dart).
library;

enum IntentKind { timer, cancelTimer, reminder, music, weather, time, date, battery, agenda, findPhone, dnd, memos, message, none }

class AssistantIntent {
  final IntentKind kind;
  final bool italian;        // answer in Italian
  final int? seconds;        // timer
  final DateTime? at;        // reminder / alarm
  final String? text;        // reminder title, memo question
  final String? command;     // music: play pause next prev volUp volDown; message: who to
  final bool? on;            // dnd

  const AssistantIntent(this.kind,
      {this.italian = false, this.seconds, this.at, this.text, this.command, this.on});

  @override
  String toString() => '$kind(sec=$seconds at=$at text=$text cmd=$command on=$on it=$italian)';
}

String _norm(String s) => s
    .toLowerCase()
    .replaceAll('’', "'")
    .replaceAll(RegExp(r"[^\p{L}\p{N}:' ]", unicode: true), ' ')
    .replaceAll(RegExp(r'\s+'), ' ')
    .trim();

const _numberWords = <String, int>{
  'a': 1, 'an': 1, 'one': 1, 'two': 2, 'three': 3, 'four': 4, 'five': 5, 'six': 6, 'seven': 7, 'eight': 8,
  'nine': 9, 'ten': 10, 'eleven': 11, 'twelve': 12, 'fifteen': 15, 'twenty': 20, 'thirty': 30, 'forty': 40,
  'forty five': 45, 'fifty': 50, 'sixty': 60, 'ninety': 90,
  'un': 1, 'uno': 1, 'una': 1, "un'": 1, 'due': 2, 'tre': 3, 'quattro': 4, 'cinque': 5, 'sei': 6, 'sette': 7,
  'otto': 8, 'nove': 9, 'dieci': 10, 'undici': 11, 'dodici': 12, 'quindici': 15, 'venti': 20, 'trenta': 30,
  'quaranta': 40, 'quarantacinque': 45, 'cinquanta': 50, 'sessanta': 60, 'novanta': 90,
};

int? _num(String w) => int.tryParse(w) ?? _numberWords[w];

final _unitRe = RegExp(
    r"(\d+(?:[.,]\d+)?|forty five|[a-z']+) ?(hours?|hrs?|h|ore|ora|minutes?|mins?|minuti|minuto|m|seconds?|secs?|secondi|secondo|s)\b");

/// Seconds in "5 minutes", "1 hour and 30 minutes", "half an hour",
/// "mezz'ora", "90 seconds", "un quarto d'ora"... or null.
int? parseDuration(String raw) {
  final s = _norm(raw);
  if (RegExp(r"half an hour|mezz ?'? ?ora|mezzora").hasMatch(s)) return 1800;
  if (RegExp(r"quarter of an hour|quarto d ?'? ?ora").hasMatch(s)) return 900;
  var total = 0.0;
  var found = false;
  for (final m in _unitRe.allMatches(s)) {
    final word = m.group(1)!;
    final n = double.tryParse(word.replaceAll(',', '.')) ?? _num(word)?.toDouble();
    if (n == null) continue;
    final u = m.group(2)!;
    final mult = u.startsWith('h') || u.startsWith('or') ? 3600 : u.startsWith('m') ? 60 : 1;
    total += n * mult;
    found = true;
  }
  if (!found) return null;
  final secs = total.round();
  return secs > 0 && secs <= 24 * 3600 ? secs : null;
}

/// A time of day in the text ("at 7", "at 7:30", "at 7 pm", "alle 19",
/// "tomorrow at 9", "domani alle 8") as the next such moment after [now].
/// [morning]: a bare hour means the next morning, not later today (alarms).
DateTime? parseTimeOfDay(String raw, DateTime now, {bool morning = false}) {
  final s = _norm(raw);
  final m = RegExp(r"\b(?:at|for|alle|all'|per le|a)\s*(\d{1,2})(?:[:. ](\d{2}))?\s*(am|pm|a m|p m)?\b").firstMatch(s) ??
      RegExp(r"\b(\d{1,2}):(\d{2})\b").firstMatch(s);
  if (m == null) {
    if (RegExp(r'\bnoon\b|\bmezzogiorno\b').hasMatch(s)) return _next(now, 12, 0, s, true);
    if (RegExp(r'\bmidnight\b|\bmezzanotte\b').hasMatch(s)) return _next(now, 0, 0, s, true);
    return null;
  }
  var h = int.parse(m.group(1)!);
  final min = int.tryParse(m.group(2) ?? '') ?? 0;
  final ampm = m.groupCount >= 3 ? m.group(3) : null;
  if (ampm != null && ampm.startsWith('p') && h < 12) h += 12;
  if (ampm != null && ampm.startsWith('a') && h == 12) h = 0;
  if (ampm == null && RegExp(r'\b(evening|tonight|afternoon|stasera|pomeriggio|di sera)\b').hasMatch(s) && h < 12) h += 12;
  if (h > 23 || min > 59) return null;
  return _next(now, h, min, s, morning || ampm != null);
}

DateTime _next(DateTime now, int h, int m, String s, bool exact) {
  final tomorrow = RegExp(r'\btomorrow\b|\bdomani\b').hasMatch(s);
  var t = DateTime(now.year, now.month, now.day, h, m);
  if (tomorrow) return t.add(const Duration(days: 1));
  // "at 7" said at 9 in the morning means 7 pm if that's still to come,
  // otherwise tomorrow morning
  if (!t.isAfter(now)) {
    if (!exact && h < 12 && !RegExp(r'\b(morning|mattina)\b').hasMatch(s) &&
        DateTime(now.year, now.month, now.day, h + 12, m).isAfter(now)) {
      return DateTime(now.year, now.month, now.day, h + 12, m);
    }
    t = t.add(const Duration(days: 1));
  }
  return t;
}

bool _has(String s, String pattern) => RegExp(pattern).hasMatch(s);

bool _looksItalian(String s) => _has(s,
    r"\b(il|lo|la|di|che|per|tra|fra|alle|ricordami|imposta|metti|sveglia|svegliami|canzone|meteo|tempo fa|ore sono|domani|oggi|accendi|spegni|attiva|disattiva|cosa|dove|mio|miei|una|uno|un'|della|nei)\b");

/// What [text] asks for, or IntentKind.none.
AssistantIntent parseAssistantIntent(String text, DateTime now) {
  final s = _norm(text);
  final it = _looksItalian(s);
  AssistantIntent i(IntentKind k, {int? seconds, DateTime? at, String? text, String? command, bool? on}) =>
      AssistantIntent(k, italian: it, seconds: seconds, at: at, text: text, command: command, on: on);

  // ---- timers
  final timerWord = _has(s, r'\b(timer|countdown|conto alla rovescia|cronometro)\b');
  if (timerWord && _has(s, r'\b(cancel|stop|delete|remove|clear|annulla|ferma|cancella|elimina|togli)\b')) {
    return i(IntentKind.cancelTimer);
  }
  final dur = parseDuration(s);
  if (timerWord && dur != null) return i(IntentKind.timer, seconds: dur);
  final wake = _has(s, r'\b(wake me|alarm|sveglia|svegliami)\b');
  // "wake me in 20 minutes" / "sveglia tra 10 minuti": a timer
  if (wake && dur != null && _has(s, r'\b(in|tra|fra)\b') && parseTimeOfDay(s, now) == null) {
    return i(IntentKind.timer, seconds: dur);
  }

  // ---- alarms and reminders (into the phone's calendar, reminded on the watch)
  final remind = RegExp(
          r"\b(?:remind me(?: to| about| that)?|reminder(?: to| for)?|ricordami(?: di| che)?|promemoria(?: per| di)?)\s+(.*)")
      .firstMatch(s);
  if (remind != null || wake) {
    DateTime? at = parseTimeOfDay(s, now, morning: wake);
    if (at == null && dur != null && _has(s, r'\b(in|tra|fra)\b')) at = now.add(Duration(seconds: dur));
    if (at != null) {
      var title = remind?.group(1) ?? '';
      title = title
          .replaceAll(RegExp(r"\b(tomorrow|today|tonight|domani|oggi|stasera)\b"), '')
          .replaceAll(RegExp(r"\b(at|alle|all'|per le|a|in|tra|fra)\s+\S+(\s*(am|pm|minutes?|minuti|hours?|ore|ora|:\d\d))?"), '')
          .replaceAll(RegExp(r"\b\d{1,2}([:.]\d{2})?\b"), '')
          .replaceAll(RegExp(r'\s+'), ' ')
          .trim()
          .replaceFirst(RegExp(r'^(to|that|about|di|che|per) '), '');
      if (title.isEmpty) title = wake ? (it ? 'Sveglia' : 'Alarm') : (it ? 'Promemoria' : 'Reminder');
      // the words as they were written ("Luca", not "luca")
      final orig = {for (final w in text.split(RegExp(r'[\s,.!?;]+'))) w.toLowerCase(): w};
      title = title.split(' ').map((w) => orig[w] ?? w).join(' ');
      return i(IntentKind.reminder, at: at, text: title[0].toUpperCase() + title.substring(1));
    }
  }

  // ---- a message to someone: "tell Marco I'm late", "di' a Marco che arrivo tardi"
  final msg = RegExp(
    r"^(?:please )?(?:tell|text|message|send (?:a )?(?:message|text) to|write to|reply to|"
    r"di' a|dì a|di a|scrivi a|rispondi a|manda un messaggio a|invia un messaggio a|manda a)\s+"
    r"([\p{L}][\p{L}'-]*)\s+(?:that |saying |to say |che |dicendo |di )?(.+)$",
    unicode: true,
  ).firstMatch(s);
  if (msg != null && !const {'me', 'mi', 'us', 'ci', 'it', 'him', 'her', 'them'}.contains(msg.group(1))) {
    final orig = {for (final w in text.split(RegExp(r'[\s,!?;]+'))) w.toLowerCase().replaceAll(RegExp(r'[.]$'), ''): w};
    String back(String x) => x.split(' ').map((w) => orig[w] ?? w).join(' ');
    var body = back(msg.group(2)!.trim());
    if (body.isNotEmpty) body = body[0].toUpperCase() + body.substring(1);
    return i(IntentKind.message, command: back(msg.group(1)!), text: body);
  }

  // ---- music
  if (_has(s, r'\b(volume up|louder|turn it up|alza (il )?volume|più forte)\b')) return i(IntentKind.music, command: 'volUp');
  if (_has(s, r'\b(volume down|quieter|turn it down|abbassa (il )?volume|più piano)\b')) {
    return i(IntentKind.music, command: 'volDown');
  }
  if (_has(s, r'\b(next (song|track)|skip|canzone successiva|prossima canzone|brano successivo|salta|avanti)\b')) {
    return i(IntentKind.music, command: 'next');
  }
  if (_has(s, r'\b(previous (song|track)|last song|go back|canzone precedente|brano precedente|indietro)\b')) {
    return i(IntentKind.music, command: 'prev');
  }
  if (_has(s, r'\b(pause|stop (the )?music|metti in pausa|pausa|ferma la musica)\b')) return i(IntentKind.music, command: 'pause');
  if (_has(s, r'\b(play (some )?music|resume|play|riprendi|fai partire|metti (la )?musica)\b') &&
      !_has(s, r'\bplay(ed|ing)? (a |the )?game\b')) {
    return i(IntentKind.music, command: 'play');
  }

  // ---- phone & settings
  if (_has(s, r"\b(find my phone|where is my phone|where's my phone|ring my phone|trova (il )?(mio )?telefono|dov'è (il )?(mio )?telefono|dove è (il )?(mio )?telefono|dove si trova (il )?(mio )?telefono|fai squillare)\b")) {
    return i(IntentKind.findPhone);
  }
  if (_has(s, r'\b(do not disturb|dnd|non disturbare|modalità silenziosa|silenzioso|silenzia)\b')) {
    final off = _has(s, r'\b(off|disable|stop|turn off|disattiva|spegni|togli)\b');
    return i(IntentKind.dnd, on: !off);
  }
  if (_has(s, r'\b(battery|batteria)\b')) return i(IntentKind.battery);

  // ---- information
  if (_has(s, r'\b(weather|rain|raining|umbrella|temperature|forecast|hot outside|cold outside|meteo|piove|pioverà|ombrello|temperatura|previsioni|che tempo)\b')) {
    return i(IntentKind.weather);
  }
  if (_has(s, r"\b(what time|the time|what's the time|che ore|che ora)\b")) return i(IntentKind.time);
  if (_has(s, r"\b(what day|what's the date|what is the date|today's date|che giorno|che data|quanti ne abbiamo)\b")) {
    return i(IntentKind.date);
  }
  if (_has(s, r"\b(calendar|agenda|schedule|meeting|meetings|appointment|appointments|next event|what's next|what do i have|impegni|appuntamenti|appuntamento|calendario|riunione|cosa ho)\b")) {
    return i(IntentKind.agenda);
  }
  if (_has(s, r'\b(memo|memos|my notes|recording|recordings|voice notes|what did i say|nei miei|note vocali|registrazioni|cosa ho detto)\b')) {
    return i(IntentKind.memos, text: text.trim());
  }
  return i(IntentKind.none, text: text.trim());
}
