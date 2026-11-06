/// The watch face's "Today" complication (firmware complications.cpp,
/// {"t":"brief","v":..,"l":..}): how many things are left today and the
/// one to remember - from the calendar, the memos' to-dos and the latest
/// memo, put in a few words by the language model when there is one.
class TodayBrief {
  final String value; // "3" (things left today), "-" when nothing is
  final String label; // "Call Marco first", "Design review", "Free day"
  final String line; // the whole day in a sentence, for the Smart Stack card and to say aloud
  const TodayBrief(this.value, this.label, [this.line = '']);

  Map<String, dynamic> toWatch() => {'t': 'brief', 'v': value, 'l': label, if (line.isNotEmpty) 'x': line};

  @override
  bool operator ==(Object o) => o is TodayBrief && o.value == value && o.label == label && o.line == line;
  @override
  int get hashCode => Object.hash(value, label, line);
}

/// The rest of today in a sentence: "2 events left: Design review at 15:00,
/// Gym at 19:00. To do: Call Marco." / "Nothing left on the calendar today."
String briefLine({required DateTime now, required List<BriefEvent> events, required List<String> todos}) {
  String hm(DateTime t) => '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
  final left = eventsLeftToday(events, now);
  final parts = <String>[
    if (left.isEmpty)
      'Nothing left on the calendar today.'
    else
      '${left.length} event${left.length == 1 ? '' : 's'} left: '
          '${left.take(3).map((e) => e.$3 ? e.$1 : '${e.$1} at ${hm(e.$2)}').join(', ')}.',
    if (todos.isNotEmpty)
      'To do: ${todos.take(2).join(', ')}${todos.length > 2 ? ' and ${todos.length - 2} more' : ''}.',
  ];
  final s = parts.join(' ');
  return s.length > 118 ? '${s.substring(0, 115).trimRight()}...' : s;
}

/// Today's events still to come, as (title, start, all day).
typedef BriefEvent = (String title, DateTime start, bool allDay);

List<BriefEvent> eventsLeftToday(List<BriefEvent> events, DateTime now) => [
  for (final e in events)
    if (e.$1.trim().isNotEmpty &&
        e.$2.year == now.year &&
        e.$2.month == now.month &&
        e.$2.day == now.day &&
        (e.$3 || !e.$2.isBefore(now)))
      e,
]..sort((a, b) => a.$2.compareTo(b.$2));

/// Without the language model: the next event, else the first to-do,
/// else the latest memo's title, else "Free day".
TodayBrief ruleBrief({
  required DateTime now,
  required List<BriefEvent> events,
  required List<String> todos,
  String? memoTitle,
}) {
  final left = eventsLeftToday(events, now);
  final n = left.length + todos.length;
  final timed = left.where((e) => !e.$3).toList();
  final label = timed.isNotEmpty
      ? timed.first.$1
      : left.isNotEmpty
      ? left.first.$1
      : todos.isNotEmpty
      ? todos.first
      : (memoTitle != null && memoTitle.trim().isNotEmpty ? memoTitle : 'Free day');
  return TodayBrief(
    n == 0 ? '-' : (n > 9 ? '9+' : '$n'),
    shortLabel(label),
    briefLine(now: now, events: events, todos: todos),
  );
}

/// At most [max] characters, cut at a word ("Design review with the team" -> "Design review").
String shortLabel(String s, {int max = 18}) {
  final t = s.replaceAll(RegExp(r'\s+'), ' ').trim();
  if (t.length <= max) return t;
  final cut = t.lastIndexOf(' ', max);
  return t.substring(0, cut > 4 ? cut : max);
}

/// What the language model is told to put in a few words.
String briefPrompt({
  required DateTime now,
  required List<BriefEvent> events,
  required List<String> todos,
  String? memoSummary,
  String? weather,
}) {
  String hm(DateTime t) => '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
  final left = eventsLeftToday(events, now);
  return [
    'It is ${hm(now)}. What is left of my day:',
    if (left.isEmpty) '- no more events today',
    for (final e in left.take(6)) '- ${e.$3 ? 'all day' : hm(e.$2)}: ${e.$1}',
    for (final t in todos.take(5)) '- to do: $t',
    if (memoSummary != null && memoSummary.trim().isNotEmpty) 'My latest voice memo: ${memoSummary.trim()}',
    if (weather != null) 'Weather: $weather',
    'In 2 to 4 words, the one thing I should keep in mind for the rest of today. '
        'Reply with those words only, no quotes. /no_think',
  ].join('\n');
}

/// The model's reply as a label, or null when it isn't usable.
String? cleanHeadline(String? out) {
  if (out == null) return null;
  final line = out.split('\n').map((l) => l.trim()).firstWhere((l) => l.isNotEmpty, orElse: () => '');
  final t = line.replaceAll(RegExp(r'''^["'*#\-• ]+|["'*.!• ]+$'''), '').trim();
  if (t.length < 3 || t.split(' ').length > 6) return null;
  return shortLabel(t);
}
