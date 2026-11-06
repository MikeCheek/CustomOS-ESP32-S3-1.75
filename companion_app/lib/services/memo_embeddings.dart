import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;
import 'package:flutter_gemma/flutter_gemma.dart';
import 'package:path_provider/path_provider.dart';
import 'ble_protocol.dart';
import 'gemma_service.dart';
import 'knowledge_base.dart';
import 'ai_profiler.dart';
import 'model_fetch.dart';

/// Search by meaning: memos and questions as vectors from an on-phone
/// embedding model (EmbeddingGemma 300M, 4-bit, multilingual - through
/// flutter_gemma, like the summary models), compared by cosine. Finds "that
/// idea about the trip" when the memo said "holiday in Sicily".
///
/// Vectors are cached in a file and only made again when a memo's text
/// changes. Keyword search (knowledge_base.dart) stays; this adds to it.
class MemoEmbeddings {
  MemoEmbeddings._();
  static final MemoEmbeddings instance = MemoEmbeddings._();

  static const _hf = 'https://huggingface.co/google/embeddinggemma-300m-4bit/resolve/main';
  static const modelFile = 'embeddinggemma-300m-4bit.tflite';
  static const sizeBytes = 75 * 1024 * 1024;

  Map<String, ({int hash, List<double> v})>? _cache;
  EmbeddingModel? _model;

  Future<bool> isInstalled() async {
    try {
      return await FlutterGemma.isModelInstalled(modelFile) &&
          await FlutterGemma.isModelInstalled('sentencepiece.model');
    } catch (_) {
      return false;
    }
  }

  /// Gated by Google on Hugging Face: uses the token saved for Gemma.
  Future<void> install({void Function(double)? onProgress}) async {
    await GemmaService.initializeEngine();
    final token = await GemmaService.instance.hfToken();
    if (token.isEmpty) throw Exception('EmbeddingGemma is gated: add your Hugging Face token below first');
    // In the background, resumable, mirrors (model_fetch.dart); then registered.
    final dir = '${(await getApplicationSupportDirectory()).path}/models/embeddings';
    final auth = {'Authorization': 'Bearer $token'};
    final model = await ModelFetch.instance.fetch(
      label: 'EmbeddingGemma',
      url: '$_hf/model.tflite',
      dest: File('$dir/$modelFile'),
      sizeBytes: sizeBytes,
      headers: auth,
      onProgress: (p) => onProgress?.call(0.97 * p),
    );
    final tok = await ModelFetch.instance.fetch(
      label: 'EmbeddingGemma tokenizer',
      url: '$_hf/sentencepiece.model',
      dest: File('$dir/sentencepiece.model'),
      headers: auth,
    );
    await FlutterGemma.installEmbedder().modelFromFile(model.path).tokenizerFromFile(tok.path).install();
    onProgress?.call(1);
  }

  Future<void> delete() async {
    await _model?.close();
    _model = null;
    try {
      await FlutterGemma.uninstallModel(modelFile);
    } catch (_) {}
    final own = Directory('${(await getApplicationSupportDirectory()).path}/models/embeddings');
    if (await own.exists()) await own.delete(recursive: true);
    final f = await _file();
    if (await f.exists()) await f.delete();
    _cache = {};
  }

  Future<File> _file() async => File('${(await getApplicationSupportDirectory()).path}/memo_vectors.json');

  Future<Map<String, ({int hash, List<double> v})>> _load() async {
    if (_cache != null) return _cache!;
    final m = <String, ({int hash, List<double> v})>{};
    try {
      final j = jsonDecode(await (await _file()).readAsString()) as Map<String, dynamic>;
      j.forEach((k, e) => m[k] = (hash: e['h'] as int, v: [for (final x in e['v'] as List) (x as num).toDouble()]));
    } catch (_) {}
    return _cache = m;
  }

  Future<void> _save() async {
    final out = {
      for (final e in _cache!.entries) e.key: {'h': e.value.hash, 'v': e.value.v},
    };
    await (await _file()).writeAsString(jsonEncode(out));
  }

  static String _doc(RecordingEntry e) {
    final t = e.transcript.length > 1500 ? e.transcript.substring(0, 1500) : e.transcript;
    return '${e.displayTitle}. ${e.summary} $t';
  }

  Future<EmbeddingModel> _embedder() async {
    if (_model != null) return _model!;
    await GemmaService.initializeEngine();
    // After a restart the active embedder is gone: installing again finds the
    // files on the phone and only registers them.
    if (!FlutterGemma.hasActiveEmbedder()) await install();
    return _model = await FlutterGemma.getActiveEmbedder();
  }

  static double _cos(List<double> a, List<double> b) {
    var d = 0.0, na = 0.0, nb = 0.0;
    for (var i = 0; i < a.length && i < b.length; i++) {
      d += a[i] * b[i];
      na += a[i] * a[i];
      nb += b[i] * b[i];
    }
    return na == 0 || nb == 0 ? 0 : d / math.sqrt(na * nb);
  }

  /// Memos most similar in meaning to [query], best first (score 0..1).
  /// Empty when the model isn't installed.
  Future<List<(RecordingEntry, double)>> rank(String query, List<RecordingEntry> entries, {int limit = 8}) async {
    if (query.trim().length < 3 || !await isInstalled()) return const [];
    final cache = await _load();
    final model = await _embedder();
    var changed = false;
    for (final e in entries) {
      if (e.transcript.isEmpty && e.summary.isEmpty) continue;
      final doc = _doc(e);
      final h = doc.hashCode;
      if (cache[e.name]?.hash == h) continue;
      cache[e.name] = (hash: h, v: await model.generateEmbedding(doc, taskType: TaskType.retrievalDocument));
      changed = true;
    }
    if (changed) await _save();
    final q = await AiProfiler.instance.track(
      model: 'EmbeddingGemma',
      task: 'Search by meaning',
      where: () => 'CPU',
      input: query.length.toDouble(),
      unit: 'chars',
      run: () => model.generateEmbedding(query, taskType: TaskType.retrievalQuery),
    );
    final scored = [
      for (final e in entries)
        if (cache[e.name] != null) (e, _cos(q, cache[e.name]!.v)),
    ]..sort((a, b) => b.$2.compareTo(a.$2));
    return scored.take(limit).toList();
  }

  /// Context for Ask AI: keyword hits and meaning hits together.
  Future<List<(RecordingEntry, String)>> contextFor(KnowledgeBase kb, String question, {int n = 5}) async {
    final byWords = kb.contextFor(question, n: n);
    List<(RecordingEntry, double)> byMeaning = const [];
    try {
      byMeaning = await rank(question, kb.entries, limit: n);
    } catch (e) {
      print('[Embeddings] $e');
    }
    final out = [...byWords];
    for (final (e, s) in byMeaning) {
      if (s < 0.35 || out.any((c) => c.$1.name == e.name)) continue;
      out.add((e, KnowledgeBase.excerpt(e)));
    }
    return out.take(n + 2).toList();
  }
}
