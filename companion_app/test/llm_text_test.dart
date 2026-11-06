import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/llm_text.dart';

void main() {
  test('leaked channel tokens over and over: garbled', () {
    const raw =
        '<|channel|><|channel>thought?,<channel|><|channel>thought? user<channel|><|channel>thought? '
        'an<channel|><|channel>thought? instru<channel|><|channel>thought? <|channel|>thought? <|channel|>thought?';
    expect(cleanLlmText(raw), isNull);
  });

  test('the final channel is the answer', () {
    expect(
      cleanLlmText(
        '<|channel|>analysis<|message|>The user wants a joke.<|end|><|start|>assistant'
        '<|channel|>final<|message|>Why did the watch go to school? To get more time.',
      ),
      'Why did the watch go to school? To get more time.',
    );
  });

  test('thinking and turn tokens are dropped', () {
    expect(cleanLlmText('<think>\nhmm\n</think>\nSure: it is sunny.<end_of_turn>'), 'Sure: it is sunny.');
    expect(cleanLlmText('It is sunny.<think>and then'), 'It is sunny.');
    expect(cleanLlmText('<start_of_turn>model\nHello there.'), 'Hello there.');
  });

  test('a normal answer is kept as it is', () {
    const a = 'Your next meeting is the design review at 15:00, and it will rain later.';
    expect(cleanLlmText(a), a);
    expect(cleanLlmText('{"tool":"timer","seconds":300}'), '{"tool":"timer","seconds":300}');
  });
}
