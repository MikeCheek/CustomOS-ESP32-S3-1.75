import 'dart:io';

import 'diarization_service.dart';
import 'gemma_service.dart';
import 'memo_embeddings.dart';
import 'model_fetch.dart';
import 'sherpa_asr.dart';
import 'sound_alerts.dart';
import 'tts_service.dart';
import 'whisper_service.dart';

enum AiFeature { transcription, summaries, speakers, search, voice, look, sounds }

extension AiFeatureInfo on AiFeature {
  String get title => switch (this) {
    AiFeature.transcription => 'Transcription',
    AiFeature.summaries => 'Summaries & answers',
    AiFeature.speakers => 'Who said what',
    AiFeature.search => 'Search by meaning',
    AiFeature.voice => 'Spoken answers',
    AiFeature.look => 'Look (vision)',
    AiFeature.sounds => 'Sound alerts',
  };
}

/// One downloadable model, whatever feature it's for: the AI models screen
/// and the packs work on these, ModelJobs runs their installs.
class AiItem {
  final String id; // "asr:...", "wcpp:...", "llm:...", ...
  final AiFeature feature;
  final String name, blurb;
  final int sizeBytes;
  final bool github; // false: Hugging Face (mirrors apply)
  final bool gated; // needs the Hugging Face token
  final String? lang; // voices: ISO code
  final Future<File> Function()? sample; // voices: something to play to hear it
  final Future<bool> Function() installed;
  final Future<void> Function(void Function(double) onProgress) install; // also makes it the one used
  final Future<void> Function() select; // installed already: use this one
  final Future<bool> Function() selected;
  final Future<void> Function() delete;

  const AiItem({
    required this.id,
    required this.feature,
    required this.name,
    required this.blurb,
    required this.sizeBytes,
    required this.github,
    this.gated = false,
    this.lang,
    this.sample,
    required this.installed,
    required this.install,
    required this.select,
    required this.selected,
    required this.delete,
  });

  String get source => github ? 'GitHub' : 'Hugging Face';
}

class AiCatalog {
  static List<AiItem>? _items;

  static List<AiItem> get items => _items ??= _build();
  static AiItem? byId(String id) => items.where((i) => i.id == id).firstOrNull;
  static List<AiItem> of(AiFeature f) => items.where((i) => i.feature == f).toList();

  /// Registers every installer with ModelJobs (main.dart, at start), so an
  /// install cut short by the app closing is finished.
  static void registerJobs() {
    for (final i in items) {
      ModelJobs.instance.register(i.id, i.install);
    }
  }

  static List<AiItem> _build() {
    final w = WhisperService.instance, sa = SherpaAsr.instance, g = GemmaService.instance;
    final tts = TtsService.instance, d = DiarizationService.instance, snd = SoundAlerts.instance;
    return [
      // ---- transcription ----
      for (final m in SherpaAsr.models)
        AiItem(
          id: 'asr:${m.id}',
          feature: AiFeature.transcription,
          name: m.name,
          blurb: '${m.blurb} · sherpa-onnx',
          sizeBytes: m.sizeBytes,
          github: true,
          installed: () => sa.isInstalled(m),
          install: (p) async {
            await sa.install(m, onProgress: p);
            await w.setEngine(m.id);
          },
          select: () => w.setEngine(m.id),
          selected: () async {
            await w.loadPrefs();
            return w.engine == m.id;
          },
          delete: () async {
            await sa.delete(m);
            if (w.engine == m.id) await w.setEngine('');
          },
        ),
      for (final m in WhisperService.offered)
        AiItem(
          id: 'wcpp:${m.name}',
          feature: AiFeature.transcription,
          name: 'Whisper ${WhisperService.labels[m]}',
          blurb: '${WhisperService.blurbs[m]} · whisper.cpp',
          sizeBytes: WhisperService.sizes[m] ?? 0,
          github: false,
          installed: () => w.isDownloaded(m),
          install: (p) async {
            await w.download(model: m, onProgress: p);
            await w.setModel(m);
          },
          select: () => w.setModel(m),
          selected: () async {
            await w.loadPrefs();
            return w.engine.isEmpty && w.model == m;
          },
          delete: () => w.deleteModel(m),
        ),
      // ---- summaries / answers ----
      for (final m in GemmaService.models)
        AiItem(
          id: 'llm:${m.key}',
          feature: AiFeature.summaries,
          name: m.name,
          blurb: m.description,
          sizeBytes: m.sizeBytes,
          github: false,
          gated: m.gated,
          installed: () => g.isInstalled(m),
          install: (p) => g.install(m, onProgress: p),
          select: () => g.install(m), // installed: just makes it the active one
          selected: () async => await g.selectedKey() == m.key,
          delete: () => g.uninstall(m),
        ),
      // ---- speakers ----
      for (final v in DiarizationService.voiceModels)
        AiItem(
          id: 'spk:${v.id}',
          feature: AiFeature.speakers,
          name: v.name,
          blurb: '${v.blurb} · with pyannote segmentation',
          sizeBytes: v.sizeBytes + 7 * 1024 * 1024,
          github: true,
          installed: () async => await d.installedVoiceModel() == v.id,
          install: (p) async {
            await d.install(voice: v, onProgress: p);
            await d.setEnabled(true);
          },
          select: () => d.install(voice: v),
          selected: () async => await d.installedVoiceModel() == v.id && await d.enabled(),
          delete: () => d.delete(),
        ),
      // ---- search by meaning ----
      AiItem(
        id: 'emb:embeddinggemma',
        feature: AiFeature.search,
        name: 'EmbeddingGemma (4-bit)',
        blurb: 'Finds memos by what they mean',
        sizeBytes: MemoEmbeddings.sizeBytes,
        github: false,
        gated: true,
        installed: () => MemoEmbeddings.instance.isInstalled(),
        install: (p) => MemoEmbeddings.instance.install(onProgress: p),
        select: () async {},
        selected: () => MemoEmbeddings.instance.isInstalled(),
        delete: () => MemoEmbeddings.instance.delete(),
      ),
      // ---- voices ----
      for (final v in TtsService.voices)
        AiItem(
          id: 'tts:${v.id}',
          feature: AiFeature.voice,
          name: v.label,
          blurb: 'Piper voice · sherpa-onnx',
          sizeBytes: v.sizeBytes,
          github: true,
          lang: v.lang,
          sample: () => tts.sample(v),
          installed: () => tts.isInstalled(v),
          install: (p) async {
            await tts.install(v, onProgress: p);
            await tts.setEnabled(true);
          },
          select: () => tts.setEnabled(true),
          selected: () async => await tts.isInstalled(v) && await tts.enabled(),
          delete: () => tts.delete(v),
        ),
      // ---- look ----
      for (final m in GemmaService.visionModels)
        AiItem(
          id: 'vis:${m.key}',
          feature: AiFeature.look,
          name: m.name,
          blurb: m.description,
          sizeBytes: m.sizeBytes,
          github: false,
          gated: m.gated,
          installed: () => g.isInstalled(m),
          install: (p) => g.installVision(m, onProgress: p),
          select: () => g.installVision(m),
          selected: () async => (await g.visionModel())?.key == m.key,
          delete: () => g.uninstallVision(m),
        ),
      // ---- sound alerts ----
      for (final m in SoundAlerts.models)
        AiItem(
          id: 'snd:${m.id}',
          feature: AiFeature.sounds,
          name: m.name,
          blurb: '${m.blurb} · CED audio tagging',
          sizeBytes: m.sizeBytes,
          github: true,
          installed: () async => await snd.installedModel() == m.id,
          install: (p) => snd.install(model: m, onProgress: p),
          select: () => snd.install(model: m),
          selected: () async => await snd.installedModel() == m.id,
          delete: () => snd.delete(),
        ),
    ];
  }

  /// The phone's language ("it", "en"...).
  static String phoneLang() {
    final l = Platform.localeName.split(RegExp('[_-]')).first.toLowerCase();
    return l.isEmpty ? 'en' : l;
  }

  /// A voice for [lang], else English.
  static String voiceFor(String lang) {
    final v = TtsService.voices.where((v) => v.lang == lang).firstOrNull ?? TtsService.voices.first;
    return 'tts:${v.id}';
  }
}

/// A set of models that sets up several features in one go.
class AiPack {
  final String id, name, blurb;
  final List<String> Function() items;
  const AiPack(this.id, this.name, this.blurb, this.items);

  int get sizeBytes => items().map((i) => AiCatalog.byId(i)?.sizeBytes ?? 0).fold(0, (a, b) => a + b);
  bool get needsToken => items().any((i) => AiCatalog.byId(i)?.gated == true);
  bool get githubOnly => items().every((i) => AiCatalog.byId(i)?.github == true);

  static List<AiPack> get all {
    final lang = AiCatalog.phoneLang();
    final voice = AiCatalog.voiceFor(lang);
    final english = lang == 'en';
    return [
      AiPack(
        'essentials',
        'Essentials',
        'Transcription, summaries and answers, who said what, a voice in your language and sound alerts',
        () => [
          english ? 'asr:sherpa-onnx-moonshine-base-en-int8' : 'asr:sherpa-onnx-whisper-base',
          'llm:qwen3-0.6b-int4',
          'spk:campplus-en',
          voice,
          'snd:ced-tiny',
        ],
      ),
      AiPack(
        'github',
        'Without Hugging Face',
        'Everything that comes from GitHub: transcription, who said what, voices and sound alerts. '
            'Summaries stay the short automatic ones',
        () => [
          english ? 'asr:sherpa-onnx-moonshine-base-en-int8' : 'asr:sherpa-onnx-whisper-base',
          'spk:campplus-en',
          voice,
          if (voice != 'tts:${TtsService.voices.first.id}') 'tts:${TtsService.voices.first.id}',
          'snd:ced-mini',
        ],
      ),
      AiPack(
        'everything',
        'Everything',
        'The best of each: accurate transcription, summaries, Look, search by meaning (needs the token), '
            'voices and sound alerts. Several GB',
        () => [
          'asr:sherpa-onnx-whisper-turbo',
          'llm:qwen3-0.6b',
          'vis:fastvlm-0.5b',
          'emb:embeddinggemma',
          'spk:titanet-small',
          voice,
          'snd:ced-small',
        ],
      ),
      AiPack(
        'light',
        'Light',
        'The smallest: quick transcription and short summaries, for older phones',
        () => [
          english ? 'asr:sherpa-onnx-moonshine-tiny-en-int8' : 'asr:sherpa-onnx-whisper-tiny',
          'llm:qwen3-0.6b-int4',
          'snd:ced-tiny',
        ],
      ),
    ];
  }
}
