import 'dart:io';
import 'dart:math' as math;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:path_provider/path_provider.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;

import 'model_fetch.dart';
import 'ai_speed.dart';
import 'background.dart';
import 'speaker_merge.dart';

enum AsrKind { whisper, moonshine, senseVoice }

/// A speech-to-text model for sherpa-onnx, from its GitHub releases (no
/// Hugging Face, no account): the second transcription engine next to
/// whisper.cpp (whisper_service.dart).
class AsrModel {
  final String id; // the archive / folder name in sherpa-onnx's asr-models release
  final String name, blurb;
  final AsrKind kind;
  final int sizeBytes; // the download
  final List<String> langs; // empty = many (Whisper)
  const AsrModel(this.id, this.name, this.blurb, this.kind, this.sizeBytes, [this.langs = const []]);
}

class SherpaAsr {
  SherpaAsr._();
  static final SherpaAsr instance = SherpaAsr._();

  static const _rel = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models';
  static const _mb = 1024 * 1024;

  static const models = [
    AsrModel(
      'sherpa-onnx-moonshine-tiny-en-int8',
      'Moonshine tiny',
      'English, very fast',
      AsrKind.moonshine,
      102 * _mb,
      ['en'],
    ),
    AsrModel(
      'sherpa-onnx-moonshine-base-en-int8',
      'Moonshine base',
      'English, fast and accurate',
      AsrKind.moonshine,
      239 * _mb,
      ['en'],
    ),
    AsrModel('sherpa-onnx-whisper-tiny', 'Whisper tiny (ONNX)', 'Every language, fastest', AsrKind.whisper, 110 * _mb),
    AsrModel('sherpa-onnx-whisper-base', 'Whisper base (ONNX)', 'Every language, balanced', AsrKind.whisper, 197 * _mb),
    AsrModel(
      'sherpa-onnx-whisper-small',
      'Whisper small (ONNX)',
      'Every language, accurate, slower',
      AsrKind.whisper,
      609 * _mb,
    ),
    AsrModel(
      'sherpa-onnx-whisper-turbo',
      'Whisper large-v3 turbo',
      'Every language, the most accurate. Big and slow',
      AsrKind.whisper,
      537 * _mb,
    ),
    AsrModel(
      'sherpa-onnx-whisper-distil-small.en',
      'Distil-Whisper small',
      'English, accurate and quicker',
      AsrKind.whisper,
      432 * _mb,
      ['en'],
    ),
    AsrModel(
      'sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17',
      'SenseVoice',
      'Chinese, English, Japanese, Korean, Cantonese - quick, with punctuation',
      AsrKind.senseVoice,
      999 * _mb,
      ['zh', 'en', 'ja', 'ko', 'yue'],
    ),
  ];

  static AsrModel? byId(String? id) => models.where((m) => m.id == id).firstOrNull;

  Future<Directory> _root() async {
    final d = Directory('${(await getApplicationSupportDirectory()).path}/models/asr');
    if (!await d.exists()) await d.create(recursive: true);
    return d;
  }

  Future<Directory> _dir(AsrModel m) async => Directory('${(await _root()).path}/${m.id}');

  Future<bool> isInstalled(AsrModel m) async => (await _files(m)) != null;

  /// The model's files, or null when it isn't (fully) there.
  Future<Map<String, String>?> _files(AsrModel m) async {
    final d = await _dir(m);
    if (!await d.exists()) return null;
    final names = [
      await for (final f in d.list())
        if (f is File) f.uri.pathSegments.last,
    ];
    String? find(bool Function(String) test) {
      for (final n in names) {
        if (test(n)) return '${d.path}/$n';
      }
      return null;
    }

    final tokens = find((n) => n.endsWith('tokens.txt'));
    if (tokens == null) return null;
    switch (m.kind) {
      case AsrKind.whisper:
        final enc = find((n) => n.endsWith('encoder.int8.onnx')) ?? find((n) => n.endsWith('encoder.onnx'));
        final dec = find((n) => n.endsWith('decoder.int8.onnx')) ?? find((n) => n.endsWith('decoder.onnx'));
        return enc == null || dec == null ? null : {'encoder': enc, 'decoder': dec, 'tokens': tokens};
      case AsrKind.moonshine:
        final pre = find((n) => n == 'preprocess.onnx');
        final enc = find((n) => n.startsWith('encode'));
        final un = find((n) => n.startsWith('uncached_decode'));
        final ca = find((n) => n.startsWith('cached_decode'));
        if (pre == null || enc == null || un == null || ca == null) return null;
        return {'preprocessor': pre, 'encoder': enc, 'uncached': un, 'cached': ca, 'tokens': tokens};
      case AsrKind.senseVoice:
        final model = find((n) => n == 'model.int8.onnx') ?? find((n) => n == 'model.onnx');
        return model == null ? null : {'model': model, 'tokens': tokens};
    }
  }

  /// Downloads (in the background, resumable) and unpacks [m]. [onProgress] 0..1.
  Future<void> install(AsrModel m, {void Function(double)? onProgress}) async {
    if (await isInstalled(m)) return;
    final root = await _root();
    final tarball = File('${root.path}/${m.id}.tar.bz2');
    await ModelFetch.instance.fetch(
      label: 'Transcription - ${m.name}',
      url: '$_rel/${m.id}.tar.bz2',
      dest: tarball,
      sizeBytes: m.sizeBytes,
      onProgress: (p) => onProgress?.call(0.92 * p),
    );
    try {
      final tar = tarball.path, out = root.path;
      await extractArchive(tar, out); // creates <root>/<id>/
    } finally {
      if (await tarball.exists()) await tarball.delete();
    }
    // Only what's used: no test recordings, and the int8 files where both are there.
    final d = await _dir(m);
    final tests = Directory('${d.path}/test_wavs');
    if (await tests.exists()) await tests.delete(recursive: true);
    final names = [
      await for (final f in d.list())
        if (f is File) f.uri.pathSegments.last,
    ];
    for (final n in names) {
      if (n.endsWith('.onnx') && !n.contains('.int8.') && names.contains(n.replaceFirst('.onnx', '.int8.onnx'))) {
        await File('${d.path}/$n').delete();
      }
    }
    if (!await isInstalled(m)) throw Exception('${m.name} is incomplete - try again');
    onProgress?.call(1);
  }

  Future<void> delete(AsrModel m) async {
    final d = await _dir(m);
    if (await d.exists()) await d.delete(recursive: true);
  }

  /// Transcribes a 16 kHz mono 16-bit WAV with [m], in a background isolate.
  /// Whisper models take 30 s at a time, so long memos go in pieces cut at
  /// quiet moments; each piece keeps its time (for "who said what").
  /// [lang]: an ISO code, or "auto".
  Future<({String text, List<TimedText> pieces})> transcribe(
    AsrModel m,
    String wav16k, {
    String lang = 'auto',
    void Function(double)? onProgress,
  }) async {
    final files = await _files(m);
    if (files == null) throw Exception('${m.name} isn\'t downloaded - open AI models');
    final port = ReceivePort();
    await Isolate.spawn(_run, [port.sendPort, m.kind.index, files, wav16k, lang]);
    final pieces = <TimedText>[];
    await for (final msg in port) {
      final mm = msg as Map;
      if (mm['p'] != null) {
        onProgress?.call((mm['p'] as num).toDouble());
      } else if (mm['e'] != null) {
        port.close();
        throw Exception(mm['e']);
      } else if (mm['r'] != null) {
        for (final r in mm['r'] as List) {
          final l = r as List;
          pieces.add(TimedText((l[0] as num).toDouble(), (l[1] as num).toDouble(), l[2] as String));
        }
        port.close();
      }
    }
    final text = pieces.map((p) => p.text.trim()).where((t) => t.isNotEmpty).join(' ');
    return (text: text, pieces: pieces);
  }
}

// ---- in the isolate (sherpa-onnx is bound per isolate) ------------------------------------
void _run(List<dynamic> a) {
  final out = a[0] as SendPort;
  try {
    final kind = AsrKind.values[a[1] as int];
    final f = Map<String, String>.from(a[2] as Map);
    final samples = _readWav(a[3] as String);
    final lang = a[4] as String;
    sherpa.initBindings();
    final model = switch (kind) {
      AsrKind.whisper => sherpa.OfflineModelConfig(
        whisper: sherpa.OfflineWhisperModelConfig(
          encoder: f['encoder']!,
          decoder: f['decoder']!,
          language: lang == 'auto' ? '' : lang,
          task: 'transcribe',
        ),
        tokens: f['tokens']!,
        numThreads: AiSpeed.threads(),
        debug: false,
      ),
      AsrKind.moonshine => sherpa.OfflineModelConfig(
        moonshine: sherpa.OfflineMoonshineModelConfig(
          preprocessor: f['preprocessor']!,
          encoder: f['encoder']!,
          uncachedDecoder: f['uncached']!,
          cachedDecoder: f['cached']!,
        ),
        tokens: f['tokens']!,
        numThreads: AiSpeed.threads(),
        debug: false,
      ),
      AsrKind.senseVoice => sherpa.OfflineModelConfig(
        senseVoice: sherpa.OfflineSenseVoiceModelConfig(
          model: f['model']!,
          language: const {'zh', 'en', 'ja', 'ko', 'yue'}.contains(lang) ? lang : 'auto',
          useInverseTextNormalization: true,
        ),
        tokens: f['tokens']!,
        numThreads: AiSpeed.threads(),
        debug: false,
      ),
    };
    final rec = sherpa.OfflineRecognizer(sherpa.OfflineRecognizerConfig(model: model));
    final result = <List<Object>>[];
    try {
      for (final (s, e) in speechPieces(samples)) {
        final stream = rec.createStream();
        try {
          stream.acceptWaveform(samples: Float32List.sublistView(samples, s, e), sampleRate: 16000);
          rec.decode(stream);
          final t = rec.getResult(stream).text.trim();
          if (t.isNotEmpty) result.add([s / 16000.0, e / 16000.0, t]);
        } finally {
          stream.free();
        }
        out.send({'p': e / samples.length});
      }
    } finally {
      rec.free();
    }
    out.send({'r': result});
  } catch (e) {
    out.send({'e': 'Transcription failed: $e'});
  }
}

/// The stretches with speech, as (start, end) sample offsets: cut at
/// pauses of half a second or more, so each piece is about one utterance -
/// one speaker's turn, which is what "who said what" lines up with. The
/// room's level is measured from the recording itself (its quietest
/// tenth). Bits under a second join the piece before; pieces over 28 s
/// (what Whisper takes at a time) are split at their quietest moment;
/// pieces without speech are left out. Public for tests.
List<(int, int)> speechPieces(Float32List s, {int rate = 16000}) {
  final frame = rate ~/ 50; // 20 ms
  final n = s.length ~/ frame;
  if (n == 0) return s.isEmpty ? const [] : [(0, s.length)];
  final rms = Float64List(n);
  for (var f = 0; f < n; f++) {
    var e = 0.0;
    for (var k = f * frame; k < (f + 1) * frame; k++) {
      e += s[k] * s[k];
    }
    rms[f] = math.sqrt(e / frame);
  }
  final sorted = Float64List.fromList(rms)..sort();
  final floor = sorted[n ~/ 10], loud = sorted[n * 9 ~/ 10];
  // ~10 dB over the room and never under -50 dBFS - but never above a third of the
  // loud parts either (a recording with hardly any pause has no "room" to measure)
  final thr = math.max(0.003, math.min(floor * 3, loud * 0.3));
  final voiced = [for (final r in rms) r > thr];

  // cut in the middle of every pause of 0.5 s or more
  final minPause = 25; // frames
  final cuts = <int>[0];
  var f = 0;
  while (f < n) {
    if (voiced[f]) {
      f++;
      continue;
    }
    final a = f;
    while (f < n && !voiced[f]) {
      f++;
    }
    if (f - a >= minPause && a > 0 && f < n) cuts.add(((a + f) ~/ 2) * frame);
  }
  cuts.add(s.length);

  // pieces, without the silent ones; short bits join the one before
  var pieces = <(int, int)>[];
  for (var i = 0; i + 1 < cuts.length; i++) {
    final a = cuts[i], b = cuts[i + 1];
    final fa = a ~/ frame, fb = math.min(n, (b + frame - 1) ~/ frame);
    var speech = 0;
    for (var k = fa; k < fb; k++) {
      if (voiced[k]) speech++;
    }
    if (speech < 5) continue; // < 0.1 s of speech: nothing to read
    // under a second of speech, right after the piece before: part of it
    if (pieces.isNotEmpty && speech < 50 && a - pieces.last.$2 < rate) {
      pieces[pieces.length - 1] = (pieces.last.$1, b);
    } else {
      pieces.add((a, b));
    }
  }
  // over 28 s: split at the quietest 100 ms of the last 4 s
  const maxS = 28;
  final out = <(int, int)>[];
  for (var (a, b) in pieces) {
    while (b - a > maxS * rate) {
      final cut = _quietCut(s, a + maxS * rate - 4 * rate, a + maxS * rate, rate);
      out.add((a, cut));
      a = cut;
    }
    out.add((a, b));
  }
  pieces = out;
  return pieces;
}

int _quietCut(Float32List s, int from, int to, int rate) {
  final win = rate ~/ 10;
  var best = to, bestE = double.infinity;
  for (var w = from; w + win <= to; w += win ~/ 2) {
    var e = 0.0;
    for (var k = w; k < w + win; k++) {
      e += s[k] * s[k];
    }
    if (e < bestE) {
      bestE = e;
      best = w + win ~/ 2;
    }
  }
  return best;
}

/// A 16-bit mono WAV's samples as -1..1.
Float32List _readWav(String path) {
  final b = File(path).readAsBytesSync();
  final bd = ByteData.sublistView(b);
  var p = 12, off = 44, len = b.length - 44;
  while (p + 8 <= b.length) {
    final id = String.fromCharCodes(b.sublist(p, p + 4));
    final l = bd.getUint32(p + 4, Endian.little);
    if (id == 'data') {
      off = p + 8;
      len = (l == 0 || off + l > b.length) ? b.length - off : l;
      break;
    }
    p += 8 + l + (l & 1);
  }
  final n = len ~/ 2;
  final out = Float32List(n);
  for (var i = 0; i < n; i++) {
    out[i] = bd.getInt16(off + i * 2, Endian.little) / 32768.0;
  }
  return out;
}
