/// Language helpers for voice memos, all local (no model needed): tokenizing,
/// stopwords (English + Italian, some Spanish/French/German), keywords,
/// action items and names. Used for search, topics and collections, and as
/// the fallback when no summary model is installed.
class TextInsights {
  static final _word = RegExp(r"[\p{L}\p{N}][\p{L}\p{N}'’-]*", unicode: true);

  static const Set<String> stopwords = {
    // English
    'the', 'and', 'for', 'that', 'this', 'with', 'you', 'are', 'was', 'were', 'have', 'has', 'had', 'but', 'not',
    'all', 'can', 'will', 'just', 'about', 'what', 'when', 'where', 'which', 'who', 'how', 'why', 'there', 'their',
    'they', 'them', 'then', 'than', 'from', 'into', 'out', 'our', 'your', 'yours', 'his', 'her', 'she', 'him',
    'its', 'it’s', "it's", 'i’m', "i'm", 'also', 'some', 'any', 'more', 'most', 'very', 'really', 'like',
    'get', 'got', 'going', 'gonna', 'want', 'need', 'think', 'know', 'yeah', 'okay', 'well', 'one', 'two', 'thing',
    'things', 'something', 'anything', 'would', 'could', 'should', 'been', 'being', 'did', 'does', 'doing', 'done',
    'make', 'made', 'let', 'say', 'said', 'see', 'now', 'today', 'tomorrow', 'yesterday', 'here', 'these', 'those',
    'other', 'over', 'only', 'same', 'such', 'too', 'much', 'many', 'maybe', 'because', 'while', 'after', 'before',
    'again', 'still', 'even', 'back', 'down', 'off', 'own', 'way', 'time', 'lot', 'kind', 'sort', 'mean', 'right',
    'uh', 'um', 'hmm', 'blank_audio', 'music',
    // Italian
    'che', 'non', 'per', 'una', 'uno', 'con', 'del', 'della', 'dei', 'delle', 'degli', 'nel', 'nella', 'nei',
    'sono', 'sei', 'era', 'ero', 'come', 'anche', 'questo', 'questa', 'questi', 'queste', 'quello', 'quella',
    'perché', 'perche', 'quando', 'dove', 'cosa', 'più', 'piu', 'molto', 'tutto', 'tutti', 'tutte', 'alla', 'alle',
    'allo', 'agli', 'dal', 'dalla', 'dai', 'dalle', 'sul', 'sulla', 'sui', 'sulle', 'mio', 'mia', 'miei', 'mie',
    'tuo', 'tua', 'suo', 'sua', 'loro', 'noi', 'voi', 'lui', 'lei', 'gli', 'ho', 'hai', 'abbiamo', 'avete', 'hanno',
    'essere', 'fare', 'fatto', 'poi', 'già', 'gia', 'ancora', 'solo', 'però', 'pero', 'quindi', 'allora', 'ecco',
    'cioè', 'cioe', 'tipo', 'dire', 'detto', 'oggi', 'domani', 'ieri', 'adesso', 'ora', 'qui', 'qua', 'mi', 'ti',
    'ci', 'vi', 'si', 'ne', 'il', 'lo', 'la', 'le', 'un', 'di', 'da', 'in', 'su', 'tra', 'fra', 'ma', 'se', 'ed',
    'devo', 'deve', 'dobbiamo', 'bisogna', 'cose', 'altro', 'altra', 'sempre', 'mai', 'bene', 'vabbè',
    'insomma', 'praticamente', 'comunque', 'sto', 'sta', 'stai', 'stiamo', 'stanno', 'fa', 'fai', 'va', 'vado',
    // Spanish / French / German (common)
    'que', 'los', 'las', 'por', 'para', 'como', 'muy', 'esto', 'esta', 'les', 'des', 'est', 'pas',
    'pour', 'dans', 'avec', 'une', 'sur', 'der', 'die', 'das', 'und', 'ist', 'nicht', 'mit', 'ich', 'ein',
    'eine', 'auf', 'den', 'dem', 'zu',
  };

  // Very common function words per language: enough to tell the language of
  // a transcript (whisper_ggml doesn't report the language it detected).
  static const Map<String, Set<String>> _langWords = {
    'en': {'the', 'and', 'to', 'of', 'is', 'that', 'it', 'in', 'you', 'this', 'for', 'have', 'with', 'what', 'be',
      'are', 'was', 'not', 'but', 'we', 'my', 'so', 'just', 'need', 'do', 'on', 'i', 'a', 'about', 'will'},
    'it': {'il', 'di', 'che', 'e', 'la', 'per', 'non', 'un', 'una', 'sono', 'è', 'del', 'della', 'con', 'mi', 'ho',
      'anche', 'come', 'questo', 'devo', 'poi', 'ma', 'gli', 'le', 'lo', 'nel', 'alla', 'perché', 'più', 'cosa'},
    'es': {'el', 'de', 'que', 'y', 'la', 'los', 'en', 'es', 'por', 'para', 'con', 'una', 'las', 'del', 'pero',
      'como', 'muy', 'tengo', 'esto', 'yo', 'está', 'hay', 'también', 'qué', 'se', 'al', 'lo', 'mi', 'porque'},
    'fr': {'le', 'de', 'et', 'la', 'les', 'des', 'est', 'que', 'un', 'une', 'pour', 'pas', 'je', 'dans', 'il', 'qui',
      'sur', 'avec', 'ce', 'au', 'du', 'mais', 'on', 'nous', 'vous', 'faire', 'cette', 'aussi', 'très', 'ça'},
    'de': {'der', 'die', 'und', 'das', 'ist', 'nicht', 'ich', 'zu', 'den', 'mit', 'ein', 'eine', 'es', 'auf', 'für',
      'auch', 'sich', 'dass', 'wir', 'aber', 'noch', 'dem', 'wie', 'muss', 'habe', 'sie', 'von', 'oder', 'mal'},
    'pt': {'o', 'de', 'que', 'e', 'a', 'do', 'da', 'em', 'um', 'para', 'é', 'com', 'não', 'uma', 'os', 'no', 'na',
      'mas', 'eu', 'você', 'isso', 'muito', 'também', 'tenho', 'preciso', 'porque', 'nós', 'mais', 'está'},
  };

  /// Language of [text] as an ISO code ('' if unsure) and a 0..1 confidence.
  static ({String code, double confidence}) detectLanguage(String text) {
    final ws = words(text);
    if (ws.length < 3) return (code: '', confidence: 0);
    final scores = <String, int>{};
    for (final w in ws) {
      _langWords.forEach((lang, set) {
        if (set.contains(w)) scores[lang] = (scores[lang] ?? 0) + 1;
      });
    }
    if (scores.isEmpty) return (code: '', confidence: 0);
    final ranked = scores.entries.toList()..sort((a, b) => b.value.compareTo(a.value));
    final best = ranked.first;
    final second = ranked.length > 1 ? ranked[1].value : 0;
    final total = scores.values.fold<int>(0, (a, b) => a + b);
    final conf = (best.value - second) / total;
    return (code: best.value >= 2 && conf > 0.15 ? best.key : '', confidence: conf.clamp(0.0, 1.0).toDouble());
  }

  /// Lower-case word tokens (letters/digits, any script).
  static List<String> words(String text) =>
      _word.allMatches(text.toLowerCase()).map((m) => m.group(0)!.replaceAll('’', "'")).toList();

  /// Light stemming so "meetings"/"meeting", "progetti"/"progetto" meet.
  static String stem(String w) {
    if (w.length > 5) {
      for (final suf in const ['ations', 'ation', 'ments', 'ment', 'ings', 'ing', 'zioni', 'zione']) {
        if (w.endsWith(suf) && w.length - suf.length >= 4) return w.substring(0, w.length - suf.length);
      }
    }
    if (w.length > 4) {
      final last = w[w.length - 1];
      if (last == 's' && !w.endsWith('ss')) return w.substring(0, w.length - 1);
      // Italian plural/gender endings
      if ('aeio'.contains(last)) return w.substring(0, w.length - 1);
    }
    return w;
  }

  /// Index terms: stemmed, no stopwords, no short or numeric tokens.
  static List<String> terms(String text) {
    final out = <String>[];
    for (final w in words(text)) {
      if (w.length < 3 || stopwords.contains(w) || RegExp(r'^\d+$').hasMatch(w)) continue;
      out.add(stem(w));
    }
    return out;
  }

  /// The surface word shown for a stem: its most frequent original form.
  static Map<String, String> surfaceForms(Iterable<String> texts) {
    final counts = <String, Map<String, int>>{};
    for (final t in texts) {
      for (final w in words(t)) {
        if (w.length < 3 || stopwords.contains(w)) continue;
        final s = stem(w);
        final m = counts.putIfAbsent(s, () => {});
        m[w] = (m[w] ?? 0) + 1;
      }
    }
    return counts.map((s, m) {
      var best = s, n = -1;
      m.forEach((w, c) {
        if (c > n) {
          n = c;
          best = w;
        }
      });
      return MapEntry(s, best);
    });
  }

  static List<String> sentences(String text) => text
      .split(RegExp(r'(?<=[.!?…])\s+|\n+'))
      .map((s) => s.trim())
      .where((s) => s.length > 3)
      .toList();

  // Phrases that introduce something to do.
  static final _actionCue = RegExp(
    r"\b(i need to|i have to|i must|i should|remember to|don't forget|do not forget|need to|have to|must|todo|to do|"
    r"call|email|send|buy|book|schedule|pay|fix|finish|prepare|check|follow up|"
    r"devo|dobbiamo|bisogna|ricordati|ricordarmi|ricorda di|non dimenticare|da fare|chiamare|chiama|mandare|"
    r"inviare|comprare|prenotare|pagare|sistemare|finire|preparare|controllare|scrivere a)\b",
    caseSensitive: false,
  );

  /// Sentences that look like things to do (fallback when there's no model).
  static List<String> actionItems(String transcript, {int max = 5}) {
    final out = <String>[];
    for (final s in sentences(transcript)) {
      if (_actionCue.hasMatch(s)) {
        out.add(s.length > 140 ? '${s.substring(0, 137)}…' : s);
        if (out.length >= max) break;
      }
    }
    return out;
  }

  static final _cap = RegExp(r"\b([A-ZÀ-Ý][\p{Ll}'’]{2,})(?:\s+([A-ZÀ-Ý][\p{Ll}'’]{2,}))?", unicode: true);
  static const _notNames = {
    'The', 'This', 'That', 'And', 'But', 'Then', 'When', 'What', 'Okay', 'Yes', 'Monday', 'Tuesday', 'Wednesday',
    'Thursday', 'Friday', 'Saturday', 'Sunday', 'January', 'February', 'March', 'April', 'May', 'June', 'July',
    'August', 'September', 'October', 'November', 'December', 'Allora', 'Quindi', 'Poi', 'Questo', 'Questa',
    'Lunedì', 'Martedì', 'Mercoledì', 'Giovedì', 'Venerdì', 'Sabato', 'Domenica', 'Gennaio', 'Febbraio', 'Marzo',
    'Aprile', 'Maggio', 'Giugno', 'Luglio', 'Agosto', 'Settembre', 'Ottobre', 'Novembre', 'Dicembre', 'Ciao',
    'Comunque', 'Però', 'Perché', 'Anche', 'Devo', 'Sono', 'Ricordati',
  };

  /// Capitalized words that aren't at the start of a sentence: likely names
  /// (fallback when there's no model).
  static List<String> people(String transcript, {int max = 6}) {
    final counts = <String, int>{};
    for (final s in sentences(transcript)) {
      for (final m in _cap.allMatches(s)) {
        if (m.start == 0) continue; // sentence start: capitalized anyway
        final name = [m.group(1), m.group(2)].whereType<String>().join(' ');
        if (_notNames.contains(m.group(1))) continue;
        counts[name] = (counts[name] ?? 0) + 1;
      }
    }
    final list = counts.keys.toList()..sort((a, b) => counts[b]!.compareTo(counts[a]!));
    return list.take(max).toList();
  }

  /// The best ~[len]-character window of [text] for the query [terms], with
  /// the character ranges to highlight.
  static ({String text, List<(int, int)> hits}) snippet(String text, Set<String> queryStems, {int len = 180}) {
    if (text.isEmpty) return (text: '', hits: const []);
    final ms = _word.allMatches(text).toList();
    final hitPos = <(int, int)>[];
    for (final m in ms) {
      final w = m.group(0)!.toLowerCase();
      final st = stem(w);
      if (queryStems.contains(st) || queryStems.any((q) => q.length >= 3 && w.startsWith(q))) {
        hitPos.add((m.start, m.end));
      }
    }
    if (hitPos.isEmpty) {
      final t = text.length > len ? '${text.substring(0, len)}…' : text;
      return (text: t, hits: const []);
    }
    // window with the most hits
    var bestStart = 0, bestCount = -1;
    for (final h in hitPos) {
      final start = (h.$1 - 40).clamp(0, text.length);
      final count = hitPos.where((x) => x.$1 >= start && x.$2 <= start + len).length;
      if (count > bestCount) {
        bestCount = count;
        bestStart = start;
      }
    }
    // snap to a word boundary (not too far back: text may have no spaces)
    final windowStart = bestStart;
    while (bestStart > 0 && text[bestStart - 1] != ' ') {
      bestStart--;
      if (windowStart - bestStart > 20) break;
    }
    final end = (bestStart + len).clamp(0, text.length);
    final pre = bestStart > 0 ? '…' : '';
    final post = end < text.length ? '…' : '';
    final shown = '$pre${text.substring(bestStart, end)}$post';
    final hits = [
      for (final h in hitPos)
        if (h.$1 >= bestStart && h.$2 <= end) (h.$1 - bestStart + pre.length, h.$2 - bestStart + pre.length)
    ];
    return (text: shown, hits: hits);
  }
}
