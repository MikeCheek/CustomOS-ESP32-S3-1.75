import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/services/smart_replies.dart';

void main() {
  test('model output becomes clean replies', () {
    final r = SmartReplies.parse('Here are some replies:\n1. "Sure, see you at 8!"\n- Can I let you know later?\n'
        '* What should I bring?\nSure, see you at 8!\nExtra one');
    expect(r, ['Sure, see you at 8!', 'Can I let you know later?', 'What should I bring?']);
  });

  test('long lines are cut on a word', () {
    final r = SmartReplies.parse('Thanks so much, I really appreciate it a lot');
    expect(r.single.length <= SmartReplies.maxChars + 1, true);
    expect(r.single.endsWith('…'), true);
  });
}
