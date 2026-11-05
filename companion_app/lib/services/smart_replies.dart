import 'package:shared_preferences/shared_preferences.dart';

/// Smart replies: for a message the watch can answer, the on-phone LLM
/// suggests a few short replies written for it; the watch shows them first
/// in its reply list ({"t":"sr"}, COMPANION_PROTOCOL.md).
class SmartReplies {
  static const maxReplies = 3;
  static const maxChars = 38; // the watch's quick reply length
  static const _key = 'smart_replies_on';

  static Future<bool> enabled() async => (await SharedPreferences.getInstance()).getBool(_key) ?? true;
  static Future<void> setEnabled(bool on) async => (await SharedPreferences.getInstance()).setBool(_key, on);

  static String prompt(String app, String sender, String message) =>
      'A message arrived on the user\'s phone (app: $app${sender.isEmpty ? '' : ', from: $sender'}):\n'
      '"$message"\n\n'
      'Write $maxReplies different short replies the user could send back, in the language of the message, '
      'each under $maxChars characters: one positive, one neutral or asking for time, one natural follow-up. '
      'One reply per line, no numbering, no quotes, no explanation. /no_think';

  /// The model's answer -> up to [maxReplies] clean, distinct replies.
  static List<String> parse(String raw) {
    final out = <String>[];
    for (var l in raw.replaceAll(RegExp(r'<think>[\s\S]*?</think>'), '').split('\n')) {
      l = l
          .trim()
          .replaceFirst(RegExp(r'^(\d+[.)]|[-*•])\s*'), '') // list markers
          .replaceAll(RegExp(r'^["“”\x27]+|["“”\x27]+$'), '') // quotes
          .trim();
      if (l.isEmpty || l.endsWith(':') || l.length > maxChars + 12) continue;
      if (l.length > maxChars) {
        final cut = l.substring(0, maxChars);
        final sp = cut.lastIndexOf(' ');
        l = '${(sp > 20 ? cut.substring(0, sp) : cut).trimRight()}…';
      }
      if (out.any((o) => o.toLowerCase() == l.toLowerCase())) continue;
      out.add(l);
      if (out.length == maxReplies) break;
    }
    return out;
  }
}
