import 'dart:io';
import 'dart:isolate';
import 'package:archive/archive.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;
import 'speaker_merge.dart';

/// "Who said what": speaker diarization with sherpa-onnx (Apache-2.0), on
/// the phone. Two models, downloaded once (about 37 MB):
///  - pyannote segmentation 3.0 (MIT), int8 - finds where someone speaks
///  - 3D-Speaker CAM++ (Apache-2.0, VoxCeleb) - tells the voices apart
/// The memo pipeline runs it after Whisper on the same 16 kHz mono file;
/// speaker_merge.dart then matches the voices to the words.
class DiarizationService {
  DiarizationService._();
  static final DiarizationService instance = DiarizationService._();

  static const _rel = 'https://github.com/k2-fsa/sherpa-onnx/releases/download';
  static const _segUrl = '$_rel/speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2';
  static const _embUrl = '$_rel/speaker-recongition-models/3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx';
  static const _segFile = 'segmentation.int8.onnx';
  static const _embFile = 'campplus_voxceleb.onnx';
  static const sizeBytes = 37 * 1024 * 1024;
  static const _enabledKey = 'diarize_on';

  Future<Directory> _dir() async {
    final d = Directory('${(await getApplicationSupportDirectory()).path}/diarization');
    if (!await d.exists()) await d.create(recursive: true);
    return d;
  }

  Future<bool> isInstalled() async {
    final d = await _dir();
    return await File('${d.path}/$_segFile').exists() && await File('${d.path}/$_embFile').exists();
  }

  /// Split memos by speaker when the models are there (on by default).
  Future<bool> enabled() async => (await SharedPreferences.getInstance()).getBool(_enabledKey) ?? true;
  Future<void> setEnabled(bool on) async => (await SharedPreferences.getInstance()).setBool(_enabledKey, on);

  Future<bool> ready() async => await enabled() && await isInstalled();

  Future<void> _download(String url, File to, void Function(int got) onBytes) async {
    final client = HttpClient()..connectionTimeout = const Duration(seconds: 30);
    try {
      final res = await (await client.getUrl(Uri.parse(url))).close();
      if (res.statusCode != 200) throw Exception('Download failed (${res.statusCode})');
      final tmp = File('${to.path}.part');
      final sink = tmp.openWrite();
      var got = 0;
      await for (final c in res) {
        sink.add(c);
        got += c.length;
        onBytes(got);
      }
      await sink.close();
      await tmp.rename(to.path);
    } finally {
      client.close();
    }
  }

  /// [onProgress] 0..1.
  Future<void> install({void Function(double)? onProgress}) async {
    final d = await _dir();
    const segBytes = 7 * 1024 * 1024, embBytes = 30 * 1024 * 1024;
    final seg = File('${d.path}/$_segFile');
    if (!await seg.exists()) {
      final tarball = File('${d.path}/seg.tar.bz2');
      await _download(_segUrl, tarball, (g) => onProgress?.call(0.2 * g / segBytes));
      // Only the int8 model is needed out of the archive.
      final tar = TarDecoder().decodeBytes(BZip2Decoder().decodeBytes(await tarball.readAsBytes()));
      final model = tar.files.where((f) => f.isFile && f.name.endsWith('/model.int8.onnx')).firstOrNull;
      await tarball.delete();
      if (model == null) throw Exception('The segmentation model is missing from the download');
      await seg.writeAsBytes(model.content);
    }
    final emb = File('${d.path}/$_embFile');
    if (!await emb.exists()) {
      await _download(_embUrl, emb, (g) => onProgress?.call(0.2 + 0.8 * (g / embBytes).clamp(0, 1)));
    }
    onProgress?.call(1);
  }

  Future<void> delete() async {
    final d = await _dir();
    if (await d.exists()) await d.delete(recursive: true);
  }

  /// The speaker turns in a 16 kHz mono WAV. Runs in a background isolate.
  Future<List<SpeakerTurn>> run(String mono16kWav) async {
    final d = await _dir();
    final seg = '${d.path}/$_segFile', emb = '${d.path}/$_embFile';
    final raw = await Isolate.run(() => _diarize(seg, emb, mono16kWav));
    return [for (final r in raw) SpeakerTurn(r[0], r[1], r[2].toInt())];
  }
}

// Top level: runs in the isolate (sherpa-onnx is bound per isolate).
List<List<double>> _diarize(String seg, String emb, String wav) {
  sherpa.initBindings();
  final threads = Platform.numberOfProcessors >= 4 ? 2 : 1;
  final sd = sherpa.OfflineSpeakerDiarization(sherpa.OfflineSpeakerDiarizationConfig(
    segmentation: sherpa.OfflineSpeakerSegmentationModelConfig(
      pyannote: sherpa.OfflineSpeakerSegmentationPyannoteModelConfig(model: seg),
      numThreads: threads,
      debug: false,
    ),
    embedding: sherpa.SpeakerEmbeddingExtractorConfig(model: emb, numThreads: threads, debug: false),
    // Number of speakers unknown: cluster by distance.
    clustering: const sherpa.FastClusteringConfig(numClusters: -1, threshold: 0.5),
    minDurationOn: 0.3,
    minDurationOff: 0.5,
  ));
  try {
    final wave = sherpa.readWave(wav);
    if (wave.samples.isEmpty) return const [];
    if (wave.sampleRate != sd.sampleRate) {
      throw Exception('Speaker split needs ${sd.sampleRate} Hz audio, got ${wave.sampleRate}');
    }
    return [
      for (final s in sd.process(samples: wave.samples)) [s.start, s.end, s.speaker.toDouble()],
    ];
  } finally {
    sd.free();
  }
}
