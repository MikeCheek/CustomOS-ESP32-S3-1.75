import 'dart:io';
import 'dart:typed_data';
import 'dart:async';
import 'dart:convert';
import 'package:flutter_gemma/flutter_gemma.dart';
import 'package:flutter_gemma_litertlm/flutter_gemma_litertlm.dart';
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'text_insights.dart';
import 'smart_replies.dart';
import 'ai_speed.dart';
import 'ai_profiler.dart';
import 'llm_text.dart';
import 'model_fetch.dart';

/// On-device summaries with flutter_gemma (LiteRT-LM engine).
///
/// Why it didn't work before:
///  - models were installed with the default file type (.task), but the
///    only engine registered handles .litertlm, so loading always failed
///    with "no engine for this model";
///  - the Gemma models are gated on Hugging Face (they need an account
///    token), so their downloads failed with 401;
///  - after an app restart nothing loaded the model again, so every
///    summary silently fell back to "first 200 characters".
/// Now: the default model is Qwen3 0.6B (not gated), Gemma can be used with
/// a Hugging Face token, the right file type is passed, and the installed
/// model is loaded on demand.
class GemmaModelInfo {
  final String key;
  final String name;
  final int sizeBytes;
  final String url;
  final String description;
  final ModelType type;
  final bool gated;

  const GemmaModelInfo({
    required this.key,
    required this.name,
    required this.sizeBytes,
    required this.url,
    required this.description,
    required this.type,
    this.gated = false,
  });

  /// flutter_gemma identifies installed models by file name.
  String get fileName => Uri.parse(url).pathSegments.last;
}

class GemmaService {
  static GemmaService? _instance;
  GemmaService._();
  static GemmaService get instance => _instance ??= GemmaService._();

  static const List<GemmaModelInfo> models = [
    GemmaModelInfo(
      key: 'qwen3-0.6b',
      name: 'Qwen3 0.6B',
      // File names checked against huggingface.co/api/models/litert-community/
      // Qwen3-0.6B (Oct 2026) - the old _multi-prefill-seq_ name no longer exists.
      sizeBytes: 614236160,
      url: 'https://huggingface.co/litert-community/Qwen3-0.6B/resolve/main/Qwen3-0.6B.litertlm',
      description: 'Multilingual, good summaries. No account needed.',
      type: ModelType.qwen3,
    ),
    GemmaModelInfo(
      key: 'qwen3-0.6b-int4',
      name: 'Qwen3 0.6B (int4)',
      sizeBytes: 497664000,
      url: 'https://huggingface.co/litert-community/Qwen3-0.6B/resolve/main/qwen3_0_6b_mixed_int4.litertlm',
      description: 'Same model, smaller and faster, slightly less precise. No account needed.',
      type: ModelType.qwen3,
    ),
    GemmaModelInfo(
      key: 'gemma-3-1b',
      name: 'Gemma 3 1B',
      sizeBytes: 584417280,
      url:
          'https://huggingface.co/litert-community/Gemma3-1B-IT/resolve/main/Gemma3-1B-IT_multi-prefill-seq_q4_ekv4096.litertlm',
      description: 'Google Gemma. Needs a Hugging Face token (gated model).',
      type: ModelType.gemmaIt,
      gated: true,
    ),
    GemmaModelInfo(
      key: 'gemma-3-270m',
      name: 'Gemma 3 270M',
      sizeBytes: 304005120,
      // Repo is litert-community/gemma-3-270m-it (lower case); the old
      // Gemma3-270M-IT path doesn't exist.
      url: 'https://huggingface.co/litert-community/gemma-3-270m-it/resolve/main/gemma3-270m-it-q8.litertlm',
      description: 'Smallest and fastest, basic quality. Needs a token (accept the license on Hugging Face once).',
      type: ModelType.gemmaIt,
      gated: true,
    ),
  ];

  static GemmaModelInfo byKey(String? key) => models.firstWhere((m) => m.key == key, orElse: () => models.first);

  bool _initialized = false;
  InferenceModel? _model;
  String? _loadedKey;

  /// Call once at startup (main.dart). Safe to call again.
  static Future<void> initializeEngine() async {
    final self = instance;
    if (self._initialized) return;
    final p = await SharedPreferences.getInstance();
    final token = p.getString('hf_token');
    await FlutterGemma.initialize(
      inferenceEngines: const [LiteRtLmEngine()],
      huggingFaceToken: (token == null || token.isEmpty) ? null : token,
    );
    self._initialized = true;
  }

  Future<String?> selectedKey() async {
    final p = await SharedPreferences.getInstance();
    return p.getString('llm_model');
  }

  Future<String> hfToken() async {
    final p = await SharedPreferences.getInstance();
    return p.getString('hf_token') ?? '';
  }

  Future<void> setHfToken(String token) async {
    final p = await SharedPreferences.getInstance();
    await p.setString('hf_token', token.trim());
  }

  Future<bool> isInstalled(GemmaModelInfo m) async {
    try {
      await initializeEngine();
      return await FlutterGemma.isModelInstalled(m.fileName);
    } catch (_) {
      return false;
    }
  }

  /// Where the app keeps a model it downloaded itself (model_fetch.dart).
  static Future<File> localFile(GemmaModelInfo m) async =>
      File('${(await getApplicationSupportDirectory()).path}/models/llm/${m.fileName}');

  /// Downloads [m] in the background (keeps going with the app closed,
  /// resumes, tries mirrors) and registers the file with flutter_gemma.
  Future<void> _download(GemmaModelInfo m, void Function(double)? onProgress) async {
    if (await isInstalled(m)) return;
    final token = await hfToken();
    if (m.gated && token.isEmpty) {
      throw Exception('${m.name} is gated: add your Hugging Face token first');
    }
    await ModelFetch.instance.fetch(
      label: m.name,
      url: m.url,
      dest: await localFile(m),
      sizeBytes: m.sizeBytes,
      headers: token.isEmpty ? const {} : {'Authorization': 'Bearer $token'},
      onProgress: onProgress,
    );
  }

  /// Makes [m] flutter_gemma's active model: from the app's own file when
  /// it's there, else flutter_gemma's copy (models installed before).
  Future<void> _activate(GemmaModelInfo m) async {
    final f = await localFile(m);
    final b = FlutterGemma.installModel(modelType: m.type, fileType: ModelFileType.litertlm);
    try {
      await (await f.exists() ? b.fromFile(f.path) : b.fromNetwork(m.url)).install();
    } on DownloadException catch (e) {
      throw Exception(e.error.toUserMessage());
    }
  }

  /// Downloads (if needed) and makes [m] the active model. [onProgress] 0..1.
  Future<void> install(GemmaModelInfo m, {void Function(double)? onProgress}) async {
    await initializeEngine();
    await _download(m, onProgress);
    await _activate(m);
    final p = await SharedPreferences.getInstance();
    await p.setString('llm_model', m.key);
    await _closeModel();
  }

  // ---- vision: "Look" (screens/look_screen.dart) -------------------------------------
  /// Models that see images. Kept apart from the summary model: Look loads
  /// one for a question, then the summary model comes back on its own.
  static const List<GemmaModelInfo> visionModels = [
    GemmaModelInfo(
      key: 'fastvlm-0.5b',
      name: 'FastVLM 0.5B',
      sizeBytes: 520 * 1024 * 1024,
      url: 'https://huggingface.co/litert-community/FastVLM-0.5B/resolve/main/FastVLM-0.5B.litertlm',
      description: 'Small and quick: describes what the camera sees',
      type: ModelType.general,
    ),
    GemmaModelInfo(
      key: 'gemma-3n-e2b',
      name: 'Gemma 3n E2B',
      sizeBytes: 3100 * 1024 * 1024,
      url: 'https://huggingface.co/google/gemma-3n-E2B-it-litert-lm/resolve/main/gemma-3n-E2B-it-int4.litertlm',
      description: 'Sharper eyes: reads signs, labels and menus, translates them',
      type: ModelType.gemmaIt,
      gated: true,
    ),
    GemmaModelInfo(
      key: 'gemma-3n-e4b',
      name: 'Gemma 3n E4B',
      sizeBytes: 4400 * 1024 * 1024,
      url: 'https://huggingface.co/google/gemma-3n-E4B-it-litert-lm/resolve/main/gemma-3n-E4B-it-int4.litertlm',
      description: 'The sharpest: small print, handwriting, harder questions. Needs a recent phone',
      type: ModelType.gemmaIt,
      gated: true,
    ),
  ];

  Future<GemmaModelInfo?> visionModel() async {
    final key = (await SharedPreferences.getInstance()).getString('vision_model');
    for (final m in visionModels) {
      if (m.key == key && await isInstalled(m)) return m;
    }
    for (final m in visionModels.reversed) {
      if (await isInstalled(m)) return m;
    }
    return null;
  }

  /// Downloads [m] for Look (doesn't change the summary model).
  Future<void> installVision(GemmaModelInfo m, {void Function(double)? onProgress}) async {
    await initializeEngine();
    await _download(m, onProgress);
    await _activate(m);
    await (await SharedPreferences.getInstance()).setString('vision_model', m.key);
    await _closeModel(); // installing made it the active one
  }

  Future<void> uninstallVision(GemmaModelInfo m) async {
    await initializeEngine();
    await _closeModel();
    await FlutterGemma.uninstallModel(m.fileName);
    final f = await localFile(m);
    if (await f.exists()) await f.delete();
  }

  /// [prompt] about the photo [jpeg], or null without a vision model.
  Future<String?> look(Uint8List jpeg, String prompt) async {
    final m = await visionModel();
    if (m == null) return null;
    return _serial(() async {
      await _closeModel();
      await _activate(m);
      final model = await AiProfiler.instance.track(
        model: m.name,
        task: 'Load',
        where: _where,
        run: () => AiSpeed.load(maxTokens: 2048, supportImage: true, maxNumImages: 1), // GPU when it can
      );
      try {
        final chat = await model.createChat(temperature: 0.2, topK: 20, supportImage: true, modelType: m.type);
        final r = await AiProfiler.instance.track(
          model: m.name,
          task: 'Look',
          where: _where,
          input: prompt.length.toDouble(),
          unit: 'chars',
          run: () async {
            await chat.addQueryChunk(Message.withImage(text: prompt, imageBytes: jpeg, isUser: true));
            return chat.generateChatResponse();
          },
        );
        final raw = r is TextResponse ? r.token : r.toString();
        return raw.replaceAll(RegExp(r'<think>[\s\S]*?</think>'), '').replaceAll(RegExp(r'[*#`]'), '').trim();
      } finally {
        try {
          await model.close();
        } catch (_) {}
      }
    });
  }

  Future<void> uninstall(GemmaModelInfo m) async {
    await initializeEngine();
    if (_loadedKey == m.key) await _closeModel();
    await FlutterGemma.uninstallModel(m.fileName);
    final f = await localFile(m);
    if (await f.exists()) await f.delete();
    final p = await SharedPreferences.getInstance();
    if (p.getString('llm_model') == m.key) {
      await p.remove('llm_model');
      await FlutterGemma.clearActiveInferenceIdentity();
    }
  }

  /// Drops the loaded model, so the next request loads it again (on the CPU / GPU now chosen).
  Future<void> reload() => _serial(_closeModel);

  /// Loading the model is the slowest step (AI models > Performance): the
  /// assistant starts it while the question is still being transcribed, so
  /// the answer doesn't wait for it. Does nothing when it's loaded already.
  void warmUp() {
    if (_model != null) return;
    unawaited(
      _serial(() async {
        try {
          await _ensureModel();
        } catch (e) {
          print('[LLM] warm-up failed: $e');
        }
      }),
    );
  }

  /// Kept loaded between requests, so a follow-up answers right away; let go
  /// after [_idle] without one (it holds hundreds of MB), or at once when
  /// the phone runs short of memory.
  static const _idle = Duration(minutes: 10);
  Timer? _idleTimer;

  void _touch() {
    _idleTimer?.cancel();
    _idleTimer = Timer(_idle, () => unawaited(_serial(_closeModel)));
  }

  /// The phone is short of memory (main.dart): the model goes, it's loaded again when needed.
  void onMemoryPressure() {
    if (_model == null) return;
    _idleTimer?.cancel();
    unawaited(_serial(_closeModel));
  }

  Future<void> _closeModel() async {
    final m = _model;
    _model = null;
    _loadedKey = null;
    if (m != null) {
      try {
        await m.close();
      } catch (_) {}
    }
  }

  /// Loads the selected model if it's installed. Returns null if none is.
  Future<InferenceModel?> _ensureModel() async {
    final key = await selectedKey();
    if (key == null) return null;
    if (_model != null && _loadedKey == key) return _model;
    final info = byKey(key);
    if (!await isInstalled(info)) return null;
    await _closeModel();
    // Re-activate it (cheap if already installed) so the spec matches.
    await _activate(info);
    _model = await AiProfiler.instance.track(
      model: info.name,
      task: 'Load',
      where: _where,
      run: () => AiSpeed.load(maxTokens: 2048), // GPU when it can
    );
    _loadedKey = key;
    return _model;
  }

  Future<bool> hasModel() async {
    final key = await selectedKey();
    return key != null && await isInstalled(byKey(key));
  }

  // One generation at a time (memos, collection names, questions).
  Future<void> _queue = Future.value();
  Future<T> _serial<T>(Future<T> Function() fn) {
    final r = _queue.then((_) => fn());
    _queue = r.then((_) {}, onError: (_) {}).whenComplete(() {
      if (_model != null) _touch();
    });
    return r;
  }

  /// Where the language model runs now, for the profiler.
  String _where() => AiSpeed.lastBackend?.toUpperCase() ?? '?';

  Future<String?> _generate(String prompt, {double temperature = 0.2, String task = 'Answer'}) =>
      _serial(() async {
        InferenceModel? model;
        try {
          model = await _ensureModel();
        } catch (e) {
          print('[LLM] load failed: $e');
        }
        if (model == null) return null;
        // A garbled answer (control tokens leaked, a word repeated - llm_text.dart)
        // counts as a failed run; it's tried once more, a little warmer, then given up.
        for (var attempt = 0; attempt < 2; attempt++) {
          final chat = await model.createChat(
            temperature: attempt == 0 ? temperature : (temperature + 0.4).clamp(0.0, 1.0),
            topK: 20,
            modelType: byKey(_loadedKey).type,
          );
          try {
            return await AiProfiler.instance.track(
              model: byKey(_loadedKey).name,
              task: task,
              where: _where,
              input: prompt.length.toDouble(),
              unit: 'chars',
              run: () async {
                await chat.addQueryChunk(Message.text(isUser: true, text: prompt));
                final response = await chat.generateChatResponse();
                final raw = response is TextResponse ? response.token : response.toString();
                final out = cleanLlmText(raw);
                if (out == null) throw const GarbledAnswer();
                return out;
              },
            );
          } on GarbledAnswer {
            print('[LLM] garbled answer (attempt ${attempt + 1})');
          }
        }
        if (++_garbledRun >= 3) await _switchAway();
        throw const GarbledAnswer();
      }).then((v) {
        _garbledRun = 0;
        return v;
      });

  // ---- a model that keeps answering garbled text ----------------------------------------
  int _garbledRun = 0; // requests in a row that came out garbled

  /// What happened the last time the model was switched away from (AI models shows it), or null.
  Future<String?> switchNotice() async => (await SharedPreferences.getInstance()).getString('llm_switch_notice');
  Future<void> clearSwitchNotice() async => (await SharedPreferences.getInstance()).remove('llm_switch_notice');

  /// Three garbled answers in a row: another installed model takes over (the
  /// smallest first is not a good guess - the one listed first that isn't
  /// this one), and AI models says so. With no other model, it only says so.
  Future<void> _switchAway() async {
    _garbledRun = 0;
    final cur = byKey(_loadedKey ?? await selectedKey());
    GemmaModelInfo? next;
    for (final m in models) {
      if (m.key != cur.key && await isInstalled(m)) {
        next = m;
        break;
      }
    }
    final p = await SharedPreferences.getInstance();
    if (next == null) {
      await p.setString(
        'llm_switch_notice',
        '${cur.name} keeps answering garbled text on this phone. Install another language model below and use it instead.',
      );
      return;
    }
    try {
      await install(next); // installed already: makes it the one used
      await p.setString(
        'llm_switch_notice',
        '${cur.name} kept answering garbled text on this phone, so ${next.name} is used now. '
            'You can pick ${cur.name} again below.',
      );
      print('[LLM] switched from ${cur.name} to ${next.name}: garbled answers');
    } catch (e) {
      print('[LLM] switch failed: $e');
    }
  }

  /// A quick check of the language model (AI models > Speed > Test): a short
  /// question, the answer as it came out, how long it took, and whether it's
  /// usable. Null without a model.
  Future<({String model, bool ok, String reply, int ms, String backend})?> selfTest() async {
    if (!await hasModel()) return null;
    return _serial(() async {
      final sw = Stopwatch()..start();
      final model = await _ensureModel();
      if (model == null) return null;
      final info = byKey(_loadedKey);
      final chat = await model.createChat(temperature: 0.2, topK: 20, modelType: info.type);
      final raw = await AiProfiler.instance.track(
        model: info.name,
        task: 'Test',
        where: _where,
        run: () async {
          await chat.addQueryChunk(
            Message.text(isUser: true, text: 'In one short sentence: why is the sky blue? /no_think'),
          );
          final r = await chat.generateChatResponse();
          return r is TextResponse ? r.token : r.toString();
        },
      );
      final clean = cleanLlmText(raw);
      return (
        model: info.name,
        ok: clean != null,
        reply: clean ?? (raw.length > 300 ? '${raw.substring(0, 300)}...' : raw),
        ms: sw.elapsedMilliseconds,
        backend: _where(),
      );
    });
  }

  /// Answers [question] from memo excerpts ([contexts], numbered 1..n), citing
  /// them as [1], [2]... Null when no model is installed.
  Future<String?> ask(String question, List<String> contexts) async {
    if (!await hasModel()) return null;
    final buf = StringBuffer();
    var budget = 2600; // keep prompt + answer inside the 2048-token context
    for (var i = 0; i < contexts.length && budget > 200; i++) {
      final c = contexts[i].length > budget ? contexts[i].substring(0, budget) : contexts[i];
      buf.writeln('[${i + 1}] $c');
      budget -= c.length;
    }
    final out = await _generate(
      task: 'Ask AI',
      'Answer the question using ONLY these notes from the user\'s voice memos. '
      'Cite the notes you used like [1] or [2]. If the notes don\'t contain the answer, say so briefly. '
      'Answer in the language of the question, in at most 5 sentences.\n\n'
      'Notes:\n$buf\nQuestion: $question /no_think',
    );
    return out;
  }

  /// The voice assistant's fallback for requests its own rules don't know
  /// (services/assistant_intents.dart): maps [request] to one of the same
  /// actions, or answers it. Returns {"tool": ..., ...} - "answer" with
  /// "text" for a plain answer - or null without a model.
  /// [previous]: the exchange just before, when this is a follow-up to it.
  Future<Map<String, dynamic>?> assistantRoute(String request, DateTime now, {String? previous}) async {
    if (!await hasModel()) return null;
    final out = await _generate(
      task: 'Assistant',
      'You are the voice assistant of a smartwatch. It is ${now.toIso8601String().substring(0, 16)}.\n'
      'Reply with ONLY one JSON object, no other text.\n'
      'If the request is one of these actions, reply with it:\n'
      '{"tool":"timer","seconds":300}\n'
      '{"tool":"reminder","text":"what to remind","minutes":20}   (in N minutes)\n'
      '{"tool":"reminder","text":"what to remind","time":"18:30","day":"today"}   (day: today or tomorrow)\n'
      '{"tool":"music","cmd":"play"}   (cmd: play, pause, next, prev, volUp, volDown)\n'
      '{"tool":"weather"}  {"tool":"agenda"}  {"tool":"battery"}  {"tool":"find_phone"}\n'
      '{"tool":"dnd","on":true}  {"tool":"memos","query":"what to look for in the user\'s voice memos"}\n'
      'Otherwise answer it yourself, in the language of the request, in at most 2 short sentences:\n'
      '{"tool":"answer","text":"..."}\n\n'
      '${previous == null ? '' : 'This follows on from:\n$previous\n\n'}'
      'Request: $request /no_think',
      temperature: 0.2,
    );
    if (out == null) return null;
    final s = out.replaceAll('```json', '').replaceAll('```', '');
    final a = s.indexOf('{'), b = s.lastIndexOf('}');
    if (a >= 0 && b > a) {
      try {
        final m = jsonDecode(s.substring(a, b + 1));
        if (m is Map<String, dynamic> && m['tool'] is String) return m;
      } catch (_) {}
    }
    // Not JSON: treat what it said as the answer.
    final text = s.trim();
    return text.isEmpty ? null : {'tool': 'answer', 'text': text};
  }

  /// Replies the user could send to [message] (services/smart_replies.dart),
  /// or null without a model.
  Future<List<String>?> suggestReplies(String app, String sender, String message) async {
    if (!await hasModel()) return null;
    final out = await _generate(task: 'Smart replies', SmartReplies.prompt(app, sender, message), temperature: 0.6);
    if (out == null) return null;
    final r = SmartReplies.parse(out);
    return r.isEmpty ? null : r;
  }

  /// A summary of notifications ([lines], oldest first) in [language]:
  /// what needs an answer first, then the rest grouped. Null without a model.
  Future<String?> digest(List<String> lines, String language) async {
    if (!await hasModel()) return null;
    var buf = lines.join('\n');
    if (buf.length > 3000) buf = buf.substring(buf.length - 3000);
    final out = await _generate(
      task: 'Catch me up',
      'These are the user\'s phone notifications, oldest first:\n$buf\n\n'
      'Summarize them for a smartwatch screen in $language, in at most 6 short lines. '
      'First what needs a reply or action (who, what), then the rest grouped by app or person. '
      'No greetings, no markdown. /no_think',
      temperature: 0.3,
    );
    return out?.replaceAll(RegExp(r'[*#`]'), '').trim();
  }

  /// Today's breakfast, lunch and dinner ideas for the Life app: [diet] ("vegan",
  /// "Mediterranean"...), [notes] (allergies, likes, dislikes - the user's own
  /// words), [avoid] (ideas from the last days, so they change), [training]
  /// (a training day: a bit more protein and carbs). Each under ~65 letters
  /// so the watch shows it. Null without a model or when the answer isn't usable.
  Future<(String, String, String)?> mealIdeas({
    required String diet,
    String notes = '',
    List<String> avoid = const [],
    bool training = false,
    DateTime? day,
  }) async {
    if (!await hasModel()) return null;
    const months = [
      'January',
      'February',
      'March',
      'April',
      'May',
      'June',
      'July',
      'August',
      'September',
      'October',
      'November',
      'December',
    ];
    final d = day ?? DateTime.now();
    final out = await _generate(
      task: 'Meal ideas',
      'Suggest today\'s meals for a healthy, balanced day. Diet: $diet. '
      '${notes.trim().isEmpty ? '' : 'About the person (follow it strictly, e.g. allergies): ${notes.trim()}. '}'
      '${training ? 'It is a training day: a little more protein and carbohydrates. ' : ''}'
      'Season: ${months[d.month - 1]}, use seasonal produce. Simple home cooking, mostly whole foods. '
      '${avoid.isEmpty ? '' : 'Do not repeat these recent ones: ${avoid.take(9).join('; ')}. '}'
      'Answer with exactly three lines and nothing else:\n'
      'Breakfast: <dish, under 60 characters>\nLunch: <dish, under 60 characters>\nDinner: <dish, under 60 characters> /no_think',
      temperature: 0.8,
    );
    return out == null ? null : parseMealIdeas(out);
  }

  /// The three dishes from the model's answer ("Breakfast: ..." lines, with or
  /// without markdown), each trimmed to fit the watch; null if one is missing.
  static (String, String, String)? parseMealIdeas(String out) {
    String? pick(String label) {
      final m = RegExp('^\\W*$label\\W*[:\\-]\\s*(.+)\$', caseSensitive: false, multiLine: true).firstMatch(out);
      if (m == null) return null;
      var t = m.group(1)!.replaceAll(RegExp(r'[*#`_]'), '').replaceAll(RegExp(r'\s+'), ' ').trim();
      t = t.replaceAll(RegExp(r'[.;]+$'), '');
      if (t.length > 66) {
        final cut = t.lastIndexOf(' ', 66);
        t = t.substring(0, cut > 30 ? cut : 66);
      }
      return t.length < 4 ? null : t;
    }

    final b = pick('Breakfast'), l = pick('Lunch'), dn = pick('Dinner');
    if (b == null || l == null || dn == null) return null;
    return (b, l, dn);
  }

  /// A message the user dictated, rewritten in [style] ("short", "friendly",
  /// "formal") in the same language, or null without a model.
  Future<String?> rewriteMessage(String text, String style) async {
    if (!await hasModel()) return null;
    final how = switch (style) {
      'short' => 'shorter and to the point, keeping everything that matters',
      'friendly' => 'warmer and friendlier, as to a friend',
      _ => 'polite and formal, as to a colleague or a client',
    };
    final out = await _generate(
      task: 'Rewrite reply',
      'Rewrite this chat message so it is $how. Keep its language and its meaning; fix obvious '
      'speech-to-text mistakes. Reply with the message only, no quotes.\n\n$text /no_think',
      temperature: 0.4,
    );
    return out?.replaceAll(RegExp(r'^["“]|["”]$'), '').trim();
  }

  /// [text] translated into [language], or null without a model.
  Future<String?> translate(String text, String language) async {
    if (!await hasModel()) return null;
    final out = await _generate(
      task: 'Translate',
      'Translate this message into $language. Reply with the translation only.\n\n$text /no_think',
      temperature: 0.1,
    );
    return out?.replaceAll(RegExp(r'^["“]|["”]$'), '').trim();
  }

  /// The evening recap's sentences (evening_recap.dart), or null without a model.
  Future<String?> eveningRecap(String prompt) async {
    if (!await hasModel()) return null;
    return _generate(task: 'Evening recap', prompt, temperature: 0.5);
  }

  /// The few words for the watch face's "Today" complication (today_brief.dart), or null without a model.
  Future<String?> todayHeadline(String prompt) async {
    if (!await hasModel()) return null;
    return _generate(task: 'Today brief', prompt, temperature: 0.3);
  }

  /// A 1-4 word name for a group of memos about [keywords], or null.
  Future<String?> nameCollection(List<String> keywords, List<String> titles) async {
    if (!await hasModel()) return null;
    final out = await _generate(
      task: 'Collection name',
      'These voice memos were grouped together.\nKeywords: ${keywords.join(', ')}\n'
      'Titles: ${titles.take(6).join('; ')}\n'
      'Give the group a short name (1-4 words, no quotes, no punctuation at the end). Reply with the name only. /no_think',
      temperature: 0.3,
    );
    if (out == null) return null;
    final line = out.split('\n').map((l) => l.trim()).firstWhere((l) => l.isNotEmpty, orElse: () => '');
    final name = line.replaceAll(RegExp(r'''^["'*#\- ]+|["'*. ]+$'''), '');
    return name.isEmpty || name.length > 40 ? null : name;
  }

  /// Title, summary, importance (1-5), tags, topics, action items and people
  /// for a transcript. Falls back to local heuristics without a model.
  /// [language]: name of the memo's language ("Italian"), if known.
  Future<Map<String, dynamic>> summarizeAndOrganize(String transcript, {String? language}) =>
      _serial(() => _summarize(transcript, language: language));

  Future<Map<String, dynamic>> _summarize(String transcript, {String? language}) async {
    InferenceModel? model;
    try {
      model = await _ensureModel();
    } catch (e) {
      print('[LLM] load failed: $e');
    }
    if (model == null) return _fallback(transcript);

    // ~3000 characters keeps prompt + memo + answer inside a 2048-token context.
    final text = transcript.length > 3000 ? transcript.substring(0, 3000) : transcript;
    try {
      final chat = await model.createChat(temperature: 0.2, topK: 20, modelType: byKey(_loadedKey).type);
      final response = await AiProfiler.instance.track(
        model: byKey(_loadedKey).name,
        task: 'Memo summary',
        where: _where,
        input: text.length.toDouble(),
        unit: 'chars',
        run: () async {
          await chat.addQueryChunk(
            Message.text(
              isUser: true,
              text:
                  'You organize voice memos recorded on a smartwatch into a personal knowledge base.\n'
                  'Answer with ONLY a JSON object, no other text, all text in '
                  '${language == null || language.isEmpty ? 'the language of the memo' : language}:\n'
                  '{"title": "3-6 word title", '
                  '"summary": "2-3 sentences", '
                  '"importance": 1-5, '
                  '"tags": ["up to 4 of: work, personal, idea, todo, meeting, health, call, errand, finance, travel, family, project, deadline, reminder"], '
                  '"topics": ["2-4 short topic phrases (1-3 words) the memo is about"], '
                  '"actions": ["concrete things to do mentioned, short, empty if none"], '
                  '"people": ["names of people mentioned, empty if none"]}\n\n'
                  'Memo:\n$text /no_think',
            ),
          );
          return chat.generateChatResponse();
        },
      );
      final raw = response is TextResponse ? response.token : response.toString();
      final parsed = _parse(raw);
      if (parsed != null) return parsed;
    } catch (e) {
      print('[LLM] summarize failed: $e');
    }
    return _fallback(transcript);
  }

  Map<String, dynamic>? _parse(String raw) {
    var s = raw.replaceAll(RegExp(r'<think>[\s\S]*?</think>'), '').replaceAll('```json', '').replaceAll('```', '');
    final m = RegExp(r'\{[\s\S]*\}').firstMatch(s);
    if (m == null) return null;
    try {
      final j = jsonDecode(m.group(0)!) as Map<String, dynamic>;
      final summary = (j['summary'] ?? '').toString().trim();
      if (summary.isEmpty) return null;
      var imp = int.tryParse('${j['importance']}') ?? 3;
      imp = imp.clamp(1, 5);
      List<String> list(String k, int max, {bool lower = false}) => (j[k] is List)
          ? (j[k] as List)
                .map((e) => lower ? e.toString().toLowerCase().trim() : e.toString().trim())
                .where((t) => t.isNotEmpty && t.length < 120)
                .take(max)
                .toList()
          : <String>[];
      final title = (j['title'] ?? '').toString().trim();
      return {
        'summary': summary,
        'importance': imp,
        'tags': list('tags', 4, lower: true),
        'title': title.length > 60 ? title.substring(0, 60) : title,
        'topics': list('topics', 4, lower: true),
        'actions': list('actions', 6),
        'people': list('people', 8),
      };
    } catch (_) {
      return null;
    }
  }

  Map<String, dynamic> _fallback(String transcript) {
    final lower = transcript.toLowerCase();
    int importance = 2;
    if (lower.contains('urgent') || lower.contains('asap') || lower.contains('urgente')) {
      importance = 5;
    } else if (lower.contains('important') ||
        lower.contains('deadline') ||
        lower.contains('importante') ||
        lower.contains('scadenza')) {
      importance = 4;
    } else if (lower.contains('todo') || lower.contains('remind') || lower.contains('ricorda')) {
      importance = 3;
    }
    final tags = <String>[];
    void tag(String t, List<String> words) {
      if (words.any(lower.contains)) tags.add(t);
    }

    tag('meeting', ['meeting', 'riunione', 'call with']);
    tag('todo', ['todo', 'to do', 'da fare', 'remember', 'ricorda']);
    tag('idea', ['idea']);
    tag('work', ['work', 'project', 'lavoro', 'progetto']);
    tag('errand', ['buy', 'shop', 'comprare', 'spesa']);
    final firstSentence = transcript.split(RegExp(r'(?<=[.!?])\s')).first;
    final summary = firstSentence.length > 200 ? '${firstSentence.substring(0, 197)}...' : firstSentence;
    // Topics: the memo's most frequent meaningful words.
    final counts = <String, int>{};
    for (final t in TextInsights.terms(transcript)) {
      counts[t] = (counts[t] ?? 0) + 1;
    }
    final forms = TextInsights.surfaceForms([transcript]);
    final top = counts.keys.toList()..sort((a, b) => counts[b]!.compareTo(counts[a]!));
    final topics = top.where((t) => counts[t]! >= 2).take(3).map((t) => forms[t] ?? t).toList();
    final words = firstSentence.split(RegExp(r'\s+'));
    final title = words.take(6).join(' ');
    return {
      'summary': summary,
      'importance': importance,
      'tags': tags,
      'title': title.length > 50 ? title.substring(0, 50) : title,
      'topics': topics,
      'actions': TextInsights.actionItems(transcript),
      'people': TextInsights.people(transcript),
    };
  }
}

/// The model's answer came out garbled twice (llm_text.dart).
class GarbledAnswer implements Exception {
  const GarbledAnswer();
  @override
  String toString() => 'The AI\'s answer came out garbled - try again, or pick another model';
}
