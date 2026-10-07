import 'dart:math' as math;
import 'ble_protocol.dart';
import 'text_insights.dart';

/// One search result: the memo, a 0..1 relevance, and a snippet with the
/// matched words to highlight.
class SearchHit {
  final RecordingEntry entry;
  final double score;
  final String snippet;
  final List<(int, int)> hits;
  final int matchedTerms, queryTerms;
  const SearchHit(this.entry, this.score, this.snippet, this.hits, this.matchedTerms, this.queryTerms);
}

/// A group of memos about the same thing.
class MemoCollection {
  final String key;              // stable-ish id: its keywords
  final List<String> keywords;   // shown / used for the automatic name
  final List<RecordingEntry> items;
  final double cohesion;         // average similarity inside, 0..1
  const MemoCollection(this.key, this.keywords, this.items, this.cohesion);
  String get autoName => keywords.take(3).map(_cap).join(' · ');
  static String _cap(String s) => s.isEmpty ? s : s[0].toUpperCase() + s.substring(1);
}

class TopicStat {
  final String label;
  final int memos;            // how many memos are about it
  final double weight;        // relative importance in the corpus
  final Set<int> memoIdx;
  const TopicStat(this.label, this.memos, this.weight, this.memoIdx);
}

class ActionItem {
  final RecordingEntry entry;
  final String text;
  String get key => '${entry.name}|$text';
  const ActionItem(this.entry, this.text);
}

/// Everything derived from the memos: TF-IDF vectors, BM25 search,
/// similar memos, theme collections, topics and statistics. Rebuilt when the
/// memo list changes; pure Dart, works offline and without a model.
class KnowledgeBase {
  final List<RecordingEntry> entries;
  final List<Map<String, double>> _tf = [];     // weighted term counts per memo
  final List<Map<String, double>> _vec = [];    // L2-normalized TF-IDF per memo
  final List<double> _len = [];                 // weighted length per memo
  final Map<String, int> _df = {};
  final Map<String, double> _idf = {};
  late final Map<String, String> _forms;
  double _avgLen = 1;
  List<MemoCollection>? _collections;

  KnowledgeBase._(this.entries);

  factory KnowledgeBase.build(List<RecordingEntry> entries) {
    final kb = KnowledgeBase._(entries);
    kb._index();
    return kb;
  }

  bool get isEmpty => entries.isEmpty;
  int get processedCount => entries.where((e) => e.transcript.isNotEmpty).length;

  // ---- indexing -------------------------------------------------------------------

  static String _docText(RecordingEntry e) => [e.title, e.summary, e.transcript, ...e.tags, ...e.topics].join(' ');

  void _index() {
    for (final e in entries) {
      final tf = <String, double>{};
      void add(String text, double w) {
        for (final t in TextInsights.terms(text)) {
          tf[t] = (tf[t] ?? 0) + w;
        }
      }
      add(e.title, 3);
      add(e.summary, 2);
      add(e.transcript, 1);
      add(e.tags.join(' '), 2.5);
      add(e.topics.join(' '), 3);
      add(e.people.join(' '), 1.5);
      if (tf.isEmpty) add(e.displayTitle, 1);
      _tf.add(tf);
      _len.add(tf.values.fold(0.0, (a, b) => a + b));
      for (final t in tf.keys) {
        _df[t] = (_df[t] ?? 0) + 1;
      }
    }
    final n = entries.length;
    _avgLen = n == 0 ? 1.0 : math.max(1.0, _len.fold<double>(0.0, (a, b) => a + b) / n);
    _df.forEach((t, df) => _idf[t] = math.log(1 + (n - df + 0.5) / (df + 0.5)));
    for (final tf in _tf) {
      final v = <String, double>{};
      var norm = 0.0;
      tf.forEach((t, c) {
        final w = (1 + math.log(c)) * (_idf[t] ?? 0);
        if (w > 0) {
          v[t] = w;
          norm += w * w;
        }
      });
      norm = math.sqrt(norm);
      if (norm > 0) v.updateAll((_, w) => w / norm);
      _vec.add(v);
    }
    _forms = TextInsights.surfaceForms(entries.map(_docText));
  }

  String surface(String stem) => _forms[stem] ?? stem;

  static double _cos(Map<String, double> a, Map<String, double> b) {
    if (a.length > b.length) return _cos(b, a);
    var s = 0.0;
    a.forEach((t, w) {
      final o = b[t];
      if (o != null) s += w * o;
    });
    return s;
  }

  // ---- search ---------------------------------------------------------------------------

  /// BM25 + cosine + coverage, blended into one 0..1 relevance. Query words
  /// also match longer words they start with ("proj" -> project).
  List<SearchHit> search(String query, {Set<String> tags = const {}, int minImportance = 1, int limit = 50}) {
    final qTerms = TextInsights.terms(query).toSet();
    final rawQ = query.trim().toLowerCase();
    if (qTerms.isEmpty && rawQ.length < 2) return [];
    // expand each query term with vocabulary terms it prefixes
    final expanded = <String, Map<String, double>>{}; // query term -> {index term: weight}
    for (final q in qTerms) {
      final m = <String, double>{};
      if (_df.containsKey(q)) m[q] = 1.0;
      if (q.length >= 3) {
        for (final t in _df.keys) {
          if (t != q && t.startsWith(q)) m[t] = 0.7;
        }
      }
      expanded[q] = m;
    }
    // query vector for cosine
    final qv = <String, double>{};
    expanded.forEach((_, m) => m.forEach((t, w) => qv[t] = math.max(qv[t] ?? 0.0, w * (_idf[t] ?? 0.0))));
    final qn = math.sqrt(qv.values.fold<double>(0.0, (a, b) => a + b * b));
    if (qn > 0) qv.updateAll((_, w) => w / qn);

    const k1 = 1.2, b = 0.75;
    final raw = <int, (double, double, int)>{}; // idx -> (bm25, cos, matched)
    for (var i = 0; i < entries.length; i++) {
      final e = entries[i];
      if (e.importance < minImportance) continue;
      if (tags.isNotEmpty && !e.tags.any(tags.contains) && !e.topics.any(tags.contains)) continue;
      final tf = _tf[i];
      var bm = 0.0;
      var matched = 0;
      expanded.forEach((q, m) {
        var best = 0.0;
        m.forEach((t, w) {
          final f = tf[t];
          if (f == null) return;
          final idf = _idf[t] ?? 0;
          final s = w * idf * (f * (k1 + 1)) / (f + k1 * (1 - b + b * _len[i] / _avgLen));
          if (s > best) best = s;
        });
        if (best > 0) matched++;
        bm += best;
      });
      // exact phrase anywhere: strong signal
      final phrase = rawQ.length >= 3 && _docText(e).toLowerCase().contains(rawQ);
      if (phrase) bm *= 1.3;
      if (bm <= 0 && !phrase) continue;
      raw[i] = (bm, _cos(qv, _vec[i]), matched);
    }
    if (raw.isEmpty) return [];
    final maxBm = raw.values.map((r) => r.$1).fold<double>(0.0, (a, b) => math.max(a, b));
    final hits = <SearchHit>[];
    final qStems = qTerms;
    raw.forEach((i, r) {
      final coverage = qTerms.isEmpty ? 1.0 : r.$3 / qTerms.length;
      // Absolute, not relative to the best hit: BM25 per query word,
      // saturated (2.0 per word is a solid match), plus similarity and how
      // many of the words matched. A weak best hit now scores low.
      final perWord = r.$1 / math.max(1, qTerms.length);
      final bmSat = perWord / (perWord + 2.0);
      final rel = maxBm > 0 ? r.$1 / maxBm : 0.0;   // small tie-breaker among results
      final score = (0.45 * bmSat + 0.3 * r.$2.clamp(0, 1) + 0.2 * coverage + 0.05 * rel).clamp(0.0, 1.0);
      final e = entries[i];
      final source = e.transcript.isNotEmpty ? e.transcript : e.summary;
      final sn = TextInsights.snippet(source, qStems);
      hits.add(SearchHit(e, score, sn.text, sn.hits, r.$3, qTerms.length));
    });
    hits.sort((a, b) => b.score.compareTo(a.score));
    return hits.take(limit).toList();
  }

  /// The memos most similar to [e] (cosine of TF-IDF vectors).
  List<(RecordingEntry, double)> related(RecordingEntry e, {int n = 5, double min = 0.08}) {
    final i = entries.indexWhere((x) => x.name == e.name);
    if (i < 0) return [];
    final out = <(RecordingEntry, double)>[];
    for (var j = 0; j < entries.length; j++) {
      if (j == i) continue;
      final s = _cos(_vec[i], _vec[j]);
      if (s >= min) out.add((entries[j], s));
    }
    out.sort((a, b) => b.$2.compareTo(a.$2));
    return out.take(n).toList();
  }

  // ---- collections ------------------------------------------------------------------------

  /// Theme collections: average-link clustering of the memo vectors. Memos
  /// that fit nowhere are left out (see [unclustered]).
  List<MemoCollection> get collections => _collections ??= _cluster();

  List<RecordingEntry> get unclustered {
    final inC = collections.expand((c) => c.items.map((e) => e.name)).toSet();
    return entries.where((e) => !inC.contains(e.name)).toList();
  }

  List<MemoCollection> _cluster({double threshold = 0.14, int maxMemos = 300}) {
    // the most recent memos with any content (clustering is O(n^3))
    var idx = [for (var i = 0; i < entries.length; i++) if (_vec[i].isNotEmpty) i];
    if (idx.length > maxMemos) {
      idx.sort((x, y) => (entries[y].recordedAt ?? DateTime(2000)).compareTo(entries[x].recordedAt ?? DateTime(2000)));
      idx = idx.sublist(0, maxMemos);
    }
    if (idx.length < 2) return [];
    // pairwise similarity of memos (kept for cohesion) and of clusters
    final n = idx.length;
    final sim = List.generate(n, (_) => List<double>.filled(n, 0));
    for (var a = 0; a < n; a++) {
      for (var b = a + 1; b < n; b++) {
        final s = _cos(_vec[idx[a]], _vec[idx[b]]);
        sim[a][b] = s;
        sim[b][a] = s;
      }
    }
    final cs = [for (final row in sim) List<double>.from(row)];
    final members = [for (var a = 0; a < n; a++) <int>[a]];
    final alive = List<bool>.filled(n, true);
    // Average-link agglomeration (Lance-Williams update).
    while (true) {
      var best = -1.0, bi = -1, bj = -1;
      for (var i = 0; i < n; i++) {
        if (!alive[i]) continue;
        final row = cs[i];
        for (var j = i + 1; j < n; j++) {
          if (alive[j] && row[j] > best) {
            best = row[j];
            bi = i;
            bj = j;
          }
        }
      }
      if (bi < 0 || best < threshold) break;
      final ni = members[bi].length, nj = members[bj].length;
      for (var k = 0; k < n; k++) {
        if (!alive[k] || k == bi || k == bj) continue;
        final v = (ni * cs[bi][k] + nj * cs[bj][k]) / (ni + nj);
        cs[bi][k] = v;
        cs[k][bi] = v;
      }
      members[bi].addAll(members[bj]);
      alive[bj] = false;
    }
    final clusters = [for (var i = 0; i < n; i++) if (alive[i]) members[i]];

    final out = <MemoCollection>[];
    for (final c in clusters.where((c) => c.length >= 2)) {
      // keywords: strongest terms of the centroid
      final centroid = <String, double>{};
      for (final a in c) {
        _vec[idx[a]].forEach((t, w) => centroid[t] = (centroid[t] ?? 0) + w);
      }
      final kws = centroid.keys.toList()..sort((x, y) => centroid[y]!.compareTo(centroid[x]!));
      final keywords = kws.take(5).map(surface).toList();
      var coh = 0.0, pairs = 0;
      for (var i = 0; i < c.length; i++) {
        for (var j = i + 1; j < c.length; j++) {
          coh += sim[c[i]][c[j]];
          pairs++;
        }
      }
      final items = c.map((a) => entries[idx[a]]).toList()
        ..sort((x, y) => (y.recordedAt ?? DateTime(2000)).compareTo(x.recordedAt ?? DateTime(2000)));
      out.add(MemoCollection(kws.take(3).join('+'), keywords, items, pairs > 0 ? coh / pairs : 0));
    }
    out.sort((a, b) => b.items.length.compareTo(a.items.length));
    return out;
  }

  /// Memos grouped by tag.
  Map<String, List<RecordingEntry>> get byTag {
    final m = <String, List<RecordingEntry>>{};
    for (final e in entries) {
      for (final t in e.tags) {
        m.putIfAbsent(t, () => []).add(e);
      }
    }
    return Map.fromEntries(m.entries.toList()..sort((a, b) => b.value.length.compareTo(a.value.length)));
  }

  // ---- topics -------------------------------------------------------------------------------

  /// What the memos are about: the model's topic phrases where there are
  /// some, plus each memo's strongest terms.
  List<TopicStat> topTopics({int n = 10}) {
    final memos = <String, Set<int>>{};
    final weight = <String, double>{};
    for (var i = 0; i < entries.length; i++) {
      final e = entries[i];
      final labels = <String>{};
      for (final t in e.topics) {
        final l = t.trim().toLowerCase();
        if (l.isNotEmpty) labels.add(l);
      }
      // strongest terms of this memo
      final v = _vec[i].entries.toList()..sort((a, b) => b.value.compareTo(a.value));
      for (final t in v.take(labels.isEmpty ? 3 : 1)) {
        labels.add(surface(t.key));
      }
      for (final l in labels) {
        memos.putIfAbsent(l, () => {}).add(i);
        weight[l] = (weight[l] ?? 0) + 1 + (e.importance - 3) * 0.1;
      }
    }
    final list = memos.keys.toList()
      ..sort((a, b) => (weight[b]!).compareTo(weight[a]!));
    final top = list.where((l) => memos[l]!.length >= (entries.length >= 6 ? 2 : 1)).take(n);
    final maxW = top.isEmpty ? 1.0 : weight[top.first]!;
    return [for (final l in top) TopicStat(l, memos[l]!.length, weight[l]! / maxW, memos[l]!)];
  }

  /// Memos per week for each topic, oldest week first ([weeks] weeks back).
  Map<String, List<int>> topicTrend(List<TopicStat> topics, {int weeks = 8}) {
    final start = _weeksBack(weeks);
    final out = <String, List<int>>{};
    for (final t in topics) {
      final series = List<int>.filled(weeks, 0);
      for (final i in t.memoIdx) {
        final d = entries[i].recordedAt;
        if (d == null) continue;
        final w = _weekIdx(d, start);
        if (w >= 0 && w < weeks) series[w]++;
      }
      out[t.label] = series;
    }
    return out;
  }

  // ---- statistics ----------------------------------------------------------------------------

  // Calendar arithmetic (not Duration) so DST changes don't shift weeks.
  static DateTime _weekStart(DateTime d) => DateTime(d.year, d.month, d.day - (d.weekday - 1));

  static DateTime _weeksBack(int weeks) {
    final s = _weekStart(DateTime.now());
    return DateTime(s.year, s.month, s.day - 7 * (weeks - 1));
  }

  /// Whole weeks from [start] (a week start) to [d]'s week; a 23/25 h DST day rounds away.
  static int _weekIdx(DateTime d, DateTime start) => (_weekStart(d).difference(start).inHours / 168).round();

  /// Seconds of audio: the measured length, else estimated from the size
  /// (16 kHz stereo WAV = 64 KB/s).
  /// (MP3 not measured yet: assume 128 kbit/s.)
  static int durationOf(RecordingEntry e) {
    if (e.durationSec > 0) return e.durationSec;
    if (e.name.toLowerCase().endsWith('.mp3')) return (e.size / 16000).round();
    return e.size > 44 ? ((e.size - 44) / 64000).round() : 0;
  }

  int get totalSeconds => entries.fold(0, (a, e) => a + durationOf(e));
  int get totalWords => entries.fold(0, (a, e) => a + TextInsights.words(e.transcript).length);

  /// Memos per week, oldest first.
  List<int> perWeek({int weeks = 12}) {
    final start = _weeksBack(weeks);
    final out = List<int>.filled(weeks, 0);
    for (final e in entries) {
      final d = e.recordedAt;
      if (d == null) continue;
      final w = _weekIdx(d, start);
      if (w >= 0 && w < weeks) out[w]++;
    }
    return out;
  }

  /// Start dates of the weeks [perWeek] covers.
  List<DateTime> weekStarts({int weeks = 12}) {
    final start = _weeksBack(weeks);
    return [for (var i = 0; i < weeks; i++) DateTime(start.year, start.month, start.day + 7 * i)];
  }

  /// [weekday 0=Mon..6][3-hour slot 0..7] counts.
  List<List<int>> heatmap() {
    final h = List.generate(7, (_) => List<int>.filled(8, 0));
    for (final e in entries) {
      final d = e.recordedAt;
      if (d == null) continue;
      h[d.weekday - 1][d.hour ~/ 3]++;
    }
    return h;
  }

  List<int> importanceHistogram() {
    final h = List<int>.filled(5, 0);
    for (final e in entries.where((e) => e.summary.isNotEmpty)) {
      h[(e.importance - 1).clamp(0, 4)]++;
    }
    return h;
  }

  /// Memos by length: <30 s, 30 s-1 min, 1-3 min, 3-10 min, >10 min.
  List<int> lengthHistogram() {
    final h = List<int>.filled(5, 0);
    for (final e in entries) {
      final s = durationOf(e);
      final b = s < 30 ? 0 : s < 60 ? 1 : s < 180 ? 2 : s < 600 ? 3 : 4;
      h[b]++;
    }
    return h;
  }

  Map<String, int> tagCounts() {
    final m = <String, int>{};
    for (final e in entries) {
      for (final t in e.tags) {
        m[t] = (m[t] ?? 0) + 1;
      }
    }
    return Map.fromEntries(m.entries.toList()..sort((a, b) => b.value.compareTo(a.value)));
  }

  /// People mentioned across memos, most frequent first.
  List<(String, int)> people({int n = 12}) {
    final m = <String, int>{};
    final display = <String, String>{};
    for (final e in entries) {
      for (final p in e.people.toSet()) {
        final k = p.trim().toLowerCase();
        if (k.isEmpty) continue;
        m[k] = (m[k] ?? 0) + 1;
        display[k] ??= p.trim();
      }
    }
    final list = m.keys.toList()..sort((a, b) => m[b]!.compareTo(m[a]!));
    return [for (final k in list.take(n)) (display[k]!, m[k]!)];
  }

  /// All action items, newest memo first.
  List<ActionItem> actions() {
    final sorted = [...entries]
      ..sort((a, b) => (b.recordedAt ?? DateTime(2000)).compareTo(a.recordedAt ?? DateTime(2000)));
    return [for (final e in sorted) for (final a in e.actions) ActionItem(e, a)];
  }

  /// Excerpts for answering a question: the best matching memos, as text.
  List<(RecordingEntry, String)> contextFor(String question, {int n = 5}) {
    final hits = search(question, limit: n);
    return [
      for (final h in hits)
        (
          h.entry,
          '${h.entry.displayTitle}'
              '${h.entry.recordedAt != null ? ' (${h.entry.recordedAt!.day}/${h.entry.recordedAt!.month})' : ''}: '
              '${h.entry.summary} ${h.entry.transcript.length > 700 ? '${h.entry.transcript.substring(0, 700)}…' : h.entry.transcript}'
        )
    ];
  }
}
