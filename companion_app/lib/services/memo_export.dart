import 'dart:io';

import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';

import 'ble_protocol.dart';
import 'speaker_merge.dart';

/// Memos out of the app: the audio, the transcript as text, or everything
/// (title, summary, to-dos, people, the transcript with who said what) as
/// Markdown - one memo, a collection, or all the to-dos.
class MemoExport {
  static String transcriptOf(RecordingEntry e) =>
      e.speakers.isNotEmpty ? labeledTranscript(e.speakers, e.speakerNames) : e.transcript;

  static String _date(DateTime? d) => d == null
      ? ''
      : '${d.year}-${d.month.toString().padLeft(2, '0')}-${d.day.toString().padLeft(2, '0')} '
            '${d.hour.toString().padLeft(2, '0')}:${d.minute.toString().padLeft(2, '0')}';

  /// One memo as Markdown.
  static String markdown(RecordingEntry e, {int level = 1}) {
    final h = '#' * level;
    final b = StringBuffer('$h ${e.displayTitle}\n\n');
    final meta = [
      if (e.recordedAt != null) _date(e.recordedAt),
      if (e.durationSec > 0) '${e.durationSec ~/ 60}:${(e.durationSec % 60).toString().padLeft(2, '0')}',
      if (e.topics.isNotEmpty) e.topics.join(', '),
    ];
    if (meta.isNotEmpty) b.writeln('_${meta.join(' · ')}_\n');
    if (e.summary.isNotEmpty) b.writeln('${e.summary}\n');
    if (e.actions.isNotEmpty) {
      b.writeln('$h# To do\n');
      for (final a in e.actions) {
        b.writeln('- [ ] $a');
      }
      b.writeln();
    }
    if (e.people.isNotEmpty) b.writeln('**People:** ${e.people.join(', ')}\n');
    final t = transcriptOf(e);
    if (t.isNotEmpty) b.writeln('$h# Transcript\n\n$t\n');
    return b.toString();
  }

  /// Several memos (a collection, or a search) as one Markdown document.
  static String markdownAll(String title, List<RecordingEntry> memos) {
    final b = StringBuffer('# $title\n\n');
    for (final e in memos) {
      b.writeln(markdown(e, level: 2));
    }
    return b.toString();
  }

  /// Every open to-do, by memo.
  static String todos(List<RecordingEntry> memos) {
    final b = StringBuffer('# To do\n\n');
    for (final e in memos.where((m) => m.actions.isNotEmpty)) {
      b.writeln('## ${e.displayTitle}${e.recordedAt != null ? ' (${_date(e.recordedAt)})' : ''}\n');
      for (final a in e.actions) {
        b.writeln('- [ ] $a');
      }
      b.writeln();
    }
    return b.toString();
  }

  static String _safe(String s) {
    final t = s.replaceAll(RegExp(r'[^\w\- ]+'), '').trim().replaceAll(' ', '_');
    return t.isEmpty ? 'memo' : (t.length > 60 ? t.substring(0, 60) : t);
  }

  static Future<void> _shareText(String name, String text, String subject) async {
    final dir = await getTemporaryDirectory();
    final f = File('${dir.path}/$name');
    await f.writeAsString(text);
    await SharePlus.instance.share(ShareParams(files: [XFile(f.path)], subject: subject, title: subject));
  }

  static Future<void> shareMarkdown(RecordingEntry e) =>
      _shareText('${_safe(e.displayTitle)}.md', markdown(e), e.displayTitle);

  static Future<void> shareTranscript(RecordingEntry e) =>
      _shareText('${_safe(e.displayTitle)}.txt', transcriptOf(e), e.displayTitle);

  static Future<void> shareAll(String title, List<RecordingEntry> memos) =>
      _shareText('${_safe(title)}.md', markdownAll(title, memos), title);

  static Future<void> shareTodos(List<RecordingEntry> memos) => _shareText('To_do.md', todos(memos), 'To do');

  /// The recording itself (WAV or MP3), from the phone's copy.
  static Future<bool> shareAudio(RecordingEntry e, String path) async {
    if (!await File(path).exists()) return false;
    await SharePlus.instance.share(ShareParams(files: [XFile(path)], subject: e.displayTitle, title: e.displayTitle));
    return true;
  }
}
