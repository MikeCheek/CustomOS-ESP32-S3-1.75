import 'dart:convert';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/ble_protocol.dart';

class WatchfaceConfig {
  final String id;
  final String name;
  final String? thumbnailBg;
  final WatchfaceLayout layout;
  final bool isPreset;
  final DateTime createdAt;

  WatchfaceConfig({
    required this.id,
    required this.name,
    this.thumbnailBg,
    required this.layout,
    this.isPreset = false,
    DateTime? createdAt,
  }) : createdAt = createdAt ?? DateTime.now();

  Map<String, dynamic> toJson() => {
    'id': id,
    'name': name,
    'thumbnailBg': thumbnailBg ?? layout.bgColor,
    'layout': layout.toJson(),
    'isPreset': isPreset,
    'createdAt': createdAt.toIso8601String(),
  };

  factory WatchfaceConfig.fromJson(Map<String, dynamic> json) => WatchfaceConfig(
    id: json['id'] ?? '',
    name: json['name'] ?? 'Untitled',
    thumbnailBg: json['thumbnailBg'],
    layout: WatchfaceLayout.fromJson(json['layout'] ?? {}),
    isPreset: json['isPreset'] ?? false,
    createdAt: json['createdAt'] != null ? DateTime.parse(json['createdAt']) : null,
  );

  WatchfaceConfig copyWith({String? name, WatchfaceLayout? layout}) {
    return WatchfaceConfig(
      id: id,
      name: name ?? this.name,
      thumbnailBg: thumbnailBg,
      layout: layout ?? this.layout,
      isPreset: isPreset,
      createdAt: createdAt,
    );
  }
}

class WatchfaceStorage {
  static const _key = 'watchface_configs';
  static const _activeKey = 'watchface_active_id';

  Future<List<WatchfaceConfig>> loadAll() async {
    final prefs = await SharedPreferences.getInstance();
    final str = prefs.getString(_key);
    if (str == null) return _builtInPresets();

    try {
      final list = (jsonDecode(str) as List)
          .map((e) => WatchfaceConfig.fromJson(e as Map<String, dynamic>))
          .toList();
      // Ensure built-in presets are always present
      final existingIds = list.map((e) => e.id).toSet();
      for (final preset in _builtInPresets()) {
        if (!existingIds.contains(preset.id)) {
          list.insert(0, preset);
        }
      }
      return list;
    } catch (_) {
      return _builtInPresets();
    }
  }

  Future<void> saveAll(List<WatchfaceConfig> configs) async {
    final prefs = await SharedPreferences.getInstance();
    final jsonList = configs.map((c) => c.toJson()).toList();
    await prefs.setString(_key, jsonEncode(jsonList));
  }

  Future<void> save(WatchfaceConfig config) async {
    final configs = await loadAll();
    final idx = configs.indexWhere((c) => c.id == config.id);
    if (idx >= 0) {
      configs[idx] = config;
    } else {
      configs.add(config);
    }
    await saveAll(configs);
  }

  Future<void> delete(String id) async {
    final configs = await loadAll();
    configs.removeWhere((c) => c.id == id);
    await saveAll(configs);
  }

  Future<String> getActiveId() async {
    final prefs = await SharedPreferences.getInstance();
    return prefs.getString(_activeKey) ?? '';
  }

  Future<void> setActiveId(String id) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_activeKey, id);
  }

  static String generateId() {
    return 'wf_${DateTime.now().millisecondsSinceEpoch}';
  }

  static List<WatchfaceConfig> _builtInPresets() {
    return [
      WatchfaceConfig(
        id: 'preset_classic',
        name: 'Classic',
        layout: WatchfaceLayout(bgColor: '#000000', widgets: [
          WatchfaceWidget(type: 'analog_clock', x: 233, y: 233, radius: 180, color: '#7B61FF'),
          WatchfaceWidget(type: 'digital_time', x: 233, y: 140, fontSize: 48, color: '#F2F2F2'),
          WatchfaceWidget(type: 'date', x: 233, y: 330, fontSize: 14, color: '#6E6E6E'),
          WatchfaceWidget(type: 'battery', x: 233, y: 400, fontSize: 12, color: '#39D6FF'),
        ]),
        isPreset: true,
      ),
      WatchfaceConfig(
        id: 'preset_digital',
        name: 'Digital',
        layout: WatchfaceLayout(bgColor: '#000000', widgets: [
          WatchfaceWidget(type: 'digital_time', x: 233, y: 180, fontSize: 64, color: '#39D6FF'),
          WatchfaceWidget(type: 'date', x: 233, y: 280, fontSize: 18, color: '#F2F2F2'),
          WatchfaceWidget(type: 'seconds_ring', x: 233, y: 233, radius: 210, color: '#7B61FF'),
          WatchfaceWidget(type: 'battery', x: 233, y: 400, fontSize: 12, color: '#6E6E6E'),
        ]),
        isPreset: true,
      ),
      WatchfaceConfig(
        id: 'preset_sport',
        name: 'Sport',
        layout: WatchfaceLayout(bgColor: '#000000', widgets: [
          WatchfaceWidget(type: 'digital_time', x: 233, y: 120, fontSize: 48, color: '#2FC765'),
          WatchfaceWidget(type: 'steps', x: 120, y: 240, fontSize: 24, color: '#2FC765'),
          WatchfaceWidget(type: 'heart_rate', x: 346, y: 240, fontSize: 24, color: '#F84D61'),
          WatchfaceWidget(type: 'date', x: 233, y: 340, fontSize: 14, color: '#F2F2F2'),
          WatchfaceWidget(type: 'battery', x: 233, y: 400, fontSize: 12, color: '#6E6E6E'),
        ]),
        isPreset: true,
      ),
      WatchfaceConfig(
        id: 'preset_minimal',
        name: 'Minimal',
        layout: WatchfaceLayout(bgColor: '#000000', widgets: [
          WatchfaceWidget(type: 'digital_time', x: 233, y: 200, fontSize: 72, color: '#F2F2F2'),
          WatchfaceWidget(type: 'date', x: 233, y: 300, fontSize: 16, color: '#6E6E6E'),
        ]),
        isPreset: true,
      ),
      WatchfaceConfig(
        id: 'preset_weather',
        name: 'Weather',
        layout: WatchfaceLayout(bgColor: '#000000', widgets: [
          WatchfaceWidget(type: 'digital_time', x: 233, y: 140, fontSize: 48, color: '#F2F2F2'),
          WatchfaceWidget(type: 'weather', x: 233, y: 240, fontSize: 28, color: '#39D6FF'),
          WatchfaceWidget(type: 'date', x: 233, y: 330, fontSize: 14, color: '#6E6E6E'),
          WatchfaceWidget(type: 'battery', x: 233, y: 400, fontSize: 12, color: '#6E6E6E'),
        ]),
        isPreset: true,
      ),
    ];
  }
}
