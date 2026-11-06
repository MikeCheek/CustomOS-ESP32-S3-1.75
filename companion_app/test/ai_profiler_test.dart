import 'dart:convert';
import 'dart:io';

import 'package:amoled_companion/services/ai_profiler.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  late Directory dir;
  setUp(() {
    dir = Directory.systemTemp.createTempSync('aiprof');
    AiProfiler.debugDir = dir;
  });
  tearDown(() => dir.deleteSync(recursive: true));

  test('times runs, average / fastest / slowest, failures', () async {
    final p = AiProfiler.fresh();
    for (final ms in [30, 60, 90]) {
      await p.track(
        model: 'M',
        task: 'Answer',
        where: () => 'GPU',
        run: () => Future.delayed(Duration(milliseconds: ms)),
      );
    }
    await expectLater(
      p.track(model: 'M', task: 'Answer', where: () => 'GPU', run: () async => throw StateError('boom')),
      throwsStateError,
    );
    final s = (await p.stats()).single;
    expect(s.count, 4);
    expect(s.failures, 1);
    expect(s.minMs, greaterThanOrEqualTo(30));
    expect(s.maxMs, greaterThanOrEqualTo(90));
    expect(s.avgMs, inInclusiveRange(s.minMs, s.maxMs));
    expect(s.wheres, {'GPU'});
    expect(s.runs.last.error, contains('boom'));
  });

  test('loading the model is apart from the average, with its own line', () async {
    final p = AiProfiler.fresh();
    await p.track(
      model: 'Q',
      task: 'Load',
      where: () => 'GPU',
      run: () => Future.delayed(const Duration(milliseconds: 200)),
    );
    await p.track(
      model: 'Q',
      task: 'Answer',
      where: () => 'GPU',
      run: () => Future.delayed(const Duration(milliseconds: 20)),
    );
    final s = (await p.stats()).single;
    expect(s.maxMs, lessThan(150)); // the load isn't in it
    expect(s.byTask.map((t) => t.model).toSet(), {'Load', 'Answer'});
  });

  test('speed per second of audio', () async {
    final p = AiProfiler.fresh();
    await p.track(
      model: 'W',
      task: 'Transcription',
      where: () => 'CPU',
      input: 10,
      unit: 's',
      run: () => Future.delayed(const Duration(milliseconds: 50)),
    );
    final rtf = (await p.stats()).single.realTimeFactor!;
    expect(rtf, lessThan(1));
  });

  test('a run left unfinished (the app died) shows as a crash next time', () async {
    File('${dir.path}/ai_running.json').writeAsStringSync(
      jsonEncode({'m': 'Qwen3 0.6B', 't': 'Load', 'w': 'GPU', 'at': DateTime.now().millisecondsSinceEpoch}),
    );
    final p = AiProfiler.fresh();
    final r = (await p.runs()).single;
    expect(r.crash, isTrue);
    expect(r.where, 'GPU');
    expect(r.model, 'Qwen3 0.6B');
    expect(File('${dir.path}/ai_running.json').existsSync(), isFalse);
    // and it's kept
    expect((await AiProfiler.fresh().runs()).single.crash, isTrue);
  });
}
