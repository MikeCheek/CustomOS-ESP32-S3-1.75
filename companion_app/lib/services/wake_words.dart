import 'dart:io';
import 'dart:typed_data';
import 'ble_service.dart';

/// A WakeNet9 wake word (or, [commands], the MultiNet7 command model)
/// from Espressif's esp-sr repository (Apache-2.0).
class WakeWord {
  final String dir; // model folder in esp-sr and in /srmodels on the watch
  final String word;
  final bool commands;
  const WakeWord(this.dir, this.word, {this.commands = false});

  // _MODEL_INFO_ last: the watch only lists a model once that one is there.
  List<String> get files =>
      commands ? const ['mn7_data', 'mn7_index', '_MODEL_INFO_'] : const ['wn9_data', 'wn9_index', '_MODEL_INFO_'];
  String get _base => commands ? WakeWords._mnBase : WakeWords._wnBase;
}

/// Wake words for the watch (firmware voice_sr.cpp). Each is three files,
/// about 290 KB together, saved on the watch's SD card in /srmodels/<dir>/.
class WakeWords {
  static const offered = [
    WakeWord('wn9_hiesp', 'Hi ESP'),
    WakeWord('wn9_alexa', 'Alexa'),
    WakeWord('wn9_jarvis_tts', 'Jarvis'),
    WakeWord('wn9_computer_tts', 'Computer'),
    WakeWord('wn9_heywillow_tts', 'Hey Willow'),
    WakeWord('wn9_mycroft_tts', 'Mycroft'),
    WakeWord('wn9_sophia_tts', 'Sophia'),
    WakeWord('wn9_hijoy_tts', 'Hi Joy'),
    WakeWord('wn9_heyivy_tts2', 'Hey Ivy'),
    WakeWord('wn9_hijason_tts2', 'Hi Jason'),
  ];

  /// Offline commands after the wake word ("next song", "set a timer for
  /// five minutes", "find my phone"...), English, ~2.7 MB. They run on the
  /// watch, no phone or network needed.
  static const commands = WakeWord('mn7_en', 'Offline commands', commands: true);

  static const _wnBase = 'https://raw.githubusercontent.com/espressif/esp-sr/master/model/wakenet_model';
  static const _mnBase = 'https://raw.githubusercontent.com/espressif/esp-sr/master/model/multinet_model';

  static Future<Uint8List> _get(String url) async {
    final client = HttpClient()..connectionTimeout = const Duration(seconds: 20);
    try {
      final req = await client.getUrl(Uri.parse(url));
      final res = await req.close();
      if (res.statusCode != 200) throw Exception('Download failed (${res.statusCode})');
      final b = BytesBuilder(copy: false);
      await for (final c in res) {
        b.add(c);
      }
      return b.takeBytes();
    } finally {
      client.close();
    }
  }

  /// Downloads [w] and sends it to the watch over Bluetooth as
  /// "wn:<dir>/<file>" (accepted without a prompt). [onProgress] 0..1.
  static Future<void> install(BleService ble, WakeWord w, {void Function(double)? onProgress}) async {
    final files = w.files;
    final data = <String, Uint8List>{};
    for (var i = 0; i < files.length; i++) {
      data[files[i]] = await _get('${w._base}/${w.dir}/${files[i]}');
      onProgress?.call(0.15 * (i + 1) / files.length);
    }
    final total = data.values.fold<int>(0, (a, b) => a + b.length);
    var done = 0;
    for (final f in files) {
      final bytes = data[f]!;
      await ble.sendFileWithProgress(
        'wn:${w.dir}/$f',
        bytes,
        onProgress: (sent) {
          onProgress?.call(0.15 + 0.85 * (done + sent) / total);
        },
      );
      done += bytes.length;
      // The watch saves one file at a time: let it write this one first.
      await Future.delayed(Duration(milliseconds: bytes.length > 1000000 ? 4000 : 1500));
    }
    onProgress?.call(1);
  }
}
