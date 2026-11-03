/// Puts the transcript and the speaker turns together: "who said what".
///
/// Whisper gives timed pieces of text, speaker diarization (sherpa-onnx)
/// gives timed turns with a speaker number. Each piece of text goes to the
/// speaker it overlaps most; consecutive pieces of the same speaker become
/// one line. Pure Dart, tested in test/speaker_merge_test.dart.
library;

/// A piece of the transcript with its time in the recording (seconds).
class TimedText {
  final double start, end;
  final String text;
  const TimedText(this.start, this.end, this.text);
}

/// A stretch of the recording where one speaker talks.
class SpeakerTurn {
  final double start, end;
  final int speaker;
  const SpeakerTurn(this.start, this.end, this.speaker);
}

/// One line of the conversation, stored with the memo.
class SpeakerLine {
  final int speaker; // 0, 1, ... in order of first appearance
  final double start, end;
  final String text;
  const SpeakerLine(this.speaker, this.start, this.end, this.text);

  Map<String, dynamic> toJson() => {
        'k': speaker,
        's': double.parse(start.toStringAsFixed(2)),
        'e': double.parse(end.toStringAsFixed(2)),
        't': text,
      };

  factory SpeakerLine.fromJson(Map<String, dynamic> j) => SpeakerLine(
        (j['k'] as num?)?.toInt() ?? 0,
        (j['s'] as num?)?.toDouble() ?? 0,
        (j['e'] as num?)?.toDouble() ?? 0,
        (j['t'] as String?) ?? '',
      );
}

/// Whisper's markers for no speech, which aren't anyone's words.
final _nonSpeech = RegExp(r'^\s*[\[(][^\])]*[\])]\s*$');

List<SpeakerLine> mergeSpeakers(List<TimedText> pieces, List<SpeakerTurn> turns) {
  if (pieces.isEmpty || turns.isEmpty) return const [];
  final order = <int, int>{}; // diarizer's number -> order of first appearance
  final out = <SpeakerLine>[];
  for (final p in pieces) {
    final text = p.text.trim();
    if (text.isEmpty || _nonSpeech.hasMatch(text)) continue;
    // Most overlap wins; with none (a gap), the nearest turn.
    final overlap = <int, double>{};
    for (final t in turns) {
      final o = (p.end < t.end ? p.end : t.end) - (p.start > t.start ? p.start : t.start);
      if (o > 0) overlap[t.speaker] = (overlap[t.speaker] ?? 0) + o;
    }
    int who;
    if (overlap.isNotEmpty) {
      who = overlap.entries.reduce((a, b) => b.value > a.value ? b : a).key;
    } else {
      final mid = (p.start + p.end) / 2;
      double dist(SpeakerTurn t) => mid < t.start ? t.start - mid : (mid > t.end ? mid - t.end : 0);
      who = turns.reduce((a, b) => dist(b) < dist(a) ? b : a).speaker;
    }
    final k = order.putIfAbsent(who, () => order.length);
    if (out.isNotEmpty && out.last.speaker == k) {
      final l = out.removeLast();
      out.add(SpeakerLine(k, l.start, p.end, '${l.text} $text'));
    } else {
      out.add(SpeakerLine(k, p.start, p.end, text));
    }
  }
  return out;
}

int speakerCount(List<SpeakerLine> lines) => lines.map((l) => l.speaker).toSet().length;

/// "Speaker 2", or the name the user gave them.
String speakerName(int speaker, Map<String, String> names) {
  final n = names['$speaker']?.trim() ?? '';
  return n.isNotEmpty ? n : 'Speaker ${speaker + 1}';
}

/// The conversation as text, one line per turn - what the summary model reads.
String labeledTranscript(List<SpeakerLine> lines, Map<String, String> names) =>
    lines.map((l) => '${speakerName(l.speaker, names)}: ${l.text}').join('\n');
