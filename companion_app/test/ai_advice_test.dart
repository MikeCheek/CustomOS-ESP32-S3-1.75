import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/ai_advice.dart';
import 'package:amoled_companion/services/ai_profiler.dart';

AiRun r(String m, String t, int ms, {String w = 'CPU, 4 threads', double? s, String? e}) =>
    AiRun(model: m, task: t, where: w, at: DateTime(2026), ms: ms, input: s, unit: s == null ? null : 's', error: e);

void main() {
  test('slow transcription points to a faster model, and the fastest is named', () {
    final a = aiAdvice([
      AiStats('Whisper base (whisper.cpp)', [r('W', 'Transcription', 22000, s: 10)]),
      AiStats('Moonshine tiny', [r('M', 'Transcription', 1000, s: 10)]),
    ]);
    expect(a.any((x) => x.startsWith('Whisper base (whisper.cpp) is slower than real time')), isTrue);
    expect(a.any((x) => x.contains('Fastest transcription on this phone: Moonshine tiny, 22x')), isTrue);
  });

  test('a language model only on the CPU, and slow answers', () {
    final a = aiAdvice([
      AiStats('Gemma 3 1B', [r('G', 'Load', 9000, w: 'CPU'), r('G', 'Memo summary', 30000, w: 'CPU')]),
    ]);
    expect(a.any((x) => x.contains('only run on the CPU')), isTrue);
    expect(a.any((x) => x.contains('"Memo summary"')), isTrue);
  });

  test('on the GPU and quick: nothing to say', () {
    expect(
      aiAdvice([
        AiStats('Gemma 3 1B', [r('G', 'Load', 4000, w: 'GPU'), r('G', 'Answer', 3000, w: 'GPU')]),
      ]),
      isEmpty,
    );
  });

  test('a model that always fails', () {
    final a = aiAdvice([
      AiStats('Voice: English - Amy', [r('V', 'Speech', 10, e: 'x'), r('V', 'Speech', 10, e: 'x')]),
    ]);
    expect(a.single, contains('failed every time'));
  });
}
