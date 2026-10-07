import 'dart:convert';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/gemma_service.dart';
import '../services/knowledge_base.dart';
import 'notes_provider.dart';

/// The search index, collections and statistics over all memos. Rebuilt
/// whenever the memo list changes (a transcript or summary arrives...).
final Provider<KnowledgeBase> knowledgeProvider = Provider<KnowledgeBase>((ref) {
  final recordings = ref.watch(notesProvider.select((s) => s.recordings));
  return KnowledgeBase.build(recordings);
});

/// Names of theme collections: AI-made or set by the user, keyed by the
/// collection's keywords. Kept in SharedPreferences.
class CollectionNamesNotifier extends StateNotifier<Map<String, String>> {
  static const _key = 'collection_names';
  bool _naming = false;

  CollectionNamesNotifier() : super(const {}) {
    _load();
  }

  Future<void> _load() async {
    final p = await SharedPreferences.getInstance();
    try {
      final m = jsonDecode(p.getString(_key) ?? '{}') as Map<String, dynamic>;
      state = m.map((k, v) => MapEntry(k, v.toString()));
    } catch (_) {}
  }

  Future<void> _save() async {
    final p = await SharedPreferences.getInstance();
    await p.setString(_key, jsonEncode(state));
  }

  String nameOf(MemoCollection c) => state[c.key] ?? c.autoName;

  Future<void> rename(MemoCollection c, String name) async {
    final n = name.trim();
    state = {...state}..removeWhere((k, _) => k == c.key);
    if (n.isNotEmpty) state = {...state, c.key: n};
    await _save();
  }

  bool get naming => _naming;

  /// Asks the on-device model to name collections that have no name yet.
  /// Returns how many were named (0 without a model).
  Future<int> nameMissing(KnowledgeBase kb, {bool all = false}) async {
    if (_naming) return 0;
    _naming = true;
    var named = 0;
    try {
      if (!await GemmaService.instance.hasModel()) return 0;
      for (final c in kb.collections) {
        if (!all && state.containsKey(c.key)) continue;
        final name = await GemmaService.instance
            .nameCollection(c.keywords, c.items.map((e) => e.displayTitle).toList());
        if (name == null || !mounted) continue;
        state = {...state, c.key: name};
        named++;
      }
      if (named > 0) await _save();
    } catch (e) {
      print('[Knowledge] naming collections: $e');
    } finally {
      _naming = false;
    }
    return named;
  }
}

final StateNotifierProvider<CollectionNamesNotifier, Map<String, String>> collectionNamesProvider =
    StateNotifierProvider<CollectionNamesNotifier, Map<String, String>>((ref) => CollectionNamesNotifier());

/// Action items ticked off (key = memo name + text), in SharedPreferences.
class DoneActionsNotifier extends StateNotifier<Set<String>> {
  static const _key = 'actions_done';
  DoneActionsNotifier() : super(const {}) {
    SharedPreferences.getInstance().then((p) {
      if (mounted) state = (p.getStringList(_key) ?? const []).toSet();
    });
  }

  Future<void> toggle(String key) async {
    state = state.contains(key) ? ({...state}..remove(key)) : {...state, key};
    final p = await SharedPreferences.getInstance();
    await p.setStringList(_key, state.toList());
  }
}

final StateNotifierProvider<DoneActionsNotifier, Set<String>> doneActionsProvider = StateNotifierProvider<DoneActionsNotifier, Set<String>>((ref) => DoneActionsNotifier());
