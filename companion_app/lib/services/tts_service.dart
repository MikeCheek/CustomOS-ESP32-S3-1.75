import 'dart:convert';
import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:path_provider/path_provider.dart';
import 'package:piper_phonemize/piper_phonemize.dart' as piper;
import 'package:shared_preferences/shared_preferences.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;
import 'ai_speed.dart';
import 'ai_profiler.dart';
import 'background.dart';
import 'model_fetch.dart';

/// A Piper voice (MIT, rhasspy/piper), packaged for sherpa-onnx.
class TtsVoice {
  final String id; // folder in sherpa-onnx's tts-models release
  final String label;
  final String lang; // ISO 639-1
  final int sizeBytes;
  const TtsVoice(this.id, this.label, this.lang, this.sizeBytes);
}

/// Spoken answers: text-to-speech on the phone (sherpa-onnx + Piper), the
/// audio played on the watch's speaker. Each voice is downloaded once.
class TtsService {
  TtsService._();
  static final TtsService instance = TtsService._();

  static const voices = [
    TtsVoice('vits-piper-en_US-amy-low', 'English - Amy', 'en', 67 * 1024 * 1024),
    TtsVoice('vits-piper-en_US-lessac-medium', 'English - Lessac', 'en', 64 * 1024 * 1024),
    TtsVoice('vits-piper-en_US-ryan-medium', 'English - Ryan', 'en', 64 * 1024 * 1024),
    TtsVoice('vits-piper-en_GB-alan-medium', 'English (UK) - Alan', 'en', 64 * 1024 * 1024),
    TtsVoice('vits-piper-it_IT-riccardo-x_low', 'Italiano - Riccardo', 'it', 26 * 1024 * 1024),
    TtsVoice('vits-piper-it_IT-paola-medium', 'Italiano - Paola', 'it', 64 * 1024 * 1024),
    TtsVoice('vits-piper-de_DE-thorsten-medium', 'Deutsch - Thorsten', 'de', 64 * 1024 * 1024),
    TtsVoice('vits-piper-es_ES-davefx-medium', 'Español - Davefx', 'es', 64 * 1024 * 1024),
    TtsVoice('vits-piper-es_MX-claude-high', 'Español (MX) - Claude', 'es', 64 * 1024 * 1024),
    TtsVoice('vits-piper-fr_FR-siwis-medium', 'Français - Siwis', 'fr', 64 * 1024 * 1024),
    TtsVoice('vits-piper-pt_BR-faber-medium', 'Português (BR) - Faber', 'pt', 64 * 1024 * 1024),
    TtsVoice('vits-piper-pt_PT-tugao-medium', 'Português - Tugão', 'pt', 64 * 1024 * 1024),
    TtsVoice('vits-piper-pl_PL-gosia-medium', 'Polski - Gosia', 'pl', 64 * 1024 * 1024),
    TtsVoice('vits-piper-ro_RO-mihai-medium', 'Română - Mihai', 'ro', 64 * 1024 * 1024),
    TtsVoice('vits-piper-ru_RU-irina-medium', 'Русский - Irina', 'ru', 64 * 1024 * 1024),
    TtsVoice('vits-piper-uk_UA-ukrainian_tts-medium', 'Українська', 'uk', 76 * 1024 * 1024),
    TtsVoice('vits-piper-zh_CN-huayan-medium', '中文 - Huayan', 'zh', 64 * 1024 * 1024),
  ];
  static const _base = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models';
  static const _enabledKey = 'tts_answers_on';

  Future<Directory> _root() async {
    final d = Directory('${(await getApplicationSupportDirectory()).path}/tts');
    if (!await d.exists()) await d.create(recursive: true);
    return d;
  }

  Future<String?> _model(Directory dir) async {
    if (!await dir.exists()) return null;
    await for (final f in dir.list()) {
      if (f is File && f.path.endsWith('.onnx')) return f.path;
    }
    return null;
  }

  Future<bool> isInstalled(TtsVoice v) async {
    final d = Directory('${(await _root()).path}/${v.id}');
    return await _model(d) != null && await File('${d.path}/tokens.txt').exists();
  }

  Future<bool> enabled() async => (await SharedPreferences.getInstance()).getBool(_enabledKey) ?? true;
  Future<void> setEnabled(bool on) async => (await SharedPreferences.getInstance()).setBool(_enabledKey, on);

  /// After a spoken answer the watch listens a few seconds for a follow-up (no wake word, no tap).
  Future<bool> followUps() async => (await SharedPreferences.getInstance()).getBool('tts_follow_ups') ?? true;
  Future<void> setFollowUps(bool on) async => (await SharedPreferences.getInstance()).setBool('tts_follow_ups', on);

  /// The evening recap (evening_recap.dart): on, at what minute of the day, the last day it was given.
  Future<bool> recapOn() async => (await SharedPreferences.getInstance()).getBool('recap_on') ?? true;
  Future<void> setRecapOn(bool on) async => (await SharedPreferences.getInstance()).setBool('recap_on', on);
  Future<int> recapMinute() async => (await SharedPreferences.getInstance()).getInt('recap_min') ?? 20 * 60 + 30;
  Future<void> setRecapMinute(int m) async => (await SharedPreferences.getInstance()).setInt('recap_min', m);
  Future<int> recapDay() async => (await SharedPreferences.getInstance()).getInt('recap_day') ?? 0;
  Future<void> setRecapDay(int d) async => (await SharedPreferences.getInstance()).setInt('recap_day', d);

  /// The watch's morning briefing is also said, on its speaker.
  Future<bool> briefingAloud() async => (await SharedPreferences.getInstance()).getBool('tts_briefing') ?? true;
  Future<void> setBriefingAloud(bool on) async => (await SharedPreferences.getInstance()).setBool('tts_briefing', on);

  /// [onProgress] 0..1 (the last part, unpacking, has no steps).
  Future<void> install(TtsVoice v, {void Function(double)? onProgress}) async {
    final root = await _root();
    final tarball = File('${root.path}/${v.id}.tar.bz2');
    // In the background, resumable (model_fetch.dart)
    await ModelFetch.instance.fetch(
      label: 'Voice - ${v.label}',
      url: '$_base/${v.id}.tar.bz2',
      dest: tarball,
      sizeBytes: v.sizeBytes,
      onProgress: (p) => onProgress?.call(0.9 * p),
    );
    try {
      final tar = tarball.path, out = root.path;
      await extractArchive(tar, out); // creates <root>/<id>/
    } finally {
      if (await tarball.exists()) await tarball.delete();
    }
    if (!await isInstalled(v)) throw Exception('The voice is incomplete - try again');
    onProgress?.call(0.95);
    await espeakData(); // now, so the first answer doesn't wait for it
    onProgress?.call(1);
  }

  Future<String>? _espeak;

  /// espeak-ng's data (pronunciation rules for the phonemizer), copied out of
  /// the app once (~19 MB). A copy cut short (the app closed halfway) has no
  /// ".complete" mark and is redone.
  Future<String> espeakData() => _espeak ??=
      () async {
        final dir = Directory('${(await getApplicationSupportDirectory()).path}/espeak-ng-data');
        final done = File('${dir.path}/.complete');
        if (await dir.exists() && !await done.exists()) await dir.delete(recursive: true);
        final path = await piper.extractEspeakNgData();
        await done.writeAsString('1');
        return path;
      }().catchError((Object e) {
        _espeak = null; // try again next time
        throw e;
      });

  /// A sentence in [lang] to hear what a voice sounds like.
  static String sampleText(String lang) => switch (lang) {
    'it' => 'Ciao, sono il tuo orologio. La prossima riunione è alle tre e il pomeriggio sarà soleggiato.',
    'de' => 'Hallo, ich bin deine Uhr. Dein nächster Termin ist um drei, und der Nachmittag wird sonnig.',
    'es' => 'Hola, soy tu reloj. Tu próxima reunión es a las tres y la tarde será soleada.',
    'fr' => 'Bonjour, je suis ta montre. Ta prochaine réunion est à quinze heures et l\'après-midi sera ensoleillé.',
    'pt' => 'Olá, eu sou o teu relógio. A próxima reunião é às três e a tarde vai estar ensolarada.',
    'pl' => 'Cześć, jestem twoim zegarkiem. Następne spotkanie jest o piętnastej, a popołudnie będzie słoneczne.',
    'ro' => 'Salut, sunt ceasul tău. Următoarea întâlnire este la ora trei, iar după-amiaza va fi însorită.',
    'ru' => 'Привет, я твои часы. Следующая встреча в три часа, и после обеда будет солнечно.',
    'uk' => 'Привіт, я твій годинник. Наступна зустріч о третій, і після обіду буде сонячно.',
    'zh' => '你好，我是你的手表。下一个会议在三点，下午会是晴天。',
    _ => 'Hi, I\'m your watch. Your next meeting is at three, and it\'ll be sunny all afternoon.',
  };

  /// Where Piper's own recording of [v] is (Hugging Face, rhasspy/piper-voices),
  /// to hear a voice before downloading it.
  static String? sampleUrl(TtsVoice v) {
    final m = RegExp(r'^vits-piper-(([a-z]{2,3})_[A-Z]{2})-(.+)-([a-z_]+)$').firstMatch(v.id);
    if (m == null) return null;
    return 'https://huggingface.co/rhasspy/piper-voices/resolve/main/${m[2]}/${m[1]}/${m[3]}/${m[4]}/samples/speaker_0.mp3';
  }

  /// A file to play to hear [v]: said on this phone when it's installed
  /// (what the watch will get), else Piper's recording of it, downloaded.
  Future<File> sample(TtsVoice v) async {
    final tmp = await getTemporaryDirectory();
    if (await isInstalled(v)) {
      final f = File('${tmp.path}/voice_sample.wav');
      return f.writeAsBytes(await speak(v, sampleText(v.lang)));
    }
    final url = sampleUrl(v);
    if (url == null) throw Exception('No sample for this voice');
    final f = File('${tmp.path}/voice_sample_${v.id}.mp3');
    if (await f.exists() && await f.length() > 0) return f;
    final c = HttpClient()..connectionTimeout = const Duration(seconds: 15);
    try {
      final res = await (await c.getUrl(Uri.parse(url))).close();
      if (res.statusCode != 200) throw Exception('No sample online (${res.statusCode})');
      await res.pipe(f.openWrite());
      return f;
    } catch (e) {
      if (await f.exists()) await f.delete();
      rethrow;
    } finally {
      c.close();
    }
  }

  Future<void> delete(TtsVoice v) async {
    final d = Directory('${(await _root()).path}/${v.id}');
    if (await d.exists()) await d.delete(recursive: true);
  }

  /// An installed voice for [lang] ("it", "en"...), else any installed one.
  Future<TtsVoice?> voiceFor(String lang) async {
    TtsVoice? any;
    for (final v in voices) {
      if (!await isInstalled(v)) continue;
      if (v.lang == lang) return v;
      any ??= v;
    }
    return any;
  }

  /// [text] spoken by [v], as a 16 kHz mono 16-bit WAV (what the watch plays).
  Future<Uint8List> speak(TtsVoice v, String text) async {
    final dir = '${(await _root()).path}/${v.id}';
    final model = await _model(Directory(dir));
    if (model == null) throw Exception('Voice not installed');
    return AiProfiler.instance.track(
      model: 'Voice: ${v.label}',
      task: 'Speech',
      where: () => 'CPU, ${AiSpeed.threads()} threads',
      input: text.length.toDouble(),
      unit: 'chars',
      run: () async => _synthesizeInBackground(model, dir, await espeakData(), text),
    );
  }
}

// Top level, so the isolate gets only the three strings (see background.dart).
Future<Uint8List> _synthesizeInBackground(String model, String dir, String espeakData, String text) =>
    Isolate.run(() => _synthesize(model, dir, espeakData, text));

// Top level: runs in the isolate (sherpa-onnx is bound per isolate).
// A voice that gives back nothing usable (no samples, a rate of 0, or
// samples that aren't numbers) is tried once more on one thread, then
// reported plainly - converting such output used to fail with
// "Infinity or NaN toInt".
Uint8List _synthesize(String model, String dir, String espeakData, String text) {
  sherpa.initBindings();
  // sherpa-onnx 2 dropped its built-in espeak-ng (it ignores dataDir and
  // gives back no audio for text): the text is turned into phonemes here
  final phonemes = phonemize(text, espeakData, espeakVoice(dir));
  if (phonemes.isEmpty) throw Exception('Nothing to say in "$text"');
  String? why;
  for (final threads in {AiSpeed.threads(), 1}) {
    final tts = sherpa.OfflineTts(
      sherpa.OfflineTtsConfig(
        model: sherpa.OfflineTtsModelConfig(
          vits: sherpa.OfflineTtsVitsModelConfig(model: model, tokens: '$dir/tokens.txt'),
          numThreads: threads,
          debug: false,
        ),
        maxNumSentences: 2,
      ),
    );
    try {
      final a = tts.generateWithConfig(
        text: '',
        config: sherpa.OfflineTtsGenerationConfig(phonemeCodepoints: phonemes),
      );
      why = _unusable(a.samples, a.sampleRate);
      if (why == null) return pcm16Wav(a.samples, a.sampleRate);
      why = '$why; the voice runs at ${_rate(tts)} Hz';
    } finally {
      tts.free();
    }
  }
  throw Exception('The voice gave no audio ($why) - try reinstalling it in AI models');
}

/// [text] as Piper phonemes (Unicode code points), one list per sentence.
/// [espeakData]: espeak-ng's data folder. Empty sentences are left out.
List<List<int>> phonemize(String text, String espeakData, String voice) {
  piper.initBindings();
  // the first call sets espeak-ng up (0 after that: already done); -1 failed
  if (piper.PiperPhonemize.initialize(espeakData) < 0) {
    throw Exception('The pronunciation data (espeak-ng) didn\'t load');
  }
  return [
    for (final s in piper.PiperPhonemize.phonemize(text, voice: voice))
      if (s.isNotEmpty) s,
  ];
}

/// The espeak-ng voice a Piper voice was trained with: from its .onnx.json
/// when the voice has one, else from its name (vits-piper-`lang`_`REGION`-...).
String espeakVoice(String dir) {
  try {
    for (final f in Directory(dir).listSync()) {
      if (f is File && f.path.endsWith('.onnx.json')) {
        final v = (jsonDecode(f.readAsStringSync()) as Map)['espeak']?['voice'];
        if (v is String && v.isNotEmpty) return v;
      }
    }
  } catch (_) {}
  return espeakVoiceFor(dir.split('/').last);
}

/// From a voice id like vits-piper-en_US-amy-low: "en-us".
String espeakVoiceFor(String id) {
  final m = RegExp(r'piper-([a-z]{2,3})_([A-Z]{2})').firstMatch(id);
  if (m == null) return 'en-us';
  final lang = m[1]!, region = m[2]!;
  const special = {'en_GB': 'en-gb-x-rp', 'es_MX': 'es-419', 'pt_BR': 'pt-br', 'en_US': 'en-us', 'zh_CN': 'cmn'};
  return special['${lang}_$region'] ?? lang;
}

int _rate(sherpa.OfflineTts tts) {
  try {
    return tts.sampleRate;
  } catch (_) {
    return -1;
  }
}

/// Why this output can't be played, or null when it can.
String? _unusable(Float32List x, int rate) {
  if (rate <= 0) return 'sample rate $rate';
  if (x.isEmpty) return 'no samples';
  var bad = 0;
  for (final v in x) {
    if (!v.isFinite) bad++;
  }
  return bad > x.length ~/ 2 ? '$bad of ${x.length} samples not numbers' : null;
}

/// Float samples at [rate] -> 16 kHz mono 16-bit WAV (linear resampling).
Uint8List pcm16Wav(Float32List x, int rate, {int outRate = 16000}) {
  if (rate <= 0 || x.isEmpty) rate = outRate; // nothing to resample (callers check first)
  final n = rate == outRate ? x.length : (x.length * outRate / rate).floor();
  final b = ByteData(44 + n * 2);
  for (var i = 0; i < n; i++) {
    double v;
    if (rate == outRate) {
      v = x[i];
    } else {
      final t = i * rate / outRate;
      final i0 = t.floor();
      final i1 = i0 + 1 < x.length ? i0 + 1 : i0;
      v = x[i0] + (x[i1] - x[i0]) * (t - i0);
    }
    if (!v.isFinite) v = 0; // a stray NaN / Infinity sample: silence, not a crash
    b.setInt16(44 + i * 2, (v * 32767).round().clamp(-32768, 32767), Endian.little);
  }
  void tag(int o, String s) {
    for (var k = 0; k < 4; k++) {
      b.setUint8(o + k, s.codeUnitAt(k));
    }
  }

  tag(0, 'RIFF');
  b.setUint32(4, 36 + n * 2, Endian.little);
  tag(8, 'WAVE');
  tag(12, 'fmt ');
  b.setUint32(16, 16, Endian.little);
  b.setUint16(20, 1, Endian.little);
  b.setUint16(22, 1, Endian.little);
  b.setUint32(24, outRate, Endian.little);
  b.setUint32(28, outRate * 2, Endian.little);
  b.setUint16(32, 2, Endian.little);
  b.setUint16(34, 16, Endian.little);
  tag(36, 'data');
  b.setUint32(40, n * 2, Endian.little);
  return b.buffer.asUint8List();
}
