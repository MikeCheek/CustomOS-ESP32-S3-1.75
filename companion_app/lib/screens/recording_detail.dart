import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../providers/knowledge_provider.dart';
import '../providers/notes_provider.dart';
import '../services/knowledge_base.dart';
import '../services/ble_protocol.dart';
import '../theme/app_theme.dart';
import '../widgets/charts.dart';
import '../widgets/common.dart';
import '../widgets/memo_player_bar.dart';
import '../services/memo_player.dart';
import '../services/whisper_service.dart';
import '../services/text_insights.dart';
import 'memo_insights.dart';

class RecordingDetail extends ConsumerStatefulWidget {
  final RecordingEntry entry;
  const RecordingDetail({super.key, required this.entry});

  @override
  ConsumerState<RecordingDetail> createState() => _RecordingDetailState();
}

class _RecordingDetailState extends ConsumerState<RecordingDetail> {
  late TextEditingController _summary;
  late TextEditingController _title;
  final _tag = TextEditingController();
  bool _editing = false;
  late int _importance;
  late List<String> _tags;

  @override
  void initState() {
    super.initState();
    _summary = TextEditingController(text: widget.entry.summary);
    _title = TextEditingController(text: widget.entry.title);
    _importance = widget.entry.importance;
    _tags = List.from(widget.entry.tags);
  }

  @override
  void dispose() {
    _summary.dispose();
    _title.dispose();
    _tag.dispose();
    super.dispose();
  }

  RecordingEntry _current(NotesState notes) =>
      notes.recordings.firstWhere((e) => e.name == widget.entry.name, orElse: () => widget.entry);

  Future<void> _save(RecordingEntry e) async {
    // copyWith: keeps topics, to-dos, people, favorite...
    await ref.read(notesProvider.notifier).updateRecording(e.copyWith(
          title: _title.text.trim(),
          summary: _summary.text.trim(),
          importance: _importance,
          tags: List.of(_tags),
        ));
    if (!mounted) return;
    setState(() => _editing = false);
    showSnack(context, 'Saved');
  }

  @override
  Widget build(BuildContext context) {
    final notes = ref.watch(notesProvider);
    final e = _current(notes);
    // Outside edit mode, show what's stored (it changes when a summary finishes).
    final importance = _editing ? _importance : e.importance;
    final tags = _editing ? _tags : e.tags;
    final busy = notes.processingName == e.name;
    final downloaded = notes.downloadedFiles.contains(e.name);
    final connected = ref.watch(bleProvider).isConnected;

    return Scaffold(
      appBar: AppBar(
        title: Text(e.displayTitle, overflow: TextOverflow.ellipsis),
        actions: [
          IconButton(
            tooltip: e.favorite ? 'Remove from favorites' : 'Favorite',
            onPressed: () => ref.read(notesProvider.notifier).toggleFavorite(e.name),
            icon: Icon(e.favorite ? Icons.star_rounded : Icons.star_border_rounded,
                color: e.favorite ? AppColors.warning : null),
          ),
          if (_editing)
            IconButton(onPressed: () => _save(e), icon: const Icon(Icons.check_rounded))
          else
            IconButton(
              onPressed: () => setState(() {
                _summary.text = e.summary;
                _title.text = e.title;
                _importance = e.importance;
                _tags = List.from(e.tags);
                _editing = true;
              }),
              icon: const Icon(Icons.edit_outlined),
            ),
          PopupMenuButton<String>(
            icon: const Icon(Icons.more_horiz_rounded),
            color: AppColors.surfaceLight,
            onSelected: (v) async {
              if (v == 'copy') {
                await Clipboard.setData(ClipboardData(text: e.transcript));
                if (context.mounted) showSnack(context, 'Transcript copied');
              } else if (v == 'resum') {
                ref.read(notesProvider.notifier).resummarize(e.name);
              } else if (v == 'retx') {
                ref.read(notesProvider.notifier).processRecording(e.name);
              } else if (v == 'delete') {
                _confirmDelete(e, connected);
              }
            },
            itemBuilder: (_) => [
              if (e.transcript.isNotEmpty) const PopupMenuItem(value: 'copy', child: Text('Copy transcript')),
              if (e.transcript.isNotEmpty) const PopupMenuItem(value: 'resum', child: Text('Summarize again')),
              if (downloaded) const PopupMenuItem(value: 'retx', child: Text('Transcribe again')),
              const PopupMenuItem(value: 'delete', child: Text('Delete')),
            ],
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          if (busy)
            Padding(
              padding: const EdgeInsets.only(bottom: 12),
              child: Panel(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(notes.processingStage, style: const TextStyle(color: AppColors.text, fontWeight: FontWeight.w600)),
                    const SizedBox(height: 10),
                    LinearProgressIndicator(value: notes.processingProgress >= 0 ? notes.processingProgress : null),
                  ],
                ),
              ),
            ),
          if (downloaded) ...[
            MemoPlayerBar(name: e.name),
            const SizedBox(height: 8),
          ],
          if (_editing) ...[
            const SectionLabel('Title'),
            TextField(controller: _title, decoration: InputDecoration(hintText: e.displayTitle)),
          ] else
            Padding(
              padding: const EdgeInsets.only(left: 4, top: 4),
              child: Text(
                [
                  fmtDate(e.recordedAt),
                  fmtDuration(KnowledgeBase.durationOf(e)),
                  if (e.transcript.isNotEmpty) '${e.transcript.split(RegExp(r'\s+')).length} words',
                ].where((s) => s.isNotEmpty).join('  ·  '),
                style: const TextStyle(color: AppColors.textDim, fontSize: 13),
              ),
            ),
          if (!_editing && (e.transcript.isNotEmpty || e.languageSet))
            Padding(
              padding: const EdgeInsets.only(top: 10),
              child: Align(
                alignment: Alignment.centerLeft,
                child: ActionChip(
                  avatar: const Icon(Icons.translate_rounded, size: 16),
                  label: Builder(builder: (_) {
                    // Memos transcribed before languages were tracked: guess now.
                    final code = e.language.isNotEmpty ? e.language : TextInsights.detectLanguage(e.transcript).code;
                    return Text(code.isEmpty
                        ? 'Language unknown · change'
                        : '${WhisperService.languageName(code)} · ${e.languageSet ? 'set by you' : 'detected'}');
                  }),
                  onPressed: busy ? null : () => _pickLanguage(e),
                ),
              ),
            ),
          const SectionLabel('Summary'),
          Panel(
            child: _editing
                ? TextField(
                    controller: _summary,
                    maxLines: null,
                    style: const TextStyle(color: AppColors.text, fontSize: 15, height: 1.45),
                    decoration: const InputDecoration(hintText: 'Write a summary', filled: false, contentPadding: EdgeInsets.zero),
                  )
                : Text(
                    e.summary.isNotEmpty ? e.summary : 'No summary yet.',
                    style: TextStyle(
                        color: e.summary.isNotEmpty ? AppColors.text : AppColors.textDim, fontSize: 15, height: 1.45),
                  ),
          ),
          const SectionLabel('Importance'),
          Row(
            children: [
              for (var i = 1; i <= 5; i++) ...[
                Expanded(
                  child: GestureDetector(
                    onTap: _editing ? () => setState(() => _importance = i) : null,
                    child: Container(
                      height: 44,
                      decoration: BoxDecoration(
                        color: i <= importance ? _color(importance).withValues(alpha: 0.22) : AppColors.surfaceLight,
                        borderRadius: BorderRadius.circular(12),
                      ),
                      alignment: Alignment.center,
                      child: Text('$i',
                          style: TextStyle(
                              color: i <= importance ? _color(importance) : AppColors.textDim,
                              fontWeight: FontWeight.w700)),
                    ),
                  ),
                ),
                if (i < 5) const SizedBox(width: 8),
              ],
            ],
          ),
          const SectionLabel('Tags'),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (final t in tags)
                InputChip(
                  label: Text('#$t'),
                  onDeleted: _editing ? () => setState(() => _tags.remove(t)) : null,
                ),
              if (_editing)
                SizedBox(
                  width: 140,
                  child: TextField(
                    controller: _tag,
                    decoration: const InputDecoration(hintText: 'Add tag', isDense: true),
                    onSubmitted: (v) {
                      final t = v.trim().toLowerCase();
                      if (t.isNotEmpty && !_tags.contains(t)) setState(() => _tags.add(t));
                      _tag.clear();
                    },
                  ),
                ),
              if (tags.isEmpty && !_editing)
                const Text('No tags', style: TextStyle(color: AppColors.textDim)),
            ],
          ),
          if (e.topics.isNotEmpty) ...[
            const SectionLabel('Topics'),
            Wrap(spacing: 8, runSpacing: 8, children: [
              for (final t in e.topics)
                ActionChip(
                  avatar: const Icon(Icons.topic_outlined, size: 16),
                  label: Text(t),
                  onPressed: () =>
                      Navigator.push(context, MaterialPageRoute(builder: (_) => MemoSearchScreen(initialQuery: t))),
                ),
            ]),
          ],
          if (e.actions.isNotEmpty) ...[
            const SectionLabel('To-dos'),
            Panel(
              padding: const EdgeInsets.symmetric(vertical: 6),
              child: Column(children: [for (final a in e.actions) ActionRow(ActionItem(e, a), showMemo: false)]),
            ),
          ],
          if (e.people.isNotEmpty) ...[
            const SectionLabel('People'),
            Wrap(spacing: 8, runSpacing: 8, children: [
              for (final p in e.people)
                ActionChip(
                  avatar: const Icon(Icons.person_outline_rounded, size: 16),
                  label: Text(p),
                  onPressed: () =>
                      Navigator.push(context, MaterialPageRoute(builder: (_) => MemoSearchScreen(initialQuery: p))),
                ),
            ]),
          ],
          const SectionLabel('Transcript'),
          Panel(
            child: e.transcript.isNotEmpty
                ? FollowAlongTranscript(name: e.name, text: e.transcript)
                : Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        !downloaded
                            ? 'Download the recording to transcribe it.'
                            : e.name.toLowerCase().endsWith('.mp3')
                                ? 'MP3 files can\'t be transcribed - only the watch\'s WAV recordings.'
                                : e.noSpeech
                                    ? 'No speech was found in this recording. Try again, or in another language.'
                                    : 'Not transcribed yet.',
                        style: const TextStyle(color: AppColors.textDim),
                      ),
                      const SizedBox(height: 14),
                      if (downloaded && e.name.toLowerCase().endsWith('.mp3'))
                        const SizedBox.shrink()
                      else if (downloaded)
                        FilledButton.icon(
                          onPressed: busy ? null : () => ref.read(notesProvider.notifier).processRecording(e.name),
                          icon: Icon(e.noSpeech ? Icons.refresh_rounded : Icons.auto_awesome_rounded, size: 18),
                          label: Text(e.noSpeech ? 'Try again' : 'Transcribe & summarize'),
                        )
                      else
                        OutlinedButton.icon(
                          onPressed: connected
                              ? () => ref.read(notesProvider.notifier).downloadFile(e.name, notes.recordings.indexOf(e))
                              : null,
                          icon: const Icon(Icons.download_rounded, size: 18),
                          label: Text(connected ? 'Download from watch' : 'Connect the watch to download'),
                        ),
                    ],
                  ),
          ),
          ..._related(e),
        ],
      ),
    );
  }

  /// Wrong language? Pick the right one: the memo is transcribed again in
  /// it, then summary, topics, to-dos and insights are redone.
  Future<void> _pickLanguage(RecordingEntry e) async {
    final langs = WhisperService.languages.entries.where((l) => l.key != 'auto').toList();
    final code = await showModalBottomSheet<String>(
      context: context,
      backgroundColor: AppColors.surface,
      isScrollControlled: true,
      builder: (ctx) => SafeArea(
        child: ConstrainedBox(
          constraints: BoxConstraints(maxHeight: MediaQuery.of(ctx).size.height * 0.7),
          child: ListView(
            shrinkWrap: true,
            children: [
              const Padding(
                padding: EdgeInsets.fromLTRB(20, 18, 20, 4),
                child: Text('Language of this memo',
                    style: TextStyle(color: AppColors.text, fontSize: 17, fontWeight: FontWeight.w700)),
              ),
              const Padding(
                padding: EdgeInsets.fromLTRB(20, 0, 20, 8),
                child: Text('It will be transcribed again in this language, then summarized.',
                    style: TextStyle(color: AppColors.textDim, fontSize: 13)),
              ),
              for (final l in langs)
                ListTile(
                  title: Text(l.value),
                  trailing: l.key == e.language ? const Icon(Icons.check_rounded, color: AppColors.accent) : null,
                  onTap: () => Navigator.pop(ctx, l.key),
                ),
            ],
          ),
        ),
      ),
    );
    if (code == null || !mounted) return;
    final notes = ref.read(notesProvider);
    if (!notes.downloadedFiles.contains(e.name)) {
      // Not on the phone: just remember it for when it gets transcribed.
      await ref.read(notesProvider.notifier).updateRecording(e.copyWith(language: code, languageSet: true));
      if (mounted) showSnack(context, 'Language set - download the memo to transcribe it');
      return;
    }
    if (!await WhisperService.instance.isDownloaded()) {
      if (mounted) showSnack(context, 'Download a speech model in Transcription & AI first');
      return;
    }
    ref.read(notesProvider.notifier).changeLanguage(e.name, code);
  }

  List<Widget> _related(RecordingEntry e) {
    final rel = ref.watch(knowledgeProvider).related(e);
    if (rel.isEmpty) return const [];
    return [
      const SectionLabel('Related memos'),
      for (final r in rel) MemoTile(r.$1, trailing: ScoreBar(r.$2.clamp(0.0, 1.0))),
    ];
  }

  Color _color(int i) {
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

  void _confirmDelete(RecordingEntry e, bool connected) {
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Delete memo?'),
        content: Text(connected
            ? 'Delete "${e.name}" from the watch and this phone?'
            : 'Delete "${e.name}" from this phone? (The watch is not connected, so its copy stays.)'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(
            onPressed: () async {
              Navigator.pop(ctx);
              if (ref.read(memoPlayerProvider).isMemo(e.name)) await ref.read(memoPlayerProvider.notifier).stop();
              if (connected) {
                await ref.read(notesProvider.notifier).deleteFromWatch(e.name);
              } else {
                await ref.read(notesProvider.notifier).deleteLocal(e.name);
              }
              if (mounted) Navigator.pop(context);
            },
            child: const Text('Delete', style: TextStyle(color: AppColors.danger)),
          ),
        ],
      ),
    );
  }
}
