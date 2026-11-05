import 'package:flutter_test/flutter_test.dart';
import 'package:amoled_companion/providers/home_provider.dart';

void main() {
  test('reads the watch\'s gestures and tiles', () {
    final h = HomeGestures.fromWatch({
      'e': 'home',
      'a': {
        'tap': 'apps',
        'double': 'playpause',
        'hold': 'faces',
        'up': 'stack',
        'down': 'control',
        'left': 'tiles',
        'right': 'notifs',
        'boot': 'control',
      },
      'style': 'standard',
      'tiles': ['music', 'bogus', 'timer'],
    });
    expect(h.style?.key, 'standard');
    expect(h.tiles, ['music', 'timer']);
    expect(h.withAction('tap', 'torch').style, isNull); // Custom
  });

  test('every style sets every gesture to something it allows', () {
    for (final st in homeStyles) {
      for (final s in homeSlots) {
        final a = st.actions[s.key];
        expect(a, isNotNull, reason: '${st.key}.${s.key}');
        expect(actionsFor(s.key).any((x) => x.key == a), isTrue, reason: '${st.key}.${s.key}');
      }
    }
  });

  test('the face\'s own is only for a tap, and hold is never nothing', () {
    expect(actionsFor('tap').any((a) => a.key == 'face'), isTrue);
    expect(actionsFor('up').any((a) => a.key == 'face'), isFalse);
    expect(actionsFor('hold').any((a) => a.key == 'none'), isFalse);
  });
}
