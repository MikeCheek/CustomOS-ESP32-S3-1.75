import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/ble_protocol.dart';

/// Local persistence for recording metadata, transcripts, summaries, etc.
class NotesStorage {
  static const String _key = 'notes_recordings';

  /// Load all recording entries from SharedPreferences
  static Future<List<RecordingEntry>> loadAll() async {
    final prefs = await SharedPreferences.getInstance();
    final json = prefs.getString(_key);
    if (json == null || json.isEmpty) return [];
    try {
      final list = jsonDecode(json) as List<dynamic>;
      return list.map((e) => RecordingEntry.fromJson(e as Map<String, dynamic>)).toList();
    } catch (_) {
      return [];
    }
  }

  /// Save all recording entries to SharedPreferences
  static Future<void> saveAll(List<RecordingEntry> entries) async {
    final prefs = await SharedPreferences.getInstance();
    final json = jsonEncode(entries.map((e) => e.toJson()).toList());
    await prefs.setString(_key, json);
  }

  /// Add or update a recording entry (matched by name)
  static Future<void> save(RecordingEntry entry) async {
    final entries = await loadAll();
    final idx = entries.indexWhere((e) => e.name == entry.name);
    if (idx >= 0) {
      entries[idx] = entry;
    } else {
      entries.add(entry);
    }
    await saveAll(entries);
  }

  /// Delete a recording entry by name
  static Future<void> delete(String name) async {
    final entries = await loadAll();
    entries.removeWhere((e) => e.name == name);
    await saveAll(entries);
  }

  /// Get a single recording entry by name
  static Future<RecordingEntry?> get(String name) async {
    final entries = await loadAll();
    try {
      return entries.firstWhere((e) => e.name == name);
    } catch (_) {
      return null;
    }
  }

  /// Clear all recording metadata (does NOT delete WAV files from SD)
  static Future<void> clearAll() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove(_key);
  }
}
