/// The evening recap (AI models > Spoken answers > Evening recap): at the
/// time set, the day in a few sentences - the memos recorded, the to-dos
/// left, how tomorrow starts - said on the watch and kept on its Smart Stack
/// (RECAP card) until the morning. Worded by the language model when there
/// is one, else put together from the parts ([recapRules]).
class RecapInput {
  final List<String> memoTitles; // today's memos, oldest first
  final List<String> memoSummaries;
  final List<String> openTodos;
  final (String title, DateTime start, bool allDay)? tomorrowFirst;
  const RecapInput({
    this.memoTitles = const [],
    this.memoSummaries = const [],
    this.openTodos = const [],
    this.tomorrowFirst,
  });
  bool get isEmpty => memoTitles.isEmpty && openTodos.isEmpty && tomorrowFirst == null;
}

String _hm(DateTime t) => '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';

String _list(List<String> xs, int n) {
  final shown = xs.take(n).toList();
  final more = xs.length - shown.length;
  if (shown.length == 1) return shown.first;
  final head = shown.sublist(0, shown.length - 1).join(', ');
  return more > 0 ? '$head, ${shown.last} and $more more' : '$head and ${shown.last}';
}

/// Without the language model.
String recapRules(RecapInput r) {
  final p = <String>[];
  final m = r.memoTitles.where((t) => t.trim().isNotEmpty).toList();
  if (m.isNotEmpty) p.add('Today you recorded ${m.length} memo${m.length == 1 ? '' : 's'}: ${_list(m, 3)}.');
  if (r.openTodos.isNotEmpty) {
    p.add(
      r.openTodos.length == 1
          ? 'One to-do is still open: ${r.openTodos.first}.'
          : '${r.openTodos.length} to-dos are still open, first ${r.openTodos.first}.',
    );
  } else if (m.isNotEmpty) {
    p.add('No to-dos left.');
  }
  final t = r.tomorrowFirst;
  if (t != null) {
    p.add(t.$3 ? 'Tomorrow: ${t.$1}.' : 'Tomorrow starts with ${t.$1} at ${_hm(t.$2)}.');
  } else {
    p.add('Nothing on the calendar tomorrow morning.');
  }
  return p.join(' ');
}

String recapPrompt(RecapInput r) => [
  'Write my evening recap: 2 or 3 short, warm sentences to be read aloud. No lists, no markdown.',
  if (r.memoSummaries.isNotEmpty) 'Today\'s voice memos: ${r.memoSummaries.take(5).join(' | ')}',
  if (r.openTodos.isNotEmpty) 'Open to-dos: ${r.openTodos.take(5).join('; ')}',
  if (r.tomorrowFirst != null)
    'Tomorrow starts with: ${r.tomorrowFirst!.$1}${r.tomorrowFirst!.$3 ? ' (all day)' : ' at ${_hm(r.tomorrowFirst!.$2)}'}',
  'Reply with the recap only. /no_think',
].join('\n');

/// The model's reply as the recap, or null when it isn't usable.
String? cleanRecap(String? out) {
  if (out == null) return null;
  final t = out.replaceAll(RegExp(r'[*#`]'), '').replaceAll(RegExp(r'\s+'), ' ').trim();
  if (t.length < 20) return null;
  return t.length > 400 ? '${t.substring(0, 397).trimRight()}...' : t;
}

/// Whether the recap is due at [now] for a [minute] of the day (20:30 = 1230),
/// not yet given on [lastDay] (yyyymmdd). It stays due until midnight, so a
/// watch that connects late still gets it.
bool recapDue(DateTime now, int minute, int lastDay) {
  final day = now.year * 10000 + now.month * 100 + now.day;
  return day != lastDay && now.hour * 60 + now.minute >= minute;
}
