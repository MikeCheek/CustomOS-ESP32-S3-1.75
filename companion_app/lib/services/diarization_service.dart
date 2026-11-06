import 'dart:io';
import 'dart:isolate';
import 'package:archive/archive.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;
import 'model_fetch.dart';
import 'ai_speed.dart';
import 'ai_profiler.dart';
import 'speaker_merge.dart';

/// "Who said what": speaker diarization with sherpa-onnx (Apache-2.0), on
/// the phone. Two models, downloaded once (about 37 MB):
///  - pyannote segmentation 3.0 (MIT), int8 - finds where someone speaks
///  - 3D-Speaker CAM++ (Apache-2.0, VoxCeleb) - tells the voices apart
/// The memo pipeline runs it after Whisper on the same 16 kHz mono file;
/// speaker_merge.dart then matches the voices to the words.
/// A voice model for telling speakers apart.
class SpeakerModel {
  final String id, name, blurb, file;
  final int sizeBytes;
  const SpeakerModel(this.id, this.name, this.blurb, this.file, this.sizeBytes);
}

class DiarizationService {
  DiarizationService._();
  static final DiarizationService instance = DiarizationService._();

  static const _rel = 'https://github.com/k2-fsa/sherpa-onnx/releases/download';
  static const _segUrl = '$_rel/speaker-segmentation-models/sherpa-onnx-pyannote-segmentation-3-0.tar.bz2';
  static const _segFile = 'segmentation.int8.onnx';
  static const _embFile = 'campplus_voxceleb.onnx'; // whichever voice model is installed
  static const _segBytes = 7 * 1024 * 1024;

  /// The voice models that tell speakers apart (sherpa-onnx, on GitHub).
  static const voiceModels = [
    SpeakerModel(
      'campplus-en',
      'CAM++ (English)',
      'The default: light and accurate',
      '3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx',
      28 * 1024 * 1024,
    ),
    SpeakerModel(
      'wespeaker-r34',
      'WeSpeaker ResNet34',
      'Strong on English conversations',
      'wespeaker_en_voxceleb_resnet34_LM.onnx',
      25 * 1024 * 1024,
    ),
    SpeakerModel(
      'titanet-small',
      'NVIDIA TitaNet small',
      'Robust in noisy rooms',
      'nemo_en_titanet_small.onnx',
      38 * 1024 * 1024,
    ),
    SpeakerModel(
      'eres2net-zh',
      '3D-Speaker ERes2Net',
      'Best for Chinese, fine for others',
      '3dspeaker_speech_eres2net_base_sv_zh-cn_3dspeaker_16k.onnx',
      37 * 1024 * 1024,
    ),
  ];
  static int get sizeBytes => _segBytes + voiceModels.first.sizeBytes;
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

  /// Which voice model is installed, or null.
  Future<String?> installedVoiceModel() async {
    if (!await isInstalled()) return null;
    final f = File('${(await _dir()).path}/voice_model.txt');
    return await f.exists() ? (await f.readAsString()).trim() : voiceModels.first.id;
  }

  /// Installs the segmentation model and [voice] (another voice model
  /// replaces the one there). Downloads in the background. [onProgress] 0..1.
  Future<void> install({SpeakerModel? voice, void Function(double)? onProgress}) async {
    final v = voice ?? voiceModels.first;
    final d = await _dir();
    final seg = File('${d.path}/$_segFile');
    if (!await seg.exists()) {
      final tarball = File('${d.path}/seg.tar.bz2');
      await ModelFetch.instance.fetch(
        label: 'Speakers - segmentation',
        url: _segUrl,
        dest: tarball,
        sizeBytes: _segBytes,
        onProgress: (p) => onProgress?.call(0.2 * p),
      );
      // Only the int8 model is needed out of the archive.
      final tar = TarDecoder().decodeBytes(BZip2Decoder().decodeBytes(await tarball.readAsBytes()));
      final model = tar.files.where((f) => f.isFile && f.name.endsWith('/model.int8.onnx')).firstOrNull;
      await tarball.delete();
      if (model == null) throw Exception('The segmentation model is missing from the download');
      await seg.writeAsBytes(model.content);
    }
    final emb = File('${d.path}/$_embFile');
    final have = await installedVoiceModel();
    if (have != v.id || !await emb.exists()) {
      final dl = File('${d.path}/${v.file}');
      await ModelFetch.instance.fetch(
        label: 'Speakers - ${v.name}',
        url: '$_rel/speaker-recongition-models/${v.file}',
        dest: dl,
        sizeBytes: v.sizeBytes,
        onProgress: (p) => onProgress?.call(0.2 + 0.8 * p),
      );
      if (await emb.exists()) await emb.delete();
      await dl.rename(emb.path);
      await File('${d.path}/voice_model.txt').writeAsString(v.id);
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
    final raw = await AiProfiler.instance.track(
      model: 'Speakers: ${await installedVoiceModel() ?? 'pyannote'}',
      task: 'Who said what',
      where: () => 'CPU, ${AiSpeed.threads()} threads',
      input: AiProfiler.wavSeconds(mono16kWav),
      unit: 's',
      run: () => _diarizeInBackground(seg, emb, mono16kWav),
    );
    return [for (final r in raw) SpeakerTurn(r[0], r[1], r[2].toInt())];
  }
}

// Top level, so the isolate gets only the three paths (see background.dart).
Future<List<List<double>>> _diarizeInBackground(String seg, String emb, String wav) =>
    Isolate.run(() => _diarize(seg, emb, wav));

// Top level: runs in the isolate (sherpa-onnx is bound per isolate).
List<List<double>> _diarize(String seg, String emb, String wav) {
  sherpa.initBindings();
  final threads = AiSpeed.threads();
  final sd = sherpa.OfflineSpeakerDiarization(
    sherpa.OfflineSpeakerDiarizationConfig(
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
    ),
  );
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
