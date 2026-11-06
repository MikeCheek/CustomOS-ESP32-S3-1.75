import 'dart:io';

import 'package:flutter_gemma/flutter_gemma.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// How fast the on-phone AI runs.
///
/// - CPU threads for the sherpa-onnx models (transcription, who said what,
///   voices): half the cores, at most 4 - on a phone that's the big cores;
///   more threads spill onto the slow ones and get slower again.
/// - Where the language model runs (summaries, answers, smart replies,
///   translation, Look): the GPU is many times faster at reading a long
///   prompt (a memo's transcript) than the CPU. Auto tries the GPU and falls
///   back to the CPU when it fails, and remembers it. If loading on the GPU
///   ever took the whole app down (a driver crash isn't an exception), the
///   next start finds the "trying" flag still set and stays on the CPU.
class AiSpeed {
  /// Threads for one sherpa-onnx model (safe in an isolate: no plugins used).
  static int threads() {
    final n = Platform.numberOfProcessors;
    return n >= 8 ? 4 : (n >= 4 ? 2 : 1);
  }

  static const _kMode = 'ai_backend'; // auto | gpu | cpu
  static const _kGpuBad = 'ai_gpu_failed';
  static const _kTrying = 'ai_gpu_trying';

  static Future<String> mode() async => (await SharedPreferences.getInstance()).getString(_kMode) ?? 'auto';

  static Future<void> setMode(String m) async {
    final p = await SharedPreferences.getInstance();
    await p.setString(_kMode, m);
    if (m != 'cpu') await p.remove(_kGpuBad); // asked for the GPU again: give it another go
  }

  /// The GPU failed on this phone before (Auto then uses the CPU).
  static Future<bool> gpuFailed() async => (await SharedPreferences.getInstance()).getBool(_kGpuBad) ?? false;

  /// Where the last model actually loaded: 'gpu' / 'cpu' / null (none yet).
  static String? lastBackend;

  /// Loads the active language model on the GPU when it can, else the CPU.
  static Future<InferenceModel> load({required int maxTokens, bool supportImage = false, int? maxNumImages}) async {
    final p = await SharedPreferences.getInstance();
    final m = p.getString(_kMode) ?? 'auto';
    // a crash while loading on the GPU last time: the flag is still there
    if (p.getBool(_kTrying) ?? false) {
      await p.setBool(_kGpuBad, true);
      await p.remove(_kTrying);
    }
    final tryGpu = m == 'gpu' || (m == 'auto' && !(p.getBool(_kGpuBad) ?? false));
    if (tryGpu) {
      await p.setBool(_kTrying, true);
      try {
        final model = await FlutterGemma.getActiveModel(
          maxTokens: maxTokens,
          preferredBackend: PreferredBackend.gpu,
          supportImage: supportImage,
          maxNumImages: maxNumImages,
        );
        await p.remove(_kTrying);
        lastBackend = 'gpu';
        return model;
      } catch (e) {
        await p.remove(_kTrying);
        await p.setBool(_kGpuBad, true);
        print('[AI] GPU unavailable ($e) - using the CPU');
      }
    }
    final model = await FlutterGemma.getActiveModel(
      maxTokens: maxTokens,
      preferredBackend: PreferredBackend.cpu,
      supportImage: supportImage,
      maxNumImages: maxNumImages,
    );
    lastBackend = 'cpu';
    return model;
  }
}
