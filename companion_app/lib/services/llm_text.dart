/// What the on-phone language model says, cleaned up for people.
///
/// Small models sometimes leak their chat template's control tokens into the
/// answer - "<|channel|>thought", "<end_of_turn>", "<think>..." - and can get
/// stuck repeating them. This keeps the answer only (the "final" channel when
/// the model uses channels), drops every control token, and returns null when
/// what's left is garbled (mostly one word over and over), so the caller can
/// try again or say so instead of showing it.
String? cleanLlmText(String raw) {
  var s = raw;
  // channels (harmony-style): the final one is the answer
  final fin = RegExp(r'<\|?channel\|?>\s*final\s*(?:<\|?message\|?>)?').allMatches(s).toList();
  if (fin.isNotEmpty) s = s.substring(fin.last.end);
  // thinking, closed or left open
  s = s.replaceAll(RegExp(r'<think>[\s\S]*?</think>'), '');
  final open = s.indexOf('<think>');
  if (open >= 0) s = s.substring(0, open);
  // a channel name right after a channel token ("<|channel|>thought?", "<|channel>analysis")
  s = s.replaceAll(RegExp(r'<\|?channel\|?>\s*(?:thought|analysis|commentary|final)?\??', caseSensitive: false), ' ');
  // any other control token: <|...|>, <|word>, <word|>, <start_of_turn>, <eos>...
  s = s.replaceAll(RegExp(r'<\|[^<>]{0,40}\|?>|<[a-z_]{1,30}\|>'), ' ');
  s = s.replaceAll(RegExp(r'</?(?:start_of_turn|end_of_turn|eos|bos|pad|s|unk)>'), ' ');
  s = s.replaceAll(RegExp(r'^\s*(?:model|assistant)\s*\n', caseSensitive: false), '');
  s = s.replaceAll(RegExp(r'[ \t]+'), ' ').replaceAll(RegExp(r'\n{3,}'), '\n\n').trim();
  if (s.isEmpty || isGarbled(s)) return null;
  // mostly control tokens with scraps of words between them: nothing was really said
  final tokens = RegExp(r'<\|?[a-z_]{1,30}\|?>').allMatches(raw).length;
  if (tokens >= 4 && s.length < raw.length * 0.25) return null;
  return s;
}

/// Mostly the same word again and again, or leftover template words.
bool isGarbled(String s) {
  final words = s.toLowerCase().split(RegExp(r'[^\p{L}\p{N}]+', unicode: true)).where((w) => w.length >= 3).toList();
  if (words.length < 8) return false;
  final counts = <String, int>{};
  for (final w in words) {
    counts[w] = (counts[w] ?? 0) + 1;
  }
  final top = counts.values.reduce((a, b) => a > b ? a : b);
  const leaked = {'channel', 'thought', 'message', 'user', 'assistant', 'model'};
  final junk = words.where(leaked.contains).length;
  return top > words.length * 0.4 || junk > words.length * 0.3;
}
