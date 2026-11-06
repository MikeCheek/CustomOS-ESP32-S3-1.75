import 'dart:io';
import 'dart:isolate';
import 'dart:typed_data';
import 'package:archive/archive_io.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;
import 'ai_speed.dart';
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
      await Isolate.run(() => extractFileToDisk(tar, out)); // creates <root>/<id>/
    } finally {
      if (await tarball.exists()) await tarball.delete();
    }
    if (!await isInstalled(v)) throw Exception('The voice is incomplete - try again');
    onProgress?.call(1);
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
    return Isolate.run(() => _synthesize(model, dir, text));
  }
}

// Top level: runs in the isolate (sherpa-onnx is bound per isolate).
Uint8List _synthesize(String model, String dir, String text) {
  sherpa.initBindings();
  final tts = sherpa.OfflineTts(
    sherpa.OfflineTtsConfig(
      model: sherpa.OfflineTtsModelConfig(
        vits: sherpa.OfflineTtsVitsModelConfig(model: model, tokens: '$dir/tokens.txt', dataDir: '$dir/espeak-ng-data'),
        numThreads: AiSpeed.threads(),
        debug: false,
      ),
      maxNumSentences: 2,
    ),
  );
  try {
    final a = tts.generate(text: text, sid: 0, speed: 1.0);
    return pcm16Wav(a.samples, a.sampleRate);
  } finally {
    tts.free();
  }
}

/// Float samples at [rate] -> 16 kHz mono 16-bit WAV (linear resampling).
Uint8List pcm16Wav(Float32List x, int rate, {int outRate = 16000}) {
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
