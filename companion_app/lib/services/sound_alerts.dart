import 'dart:async';
import 'dart:io';
import 'dart:isolate';
import 'package:path_provider/path_provider.dart';
import 'package:sherpa_onnx/sherpa_onnx.dart' as sherpa;
import 'model_fetch.dart';
import 'background.dart';
import 'phone_recorder.dart';

/// A sound worth telling the watch about, and the AudioSet classes that are it.
/// A sound tagging model size.
class SoundModel {
  final String id, name, blurb;
  final int sizeBytes;
  const SoundModel(this.id, this.name, this.blurb, this.sizeBytes);
}

class AlertSound {
  final String id, label;
  final List<String> classes;
  const AlertSound(this.id, this.label, this.classes);
}

/// Sound alerts: the phone listens and the watch shows what it heard -
/// doorbell, smoke alarm, a baby crying... Audio tagging with sherpa-onnx
/// and CED-tiny (AudioSet, 527 classes; 6 MB int8), on 2.5 s clips from the
/// phone's microphone, classified in a background isolate. Nothing leaves
/// the phone, and clips are deleted right away.
class SoundAlerts {
  SoundAlerts._();
  static final SoundAlerts instance = SoundAlerts._();

  static const sounds = [
    AlertSound('door', 'Doorbell or knocking', ['Doorbell', 'Ding-dong', 'Knock']),
    AlertSound('smoke', 'Smoke or fire alarm', ['Smoke detector, smoke alarm', 'Fire alarm']),
    AlertSound('baby', 'Baby crying', ['Baby cry, infant cry']),
    AlertSound('alarm', 'Alarm clock', ['Alarm clock']),
    AlertSound('phone', 'Phone ringing', ['Telephone bell ringing', 'Ringtone']),
    AlertSound('siren', 'Siren', [
      'Siren',
      'Police car (siren)',
      'Ambulance (siren)',
      'Fire engine, fire truck (siren)',
    ]),
    AlertSound('dog', 'Dog barking', ['Bark', 'Dog']),
    AlertSound('glass', 'Glass breaking', ['Shatter', 'Glass']),
  ];

  /// The sound tagging models (CED, Apache-2.0, from sherpa-onnx on GitHub):
  /// bigger hears more reliably, costs more memory and battery.
  static const models = [
    SoundModel('ced-tiny', 'CED tiny', 'Light, the default', 28 * 1024 * 1024),
    SoundModel('ced-mini', 'CED mini', 'More reliable', 45 * 1024 * 1024),
    SoundModel('ced-small', 'CED small', 'Most reliable, heavier', 96 * 1024 * 1024),
  ];
  static const _rel = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/audio-tagging-models';
  static int get sizeBytes => models.first.sizeBytes;
  static const threshold = 0.35;

  Future<Directory> _dir() async {
    final d = Directory('${(await getApplicationSupportDirectory()).path}/sound_alerts');
    if (!await d.exists()) await d.create(recursive: true);
    return d;
  }

  Future<String> _model() async => '${(await _dir()).path}/model.int8.onnx';
  Future<String> _labels() async => '${(await _dir()).path}/class_labels_indices.csv';

  Future<bool> isInstalled() async => await File(await _model()).exists() && await File(await _labels()).exists();

  /// Which model is installed ("ced-tiny"...), or null.
  Future<String?> installedModel() async {
    if (!await isInstalled()) return null;
    final f = File('${(await _dir()).path}/model_id.txt');
    return await f.exists() ? (await f.readAsString()).trim() : 'ced-tiny';
  }

  /// Installs [model] (replacing another size). Downloads in the background
  /// (model_fetch.dart). [onProgress] 0..1.
  Future<void> install({SoundModel? model, void Function(double)? onProgress}) async {
    final m = model ?? models.first;
    final d = await _dir();
    final tarball = File('${d.path}/${m.id}.tar.bz2');
    await ModelFetch.instance.fetch(
      label: 'Sound alerts - ${m.name}',
      url: '$_rel/sherpa-onnx-${m.id}-audio-tagging-2024-04-19.tar.bz2',
      dest: tarball,
      sizeBytes: m.sizeBytes,
      onProgress: (p) => onProgress?.call(0.9 * p),
    );
    final out = '${d.path}/x';
    final tar = tarball.path;
    if (await Directory(out).exists()) await Directory(out).delete(recursive: true);
    await extractArchive(tar, out);
    await tarball.delete();
    // keep the two files we need, flat
    await for (final f in Directory(out).list(recursive: true)) {
      if (f is File && (f.path.endsWith('/model.int8.onnx') || f.path.endsWith('/class_labels_indices.csv'))) {
        await f.rename('${d.path}/${f.uri.pathSegments.last}');
      }
    }
    await Directory(out).delete(recursive: true);
    if (!await isInstalled()) throw Exception('The sound model is incomplete - try again');
    await File('${d.path}/model_id.txt').writeAsString(m.id);
    onProgress?.call(1);
  }

  Future<void> delete() async {
    final d = await _dir();
    if (await d.exists()) await d.delete(recursive: true);
  }

  // ---- listening ---------------------------------------------------------------------
  bool _on = false;
  bool get listening => _on;
  final _heard = StreamController<AlertSound>.broadcast();
  Stream<AlertSound> get heard => _heard.stream;
  final Map<String, DateTime> _last = {};

  /// Listens until stop(); [wanted]: the AlertSound ids to report.
  Future<void> start(Set<String> wanted) async {
    if (_on) return;
    _on = true;
    final model = await _model(), labels = await _labels();
    final tmp = (await getTemporaryDirectory()).path;
    var k = 0;
    while (_on) {
      final path = '$tmp/listen_${k++ % 2}.wav';
      if (!await PhoneRecorder.start(path)) {
        _on = false;
        break;
      }
      await Future.delayed(const Duration(milliseconds: 2500));
      try {
        await PhoneRecorder.stop();
      } catch (_) {
        continue;
      }
      if (!_on) break;
      final tags = await _tagInBackground(model, labels, path);
      final now = DateTime.now();
      for (final s in sounds) {
        if (!wanted.contains(s.id)) continue;
        final p = tags.entries
            .where((e) => s.classes.contains(e.key))
            .fold<double>(0, (a, e) => e.value > a ? e.value : a);
        final last = _last[s.id];
        if (p >= threshold && (last == null || now.difference(last) > const Duration(minutes: 1))) {
          _last[s.id] = now;
          _heard.add(s);
        }
      }
      try {
        await File(path).delete();
      } catch (_) {}
    }
  }

  Future<void> stop() async {
    _on = false;
    try {
      await PhoneRecorder.cancel();
    } catch (_) {}
  }
}

// In the isolate: the clip's top classes with their probabilities.
// Top level, so the isolate gets only the three paths (see background.dart).
Future<Map<String, double>> _tagInBackground(String model, String labels, String wav) =>
    Isolate.run(() => _tag(model, labels, wav));

Map<String, double> _tag(String model, String labels, String wav) {
  sherpa.initBindings();
  final at = sherpa.AudioTagging(
    config: sherpa.AudioTaggingConfig(
      model: sherpa.AudioTaggingModelConfig(ced: model, numThreads: 1, debug: false),
      labels: labels,
    ),
  );
  try {
    final w = sherpa.readWave(wav);
    if (w.samples.isEmpty) return const {};
    final stream = at.createStream();
    stream.acceptWaveform(samples: w.samples, sampleRate: w.sampleRate);
    final events = at.compute(stream: stream, topK: 10);
    stream.free();
    return {for (final e in events) e.name: e.prob};
  } finally {
    at.free();
  }
}
