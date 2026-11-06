import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:permission_handler/permission_handler.dart';
import '../services/ai_catalog.dart';
import '../services/ai_speed.dart';
import '../services/diarization_service.dart';
import '../services/gemma_service.dart';
import '../services/memo_player.dart';
import '../services/model_fetch.dart';
import '../services/tts_service.dart';
import '../services/whisper_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'ai_profile_screen.dart';
import 'wake_word_screen.dart';

/// The on-device AI: one card per feature with its status; open it to
/// download, pick or delete its models (services/ai_catalog.dart). Downloads
/// run in the background and keep going with the app closed
/// (services/model_fetch.dart); "Set up in one go" downloads a pack or a
/// chosen set and makes them the ones used.
class AiSettingsScreen extends StatefulWidget {
  const AiSettingsScreen({super.key});

  @override
  State<AiSettingsScreen> createState() => _AiSettingsScreenState();
}

class _AiSettingsScreenState extends State<AiSettingsScreen> {
  final _whisper = WhisperService.instance;
  final _jobs = ModelJobs.instance;
  final _tokenCtrl = TextEditingController();
  final _mirrorCtrl = TextEditingController();
  final Map<String, bool> _installed = {}, _selected = {};
  final Set<String> _open = {};
  bool _loading = true, _diarOn = true, _ttsOn = true;
  int _running = 0;

  @override
  void initState() {
    super.initState();
    _jobs.addListener(_onJobs);
    _refresh();
  }

  @override
  void dispose() {
    _jobs.removeListener(_onJobs);
    _tokenCtrl.dispose();
    _mirrorCtrl.dispose();
    super.dispose();
  }

  void _onJobs() {
    final n = _jobs.progress.length;
    if (n < _running) _refresh(); // one finished: look again
    _running = n;
    if (mounted) setState(() {});
  }

  Future<void> _refresh() async {
    await _whisper.loadPrefs();
    for (final i in AiCatalog.items) {
      _installed[i.id] = await i.installed();
      _selected[i.id] = _installed[i.id]! && await i.selected();
    }
    _tokenCtrl.text = await GemmaService.instance.hfToken();
    _mirrorCtrl.text = await ModelFetch.instance.mirror();
    _diarOn = await DiarizationService.instance.enabled();
    _ttsOn = await TtsService.instance.enabled();
    if (mounted) setState(() => _loading = false);
  }

  String _size(int bytes) => bytes >= 1024 * 1024 * 1024
      ? '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(1)} GB'
      : '${(bytes / (1024 * 1024)).round()} MB';

  Future<void> _saveToken() async {
    final t = _tokenCtrl.text.trim();
    if (t != await GemmaService.instance.hfToken()) await GemmaService.instance.setHfToken(t);
  }

  Future<void> _install(AiItem i) async {
    await _saveToken();
    if (i.gated && _tokenCtrl.text.trim().isEmpty) {
      if (mounted) showSnack(context, '${i.name} needs a Hugging Face token (Account, at the bottom)');
      return;
    }
    await Permission.notification.request(); // the download's progress notification
    try {
      await _jobs.run(i.id);
      if (mounted) showSnack(context, '${i.name} is ready');
    } on ModelFetchCanceled {
      // nothing to say
    } catch (e) {
      if (mounted) showSnack(context, '${i.name}: ${e.toString().replaceFirst('Exception: ', '')}');
    }
  }

  Future<void> _select(AiItem i) async {
    try {
      await i.select();
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    }
    await _refresh();
  }

  Future<void> _delete(AiItem i) async {
    await i.delete();
    await _refresh();
  }

  Widget _card(AiItem i) {
    final busy = _jobs.busy(i.id);
    final err = _jobs.errors[i.id];
    return _ModelCard(
      title: i.name,
      subtitle: [
        i.blurb,
        _size(i.sizeBytes),
        i.github ? 'GitHub' : (i.gated ? 'Hugging Face, needs the token' : 'Hugging Face'),
        if (err != null && !busy) 'Last try: $err',
      ].join(' · '),
      selected: _selected[i.id] == true,
      installed: _installed[i.id] == true,
      busy: busy,
      progress: (_jobs.progress[i.id] ?? -1).clamp(0.0, 1.0),
      onInstall: busy ? null : () => _install(i),
      onSelect: () => _select(i),
      onDelete: () => _delete(i),
      sampling: _sampling == i.id,
      onSample: i.sample == null || busy ? null : () => _playSample(i),
      sampleHint: i.sample == null ? null : (_installed[i.id] == true ? 'Said on this phone' : 'Recording, online'),
    );
  }

  String? _sampling;

  /// Plays what a voice sounds like (made here when it's installed, else
  /// Piper's recording) - also a check that an installed one works.
  Future<void> _playSample(AiItem i) async {
    if (_sampling != null) return;
    setState(() => _sampling = i.id);
    try {
      final f = await i.sample!();
      if (!mounted) return;
      await ProviderScope.containerOf(context, listen: false).read(memoPlayerProvider.notifier).playPath(f.path);
    } catch (e) {
      if (mounted) showSnack(context, 'Couldn\'t play ${i.name}: ${e.toString().replaceFirst('Exception: ', '')}');
    } finally {
      if (mounted) setState(() => _sampling = null);
    }
  }

  List<Widget> _cards(Iterable<AiItem> items) => [
    for (final (n, i) in items.indexed) ...[if (n > 0) const SizedBox(height: 10), _card(i)],
  ];

  Widget _feature(AiFeature f, IconData icon, Color color, String what, {List<Widget> extra = const []}) {
    final items = AiCatalog.of(f);
    final sel = items.where((i) => _selected[i.id] == true).firstOrNull;
    final busy = items.where((i) => _jobs.busy(i.id)).firstOrNull;
    final installed = items.where((i) => _installed[i.id] == true).length;
    final status = busy != null
        ? 'Downloading ${busy.name}'
        : sel != null
        ? 'Ready · ${sel.name}'
        : installed > 0
        ? 'Installed, not in use'
        : 'Not set up';
    return _Capability(
      icon: icon,
      color: color,
      title: f.title,
      what: what,
      status: status,
      ready: sel != null,
      open: _open.contains(f.name),
      onToggle: () => setState(() => _open.contains(f.name) ? _open.remove(f.name) : _open.add(f.name)),
      children: [...extra, ..._cards(items)],
    );
  }

  Future<void> _openPacks() async {
    await _saveToken();
    if (!mounted) return;
    final ids = await showModalBottomSheet<List<String>>(
      context: context,
      isScrollControlled: true,
      backgroundColor: AppColors.surface,
      builder: (_) => _PacksSheet(installed: Map.of(_installed), hasToken: _tokenCtrl.text.trim().isNotEmpty),
    );
    if (ids == null || ids.isEmpty || !mounted) return;
    await Permission.notification.request();
    if (mounted) {
      showSnack(
        context,
        'Downloading ${ids.length} model${ids.length == 1 ? '' : 's'} - they keep going with the app closed',
      );
    }
    final failed = await _jobs.runAll(ids);
    await _refresh();
    if (!mounted) return;
    showSnack(
      context,
      failed.isEmpty
          ? 'All set up'
          : 'Set up, except ${failed.map((id) => AiCatalog.byId(id)?.name ?? id).join(', ')} - try those again',
    );
  }

  @override
  Widget build(BuildContext context) {
    final features = AiFeature.values;
    final ready = features.where((f) => AiCatalog.of(f).any((i) => _selected[i.id] == true)).length;
    final transcribing = AiCatalog.of(AiFeature.transcription).any((i) => _selected[i.id] == true);
    final active = [for (final e in _jobs.progress.entries) (e.key, AiCatalog.byId(e.key), e.value)];

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
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Row(
                        children: [
                          Ring(
                            value: ready / features.length,
                            size: 64,
                            stroke: 6,
                            color: AppColors.accent,
                            child: Text(
                              '$ready/${features.length}',
                              style: const TextStyle(color: AppColors.text, fontWeight: FontWeight.w700),
                            ),
                          ),
                          const SizedBox(width: 16),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text(
                                  transcribing ? 'Running on this phone' : 'Start with transcription',
                                  style: const TextStyle(
                                    color: AppColors.text,
                                    fontSize: 16,
                                    fontWeight: FontWeight.w700,
                                  ),
                                ),
                                const SizedBox(height: 4),
                                const Text(
                                  'Nothing leaves the phone. Downloads keep going with the app closed.',
                                  style: TextStyle(color: AppColors.textDim, fontSize: 12.5, height: 1.35),
                                ),
                              ],
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 14),
                      FilledButton.icon(
                        onPressed: _openPacks,
                        icon: const Icon(Icons.download_for_offline_outlined),
                        label: const Text('Set up in one go'),
                      ),
                    ],
                  ),
                ),
                if (active.isNotEmpty) ...[
                  SectionLabel(
                    'Downloading',
                    trailing: TextButton(
                      onPressed: () async {
                        await _jobs.cancelAll();
                        if (context.mounted) showSnack(context, 'Downloads stopped');
                      },
                      child: const Text('Stop all'),
                    ),
                  ),
                  Panel(
                    padding: const EdgeInsets.fromLTRB(16, 10, 6, 6),
                    child: Column(
                      children: [
                        for (final (id, item, p) in active) ...[
                          Row(
                            children: [
                              Expanded(
                                child: Text(
                                  item?.name ?? id,
                                  style: const TextStyle(color: AppColors.text, fontSize: 14),
                                ),
                              ),
                              Text(
                                _jobs.paused.contains(id)
                                    ? 'Paused'
                                    : p < 0
                                    ? 'Starting…'
                                    : (p >= 1 ? 'Setting up…' : '${(p * 100).round()}%'),
                                style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                              ),
                              // pause/resume only while it's downloading (not while unpacking)
                              if (p < 1)
                                IconButton(
                                  visualDensity: VisualDensity.compact,
                                  tooltip: _jobs.paused.contains(id) ? 'Resume' : 'Pause',
                                  icon: Icon(
                                    _jobs.paused.contains(id) ? Icons.play_arrow_rounded : Icons.pause_rounded,
                                    color: AppColors.textDim,
                                  ),
                                  onPressed: () => _jobs.paused.contains(id) ? _jobs.resumeJob(id) : _jobs.pause(id),
                                ),
                              IconButton(
                                visualDensity: VisualDensity.compact,
                                tooltip: 'Cancel',
                                icon: const Icon(Icons.close_rounded, color: AppColors.textDim),
                                onPressed: () => _jobs.cancel(id),
                              ),
                            ],
                          ),
                          Padding(
                            padding: const EdgeInsets.only(right: 10),
                            child: LinearProgressIndicator(
                              value: _jobs.paused.contains(id) ? (p > 0 ? p : 0) : (p > 0 && p < 1 ? p : null),
                              minHeight: 5,
                            ),
                          ),
                          const SizedBox(height: 10),
                        ],
                      ],
                    ),
                  ),
                ],
                const SectionLabel('Core'),
                _feature(
                  AiFeature.transcription,
                  Icons.mic_rounded,
                  AppColors.accent,
                  'Memos, the assistant and voice replies to text: sherpa-onnx models from GitHub, or whisper.cpp',
                  extra: [
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
                    const SizedBox(height: 8),
                  ],
                ),
                const SizedBox(height: 10),
                _feature(
                  AiFeature.summaries,
                  Icons.auto_awesome_rounded,
                  AppColors.accent2,
                  'Memo summaries, Ask AI, smart replies, Catch me up (a local language model)',
                ),
                const SectionLabel('Extras'),
                _feature(
                  AiFeature.speakers,
                  Icons.groups_rounded,
                  AppColors.accent3,
                  'Splits conversations in memos by speaker',
                  extra: [
                    if (AiCatalog.of(AiFeature.speakers).any((i) => _installed[i.id] == true))
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
                          await DiarizationService.instance.setEnabled(v);
                          await _refresh();
                        },
                      ),
                  ],
                ),
                const SizedBox(height: 10),
                _feature(
                  AiFeature.search,
                  Icons.manage_search_rounded,
                  AppColors.success,
                  'Finds memos by what they mean, in search, Ask AI and the assistant',
                ),
                const SizedBox(height: 10),
                _feature(
                  AiFeature.voice,
                  Icons.volume_up_rounded,
                  AppColors.warning,
                  'The assistant answers out loud on the watch, in the language it was asked in',
                  extra: [
                    if (AiCatalog.of(AiFeature.voice).any((i) => _installed[i.id] == true))
                      SwitchListTile(
                        contentPadding: const EdgeInsets.symmetric(horizontal: 4),
                        title: const Text('Say the answers', style: TextStyle(color: AppColors.text, fontSize: 14)),
                        value: _ttsOn,
                        onChanged: (v) async {
                          await TtsService.instance.setEnabled(v);
                          await _refresh();
                        },
                      ),
                  ],
                ),
                const SizedBox(height: 10),
                _feature(
                  AiFeature.look,
                  Icons.visibility_outlined,
                  AppColors.success,
                  'Describe, read or translate a photo',
                ),
                const SizedBox(height: 10),
                _feature(
                  AiFeature.sounds,
                  Icons.hearing_rounded,
                  AppColors.warning,
                  'The phone listens for doorbells, alarms, a baby crying',
                ),
                const SectionLabel('Speed'),
                const _SpeedPanel(),
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
                const SectionLabel('Account and sources'),
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
                        'Only for models marked "needs the token" (Google Gemma): accept the license on the model '
                        'page on huggingface.co, create a read token and paste it here.',
                        style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.4),
                      ),
                      const SizedBox(height: 10),
                      TextField(
                        controller: _tokenCtrl,
                        obscureText: true,
                        decoration: const InputDecoration(hintText: 'hf_…'),
                        onSubmitted: (_) async {
                          await _saveToken();
                          if (context.mounted) showSnack(context, 'Token saved');
                        },
                      ),
                      const SizedBox(height: 18),
                      const Text(
                        'Mirror for Hugging Face files',
                        style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
                      ),
                      const SizedBox(height: 4),
                      const Text(
                        'A server with the same paths as huggingface.co (your own, or a regional mirror), tried '
                        'first. If a file can\'t be had from there or from Hugging Face, hf-mirror.com is tried. '
                        'GitHub models don\'t need it.',
                        style: TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.4),
                      ),
                      const SizedBox(height: 10),
                      TextField(
                        controller: _mirrorCtrl,
                        keyboardType: TextInputType.url,
                        decoration: const InputDecoration(hintText: 'https://… (empty: none)'),
                        onSubmitted: (v) async {
                          await ModelFetch.instance.setMirror(v);
                          if (context.mounted) showSnack(context, v.trim().isEmpty ? 'No mirror' : 'Mirror saved');
                        },
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 14),
                const Text(
                  'Everything runs on the phone. Downloads go through the system, so they continue in the background '
                  'and after the app is closed, pick up where they stopped, and try mirrors; a notification shows '
                  'the progress. A model that finished while the app was closed is set up the next time it opens.',
                  style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
                ),
              ],
            ),
    );
  }
}

/// Packs and a free choice: returns the ids to download and set up.
class _PacksSheet extends StatefulWidget {
  final Map<String, bool> installed;
  final bool hasToken;
  const _PacksSheet({required this.installed, required this.hasToken});

  @override
  State<_PacksSheet> createState() => _PacksSheetState();
}

class _PacksSheetState extends State<_PacksSheet> {
  String _pack = 'essentials';
  final Set<String> _custom = {};

  List<String> _ids() {
    final all = _pack == 'custom' ? _custom.toList() : AiPack.all.firstWhere((p) => p.id == _pack).items();
    // what's already there is only made the one in use, not downloaded again
    return [
      for (final id in all)
        if (widget.hasToken || AiCatalog.byId(id)?.gated != true) id,
    ];
  }

  String _size(int bytes) => bytes >= 1024 * 1024 * 1024
      ? '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(1)} GB'
      : '${(bytes / (1024 * 1024)).round()} MB';

  int _toDownload(List<String> ids) => ids
      .where((id) => widget.installed[id] != true)
      .map((id) => AiCatalog.byId(id)?.sizeBytes ?? 0)
      .fold(0, (a, b) => a + b);

  @override
  Widget build(BuildContext context) {
    final ids = _ids();
    final skipped = (_pack == 'custom' ? _custom.toList() : AiPack.all.firstWhere((p) => p.id == _pack).items())
        .where((id) => !ids.contains(id))
        .toList();
    return DraggableScrollableSheet(
      expand: false,
      initialChildSize: 0.85,
      maxChildSize: 0.95,
      builder: (context, scroll) => ListView(
        controller: scroll,
        padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
        children: [
          Center(
            child: Container(
              width: 40,
              height: 4,
              decoration: BoxDecoration(color: AppColors.border, borderRadius: BorderRadius.circular(2)),
            ),
          ),
          const SizedBox(height: 14),
          const Text(
            'Set up in one go',
            style: TextStyle(color: AppColors.text, fontSize: 20, fontWeight: FontWeight.w700),
          ),
          const SizedBox(height: 4),
          const Text(
            'Downloads one after another in the background - you can close the app. Each becomes the model '
            'its feature uses.',
            style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
          ),
          const SizedBox(height: 14),
          for (final p in AiPack.all) ...[
            _choice(
              id: p.id,
              title: p.name,
              text: p.blurb,
              tags: [_size(p.sizeBytes), if (p.githubOnly) 'no Hugging Face', if (p.needsToken) 'some need the token'],
            ),
            const SizedBox(height: 8),
          ],
          _choice(id: 'custom', title: 'Choose', text: 'Pick the models yourself', tags: const []),
          if (_pack == 'custom') ...[
            for (final f in AiFeature.values) ...[
              SectionLabel(f.title),
              for (final i in AiCatalog.of(f))
                CheckboxListTile(
                  dense: true,
                  contentPadding: EdgeInsets.zero,
                  value: _custom.contains(i.id) || widget.installed[i.id] == true,
                  onChanged: widget.installed[i.id] == true
                      ? null
                      : (v) => setState(() => v == true ? _custom.add(i.id) : _custom.remove(i.id)),
                  title: Text(i.name, style: const TextStyle(color: AppColors.text, fontSize: 14)),
                  subtitle: Text(
                    '${_size(i.sizeBytes)} · ${widget.installed[i.id] == true ? 'installed' : i.source}'
                    '${i.gated ? ' · needs the token' : ''}',
                    style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                  ),
                ),
            ],
          ],
          if (skipped.isNotEmpty) ...[
            const SizedBox(height: 10),
            Text(
              'Left out without a Hugging Face token: ${skipped.map((id) => AiCatalog.byId(id)?.name ?? id).join(', ')}',
              style: const TextStyle(color: AppColors.warning, fontSize: 12),
            ),
          ],
          const SizedBox(height: 16),
          FilledButton(
            onPressed: ids.isEmpty ? null : () => Navigator.pop(context, ids),
            child: Text(
              ids.isEmpty
                  ? 'Choose something'
                  : 'Download ${_size(_toDownload(ids))} and set up ${ids.length} model${ids.length == 1 ? '' : 's'}',
            ),
          ),
        ],
      ),
    );
  }

  Widget _choice({required String id, required String title, required String text, required List<String> tags}) {
    final on = _pack == id;
    return Panel(
      color: on ? AppColors.accent.withValues(alpha: 0.12) : null,
      onTap: () => setState(() => _pack = id),
      padding: const EdgeInsets.all(14),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(
            on ? Icons.radio_button_checked_rounded : Icons.radio_button_off_rounded,
            color: on ? AppColors.accent : AppColors.textFaint,
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600),
                ),
                const SizedBox(height: 2),
                Text(text, style: const TextStyle(color: AppColors.textDim, fontSize: 12.5, height: 1.35)),
                if (tags.isNotEmpty) ...[
                  const SizedBox(height: 6),
                  Text(tags.join(' · '), style: const TextStyle(color: AppColors.accent2, fontSize: 12)),
                ],
              ],
            ),
          ),
        ],
      ),
    );
  }
}

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
  final VoidCallback? onSample; // voices: hear it
  final bool sampling;
  final String? sampleHint;

  const _ModelCard({
    this.onSample,
    this.sampling = false,
    this.sampleHint,
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
              if (sampleHint != null)
                sampling
                    ? const Padding(
                        padding: EdgeInsets.all(13),
                        child: SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2)),
                      )
                    : IconButton(
                        tooltip: 'Play a sample (${sampleHint!.toLowerCase()})',
                        onPressed: onSample,
                        icon: const Icon(Icons.play_circle_outline_rounded, color: AppColors.accent),
                      ),
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

/// Where the language model runs (summaries, answers, smart replies, translation, Look).
class _SpeedPanel extends StatefulWidget {
  const _SpeedPanel();

  @override
  State<_SpeedPanel> createState() => _SpeedPanelState();
}

class _SpeedPanelState extends State<_SpeedPanel> {
  String _mode = 'auto';
  bool _gpuFailed = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final m = await AiSpeed.mode();
    final f = await AiSpeed.gpuFailed();
    if (mounted) {
      setState(() {
        _mode = m;
        _gpuFailed = f;
      });
    }
  }

  Future<void> _set(String m) async {
    await AiSpeed.setMode(m);
    await GemmaService.instance.reload(); // the next request loads it on the new choice
    await _load();
  }

  @override
  Widget build(BuildContext context) {
    final now = AiSpeed.lastBackend;
    final note = _mode == 'cpu'
        ? 'The CPU: slower, but it works on every phone.'
        : _gpuFailed && _mode == 'auto'
        ? 'The GPU didn\'t work on this phone, so it uses the CPU. Choose GPU to try again.'
        : 'The GPU reads a long memo many times faster than the CPU; '
              'if it doesn\'t work, the CPU takes over by itself.';
    return Panel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'Run the language model on',
            style: TextStyle(color: AppColors.text, fontWeight: FontWeight.w600),
          ),
          const SizedBox(height: 8),
          SegmentedButton<String>(
            segments: const [
              ButtonSegment(value: 'auto', label: Text('Auto')),
              ButtonSegment(value: 'gpu', label: Text('GPU')),
              ButtonSegment(value: 'cpu', label: Text('CPU')),
            ],
            selected: {_mode},
            onSelectionChanged: (v) => _set(v.first),
          ),
          const SizedBox(height: 8),
          Text(note, style: const TextStyle(color: AppColors.textDim, fontSize: 12, height: 1.35)),
          if (now != null)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Text(
                'Last loaded on the ${now.toUpperCase()}.',
                style: const TextStyle(color: AppColors.textDim, fontSize: 12),
              ),
            ),
          const SizedBox(height: 6),
          Text(
            'Transcription, who said what and the voices use ${AiSpeed.threads()} CPU cores.',
            style: const TextStyle(color: AppColors.textDim, fontSize: 12),
          ),
          const SizedBox(height: 8),
          OutlinedButton.icon(
            onPressed: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const AiProfileScreen())),
            icon: const Icon(Icons.speed_rounded, size: 18),
            label: const Text('Performance: run times, GPU or CPU, crashes'),
          ),
        ],
      ),
    );
  }
}
