import 'dart:async';
import 'dart:convert';
import 'package:flutter_gemma/flutter_gemma.dart';
import 'package:flutter_gemma_litertlm/flutter_gemma_litertlm.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'text_insights.dart';

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
      url: 'https://huggingface.co/litert-community/Gemma3-1B-IT/resolve/main/Gemma3-1B-IT_multi-prefill-seq_q4_ekv4096.litertlm',
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

  static GemmaModelInfo byKey(String? key) =>
      models.firstWhere((m) => m.key == key, orElse: () => models.first);

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

  /// Downloads (if needed) and makes [m] the active model. [onProgress] 0..1.
  Future<void> install(GemmaModelInfo m, {void Function(double)? onProgress}) async {
    await initializeEngine();
    final token = await hfToken();
    if (m.gated && token.isEmpty && !await isInstalled(m)) {
      throw Exception('${m.name} is gated: add your Hugging Face token first');
    }
    try {
      await FlutterGemma.installModel(modelType: m.type, fileType: ModelFileType.litertlm)
          .fromNetwork(m.url, token: token.isEmpty ? null : token)
          .withProgress((p) => onProgress?.call(p / 100.0))
          .install();
    } on DownloadException catch (e) {
      throw Exception(e.error.toUserMessage());
    }
    final p = await SharedPreferences.getInstance();
    await p.setString('llm_model', m.key);
    await _closeModel();
  }

  Future<void> uninstall(GemmaModelInfo m) async {
    await initializeEngine();
    if (_loadedKey == m.key) await _closeModel();
    await FlutterGemma.uninstallModel(m.fileName);
    final p = await SharedPreferences.getInstance();
    if (p.getString('llm_model') == m.key) {
      await p.remove('llm_model');
      await FlutterGemma.clearActiveInferenceIdentity();
    }
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
    await FlutterGemma.installModel(modelType: info.type, fileType: ModelFileType.litertlm)
        .fromNetwork(info.url)
        .install();
    _model = await FlutterGemma.getActiveModel(maxTokens: 2048);
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
    _queue = r.then((_) {}, onError: (_) {});
    return r;
  }

  Future<String?> _generate(String prompt, {double temperature = 0.2}) => _serial(() async {
        InferenceModel? model;
        try {
          model = await _ensureModel();
        } catch (e) {
          print('[LLM] load failed: $e');
        }
        if (model == null) return null;
        final chat = await model.createChat(temperature: temperature, topK: 20, modelType: byKey(_loadedKey).type);
        await chat.addQueryChunk(Message.text(isUser: true, text: prompt));
        final response = await chat.generateChatResponse();
        final raw = response is TextResponse ? response.token : response.toString();
        return raw.replaceAll(RegExp(r'<think>[\s\S]*?</think>'), '').trim();
      });

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
      'Answer the question using ONLY these notes from the user\'s voice memos. '
      'Cite the notes you used like [1] or [2]. If the notes don\'t contain the answer, say so briefly. '
      'Answer in the language of the question, in at most 5 sentences.\n\n'
      'Notes:\n$buf\nQuestion: $question /no_think',
    );
    return out;
  }

  /// A 1-4 word name for a group of memos about [keywords], or null.
  Future<String?> nameCollection(List<String> keywords, List<String> titles) async {
    if (!await hasModel()) return null;
    final out = await _generate(
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
      await chat.addQueryChunk(Message.text(
        isUser: true,
        text: 'You organize voice memos recorded on a smartwatch into a personal knowledge base.\n'
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
      ));
      final response = await chat.generateChatResponse();
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
    } else if (lower.contains('important') || lower.contains('deadline') || lower.contains('importante') || lower.contains('scadenza')) {
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
