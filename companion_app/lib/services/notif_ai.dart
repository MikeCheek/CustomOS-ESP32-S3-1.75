import 'dart:async';
import 'dart:io';
import 'gemma_service.dart';
import 'smart_replies.dart';
import 'text_insights.dart';
import 'whisper_service.dart';

/// A notification the phone forwarded, kept for the AI features.
class LoggedNotif {
  final int id;
  final String app, title, text;
  final bool canReply;
  final DateTime at;
  List<String> replies = const []; // smart replies, once written
  String? translation;
  LoggedNotif(this.id, this.app, this.title, this.text, this.canReply, this.at);
}

/// Recent notifications and what the on-phone LLM does with them - the same
/// for the watch ({"e":"digest"} / {"e":"tr"}) and the app's Messages screen.
class NotifAi {
  NotifAi._();
  static final NotifAi instance = NotifAi._();

  static const _max = 40;
  final List<LoggedNotif> _log = [];
  final _changes = StreamController<void>.broadcast();
  Stream<void> get changes => _changes.stream;
  List<LoggedNotif> get recent => List.unmodifiable(_log);

  void add(int id, String app, String title, String text, bool canReply) {
    _log.removeWhere((n) => n.id == id && id != 0);
    _log.insert(0, LoggedNotif(id, app, title, text, canReply, DateTime.now()));
    if (_log.length > _max) _log.removeRange(_max, _log.length);
    _changes.add(null);
  }

  void remove(int id) {
    final before = _log.length;
    _log.removeWhere((n) => n.id == id);
    if (_log.length != before) _changes.add(null);
  }

  LoggedNotif? byId(int id) => _log.where((n) => n.id == id).firstOrNull;

  void setReplies(int id, List<String> r) {
    final n = byId(id);
    if (n == null) return;
    n.replies = r;
    _changes.add(null);
  }

  /// The user's language (the phone's), as a name the model understands.
  static String userLanguage() {
    final code = Platform.localeName.split(RegExp('[_-]')).first.toLowerCase();
    return WhisperService.languageName(code.isEmpty ? 'en' : code);
  }

  /// A short summary of the last day's notifications, what needs an answer
  /// first. Throws with a readable message.
  /// The last "Catch me up" summary (the watch can ask to hear it).
  String? lastDigest;

  Future<String> digest() async {
    final since = DateTime.now().subtract(const Duration(hours: 24));
    final items = _log.where((n) => n.at.isAfter(since)).take(25).toList();
    if (items.isEmpty) throw Exception('No notifications in the last day');
    final text = await GemmaService.instance.digest([
      for (final n in items.reversed) '${n.app}${n.title.isEmpty ? '' : ' - ${n.title}'}: ${n.text}',
    ], userLanguage());
    if (text == null) throw Exception('Install a summary model in the companion app (Transcription & AI)');
    lastDigest = text;
    return text;
  }

  /// [id]'s text in the user's language. Throws with a readable message.
  Future<String> translate(int id) async {
    final n = byId(id);
    if (n == null) throw Exception('That notification is gone from the phone');
    if (n.translation != null) return n.translation!;
    final target = userLanguage();
    final src = TextInsights.detectLanguage(n.text).code;
    if (src.isNotEmpty && WhisperService.languageName(src) == target) {
      throw Exception('It is already in $target');
    }
    final out = await GemmaService.instance.translate(n.text, target);
    if (out == null) throw Exception('Install a summary model in the companion app (Transcription & AI)');
    n.translation = out;
    _changes.add(null);
    return out;
  }

  /// Smart replies for [id], written now if they aren't yet.
  Future<List<String>> repliesFor(int id) async {
    final n = byId(id);
    if (n == null) return const [];
    if (n.replies.isNotEmpty) return n.replies;
    final r = await GemmaService.instance.suggestReplies(n.app, n.title, n.text) ?? const [];
    setReplies(id, r.take(SmartReplies.maxReplies).toList());
    return n.replies;
  }
}
