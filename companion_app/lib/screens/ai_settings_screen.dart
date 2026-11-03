import 'package:flutter/material.dart';
import 'package:whisper_ggml/whisper_ggml.dart';
import '../services/diarization_service.dart';
import '../services/gemma_service.dart';
import '../services/tts_service.dart';
import '../services/memo_embeddings.dart';
import '../services/whisper_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'wake_word_screen.dart';

/// The on-device AI: one card per capability with its status; open it to
/// download, pick or delete its models.
class AiSettingsScreen extends StatefulWidget {
  const AiSettingsScreen({super.key});

  @override
  State<AiSettingsScreen> createState() => _AiSettingsScreenState();
}

class _AiSettingsScreenState extends State<AiSettingsScreen> {
  final _whisper = WhisperService.instance;
  final _llm = GemmaService.instance;

  final Map<WhisperModel, bool> _speechDownloaded = {};
  WhisperModel? _speechBusy;
  double _speechProgress = 0;

  final Map<String, bool> _llmInstalled = {};
  String? _llmSelected;
  String? _llmBusy;
  double _llmProgress = 0;
  final _tokenCtrl = TextEditingController();
  bool _loading = true;

  final _diar = DiarizationService.instance;
  bool _diarInstalled = false, _diarOn = true, _diarBusy = false;
  double _diarProgress = 0;

  final Map<String, bool> _visInstalled = {};
  String? _visBusy;
  double _visProgress = 0;

  final _emb = MemoEmbeddings.instance;
  bool _embInstalled = false, _embBusy = false;
  double _embProgress = 0;

  final _tts = TtsService.instance;
  final Map<String, bool> _voiceInstalled = {};
  bool _ttsOn = true;
  String? _voiceBusy;
  double _voiceProgress = 0;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  void dispose() {
    _tokenCtrl.dispose();
    super.dispose();
  }

  Future<void> _refresh() async {
    await _whisper.loadPrefs();
    for (final m in WhisperService.offered) {
      _speechDownloaded[m] = await _whisper.isDownloaded(m);
    }
    for (final m in GemmaService.models) {
      _llmInstalled[m.key] = await _llm.isInstalled(m);
    }
    _llmSelected = await _llm.selectedKey();
    _tokenCtrl.text = await _llm.hfToken();
    _diarInstalled = await _diar.isInstalled();
    _diarOn = await _diar.enabled();
    for (final v in TtsService.voices) {
      _voiceInstalled[v.id] = await _tts.isInstalled(v);
    }
    _ttsOn = await _tts.enabled();
    _embInstalled = await _emb.isInstalled();
    for (final m in GemmaService.visionModels) {
      _visInstalled[m.key] = await _llm.isInstalled(m);
    }
    if (mounted) setState(() => _loading = false);
  }

  String _size(int bytes) => bytes >= 1024 * 1024 * 1024
      ? '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(1)} GB'
      : '${(bytes / (1024 * 1024)).round()} MB';

  Future<void> _downloadSpeech(WhisperModel m) async {
    setState(() {
      _speechBusy = m;
      _speechProgress = 0;
    });
    try {
      await _whisper.download(
        model: m,
        onProgress: (p) {
          if (mounted) setState(() => _speechProgress = p);
        },
      );
      await _whisper.setModel(m);
      _speechDownloaded[m] = true;
      if (mounted) showSnack(context, '${WhisperService.labels[m]} speech model ready');
    } catch (e) {
      if (mounted) showSnack(context, 'Download failed: ${e.toString().replaceFirst('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _speechBusy = null);
    }
  }

  Future<void> _installDiar() async {
    setState(() {
      _diarBusy = true;
      _diarProgress = 0;
    });
    try {
      await _diar.install(
        onProgress: (p) {
          if (mounted) setState(() => _diarProgress = p);
        },
      );
      _diarInstalled = true;
      if (mounted) showSnack(context, 'New memos with several voices are split by speaker');
    } catch (e) {
      if (mounted) showSnack(context, 'Download failed: ${e.toString().replaceFirst('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _diarBusy = false);
    }
  }

  Future<void> _installVision(GemmaModelInfo m) async {
    if (m.gated) await _llm.setHfToken(_tokenCtrl.text);
    setState(() {
      _visBusy = m.key;
      _visProgress = 0;
    });
    try {
      await _llm.installVision(
        m,
        onProgress: (p) {
          if (mounted) setState(() => _visProgress = p);
        },
      );
      _visInstalled[m.key] = true;
      if (mounted) showSnack(context, '${m.name} ready for Look');
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _visBusy = null);
    }
  }

  Future<void> _installEmb() async {
    await _llm.setHfToken(_tokenCtrl.text);
    setState(() {
      _embBusy = true;
      _embProgress = 0;
    });
    try {
      await _emb.install(
        onProgress: (p) {
          if (mounted) setState(() => _embProgress = p);
        },
      );
      _embInstalled = true;
      if (mounted) showSnack(context, 'Memo search and Ask AI now also find memos by meaning');
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _embBusy = false);
    }
  }

  Future<void> _installVoice(TtsVoice v) async {
    setState(() {
      _voiceBusy = v.id;
      _voiceProgress = 0;
    });
    try {
      await _tts.install(
        v,
        onProgress: (p) {
          if (mounted) setState(() => _voiceProgress = p);
        },
      );
      _voiceInstalled[v.id] = true;
      if (mounted) showSnack(context, 'The assistant now answers out loud on the watch');
    } catch (e) {
      if (mounted) showSnack(context, 'Download failed: ${e.toString().replaceFirst('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _voiceBusy = null);
    }
  }

  Future<void> _installLlm(GemmaModelInfo m) async {
    if (m.gated) await _llm.setHfToken(_tokenCtrl.text);
    setState(() {
      _llmBusy = m.key;
      _llmProgress = 0;
    });
    try {
      await _llm.install(
        m,
        onProgress: (p) {
          if (mounted) setState(() => _llmProgress = p);
        },
      );
      _llmInstalled[m.key] = true;
      _llmSelected = m.key;
      if (mounted) showSnack(context, '${m.name} ready for summaries');
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _llmBusy = null);
    }
  }

  final Set<String> _open = {};

  Widget _cap(
    String id,
    IconData icon,
    Color color,
    String title,
    String what,
    String status,
    bool ready,
    List<Widget> children,
  ) {
    return _Capability(
      icon: icon,
      color: color,
      title: title,
      what: what,
      status: status,
      ready: ready,
      open: _open.contains(id),
      onToggle: () => setState(() => _open.contains(id) ? _open.remove(id) : _open.add(id)),
      children: children,
    );
  }

  List<Widget> _spaced(List<Widget> cards) => [
    for (var i = 0; i < cards.length; i++) ...[if (i > 0) const SizedBox(height: 10), cards[i]],
  ];

  @override
  Widget build(BuildContext context) {
    final speechReady = _speechDownloaded[_whisper.model] == true;
    final llmReady = _llmSelected != null && _llmInstalled[_llmSelected] == true;
    final llmName = GemmaService.models.where((m) => m.key == _llmSelected).map((m) => m.name).firstOrNull;
    final voices = _voiceInstalled.values.where((x) => x).length;
    final vision = _visInstalled.values.where((x) => x).length;
    final readyCount = [
      speechReady,
      llmReady,
      _diarInstalled,
      _embInstalled,
      voices > 0,
      vision > 0,
    ].where((x) => x).length;

    return Scaffold(
      appBar: AppBar(title: const Text('AI models')),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
              children: [
                Panel(
                  gradient: const LinearGradient(
                    begin: Alignment.topLeft,
                    end: Alignment.bottomRight,
                    colors: [Color(0xFF17142B), Color(0xFF0B0B10)],
                  ),
                  child: Row(
                    children: [
                      Ring(
                        value: readyCount / 6,
                        size: 64,
                        stroke: 6,
                        color: AppColors.accent,
                        child: Text(
                          '$readyCount/6',
                          style: const TextStyle(color: AppColors.text, fontWeight: FontWeight.w700),
                        ),
                      ),
                      const SizedBox(width: 16),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(
                              speechReady ? 'Running on this phone' : 'Start with transcription',
                              style: const TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w700),
                            ),
                            const SizedBox(height: 4),
                            Text(
                              speechReady
                                  ? 'Nothing leaves the phone. Add more below when you want them.'
                                  : 'It turns memos and questions into text - everything else builds on it.',
                              style: const TextStyle(color: AppColors.textDim, fontSize: 12.5, height: 1.35),
                            ),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
                const SectionLabel('Core'),
                _cap(
                  'speech',
                  Icons.mic_rounded,
                  AppColors.accent,
                  'Transcription',
                  'Memos, the assistant and voice replies to text (Whisper)',
                  speechReady ? 'Ready · ${WhisperService.labels[_whisper.model]}' : 'Not set up',
                  speechReady,
                  [
                    ..._spaced([
                      for (final m in WhisperService.offered)
                        _ModelCard(
                          title: WhisperService.labels[m]!,
                          subtitle: '${WhisperService.blurbs[m]} · ${_size(WhisperService.sizes[m]!)}',
                          selected: _whisper.model == m,
                          installed: _speechDownloaded[m] == true,
                          busy: _speechBusy == m,
                          progress: _speechProgress,
                          onInstall: _speechBusy == null ? () => _downloadSpeech(m) : null,
                          onSelect: () async {
                            await _whisper.setModel(m);
                            setState(() {});
                          },
                          onDelete: () async {
                            await _whisper.deleteModel(m);
                            _speechDownloaded[m] = false;
                            setState(() {});
                          },
                        ),
                    ]),
                    const SizedBox(height: 4),
                    Row(
                      children: [
                        const Icon(Icons.translate_rounded, color: AppColors.textDim, size: 20),
                        const SizedBox(width: 12),
                        const Expanded(
                          child: Text('Language', style: TextStyle(color: AppColors.text, fontSize: 14)),
                        ),
                        DropdownButton<String>(
                          value: _whisper.language,
                          underline: const SizedBox.shrink(),
                          dropdownColor: AppColors.surfaceLight,
                          items: [
                            for (final e in WhisperService.languages.entries)
                              DropdownMenuItem(value: e.key, child: Text(e.value)),
                          ],
                          onChanged: (v) async {
                            if (v == null) return;
                            await _whisper.setLanguage(v);
                            setState(() {});
                          },
                        ),
                      ],
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                _cap(
                  'llm',
                  Icons.auto_awesome_rounded,
                  AppColors.accent2,
                  'Summaries & answers',
                  'Memo summaries, Ask AI, smart replies, Catch me up (local LLM)',
                  llmReady ? 'Ready · $llmName' : 'Basic summaries only',
                  llmReady,
                  _spaced([
                    for (final m in GemmaService.models)
                      _ModelCard(
                        title: m.name,
                        subtitle:
                            '${m.description}${m.gated ? ' · needs the Hugging Face token' : ''} · '
                            '${_size(m.sizeBytes)}',
                        selected: _llmSelected == m.key,
                        installed: _llmInstalled[m.key] == true,
                        busy: _llmBusy == m.key,
                        progress: _llmProgress,
                        onInstall: _llmBusy == null ? () => _installLlm(m) : null,
                        onSelect: () => _installLlm(m), // install() just re-activates an installed model
                        onDelete: () async {
                          await _llm.uninstall(m);
                          _llmInstalled[m.key] = false;
                          if (_llmSelected == m.key) _llmSelected = null;
                          setState(() {});
                        },
                      ),
                  ]),
                ),
                const SectionLabel('Extras'),
                _cap(
                  'speakers',
                  Icons.groups_rounded,
                  AppColors.accent3,
                  'Who said what',
                  'Splits conversations in memos by speaker',
                  _diarInstalled ? (_diarOn ? 'On' : 'Installed, off') : 'Not installed',
                  _diarInstalled && _diarOn,
                  [
                    _ModelCard(
                      title: 'Speakers in memos',
                      subtitle:
                          '"Speaker 1 / Speaker 2", name them in the memo. sherpa-onnx, pyannote + CAM++ · '
                          '${_size(DiarizationService.sizeBytes)}',
                      selected: _diarOn,
                      installed: _diarInstalled,
                      busy: _diarBusy,
                      progress: _diarProgress,
                      onInstall: _diarBusy ? null : _installDiar,
                      onSelect: () async {
                        await _diar.setEnabled(true);
                        setState(() => _diarOn = true);
                      },
                      onDelete: () async {
                        await _diar.delete();
                        setState(() => _diarInstalled = false);
                      },
                    ),
                    if (_diarInstalled)
                      SwitchListTile(
                        contentPadding: const EdgeInsets.symmetric(horizontal: 4),
                        title: const Text(
                          'Split new memos by speaker',
                          style: TextStyle(color: AppColors.text, fontSize: 14),
                        ),
                        subtitle: const Text(
                          'For older memos: "Transcribe again" in the memo',
                          style: TextStyle(color: AppColors.textDim, fontSize: 12),
                        ),
                        value: _diarOn,
                        onChanged: (v) async {
                          await _diar.setEnabled(v);
                          setState(() => _diarOn = v);
                        },
                      ),
                  ],
                ),
                const SizedBox(height: 10),
                _cap(
                  'search',
                  Icons.manage_search_rounded,
                  AppColors.success,
                  'Search by meaning',
                  'Finds memos by what they mean, in search, Ask AI and the assistant',
                  _embInstalled ? 'Ready' : 'Not installed',
                  _embInstalled,
                  [
                    _ModelCard(
                      title: 'EmbeddingGemma (4-bit)',
                      subtitle: 'Needs the Hugging Face token · ${_size(MemoEmbeddings.sizeBytes)}',
                      selected: true,
                      installed: _embInstalled,
                      busy: _embBusy,
                      progress: _embProgress,
                      onInstall: _embBusy ? null : _installEmb,
                      onSelect: () {},
                      onDelete: () async {
                        await _emb.delete();
                        setState(() => _embInstalled = false);
                      },
                    ),
                  ],
                ),
                const SizedBox(height: 10),
                _cap(
                  'voice',
                  Icons.volume_up_rounded,
                  AppColors.warning,
                  'Spoken answers',
                  'The assistant answers out loud on the watch (Piper voices)',
                  voices > 0
                      ? (_ttsOn ? 'On · $voices voice${voices > 1 ? 's' : ''}' : 'Installed, off')
                      : 'Not installed',
                  voices > 0 && _ttsOn,
                  [
                    ..._spaced([
                      for (final v in TtsService.voices)
                        _ModelCard(
                          title: v.label,
                          subtitle: 'sherpa-onnx · ${_size(v.sizeBytes)}',
                          selected: _ttsOn,
                          installed: _voiceInstalled[v.id] == true,
                          busy: _voiceBusy == v.id,
                          progress: _voiceProgress,
                          onInstall: _voiceBusy == null ? () => _installVoice(v) : null,
                          onSelect: () async {
                            await _tts.setEnabled(true);
                            setState(() => _ttsOn = true);
                          },
                          onDelete: () async {
                            await _tts.delete(v);
                            setState(() => _voiceInstalled[v.id] = false);
                          },
                        ),
                    ]),
                    if (voices > 0)
                      SwitchListTile(
                        contentPadding: const EdgeInsets.symmetric(horizontal: 4),
                        title: const Text('Say the answers', style: TextStyle(color: AppColors.text, fontSize: 14)),
                        subtitle: const Text(
                          'Italian answers use the Italian voice when it\'s installed',
                          style: TextStyle(color: AppColors.textDim, fontSize: 12),
                        ),
                        value: _ttsOn,
                        onChanged: (v) async {
                          await _tts.setEnabled(v);
                          setState(() => _ttsOn = v);
                        },
                      ),
                  ],
                ),
                const SizedBox(height: 10),
                _cap(
                  'look',
                  Icons.visibility_outlined,
                  AppColors.success,
                  'Look (vision)',
                  'Describe, read or translate a photo',
                  vision > 0 ? 'Ready' : 'Not installed',
                  vision > 0,
                  _spaced([
                    for (final m in GemmaService.visionModels)
                      _ModelCard(
                        title: m.name,
                        subtitle:
                            '${m.description}${m.gated ? ' · needs the Hugging Face token' : ''} · '
                            '${_size(m.sizeBytes)}',
                        selected: true,
                        installed: _visInstalled[m.key] == true,
                        busy: _visBusy == m.key,
                        progress: _visProgress,
                        onInstall: _visBusy == null ? () => _installVision(m) : null,
                        onSelect: () {},
                        onDelete: () async {
                          await _llm.uninstallVision(m);
                          setState(() => _visInstalled[m.key] = false);
                        },
                      ),
                  ]),
                ),
                const SectionLabel('On the watch'),
                RowGroup(
                  children: [
                    NavRow(
                      icon: Icons.record_voice_over_rounded,
                      title: 'Wake word & offline commands',
                      subtitle: 'Say "Alexa", "Jarvis"... then a command or a question',
                      onTap: () =>
                          Navigator.of(context).push(MaterialPageRoute(builder: (_) => const WakeWordScreen())),
                    ),
                  ],
                ),
                const SectionLabel('Account'),
                Panel(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text(
                        'Hugging Face token',
                        style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
                      ),
                      const SizedBox(height: 4),
                      const Text(
                        'Only for the models marked "needs the Hugging Face token" (Gemma): accept the license on '
                        'the model page on huggingface.co, create a read token and paste it here.',
                        style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.4),
                      ),
                      const SizedBox(height: 12),
                      TextField(
                        controller: _tokenCtrl,
                        obscureText: true,
                        decoration: const InputDecoration(hintText: 'hf_…'),
                        onSubmitted: (v) async {
                          await _llm.setHfToken(v);
                          if (context.mounted) showSnack(context, 'Token saved');
                        },
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 14),
                const Text(
                  'Everything runs on the phone: whisper.cpp for speech, a small local language model for summaries '
                  'and answers. Without a language model, memos still get a short automatic summary.',
                  style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
                ),
              ],
            ),
    );
  }
}

/// One AI capability: what it does and whether it's ready; tap for its models.
class _Capability extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String title, what, status;
  final bool ready, open;
  final VoidCallback onToggle;
  final List<Widget> children;
  const _Capability({
    required this.icon,
    required this.color,
    required this.title,
    required this.what,
    required this.status,
    required this.ready,
    required this.open,
    required this.onToggle,
    required this.children,
  });

  @override
  Widget build(BuildContext context) {
    return Panel(
      padding: EdgeInsets.zero,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          InkWell(
            borderRadius: BorderRadius.circular(22),
            onTap: onToggle,
            child: Padding(
              padding: const EdgeInsets.fromLTRB(14, 14, 10, 14),
              child: Row(
                children: [
                  IconChip(icon, color: color),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          title,
                          style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
                        ),
                        const SizedBox(height: 2),
                        Text(what, style: const TextStyle(color: AppColors.textDim, fontSize: 12.5, height: 1.3)),
                        const SizedBox(height: 8),
                        StatusPill(status, color: ready ? AppColors.success : AppColors.textDim),
                      ],
                    ),
                  ),
                  AnimatedRotation(
                    turns: open ? 0.5 : 0,
                    duration: const Duration(milliseconds: 200),
                    child: const Icon(Icons.expand_more_rounded, color: AppColors.textFaint),
                  ),
                ],
              ),
            ),
          ),
          AnimatedSize(
            duration: const Duration(milliseconds: 220),
            curve: Curves.easeOutCubic,
            alignment: Alignment.topCenter,
            child: open
                ? Padding(
                    padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
                    child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: children),
                  )
                : const SizedBox(width: double.infinity),
          ),
        ],
      ),
    );
  }
}

class _ModelCard extends StatelessWidget {
  final String title;
  final String subtitle;
  final bool selected;
  final bool installed;
  final bool busy;
  final double progress;
  final VoidCallback? onInstall;
  final VoidCallback onSelect;
  final VoidCallback onDelete;

  const _ModelCard({
    required this.title,
    required this.subtitle,
    required this.selected,
    required this.installed,
    required this.busy,
    required this.progress,
    required this.onInstall,
    required this.onSelect,
    required this.onDelete,
  });

  @override
  Widget build(BuildContext context) {
    final active = selected && installed;
    return Panel(
      color: active ? AppColors.accent.withValues(alpha: 0.10) : AppColors.surfaceLight,
      onTap: installed && !selected && !busy ? onSelect : null,
      child: Column(
        children: [
          Row(
            children: [
              Icon(
                active ? Icons.radio_button_checked_rounded : Icons.radio_button_off_rounded,
                color: active ? AppColors.accent : AppColors.textFaint,
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      title,
                      style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
                    ),
                    const SizedBox(height: 2),
                    Text(subtitle, style: const TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.3)),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              if (busy)
                const SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2))
              else if (installed)
                IconButton(
                  tooltip: 'Delete',
                  onPressed: onDelete,
                  icon: const Icon(Icons.delete_outline_rounded, color: AppColors.textDim),
                )
              else
                TextButton(onPressed: onInstall, child: const Text('Download')),
            ],
          ),
          if (busy) ...[
            const SizedBox(height: 12),
            ClipRRect(
              borderRadius: BorderRadius.circular(4),
              child: LinearProgressIndicator(value: progress > 0 ? progress : null, minHeight: 6),
            ),
            const SizedBox(height: 6),
            Align(
              alignment: Alignment.centerRight,
              child: Text(
                progress > 0 ? '${(progress * 100).round()}%' : 'Starting…',
                style: const TextStyle(color: AppColors.textDim, fontSize: 12),
              ),
            ),
          ],
        ],
      ),
    );
  }
}
