import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../../services/watchface_storage.dart';
import '../../services/ble_protocol.dart';
import '../../providers/ble_provider.dart';
import '../../theme/app_theme.dart';
import 'watchface_designer.dart';

class WatchfaceLibrary extends ConsumerStatefulWidget {
  const WatchfaceLibrary({super.key});

  @override
  ConsumerState<WatchfaceLibrary> createState() => _WatchfaceLibraryState();
}

class _WatchfaceLibraryState extends ConsumerState<WatchfaceLibrary> {
  final WatchfaceStorage _storage = WatchfaceStorage();
  List<WatchfaceConfig> _configs = [];
  bool _loading = true;
  String? _activeId;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final configs = await _storage.loadAll();
    final activeId = await _storage.getActiveId();
    setState(() {
      _configs = configs;
      _activeId = activeId;
      _loading = false;
    });
  }

  void _openDesigner({WatchfaceConfig? existing}) {
    final notifier = WatchfaceNotifier();
    if (existing != null) {
      notifier.loadFrom(existing.layout);
    }
    Navigator.push(
      context,
      MaterialPageRoute(
        builder: (_) => WatchfaceDesigner(
          existingConfig: existing,
          onSave: (config) async {
            await _storage.save(config);
            _load();
          },
        ),
      ),
    ).then((_) => _load());
  }

  void _confirmDelete(WatchfaceConfig config) {
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: AppColors.surface,
        title: Text('Delete "${config.name}"?',
            style: const TextStyle(color: AppColors.text, fontSize: 16)),
        content: const Text('This cannot be undone.',
            style: TextStyle(color: AppColors.textDim, fontSize: 14)),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('Cancel', style: TextStyle(color: AppColors.textDim)),
          ),
          TextButton(
            onPressed: () async {
              Navigator.pop(ctx);
              await _storage.delete(config.id);
              _load();
            },
            child: const Text('Delete', style: TextStyle(color: AppColors.danger)),
          ),
        ],
      ),
    );
  }

  void _confirmNewFromPreset(WatchfaceConfig preset) async {
    final config = WatchfaceConfig(
      id: WatchfaceStorage.generateId(),
      name: '${preset.name} Copy',
      layout: preset.layout,
    );
    await _storage.save(config);
    _load();
    _openDesigner(existing: config);
  }

  @override
  Widget build(BuildContext context) {
    final presets = _configs.where((c) => c.isPreset).toList();
    final customs = _configs.where((c) => !c.isPreset).toList();

    return Scaffold(
      backgroundColor: AppColors.bg,
      appBar: AppBar(
        title: const Text('Watchfaces'),
        actions: [
          IconButton(
            icon: const Icon(Icons.add_circle_outline),
            onPressed: () => _openDesigner(),
            tooltip: 'New Watchface',
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator(color: AppColors.accent))
          : ListView(
              padding: const EdgeInsets.all(16),
              children: [
                if (customs.isNotEmpty) ...[
                  const Text('My Watchfaces',
                      style: TextStyle(color: AppColors.textDim, fontSize: 13, fontWeight: FontWeight.w600)),
                  const SizedBox(height: 12),
                  _buildGrid(customs, editable: true),
                  const SizedBox(height: 24),
                ],
                const Text('Presets',
                    style: TextStyle(color: AppColors.textDim, fontSize: 13, fontWeight: FontWeight.w600)),
                const SizedBox(height: 12),
                _buildGrid(presets, editable: false),
              ],
            ),
    );
  }

  Widget _buildGrid(List<WatchfaceConfig> configs, {required bool editable}) {
    return GridView.builder(
      shrinkWrap: true,
      physics: const NeverScrollableScrollPhysics(),
      gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
        crossAxisCount: 3,
        mainAxisSpacing: 12,
        crossAxisSpacing: 12,
        childAspectRatio: 0.85,
      ),
      itemCount: configs.length + 1, // +1 for "New" tile
      itemBuilder: (ctx, index) {
        if (index == configs.length) {
          return _buildNewTile();
        }
        return _buildWatchfaceTile(configs[index], editable: editable);
      },
    );
  }

  Widget _buildNewTile() {
    return GestureDetector(
      onTap: () => _openDesigner(),
      child: Container(
        decoration: BoxDecoration(
          color: AppColors.surfaceLight,
          borderRadius: BorderRadius.circular(16),
          border: Border.all(color: AppColors.accent.withValues(alpha: 0.3), width: 1.5),
        ),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Container(
              width: 56,
              height: 56,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                border: Border.all(color: AppColors.accent, width: 1.5),
              ),
              child: const Icon(Icons.add, color: AppColors.accent, size: 28),
            ),
            const SizedBox(height: 8),
            const Text('New', style: TextStyle(color: AppColors.textDim, fontSize: 11)),
          ],
        ),
      ),
    );
  }

  Widget _buildWatchfaceTile(WatchfaceConfig config, {required bool editable}) {
    final isActive = config.id == _activeId;
    final bgColor = _hexToColor(config.thumbnailBg ?? config.layout.bgColor);

    // Pick the dominant widget color for the preview ring
    Color ringColor = AppColors.accent;
    if (config.layout.widgets.isNotEmpty) {
      ringColor = _hexToColor(config.layout.widgets.first.color);
    }

    return GestureDetector(
      onTap: () {
        if (editable) {
          _openDesigner(existing: config);
        } else {
          _confirmNewFromPreset(config);
        }
      },
      onLongPress: editable
          ? () => _showOptions(config)
          : () => _confirmNewFromPreset(config),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Stack(
            alignment: Alignment.center,
            children: [
              // Circular preview
              Container(
                width: 72,
                height: 72,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: bgColor,
                  border: Border.all(
                    color: isActive ? AppColors.accent : ringColor.withValues(alpha: 0.4),
                    width: isActive ? 2.5 : 1.5,
                  ),
                ),
                child: ClipOval(
                  child: _buildMiniPreview(config.layout),
                ),
              ),
              if (isActive)
                Positioned(
                  bottom: 0,
                  child: Container(
                    padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                    decoration: BoxDecoration(
                      color: AppColors.accent,
                      borderRadius: BorderRadius.circular(6),
                    ),
                    child: const Text('ON', style: TextStyle(color: Colors.white, fontSize: 8, fontWeight: FontWeight.bold)),
                  ),
                ),
            ],
          ),
          const SizedBox(height: 6),
          Text(
            config.name,
            style: TextStyle(
              color: isActive ? AppColors.accent : AppColors.text,
              fontSize: 11,
              fontWeight: isActive ? FontWeight.w600 : FontWeight.normal,
            ),
            overflow: TextOverflow.ellipsis,
          ),
        ],
      ),
    );
  }

  Widget _buildMiniPreview(WatchfaceLayout layout) {
    // Simplified mini preview — just show colored dots for each widget
    return CustomPaint(
      painter: _MiniPreviewPainter(widgets: layout.widgets),
    );
  }

  void _showOptions(WatchfaceConfig config) {
    showModalBottomSheet(
      context: context,
      backgroundColor: AppColors.surface,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
      ),
      builder: (ctx) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Container(
              width: 40, height: 4,
              margin: const EdgeInsets.only(top: 12),
              decoration: BoxDecoration(
                color: AppColors.textDim,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
            const SizedBox(height: 16),
            Text(config.name,
                style: const TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w600)),
            const SizedBox(height: 16),
            ListTile(
              leading: const Icon(Icons.edit, color: AppColors.accent),
              title: const Text('Edit', style: TextStyle(color: AppColors.text)),
              onTap: () {
                Navigator.pop(ctx);
                _openDesigner(existing: config);
              },
            ),
            ListTile(
              leading: const Icon(Icons.copy, color: AppColors.accent2),
              title: const Text('Duplicate', style: TextStyle(color: AppColors.text)),
              onTap: () async {
                Navigator.pop(ctx);
                final copy = WatchfaceConfig(
                  id: WatchfaceStorage.generateId(),
                  name: '${config.name} Copy',
                  layout: config.layout,
                );
                await _storage.save(copy);
                _load();
              },
            ),
            ListTile(
              leading: const Icon(Icons.send, color: AppColors.success),
              title: const Text('Send to Watch', style: TextStyle(color: AppColors.text)),
              onTap: () async {
                Navigator.pop(ctx);
                final ok = await ref.read(bleProvider.notifier).sendWatchface(config.layout);
                if (mounted) {
                  ScaffoldMessenger.of(context).showSnackBar(
                    SnackBar(
                      content: Text(ok ? 'Watchface sent!' : 'Send failed'),
                      backgroundColor: ok ? AppColors.success : AppColors.danger,
                    ),
                  );
                }
              },
            ),
            if (!config.isPreset)
              ListTile(
                leading: const Icon(Icons.delete, color: AppColors.danger),
                title: const Text('Delete', style: TextStyle(color: AppColors.danger)),
                onTap: () {
                  Navigator.pop(ctx);
                  _confirmDelete(config);
                },
              ),
            const SizedBox(height: 8),
          ],
        ),
      ),
    );
  }

  Color _hexToColor(String hex) {
    hex = hex.replaceAll('#', '');
    if (hex.length == 6) hex = 'FF$hex';
    try {
      return Color(int.parse(hex, radix: 16));
    } catch (_) {
      return Colors.white;
    }
  }
}

class _MiniPreviewPainter extends CustomPainter {
  final List<WatchfaceWidget> widgets;
  _MiniPreviewPainter({required this.widgets});

  @override
  void paint(Canvas canvas, Size size) {
    final center = Offset(size.width / 2, size.height / 2);
    final scale = size.width / 466;

    for (final w in widgets) {
      final paint = Paint()..color = _hexToColor(w.color);

      switch (w.type) {
        case 'analog_clock':
          paint
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1;
          canvas.drawCircle(center, w.radius * scale, paint);
          paint
            ..style = PaintingStyle.fill
            ..strokeWidth = 0;
          canvas.drawCircle(center, 2, paint);
          break;
        case 'seconds_ring':
          paint
            ..style = PaintingStyle.stroke
            ..strokeWidth = 2;
          canvas.drawCircle(center, w.radius * scale, paint);
          break;
        default:
          canvas.drawCircle(Offset(w.x * scale, w.y * scale), 3, paint);
          break;
      }
    }
  }

  Color _hexToColor(String hex) {
    hex = hex.replaceAll('#', '');
    if (hex.length == 6) hex = 'FF$hex';
    try {
      return Color(int.parse(hex, radix: 16));
    } catch (_) {
      return Colors.white;
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
