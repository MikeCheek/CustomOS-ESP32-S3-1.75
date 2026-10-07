import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:whisper_ggml/whisper_ggml.dart';

/// On-device speech-to-text with whisper.cpp (whisper_ggml).
///
/// Why it didn't work before: whisper_ggml's transcribe() only *looks* for
/// the model file - it never downloads it - and the app never downloaded
/// one, so every transcription failed with a missing model. Models are
/// now downloaded explicitly (streamed to disk, with progress), and the
/// watch's 16 kHz stereo WAV is converted to the 16 kHz mono PCM16 whisper
/// expects before transcribing.
class WhisperService {
  static WhisperService? _instance;
  WhisperService._();
  static WhisperService get instance => _instance ??= WhisperService._();

  final WhisperController _controller = WhisperController();
  WhisperModel _model = WhisperModel.base;
  String _language = 'auto';
  bool _prefsLoaded = false;

  static const List<WhisperModel> offered = [
    WhisperModel.tiny,
    WhisperModel.base,
    WhisperModel.small,
  ];

  static const Map<WhisperModel, String> labels = {
    WhisperModel.tiny: 'Tiny',
    WhisperModel.base: 'Base',
    WhisperModel.small: 'Small',
  };

  static const Map<WhisperModel, String> blurbs = {
    WhisperModel.tiny: 'Fastest, rough accuracy',
    WhisperModel.base: 'Good balance (recommended)',
    WhisperModel.small: 'Most accurate, slow on phones',
  };

  static const Map<WhisperModel, int> sizes = {
    WhisperModel.tiny: 75 * 1024 * 1024,
    WhisperModel.base: 142 * 1024 * 1024,
    WhisperModel.small: 466 * 1024 * 1024,
  };

  /// Languages offered in settings ('auto' = detect).
  static const Map<String, String> languages = {
    'auto': 'Detect automatically',
    'en': 'English',
    'it': 'Italian',
    'es': 'Spanish',
    'fr': 'French',
    'de': 'German',
    'pt': 'Portuguese',
    'nl': 'Dutch',
    'ro': 'Romanian',
    'pl': 'Polish',
    'ru': 'Russian',
    'uk': 'Ukrainian',
    'ar': 'Arabic',
    'zh': 'Chinese',
    'ja': 'Japanese',
  };

  static String languageName(String code) => code.isEmpty ? 'Unknown' : (languages[code] ?? code.toUpperCase());

  WhisperModel get model => _model;
  String get language => _language;

  Future<void> loadPrefs() async {
    if (_prefsLoaded) return;
    final p = await SharedPreferences.getInstance();
    final name = p.getString('whisper_model');
    _model = offered.firstWhere((m) => m.name == name, orElse: () => WhisperModel.base);
    _language = p.getString('whisper_lang') ?? 'auto';
    _prefsLoaded = true;
  }

  Future<void> setModel(WhisperModel m) async {
    _model = m;
    final p = await SharedPreferences.getInstance();
    await p.setString('whisper_model', m.name);
  }

  Future<void> setLanguage(String lang) async {
    _language = lang;
    final p = await SharedPreferences.getInstance();
    await p.setString('whisper_lang', lang);
  }

  Future<File> _modelFile(WhisperModel m) async => File(await _controller.getPath(m));

  Future<bool> isDownloaded([WhisperModel? m]) async {
    final f = await _modelFile(m ?? _model);
    return await f.exists() && await f.length() > 1024 * 1024;
  }

  /// Downloads the model to whisper_ggml's model folder. [onProgress] 0..1.
  Future<void> download({WhisperModel? model, void Function(double)? onProgress}) async {
    final m = model ?? _model;
    final target = await _modelFile(m);
    if (await isDownloaded(m)) return;
    final tmp = File('${target.path}.part');
    final client = HttpClient();
    try {
      final req = await client.getUrl(m.modelUri);
      final res = await req.close();
      if (res.statusCode != 200) {
        throw HttpException('Model download failed (HTTP ${res.statusCode})');
      }
      final total = res.contentLength > 0 ? res.contentLength : (sizes[m] ?? 0);
      final sink = tmp.openWrite();
      var got = 0;
      var lastReport = -1;
      try {
        await for (final chunk in res) {
          sink.add(chunk);
          got += chunk.length;
          if (total > 0 && onProgress != null) {
            final pct = got * 100 ~/ total;
            if (pct != lastReport) {
              lastReport = pct;
              onProgress(got / total);
            }
          }
        }
      } finally {
        await sink.close();
      }
      await tmp.rename(target.path);
    } catch (e) {
      if (await tmp.exists()) await tmp.delete();
      rethrow;
    } finally {
      client.close();
    }
  }

  Future<void> deleteModel(WhisperModel m) async {
    final f = await _modelFile(m);
    if (await f.exists()) await f.delete();
  }

  // One transcription at a time (a memo and a voice reply could overlap,
  // and two whisper models in memory at once can run the phone out of RAM).
  Future<void> _queue = Future.value();

  /// Transcribes a WAV recording. Throws with a readable message on failure.
  /// [lang]: force this language (ISO code) instead of the setting.
  Future<String> transcribe(String wavPath, {void Function(double)? onProgress, String? lang}) {
    final run = _queue.then((_) => _transcribe(wavPath, onProgress: onProgress, lang: lang));
    _queue = run.then((_) {}, onError: (_) {});
    return run;
  }

  Future<String> _transcribe(String wavPath, {void Function(double)? onProgress, String? lang}) async {
    await loadPrefs();
    if (!await isDownloaded()) {
      throw Exception('Speech model not downloaded - open AI settings');
    }
    final input = File(wavPath);
    if (!await input.exists()) throw Exception('Recording not found on the phone');
    if (wavPath.toLowerCase().endsWith('.mp3')) {
      throw Exception('Only WAV recordings can be transcribed');
    }

    final prepared = await _toMono16k(input);
    try {
      final result = await _controller.transcribe(
        model: _model,
        audioPath: prepared.path,
        lang: (lang == null || lang.isEmpty) ? _language : lang,
        onProgress: onProgress == null ? null : (p) => onProgress(p / 100.0),
      );
      if (result == null) throw Exception('Transcription failed');
      return result.transcription.text.trim();
    } finally {
      if (prepared.path != input.path && await prepared.exists()) {
        await prepared.delete();
      }
    }
  }

  /// Rewrites any 8/16-bit PCM WAV as 16 kHz mono 16-bit, which is what
  /// whisper.cpp reads. (The watch records 16 kHz *stereo*.)
  Future<File> _toMono16k(File src) async {
    final bytes = await src.readAsBytes();
    final bd = ByteData.sublistView(bytes);
    if (bytes.length < 44 || String.fromCharCodes(bytes.sublist(0, 4)) != 'RIFF') return src;

    int channels = 1, rate = 16000, bits = 16, dataOff = -1, dataLen = 0, fmt = 1;
    var p = 12;
    while (p + 8 <= bytes.length) {
      final id = String.fromCharCodes(bytes.sublist(p, p + 4));
      var len = bd.getUint32(p + 4, Endian.little);
      if (id == 'fmt ' && p + 24 <= bytes.length) {
        fmt = bd.getUint16(p + 8, Endian.little);
        channels = bd.getUint16(p + 10, Endian.little);
        rate = bd.getUint32(p + 12, Endian.little);
        bits = bd.getUint16(p + 22, Endian.little);
      } else if (id == 'data') {
        dataOff = p + 8;
        // A recording cut short can have a 0 / too-big length: use what's there.
        if (len == 0 || dataOff + len > bytes.length) len = bytes.length - dataOff;
        dataLen = len;
        break;
      }
      p += 8 + len + (len & 1);
    }
    if (dataOff < 0 || fmt != 1 || (bits != 16 && bits != 8) || channels < 1) return src;
    if (channels == 1 && rate == 16000 && bits == 16) return src;

    final bytesPerSample = bits ~/ 8;
    final frameBytes = bytesPerSample * channels;
    final frames = dataLen ~/ frameBytes;
    // mono floats
    final mono = Float64List(frames);
    for (var i = 0; i < frames; i++) {
      var sum = 0.0;
      for (var c = 0; c < channels; c++) {
        final o = dataOff + i * frameBytes + c * bytesPerSample;
        sum += bits == 16 ? bd.getInt16(o, Endian.little).toDouble() : (bytes[o] - 128) * 256.0;
      }
      mono[i] = sum / channels;
    }
    // linear resample to 16 kHz
    final outFrames = rate == 16000 ? frames : (frames * 16000 / rate).floor();
    final out = ByteData(44 + outFrames * 2);
    for (var i = 0; i < outFrames; i++) {
      double v;
      if (rate == 16000) {
        v = mono[i];
      } else {
        final t = i * rate / 16000;
        final i0 = t.floor();
        final i1 = i0 + 1 < frames ? i0 + 1 : i0;
        final f = t - i0;
        v = mono[i0] * (1 - f) + mono[i1] * f;
      }
      out.setInt16(44 + i * 2, v.round().clamp(-32768, 32767), Endian.little);
    }
    void str(int o, String s) {
      for (var k = 0; k < 4; k++) {
        out.setUint8(o + k, s.codeUnitAt(k));
      }
    }
    str(0, 'RIFF');
    out.setUint32(4, 36 + outFrames * 2, Endian.little);
    str(8, 'WAVE');
    str(12, 'fmt ');
    out.setUint32(16, 16, Endian.little);
    out.setUint16(20, 1, Endian.little);
    out.setUint16(22, 1, Endian.little);
    out.setUint32(24, 16000, Endian.little);
    out.setUint32(28, 32000, Endian.little);
    out.setUint16(32, 2, Endian.little);
    out.setUint16(34, 16, Endian.little);
    str(36, 'data');
    out.setUint32(40, outFrames * 2, Endian.little);

    final dst = File('${src.parent.path}/.whisper_${DateTime.now().microsecondsSinceEpoch}.wav');
    await dst.writeAsBytes(out.buffer.asUint8List());
    return dst;
  }
}
