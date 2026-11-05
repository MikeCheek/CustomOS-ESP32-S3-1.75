import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/speaker_merge.dart';

void main() {
  test('pieces go to the speaker they overlap most, runs are merged', () {
    final lines = mergeSpeakers(const [
      TimedText(0.0, 2.0, ' Hi, how are you?'),
      TimedText(2.0, 3.5, ' Fine, thanks.'),
      TimedText(3.5, 5.0, ' And you?'),
      TimedText(5.2, 7.0, ' Good.'),
    ], const [
      SpeakerTurn(0.0, 2.1, 7),
      SpeakerTurn(2.1, 5.1, 3),
      SpeakerTurn(5.1, 7.0, 7),
    ]);
    expect(lines.length, 3);
    expect(lines[0].speaker, 0);
    expect(lines[0].text, 'Hi, how are you?');
    expect(lines[1].speaker, 1);
    expect(lines[1].text, 'Fine, thanks. And you?');
    expect(lines[1].start, 2.0);
    expect(lines[1].end, 5.0);
    expect(lines[2].speaker, 0);
    expect(speakerCount(lines), 2);
  });

  test('gaps go to the nearest turn, no-speech markers are dropped', () {
    final lines = mergeSpeakers(const [
      TimedText(0.0, 1.0, '[BLANK_AUDIO]'),
      TimedText(9.0, 10.0, 'Later words'),
    ], const [
      SpeakerTurn(0.0, 3.0, 1),
      SpeakerTurn(4.0, 8.0, 2),
    ]);
    expect(lines.length, 1);
    expect(lines[0].text, 'Later words');
    expect(lines[0].speaker, 0); // speaker 2 is the first one heard
  });

  test('names and the labelled transcript', () {
    const lines = [SpeakerLine(0, 0, 1, 'Hello'), SpeakerLine(1, 1, 2, 'Ciao')];
    expect(labeledTranscript(lines, const {'1': 'Luca'}), 'Speaker 1: Hello\nLuca: Ciao');
    final j = lines[1].toJson();
    expect(SpeakerLine.fromJson(j).text, 'Ciao');
  });
}
