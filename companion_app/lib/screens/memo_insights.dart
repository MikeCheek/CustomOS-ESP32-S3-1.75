import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/knowledge_provider.dart';
import '../services/ble_protocol.dart';
import '../services/gemma_service.dart';
import '../services/knowledge_base.dart';
import '../theme/app_theme.dart';
import '../widgets/charts.dart';
import '../widgets/common.dart';
import 'recording_detail.dart';

// Knowledge views over the voice memos: Insights dashboard, theme
// collections, search with relevance scores, questions to the on-device AI.

String fmtDuration(int secs) {
  if (secs <= 0) return '';
  if (secs < 3600) return '${secs ~/ 60}:${(secs % 60).toString().padLeft(2, '0')}';
  return '${secs ~/ 3600}h ${(secs % 3600) ~/ 60}m';
}

String fmtDate(DateTime? d) {
  if (d == null) return '';
  final now = DateTime.now();
  final today = DateTime(now.year, now.month, now.day);
  final day = DateTime(d.year, d.month, d.day);
  final t = '${d.hour.toString().padLeft(2, '0')}:${d.minute.toString().padLeft(2, '0')}';
  if (day == today) return 'Today $t';
  if (day == today.subtract(const Duration(days: 1))) return 'Yesterday $t';
  return '${d.day}/${d.month}${d.year != now.year ? '/${d.year % 100}' : ''} $t';
}

Color importanceColor(int i) => switch (i) {
      5 => AppColors.danger,
      4 => AppColors.warning,
      3 => AppColors.accent,
      2 => AppColors.accent2,
      _ => AppColors.textDim,
    };

void openMemo(BuildContext context, RecordingEntry e) =>
    Navigator.push(context, MaterialPageRoute(builder: (_) => RecordingDetail(entry: e)));

/// Compact memo row used by the knowledge screens.
class MemoTile extends StatelessWidget {
  final RecordingEntry entry;
  final Widget? trailing;
  final Widget? below;
  const MemoTile(this.entry, {super.key, this.trailing, this.below});

  @override
  Widget build(BuildContext context) {
    final c = importanceColor(entry.importance);
    final meta = [
      fmtDate(entry.recordedAt),
      fmtDuration(KnowledgeBase.durationOf(entry)),
    ].where((s) => s.isNotEmpty).join('  ·  ');
    return Padding(
      padding: const EdgeInsets.only(bottom: 10),
      child: Panel(
        padding: const EdgeInsets.all(14),
        onTap: () => openMemo(context, entry),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(
                  width: 34,
                  height: 34,
                  decoration: BoxDecoration(color: c.withValues(alpha: 0.16), shape: BoxShape.circle),
                  child: Icon(entry.transcript.isNotEmpty ? Icons.notes_rounded : Icons.graphic_eq_rounded, color: c, size: 18),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(children: [
                        Flexible(
                          child: Text(entry.displayTitle, maxLines: 1, overflow: TextOverflow.ellipsis,
                              style: const TextStyle(color: AppColors.text, fontSize: 14, fontWeight: FontWeight.w600)),
                        ),
                        if (entry.favorite) ...[
                          const SizedBox(width: 4),
                          const Icon(Icons.star_rounded, size: 15, color: AppColors.warning),
                        ],
                      ]),
                      if (meta.isNotEmpty) Text(meta, style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                    ],
                  ),
                ),
                if (trailing != null) trailing!,
              ],
            ),
            if (below != null) ...[const SizedBox(height: 10), below!],
          ],
        ),
      ),
    );
  }
}

// ============================================================================================
// Insights dashboard
// ============================================================================================

class InsightsView extends ConsumerWidget {
  const InsightsView({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final kb = ref.watch(knowledgeProvider);
    final done = ref.watch(doneActionsProvider);
    if (kb.isEmpty) {
      return const EmptyState(
        icon: Icons.insights_rounded,
        title: 'No insights yet',
        subtitle: 'Sync and transcribe some memos: topics, trends and to-dos appear here.',
      );
    }
    final topics = kb.topTopics(n: 8);
    final trend = kb.topicTrend(topics.take(5).toList());
    final weeks = kb.weekStarts();
    final perWeek = kb.perWeek();
    final imp = kb.importanceHistogram();
    final tags = kb.tagCounts();
    final people = kb.people(n: 8);
    final actions = kb.actions().where((a) => !done.contains(a.key)).toList();
    final processed = kb.entries.isEmpty ? 0 : (100 * kb.processedCount / kb.entries.length).round();
    final hours = kb.totalSeconds / 3600;
    final highlights = [...kb.entries.where((e) => e.importance >= 4 && e.summary.isNotEmpty)]
      ..sort((a, b) => (b.recordedAt ?? DateTime(2000)).compareTo(a.recordedAt ?? DateTime(2000)));
    final thisWeek = perWeek.last, lastWeek = perWeek[perWeek.length - 2];

    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
      children: [
        GridView.count(
          crossAxisCount: 2,
          shrinkWrap: true,
          physics: const NeverScrollableScrollPhysics(),
          mainAxisSpacing: 10,
          crossAxisSpacing: 10,
          childAspectRatio: 1.55,
          children: [
            StatTile(icon: Icons.mic_rounded, value: '${kb.entries.length}', label: 'memos'),
            StatTile(
                icon: Icons.schedule_rounded,
                color: AppColors.accent2,
                value: hours >= 1 ? '${hours.toStringAsFixed(1)} h' : '${(kb.totalSeconds / 60).round()} min',
                label: 'recorded'),
            StatTile(
                icon: Icons.text_fields_rounded,
                color: AppColors.accent3,
                value: _compact(kb.totalWords),
                label: 'words transcribed'),
            StatTile(icon: Icons.auto_awesome_rounded, color: AppColors.success, value: '$processed%', label: 'processed'),
          ],
        ),
        if (topics.isNotEmpty) ...[
          const SectionLabel('What you talk about'),
          Panel(
            child: HBarList(
              items: [for (final t in topics) (t.label, t.memos)],
              onTap: (i) => _openTopic(context, kb, topics[i]),
            ),
          ),
        ],
        if (trend.values.any((s) => s.any((v) => v > 0))) ...[
          const SectionLabel('Topic trends · 8 weeks'),
          Panel(
            child: LineChart(
              series: trend,
              xLabels: [
                for (var i = 0; i < 8; i++)
                  i % 2 == 1 ? '${weeks[weeks.length - 8 + i].day}/${weeks[weeks.length - 8 + i].month}' : ''
              ],
            ),
          ),
        ],
        SectionLabel('Memos per week',
            trailing: Text(
              thisWeek == lastWeek
                  ? 'same as last week'
                  : thisWeek > lastWeek
                      ? '+${thisWeek - lastWeek} vs last week'
                      : '${thisWeek - lastWeek} vs last week',
              style: TextStyle(color: thisWeek >= lastWeek ? AppColors.success : AppColors.textDim, fontSize: 12),
            )),
        Panel(
          child: BarChart(
            values: perWeek,
            labels: [for (var i = 0; i < weeks.length; i++) i % 3 == 2 ? '${weeks[i].day}/${weeks[i].month}' : ''],
          ),
        ),
        const SectionLabel('When you record'),
        Panel(
          child: Heatmap(
            grid: kb.heatmap(),
            rowLabels: const ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'],
            colLabels: const ['0', '3', '6', '9', '12', '15', '18', '21'],
          ),
        ),
        if (imp.any((v) => v > 0)) ...[
          const SectionLabel('Importance'),
          Panel(
            child: DonutChart(
              segments: [
                for (var i = 4; i >= 0; i--)
                  (['Low', 'Minor', 'Normal', 'High', 'Critical'][i], imp[i], importanceColor(i + 1)),
              ],
              centerTop: '${imp[3] + imp[4]}',
              centerBottom: 'important',
            ),
          ),
        ],
        const SectionLabel('Memo length'),
        Panel(
          child: BarChart(
            values: kb.lengthHistogram(),
            labels: const ['<30s', '30s-1m', '1-3m', '3-10m', '>10m'],
            color: AppColors.accent3,
            highlightLast: false,
            height: 120,
          ),
        ),
        if (actions.isNotEmpty) ...[
          SectionLabel('Open to-dos',
              trailing: TextButton(
                onPressed: () => Navigator.push(context, MaterialPageRoute(builder: (_) => const ActionsScreen())),
                child: Text('All ${actions.length}'),
              )),
          Panel(
            padding: const EdgeInsets.symmetric(vertical: 6),
            child: Column(children: [for (final a in actions.take(6)) ActionRow(a)]),
          ),
        ],
        if (people.isNotEmpty) ...[
          const SectionLabel('People mentioned'),
          Panel(
            child: HBarList(
              items: [for (final p in people) (p.$1, p.$2)],
              color: AppColors.accent3,
              onTap: (i) => Navigator.push(
                  context, MaterialPageRoute(builder: (_) => MemoSearchScreen(initialQuery: people[i].$1))),
            ),
          ),
        ],
        if (tags.isNotEmpty) ...[
          const SectionLabel('Tags'),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (final t in tags.entries.take(24))
                ActionChip(
                  label: Text('#${t.key}  ${t.value}'),
                  onPressed: () => Navigator.push(
                      context, MaterialPageRoute(builder: (_) => MemoSearchScreen(initialTag: t.key))),
                ),
            ],
          ),
        ],
        if (highlights.isNotEmpty) ...[
          const SectionLabel('Important memos'),
          for (final e in highlights.take(5))
            MemoTile(e,
                below: Text(e.summary, maxLines: 2, overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.35))),
        ],
      ],
    );
  }

  static String _compact(int n) => n >= 10000 ? '${(n / 1000).round()}k' : n >= 1000 ? '${(n / 1000).toStringAsFixed(1)}k' : '$n';

  void _openTopic(BuildContext context, KnowledgeBase kb, TopicStat t) {
    final items = [for (final i in t.memoIdx) kb.entries[i]]
      ..sort((a, b) => (b.recordedAt ?? DateTime(2000)).compareTo(a.recordedAt ?? DateTime(2000)));
    Navigator.push(context, MaterialPageRoute(builder: (_) => MemoListScreen(title: t.label, items: items)));
  }
}

/// A to-do from a memo, with a checkbox (kept on the phone).
class ActionRow extends ConsumerWidget {
  final ActionItem item;
  final bool showMemo;
  const ActionRow(this.item, {super.key, this.showMemo = true});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final done = ref.watch(doneActionsProvider).contains(item.key);
    return InkWell(
      onTap: () => openMemo(context, item.entry),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Checkbox(value: done, onChanged: (_) => ref.read(doneActionsProvider.notifier).toggle(item.key)),
            Expanded(
              child: Padding(
                padding: const EdgeInsets.only(top: 12, bottom: 10, right: 10),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(item.text,
                        style: TextStyle(
                          color: done ? AppColors.textDim : AppColors.text,
                          fontSize: 14,
                          decoration: done ? TextDecoration.lineThrough : null,
                        )),
                    if (showMemo)
                      Text('${item.entry.displayTitle} · ${fmtDate(item.entry.recordedAt)}',
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: const TextStyle(color: AppColors.textFaint, fontSize: 12)),
                  ],
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class ActionsScreen extends ConsumerStatefulWidget {
  const ActionsScreen({super.key});
  @override
  ConsumerState<ActionsScreen> createState() => _ActionsScreenState();
}

class _ActionsScreenState extends ConsumerState<ActionsScreen> {
  bool _showDone = false;

  @override
  Widget build(BuildContext context) {
    final kb = ref.watch(knowledgeProvider);
    final done = ref.watch(doneActionsProvider);
    final all = kb.actions();
    final list = _showDone ? all : all.where((a) => !done.contains(a.key)).toList();
    return Scaffold(
      appBar: AppBar(title: const Text('To-dos from memos'), actions: [
        TextButton(
            onPressed: () => setState(() => _showDone = !_showDone), child: Text(_showDone ? 'Hide done' : 'Show done')),
      ]),
      body: list.isEmpty
          ? const EmptyState(
              icon: Icons.task_alt_rounded,
              title: 'Nothing to do',
              subtitle: 'Things you say you need to do in a memo show up here.')
          : ListView(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
              children: [
                Panel(padding: const EdgeInsets.symmetric(vertical: 6), child: Column(children: [for (final a in list) ActionRow(a)])),
              ],
            ),
    );
  }
}

class MemoListScreen extends StatelessWidget {
  final String title;
  final List<RecordingEntry> items;
  const MemoListScreen({super.key, required this.title, required this.items});

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(title: Text(title)),
        body: ListView(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
          children: [
            for (final e in items)
              MemoTile(e,
                  below: e.summary.isEmpty
                      ? null
                      : Text(e.summary, maxLines: 2, overflow: TextOverflow.ellipsis,
                          style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.35))),
          ],
        ),
      );
}

// ============================================================================================
// Collections
// ============================================================================================

class CollectionsView extends ConsumerStatefulWidget {
  const CollectionsView({super.key});
  @override
  ConsumerState<CollectionsView> createState() => _CollectionsViewState();
}

class _CollectionsViewState extends ConsumerState<CollectionsView> {
  bool _byTag = false;
  bool _naming = false;

  Future<void> _nameAll() async {
    if (ref.read(collectionNamesProvider.notifier).naming) {
      showSnack(context, 'Already naming the themes - one moment');
      return;
    }
    setState(() => _naming = true);
    final n = await ref.read(collectionNamesProvider.notifier).nameMissing(ref.read(knowledgeProvider), all: true);
    if (!mounted) return;
    setState(() => _naming = false);
    showSnack(context, n > 0 ? 'Named $n collections' : 'Install a summary model in AI settings to name collections');
  }

  @override
  Widget build(BuildContext context) {
    final kb = ref.watch(knowledgeProvider);
    ref.watch(collectionNamesProvider);
    final names = ref.read(collectionNamesProvider.notifier);
    final cols = kb.collections;
    final byTag = kb.byTag;
    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
      children: [
        Wrap(
          spacing: 8,
          runSpacing: 4,
          crossAxisAlignment: WrapCrossAlignment.center,
          children: [
            ChoiceChip(label: const Text('By theme'), selected: !_byTag, onSelected: (_) => setState(() => _byTag = false)),
            ChoiceChip(label: const Text('By tag'), selected: _byTag, onSelected: (_) => setState(() => _byTag = true)),
            if (!_byTag && cols.isNotEmpty)
              TextButton.icon(
                onPressed: _naming ? null : _nameAll,
                icon: _naming
                    ? const SizedBox(width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.auto_awesome_rounded, size: 16),
                label: const Text('Name with AI'),
              ),
          ],
        ),
        const SizedBox(height: 10),
        if (!_byTag) ...[
          if (cols.isEmpty)
            const Padding(
              padding: EdgeInsets.only(top: 40),
              child: EmptyState(
                icon: Icons.collections_bookmark_outlined,
                title: 'No themes yet',
                subtitle: 'Memos about the same things are grouped here once there are a few transcribed ones.',
              ),
            ),
          for (var i = 0; i < cols.length; i++)
            _CollectionCard(
              name: names.nameOf(cols[i]),
              collection: cols[i],
              color: chartPalette[i % chartPalette.length],
            ),
          if (cols.isNotEmpty && kb.unclustered.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: NavRow(
                icon: Icons.inbox_outlined,
                color: AppColors.textDim,
                title: 'Other memos',
                subtitle: '${kb.unclustered.length} not in a theme',
                onTap: () => Navigator.push(
                    context, MaterialPageRoute(builder: (_) => MemoListScreen(title: 'Other memos', items: kb.unclustered))),
              ),
            ),
        ] else ...[
          if (byTag.isEmpty)
            const Padding(
              padding: EdgeInsets.only(top: 40),
              child: EmptyState(icon: Icons.tag_rounded, title: 'No tags yet', subtitle: 'Summaries add tags to memos.'),
            )
          else
            RowGroup(children: [
              for (final t in byTag.entries)
                NavRow(
                  icon: Icons.tag_rounded,
                  color: AppColors.accent2,
                  title: t.key,
                  subtitle: '${t.value.length} memo${t.value.length == 1 ? '' : 's'}',
                  onTap: () =>
                      Navigator.push(context, MaterialPageRoute(builder: (_) => MemoListScreen(title: '#${t.key}', items: t.value))),
                ),
            ]),
        ],
      ],
    );
  }
}

class _CollectionCard extends StatelessWidget {
  final String name;
  final MemoCollection collection;
  final Color color;
  const _CollectionCard({required this.name, required this.collection, required this.color});

  @override
  Widget build(BuildContext context) {
    final c = collection;
    final last = c.items.first.recordedAt;
    return Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Panel(
        onTap: () => Navigator.push(context, MaterialPageRoute(builder: (_) => CollectionScreen(collection: c, color: color))),
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [color.withValues(alpha: 0.18), AppColors.surface],
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                IconChip(Icons.folder_special_rounded, color: color),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(name, maxLines: 1, overflow: TextOverflow.ellipsis,
                          style: const TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w700)),
                      Text('${c.items.length} memos${last != null ? ' · last ${fmtDate(last)}' : ''}',
                          style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Wrap(
              spacing: 6,
              runSpacing: 6,
              children: [
                for (final k in c.keywords)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 4),
                    decoration: BoxDecoration(color: AppColors.surfaceHigh, borderRadius: BorderRadius.circular(8)),
                    child: Text(k, style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                  ),
              ],
            ),
            const SizedBox(height: 10),
            for (final e in c.items.take(3))
              Padding(
                padding: const EdgeInsets.only(top: 3),
                child: Text('•  ${e.displayTitle}', maxLines: 1, overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: AppColors.text, fontSize: 13)),
              ),
          ],
        ),
      ),
    );
  }
}

class CollectionScreen extends ConsumerWidget {
  final MemoCollection collection;
  final Color color;
  const CollectionScreen({super.key, required this.collection, required this.color});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    ref.watch(collectionNamesProvider);
    final name = ref.read(collectionNamesProvider.notifier).nameOf(collection);
    final c = collection;
    final kb = KnowledgeBase.build(c.items);   // stats of just this theme
    final actions = kb.actions();
    final people = kb.people(n: 6);
    final secs = kb.totalSeconds;
    return Scaffold(
      appBar: AppBar(
        title: Text(name),
        actions: [
          IconButton(
            tooltip: 'Rename',
            icon: const Icon(Icons.edit_outlined),
            onPressed: () => _rename(context, ref, name),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          Row(children: [
            Expanded(child: StatTile(icon: Icons.mic_rounded, color: color, value: '${c.items.length}', label: 'memos')),
            const SizedBox(width: 10),
            Expanded(child: StatTile(icon: Icons.schedule_rounded, color: color, value: fmtDuration(secs).isEmpty ? '–' : fmtDuration(secs), label: 'audio')),
            const SizedBox(width: 10),
            Expanded(child: StatTile(icon: Icons.hub_rounded, color: color, value: '${(c.cohesion * 100).round()}%', label: 'similarity')),
          ]),
          const SectionLabel('Keywords'),
          Wrap(spacing: 8, runSpacing: 8, children: [
            for (final k in c.keywords)
              ActionChip(
                label: Text(k),
                onPressed: () =>
                    Navigator.push(context, MaterialPageRoute(builder: (_) => MemoSearchScreen(initialQuery: k))),
              ),
          ]),
          if (actions.isNotEmpty) ...[
            const SectionLabel('To-dos'),
            Panel(padding: const EdgeInsets.symmetric(vertical: 6), child: Column(children: [for (final a in actions) ActionRow(a)])),
          ],
          if (people.isNotEmpty) ...[
            const SectionLabel('People'),
            Wrap(spacing: 8, runSpacing: 8, children: [for (final p in people) Chip(label: Text('${p.$1}  ${p.$2}'))]),
          ],
          const SectionLabel('Memos'),
          for (final e in c.items)
            MemoTile(e,
                below: e.summary.isEmpty
                    ? null
                    : Text(e.summary, maxLines: 2, overflow: TextOverflow.ellipsis,
                        style: const TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.35))),
        ],
      ),
    );
  }

  void _rename(BuildContext context, WidgetRef ref, String current) {
    final ctl = TextEditingController(text: current);
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Collection name'),
        content: TextField(controller: ctl, autofocus: true, decoration: const InputDecoration(hintText: 'Name')),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(
            onPressed: () {
              ref.read(collectionNamesProvider.notifier).rename(collection, ctl.text);
              Navigator.pop(ctx);
            },
            child: const Text('Save'),
          ),
        ],
      ),
    );
  }
}

// ============================================================================================
// Search + Ask
// ============================================================================================

class MemoSearchScreen extends ConsumerStatefulWidget {
  final String initialQuery;
  final String? initialTag;
  const MemoSearchScreen({super.key, this.initialQuery = '', this.initialTag});

  @override
  ConsumerState<MemoSearchScreen> createState() => _MemoSearchScreenState();
}

class _MemoSearchScreenState extends ConsumerState<MemoSearchScreen> {
  late final TextEditingController _q = TextEditingController(text: widget.initialQuery);
  late final Set<String> _tags = {if (widget.initialTag != null) widget.initialTag!};
  bool _importantOnly = false;
  bool _asking = false;
  String? _answer;
  List<RecordingEntry> _sources = const [];

  @override
  void dispose() {
    _q.dispose();
    super.dispose();
  }

  Future<void> _ask() async {
    final question = _q.text.trim();
    if (question.length < 3) return;
    FocusScope.of(context).unfocus();
    setState(() {
      _asking = true;
      _answer = null;
    });
    final ctx = ref.read(knowledgeProvider).contextFor(question);
    String? answer;
    try {
      answer = ctx.isEmpty ? null : await GemmaService.instance.ask(question, ctx.map((c) => c.$2).toList());
    } catch (e) {
      answer = 'Couldn\'t answer: $e';
    }
    if (!mounted) return;
    setState(() {
      _asking = false;
      _sources = ctx.map((c) => c.$1).toList();
      _answer = ctx.isEmpty
          ? 'No memo talks about that.'
          : answer ?? 'Install a summary model in Transcription & AI to get answers. The best matching memos are below.';
    });
  }

  @override
  Widget build(BuildContext context) {
    final kb = ref.watch(knowledgeProvider);
    final query = _q.text.trim();
    final tagCounts = kb.tagCounts();
    final hits = query.isEmpty && _tags.isEmpty
        ? const <SearchHit>[]
        : query.isEmpty
            // tag filter only: list everything in the tag
            ? [
                for (final e in kb.entries)
                  if (e.tags.any(_tags.contains) || e.topics.any(_tags.contains))
                    if (!_importantOnly || e.importance >= 4) SearchHit(e, 1, e.summary, const [], 0, 0)
              ]
            : kb.search(query, tags: _tags, minImportance: _importantOnly ? 4 : 1);

    return Scaffold(
      appBar: AppBar(
        titleSpacing: 0,
        title: TextField(
          controller: _q,
          autofocus: widget.initialQuery.isEmpty && widget.initialTag == null,
          textInputAction: TextInputAction.search,
          onChanged: (_) => setState(() => _answer = null),
          onSubmitted: (_) => setState(() {}),
          decoration: InputDecoration(
            hintText: 'Search or ask your memos',
            filled: false,
            border: InputBorder.none,
            enabledBorder: InputBorder.none,
            focusedBorder: InputBorder.none,
            suffixIcon: query.isEmpty
                ? null
                : IconButton(
                    icon: const Icon(Icons.close_rounded),
                    onPressed: () => setState(() {
                      _q.clear();
                      _answer = null;
                    })),
          ),
        ),
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 4, 16, 32),
        children: [
          SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: Row(children: [
              FilterChip(
                label: const Text('Important'),
                selected: _importantOnly,
                onSelected: (v) => setState(() => _importantOnly = v),
              ),
              for (final t in {..._tags, ...tagCounts.keys.take(12)}) ...[
                const SizedBox(width: 6),
                FilterChip(
                  label: Text('#$t'),
                  selected: _tags.contains(t),
                  onSelected: (v) => setState(() => v ? _tags.add(t) : _tags.remove(t)),
                ),
              ],
            ]),
          ),
          const SizedBox(height: 10),
          if (query.length >= 3)
            Panel(
              padding: const EdgeInsets.all(14),
              color: AppColors.accent.withValues(alpha: 0.10),
              onTap: _asking ? null : (_answer == null ? _ask : null),
              child: _answer == null
                  ? Row(children: [
                      _asking
                          ? const SizedBox(width: 22, height: 22, child: CircularProgressIndicator(strokeWidth: 2))
                          : const Icon(Icons.auto_awesome_rounded, color: AppColors.accent),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Text(_asking ? 'Reading your memos...' : 'Ask AI: "$query"',
                            maxLines: 2,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(color: AppColors.text, fontWeight: FontWeight.w600)),
                      ),
                    ])
                  : Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Row(children: [
                          Icon(Icons.auto_awesome_rounded, color: AppColors.accent, size: 18),
                          SizedBox(width: 8),
                          Text('Answer', style: TextStyle(color: AppColors.accent, fontWeight: FontWeight.w700)),
                        ]),
                        const SizedBox(height: 8),
                        SelectableText(_answer!, style: const TextStyle(color: AppColors.text, fontSize: 14, height: 1.45)),
                        if (_sources.isNotEmpty) ...[
                          const SizedBox(height: 10),
                          Wrap(spacing: 6, runSpacing: 6, children: [
                            for (var i = 0; i < _sources.length; i++)
                              ActionChip(
                                visualDensity: VisualDensity.compact,
                                label: Text('[${i + 1}] ${_sources[i].displayTitle}', overflow: TextOverflow.ellipsis),
                                onPressed: () => openMemo(context, _sources[i]),
                              ),
                          ]),
                        ],
                      ],
                    ),
            ),
          if (query.isNotEmpty || _tags.isNotEmpty) ...[
            SectionLabel(hits.isEmpty ? 'No matches' : '${hits.length} result${hits.length == 1 ? '' : 's'}'),
            for (final h in hits)
              MemoTile(
                h.entry,
                trailing: query.isEmpty ? null : ScoreBar(h.score),
                below: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    if (h.snippet.isNotEmpty) HighlightText(h.snippet, h.hits, maxLines: 3),
                    if (h.queryTerms > 1) ...[
                      const SizedBox(height: 6),
                      Text('${h.matchedTerms} of ${h.queryTerms} words',
                          style: const TextStyle(color: AppColors.textFaint, fontSize: 11)),
                    ],
                  ],
                ),
              ),
          ] else ...[
            const SectionLabel('Try'),
            Wrap(spacing: 8, runSpacing: 8, children: [
              for (final t in kb.topTopics(n: 8))
                ActionChip(label: Text(t.label), onPressed: () => setState(() => _q.text = t.label)),
            ]),
            const SizedBox(height: 16),
            const Text(
              'Results are ranked by relevance (word weight, similarity and how many of your words match). '
              'Type a question and tap Ask AI to get an answer from your memos.',
              style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
            ),
          ],
        ],
      ),
    );
  }
}
