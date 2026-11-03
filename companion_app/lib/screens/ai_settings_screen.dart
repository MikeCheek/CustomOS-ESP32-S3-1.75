import 'package:flutter/material.dart';
import 'package:whisper_ggml/whisper_ggml.dart';
import '../services/diarization_service.dart';
import '../services/gemma_service.dart';
import '../services/whisper_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'wake_word_screen.dart';

/// Download/select the on-device models used for voice memos.
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
      await _whisper.download(model: m, onProgress: (p) {
        if (mounted) setState(() => _speechProgress = p);
      });
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
      await _diar.install(onProgress: (p) {
        if (mounted) setState(() => _diarProgress = p);
      });
      _diarInstalled = true;
      if (mounted) showSnack(context, 'New memos with several voices are split by speaker');
    } catch (e) {
      if (mounted) showSnack(context, 'Download failed: ${e.toString().replaceFirst('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _diarBusy = false);
    }
  }

  Future<void> _installLlm(GemmaModelInfo m) async {
    if (m.gated) await _llm.setHfToken(_tokenCtrl.text);
    setState(() {
      _llmBusy = m.key;
      _llmProgress = 0;
    });
    try {
      await _llm.install(m, onProgress: (p) {
        if (mounted) setState(() => _llmProgress = p);
      });
      _llmInstalled[m.key] = true;
      _llmSelected = m.key;
      if (mounted) showSnack(context, '${m.name} ready for summaries');
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _llmBusy = null);
    }
  }

  @override
  Widget build(BuildContext context) {
    final speechReady = _speechDownloaded[_whisper.model] == true;
    final llmReady = _llmSelected != null && _llmInstalled[_llmSelected] == true;

    return Scaffold(
      appBar: AppBar(title: const Text('Transcription & AI')),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
              children: [
                Row(
                  children: [
                    Expanded(
                      child: StatTile(
                        icon: Icons.mic_rounded,
                        color: speechReady ? AppColors.success : AppColors.textDim,
                        value: speechReady ? 'Ready' : 'Not set up',
                        label: 'Transcription',
                      ),
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      child: StatTile(
                        icon: Icons.auto_awesome_rounded,
                        color: llmReady ? AppColors.success : AppColors.textDim,
                        value: llmReady ? 'Ready' : 'Basic',
                        label: 'Summaries',
                      ),
                    ),
                  ],
                ),
                const SectionLabel('Speech to text (Whisper)'),
                for (final m in WhisperService.offered) ...[
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
                  const SizedBox(height: 10),
                ],
                Panel(
                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
                  child: Row(
                    children: [
                      const Icon(Icons.translate_rounded, color: AppColors.textDim),
                      const SizedBox(width: 14),
                      const Expanded(child: Text('Language', style: TextStyle(color: AppColors.text, fontSize: 15))),
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
                ),
                const SectionLabel('On the watch'),
                Panel(
                  onTap: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const WakeWordScreen())),
                  child: const Row(
                    children: [
                      Icon(Icons.record_voice_over_rounded, color: AppColors.textDim),
                      SizedBox(width: 14),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text('Wake word', style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                            SizedBox(height: 2),
                            Text('Say "Alexa", "Jarvis"... to open the assistant',
                                style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.3)),
                          ],
                        ),
                      ),
                      Icon(Icons.chevron_right_rounded, color: AppColors.textFaint),
                    ],
                  ),
                ),
                const SectionLabel('Who said what (speaker split)'),
                _ModelCard(
                  title: 'Speakers in memos',
                  subtitle: 'Splits conversations by voice: "Speaker 1 / Speaker 2", name them in the memo. '
                      'sherpa-onnx, pyannote + CAM++ · ${_size(DiarizationService.sizeBytes)}',
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
                    title: const Text('Split new memos by speaker', style: TextStyle(color: AppColors.text, fontSize: 14)),
                    subtitle: const Text('For older memos: "Transcribe again" in the memo',
                        style: TextStyle(color: AppColors.textDim, fontSize: 12)),
                    value: _diarOn,
                    onChanged: (v) async {
                      await _diar.setEnabled(v);
                      setState(() => _diarOn = v);
                    },
                  ),
                const SectionLabel('Summaries (on-device LLM)'),
                for (final m in GemmaService.models) ...[
                  _ModelCard(
                    title: m.name,
                    subtitle: '${m.description} · ${_size(m.sizeBytes)}',
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
                  const SizedBox(height: 10),
                ],
                Panel(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('Hugging Face token (for Gemma)',
                          style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                      const SizedBox(height: 4),
                      const Text(
                        'Gemma models are gated: accept the license on the model page on huggingface.co, '
                        'create a read token in your account settings and paste it here.',
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
                  'Everything runs on the phone: recordings are transcribed with whisper.cpp and summarized with a small '
                  'local language model. Without a summary model, memos still get a short automatic summary.',
                  style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
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
      color: active ? AppColors.accent.withValues(alpha: 0.10) : null,
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
                    Text(title, style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
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
              child: Text(progress > 0 ? '${(progress * 100).round()}%' : 'Starting…',
                  style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
            ),
          ],
        ],
      ),
    );
  }
}
