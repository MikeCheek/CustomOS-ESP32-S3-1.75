import 'dart:io';
import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../providers/notes_provider.dart';
import '../providers/settings_provider.dart';
import '../services/knowledge_base.dart';
import '../services/memo_player.dart';
import '../services/ble_protocol.dart';
import '../services/whisper_service.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';
import 'ai_settings_screen.dart';
import 'memo_insights.dart';
import 'phone_record_sheet.dart';
import 'recording_detail.dart';

/// Voice memos - recorded on the watch or on the phone: download,
/// transcribe, summarize.
class NotesScreen extends ConsumerStatefulWidget {
  const NotesScreen({super.key});

  @override
  ConsumerState<NotesScreen> createState() => _NotesScreenState();
}

class _NotesScreenState extends ConsumerState<NotesScreen> {
  bool _speechReady = true;
  String _filter = 'all'; // all / fav / important / todo

  @override
  void initState() {
    super.initState();
    _checkModels();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (ref.read(bleProvider).isConnected) ref.read(notesProvider.notifier).syncFromWatch();
    });
  }

  Future<void> _checkModels() async {
    await WhisperService.instance.loadPrefs();
    final ok = await WhisperService.instance.isDownloaded();
    if (mounted) setState(() => _speechReady = ok);
  }

  /// Pick a WAV/MP3 on the phone and add it to the watch's recordings.
  Future<void> _addRecording() async {
    try {
      final r = await FilePicker.platform.pickFiles(type: FileType.custom, allowedExtensions: ['wav', 'mp3']);
      final f = r?.files.single;
      if (f == null || f.path == null) return;
      final msg = await ref.read(notesProvider.notifier).addRecording(File(f.path!), f.name);
      if (mounted) showSnack(context, msg);
    } catch (e) {
      if (mounted) showSnack(context, e.toString().replaceFirst('Exception: ', ''));
    }
  }

  Future<void> _openAi() async {
    await Navigator.push(context, MaterialPageRoute(builder: (_) => const AiSettingsScreen()));
    _checkModels();
  }

  @override
  Widget build(BuildContext context) {
    final notes = ref.watch(notesProvider);
    final ble = ref.watch(bleProvider);

    ref.listen<NotesState>(notesProvider, (prev, next) {
      if (next.error != null && next.error != prev?.error) showSnack(context, next.error!);
    });

    final shown = notes.recordings.where((r) => switch (_filter) {
          'fav' => r.favorite,
          'important' => r.importance >= 4 && r.summary.isNotEmpty,
          'todo' => r.transcript.isEmpty,
          _ => true,
        }).toList();

    return DefaultTabController(
      length: 3,
      child: Scaffold(
      appBar: AppBar(
        title: const Text('Voice memos'),
        bottom: const TabBar(tabs: [Tab(text: 'Memos'), Tab(text: 'Insights'), Tab(text: 'Themes')]),
        actions: [
          IconButton(
            tooltip: 'Search & ask',
            onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => const MemoSearchScreen())),
            icon: const Icon(Icons.search_rounded),
          ),
          if (ble.isConnected)
            IconButton(
              tooltip: 'Add a recording to the watch',
              onPressed: notes.uploadingName != null ? null : _addRecording,
              icon: const Icon(Icons.library_add_rounded),
            ),
          if (ble.isConnected)
            IconButton(
              tooltip: 'Sync from watch',
              onPressed: notes.isSyncing ? null : () => ref.read(notesProvider.notifier).syncFromWatch(),
              icon: notes.isSyncing
                  ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.sync_rounded),
            ),
          PopupMenuButton<String>(
            icon: const Icon(Icons.more_horiz_rounded),
            color: AppColors.surfaceLight,
            onSelected: (v) {
              if (v == 'ai') _openAi();
              if (v == 'clear') _confirmClear();
            },
            itemBuilder: (_) => const [
              PopupMenuItem(value: 'ai', child: Text('Transcription & AI')),
              PopupMenuItem(value: 'clear', child: Text('Remove all from phone')),
            ],
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        heroTag: 'record_memo',
        onPressed: () => showPhoneRecordSheet(context),
        backgroundColor: AppColors.accent3,
        foregroundColor: AppColors.text,
        icon: const Icon(Icons.mic_rounded),
        label: const Text('Record'),
      ),
      body: TabBarView(
        children: [
      RefreshIndicator(
        onRefresh: () => ref.read(notesProvider.notifier).syncFromWatch(),
        child: ListView(
          physics: const AlwaysScrollableScrollPhysics(),
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
          children: [
            _PipelineCard(notes: notes, connected: ble.isConnected, speechReady: _speechReady, onSetup: _openAi),
            if (!_speechReady)
              Padding(
                padding: const EdgeInsets.only(bottom: 12),
                child: Panel(
                  onTap: _openAi,
                  color: AppColors.accent.withValues(alpha: 0.12),
                  child: const Row(
                    children: [
                      IconChip(Icons.auto_awesome_rounded, color: AppColors.accent),
                      SizedBox(width: 14),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text('Set up transcription',
                                style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                            SizedBox(height: 2),
                            Text('Download a speech model once (~140 MB) to turn memos into text.',
                                style: TextStyle(color: AppColors.textDim, fontSize: 13)),
                          ],
                        ),
                      ),
                      Icon(Icons.chevron_right_rounded, color: AppColors.textDim),
                    ],
                  ),
                ),
              ),
            if (notes.uploadingName != null)
              _Banner(
                icon: Icons.upload_rounded,
                title: 'Adding to the watch',
                subtitle: notes.uploadingName!,
                progress: notes.uploadProgress,
              ),
            if (notes.downloadingName != null && notes.totalToDownload > 0)
              _Banner(
                icon: (notes.transferVia ?? '').startsWith('Wi-Fi') ? Icons.wifi_rounded : Icons.bluetooth_rounded,
                title: 'Downloading ${notes.downloadedCount + 1} of ${notes.totalToDownload}',
                subtitle: [
                  notes.downloadingName!,
                  if (notes.transferVia != null) notes.transferVia!,
                  if (notes.transferRate > 0) '${notes.transferRate.round()} KB/s',
                ].join(' · '),
                progress: notes.downloadProgress,
              ),
            if (notes.processingName != null)
              _Banner(
                icon: Icons.auto_awesome_rounded,
                title: notes.processingStage,
                subtitle: notes.processingName!,
                progress: notes.processingProgress,
              ),
            if (notes.isLoading)
              const Padding(padding: EdgeInsets.only(top: 80), child: Center(child: CircularProgressIndicator()))
            else if (notes.recordings.isEmpty)
              Padding(
                padding: const EdgeInsets.only(top: 60),
                child: EmptyState(
                  icon: ble.isConnected ? Icons.mic_none_rounded : Icons.watch_off_outlined,
                  title: ble.isConnected ? 'No memos yet' : 'Watch not connected',
                  subtitle: ble.isConnected
                      ? 'Record with the Recorder app on the watch and pull down to sync, or tap Record to use the phone.'
                      : 'Tap Record to make a memo with the phone. Watch memos appear once it connects.',
                ),
              )
            else ...[
              SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: Row(children: [
                  for (final f in const [('all', 'All'), ('fav', 'Favorites'), ('important', 'Important'), ('todo', 'Not transcribed')]) ...[
                    ChoiceChip(label: Text(f.$2), selected: _filter == f.$1, onSelected: (_) => setState(() => _filter = f.$1)),
                    const SizedBox(width: 6),
                  ],
                ]),
              ),
              const SizedBox(height: 12),
              if (shown.isEmpty)
                const Padding(
                  padding: EdgeInsets.only(top: 30),
                  child: Center(child: Text('No memos here', style: TextStyle(color: AppColors.textDim))),
                ),
              for (final e in shown) ...[
                _MemoCard(
                  entry: e,
                  downloaded: notes.downloadedFiles.contains(e.name),
                  downloading: notes.downloadingName == e.name,
                  processing: notes.processingName == e.name,
                  onOpen: () => Navigator.push(
                    context,
                    MaterialPageRoute(builder: (_) => RecordingDetail(entry: e)),
                  ),
                  onDownload: ble.isConnected
                      ? () => ref.read(notesProvider.notifier).downloadFile(e.name, notes.recordings.indexOf(e))
                      : null,
                  onTranscribe: notes.pipelineRunning
                      ? null
                      : () => _speechReady ? ref.read(notesProvider.notifier).processRecording(e.name) : _openAi(),
                ),
                const SizedBox(height: 10),
              ],
            ],
          ],
        ),
      ),
      const InsightsView(),
      const CollectionsView(),
        ],
      ),
      ),
    );
  }

  void _confirmClear() {
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Remove all from phone?'),
        content: const Text('Deletes downloaded audio, transcripts and summaries on this phone. The watch keeps its recordings.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(
            onPressed: () {
              ref.read(notesProvider.notifier).clearAll();
              Navigator.pop(ctx);
            },
            child: const Text('Remove', style: TextStyle(color: AppColors.danger)),
          ),
        ],
      ),
    );
  }
}

class _Banner extends StatelessWidget {
  final IconData icon;
  final String title;
  final String subtitle;
  final double progress;
  const _Banner({required this.icon, required this.title, required this.subtitle, required this.progress});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Panel(
        child: Column(
          children: [
            Row(
              children: [
                IconChip(icon, color: AppColors.accent2),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(title, style: const TextStyle(color: AppColors.text, fontSize: 14, fontWeight: FontWeight.w600)),
                      Text(subtitle, maxLines: 1, overflow: TextOverflow.ellipsis,
                          style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            ClipRRect(
              borderRadius: BorderRadius.circular(4),
              child: LinearProgressIndicator(value: progress >= 0 ? progress : null, minHeight: 5, color: AppColors.accent2),
            ),
          ],
        ),
      ),
    );
  }
}

class _MemoCard extends StatelessWidget {
  final RecordingEntry entry;
  final bool downloaded;
  final bool downloading;
  final bool processing;
  final VoidCallback onOpen;
  final VoidCallback? onDownload;
  final VoidCallback? onTranscribe;

  const _MemoCard({
    required this.entry,
    required this.downloaded,
    required this.downloading,
    required this.processing,
    required this.onOpen,
    required this.onDownload,
    required this.onTranscribe,
  });

  static Color importanceColor(int i) {
    switch (i) {
      case 5:
        return AppColors.danger;
      case 4:
        return AppColors.warning;
      case 3:
        return AppColors.accent;
      case 2:
        return AppColors.accent2;
      default:
        return AppColors.textDim;
    }
  }

  String _title() => entry.displayTitle;

  String _meta() {
    final kb = entry.size / 1024;
    final dur = fmtDuration(KnowledgeBase.durationOf(entry));
    final size = kb > 1024 ? '${(kb / 1024).toStringAsFixed(1)} MB' : '${kb.round()} KB';
    final when = fmtDate(entry.recordedAt);
    return [if (dur.isNotEmpty) dur, size, if (when.isNotEmpty) when, if (entry.language.isNotEmpty) entry.language.toUpperCase()]
        .join('  ·  ');
  }

  @override
  Widget build(BuildContext context) {
    final hasText = entry.transcript.isNotEmpty;
    final color = importanceColor(entry.importance);
    return Panel(
      onTap: onOpen,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 42,
                height: 42,
                decoration: BoxDecoration(color: color.withValues(alpha: 0.16), shape: BoxShape.circle),
                child: Icon(hasText ? Icons.notes_rounded : Icons.graphic_eq_rounded, color: color, size: 22),
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(children: [
                      Flexible(
                        child: Text(_title(), maxLines: 1, overflow: TextOverflow.ellipsis,
                            style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                      ),
                      if (entry.favorite) ...[
                        const SizedBox(width: 4),
                        const Icon(Icons.star_rounded, size: 16, color: AppColors.warning),
                      ],
                      if (entry.name.startsWith('phone_')) ...[
                        const SizedBox(width: 4),
                        const Tooltip(
                          message: 'Recorded on the phone',
                          child: Icon(Icons.smartphone_rounded, size: 14, color: AppColors.textDim),
                        ),
                      ],
                    ]),
                    const SizedBox(height: 2),
                    Text(_meta(), style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                  ],
                ),
              ),
            ],
          ),
          if (entry.summary.isNotEmpty) ...[
            const SizedBox(height: 12),
            Text(entry.summary, maxLines: 3, overflow: TextOverflow.ellipsis,
                style: const TextStyle(color: AppColors.text, fontSize: 14, height: 1.4)),
          ],
          if (entry.actions.isNotEmpty) ...[
            const SizedBox(height: 8),
            Row(children: [
              const Icon(Icons.task_alt_rounded, size: 14, color: AppColors.success),
              const SizedBox(width: 6),
              Text('${entry.actions.length} to-do${entry.actions.length == 1 ? '' : 's'}',
                  style: const TextStyle(color: AppColors.success, fontSize: 12, fontWeight: FontWeight.w600)),
            ]),
          ],
          if (entry.tags.isNotEmpty) ...[
            const SizedBox(height: 10),
            Wrap(
              spacing: 6,
              runSpacing: 6,
              children: [
                for (final t in entry.tags)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
                    decoration: BoxDecoration(
                      color: AppColors.surfaceHigh,
                      borderRadius: BorderRadius.circular(8),
                    ),
                    child: Text('#$t', style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                  ),
              ],
            ),
          ],
          const SizedBox(height: 12),
          Row(
            children: [
              if (downloaded) ...[
                Consumer(builder: (context, ref, _) {
                  final p = ref.watch(memoPlayerProvider);
                  final playing = p.isMemo(entry.name) && p.playing;
                  return _Action(
                    icon: playing ? Icons.pause_rounded : Icons.play_arrow_rounded,
                    label: playing ? fmtMs(p.posMs) : 'Play',
                    onTap: () => ref.read(memoPlayerProvider.notifier).toggle(entry.name),
                  );
                }),
                const SizedBox(width: 8),
              ],
              if (!downloaded && !downloading)
                _Action(icon: Icons.download_rounded, label: 'Download', onTap: onDownload)
              else if (downloading)
                const _Action(icon: Icons.downloading_rounded, label: 'Downloading…', onTap: null)
              else if (!hasText)
                _Action(
                  icon: Icons.auto_awesome_rounded,
                  label: processing ? 'Working…' : 'Transcribe',
                  onTap: processing ? null : onTranscribe,
                  primary: true,
                )
              else
                const _Action(icon: Icons.check_circle_outline_rounded, label: 'Transcribed', onTap: null),
            ],
          ),
        ],
      ),
    );
  }
}

class _Action extends StatelessWidget {
  final IconData icon;
  final String label;
  final VoidCallback? onTap;
  final bool primary;
  const _Action({required this.icon, required this.label, required this.onTap, this.primary = false});

  @override
  Widget build(BuildContext context) {
    final c = primary ? AppColors.accent : AppColors.text;
    return Material(
      color: primary && onTap != null ? AppColors.accent.withValues(alpha: 0.16) : AppColors.surfaceLight,
      borderRadius: BorderRadius.circular(12),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(icon, size: 16, color: onTap == null ? AppColors.textDim : c),
              const SizedBox(width: 6),
              Text(label, style: TextStyle(color: onTap == null ? AppColors.textDim : c, fontSize: 13, fontWeight: FontWeight.w600)),
            ],
          ),
        ),
      ),
    );
  }
}

/// One tap: get new memos from the watch, download, transcribe, summarize
/// and refresh the insights. Shows the steps while it runs.
class _PipelineCard extends ConsumerWidget {
  final NotesState notes;
  final bool connected, speechReady;
  final VoidCallback onSetup;
  const _PipelineCard({required this.notes, required this.connected, required this.speechReady, required this.onSetup});

  static const _steps = [
    (Icons.sync_rounded, 'Sync'),
    (Icons.download_rounded, 'Download'),
    (Icons.record_voice_over_rounded, 'Transcribe'),
    (Icons.summarize_rounded, 'Summarize'),
    (Icons.insights_rounded, 'Insights'),
  ];

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final n = ref.read(notesProvider.notifier);
    final settings = ref.watch(settingsProvider);
    final running = notes.pipelineRunning;
    final pending = notes.recordings
        .where((r) => r.transcript.isEmpty && !r.noSpeech && !r.name.toLowerCase().endsWith('.mp3'))
        .length;
    final unsummarized = notes.recordings.where((r) => r.transcript.isNotEmpty && r.processedAt == null).length;
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Panel(
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [AppColors.accent.withValues(alpha: 0.22), AppColors.accent2.withValues(alpha: 0.08)],
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const IconChip(Icons.bolt_rounded, color: AppColors.accent),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const Text('Process everything',
                          style: TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w700)),
                      Text(
                        running
                            ? notes.pipelineStage
                            : notes.pipelineReport ??
                                (pending + unsummarized == 0
                                    ? 'All memos are transcribed and summarized'
                                    : [
                                        if (pending > 0) '$pending to transcribe',
                                        if (unsummarized > 0) '$unsummarized to summarize',
                                      ].join(' · ')),
                        maxLines: 2,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                      ),
                    ],
                  ),
                ),
                const SizedBox(width: 8),
                if (running)
                  OutlinedButton(onPressed: n.cancelPipeline, child: const Text('Stop'))
                else
                  FilledButton.icon(
                    onPressed: notes.processingTranscript
                        ? null
                        : () {
                            if (!speechReady && pending > 0) {
                              onSetup();
                              return;
                            }
                            n.runPipeline();
                          },
                    icon: const Icon(Icons.play_arrow_rounded, size: 18),
                    label: const Text('Run'),
                  ),
              ],
            ),
            if (running) ...[
              const SizedBox(height: 14),
              Row(
                children: [
                  for (var i = 0; i < _steps.length; i++) ...[
                    Expanded(
                      child: Column(
                        children: [
                          Icon(
                            i < notes.pipelineStep ? Icons.check_circle_rounded : _steps[i].$1,
                            size: 20,
                            color: i < notes.pipelineStep
                                ? AppColors.success
                                : i == notes.pipelineStep
                                    ? AppColors.accent2
                                    : AppColors.textFaint,
                          ),
                          const SizedBox(height: 4),
                          Text(_steps[i].$2,
                              style: TextStyle(
                                  color: i == notes.pipelineStep ? AppColors.text : AppColors.textDim, fontSize: 10)),
                        ],
                      ),
                    ),
                  ],
                ],
              ),
              const SizedBox(height: 10),
              ClipRRect(
                borderRadius: BorderRadius.circular(4),
                child: LinearProgressIndicator(
                  minHeight: 5,
                  value: notes.pipelineStep == 2 && notes.processingProgress >= 0 && notes.pipelineTotal > 0
                      ? (notes.pipelineDone + notes.processingProgress) / notes.pipelineTotal
                      : notes.pipelineStep == 1 && notes.totalToDownload > 0
                          ? (notes.downloadedCount + notes.downloadProgress) / notes.totalToDownload
                          : notes.pipelineTotal > 0
                              ? notes.pipelineDone / notes.pipelineTotal
                              : null,
                ),
              ),
            ] else ...[
              const SizedBox(height: 4),
              Row(
                children: [
                  Expanded(
                    child: Text(
                      connected ? 'Automatically after each sync' : 'Connect the watch to get new memos',
                      style: const TextStyle(color: AppColors.textDim, fontSize: 12),
                    ),
                  ),
                  Switch(
                    value: settings.autoProcess,
                    onChanged: (v) => ref.read(settingsProvider.notifier).update(settings.copyWith(autoProcess: v)),
                  ),
                ],
              ),
            ],
          ],
        ),
      ),
    );
  }
}
