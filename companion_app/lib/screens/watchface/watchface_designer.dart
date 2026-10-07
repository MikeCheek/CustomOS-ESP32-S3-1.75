import 'dart:math';
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../../services/ble_protocol.dart';
import '../../services/watchface_storage.dart';
import '../../providers/ble_provider.dart';
import '../../theme/app_theme.dart';
import 'property_panel.dart';

final watchfaceProvider = StateNotifierProvider<WatchfaceNotifier, WatchfaceLayout>(
  (ref) => WatchfaceNotifier(),
);

class WatchfaceNotifier extends StateNotifier<WatchfaceLayout> {
  WatchfaceNotifier() : super(WatchfaceLayout());

  int _selectedIndex = -1;
  int get selectedIndex => _selectedIndex;

  void loadFrom(WatchfaceLayout layout) {
    _selectedIndex = -1;
    state = WatchfaceLayout(
      bgColor: layout.bgColor,
      widgets: List.from(layout.widgets.map((w) => WatchfaceWidget(
        type: w.type, x: w.x, y: w.y, radius: w.radius,
        color: w.color, font: w.font, fontSize: w.fontSize,
        text: w.text, dataSrc: w.dataSrc,
      ))),
    );
  }

  void selectWidget(int index) {
    _selectedIndex = index;
    state = WatchfaceLayout(bgColor: state.bgColor, widgets: List.from(state.widgets));
  }

  void deselectWidget() {
    _selectedIndex = -1;
    state = WatchfaceLayout(bgColor: state.bgColor, widgets: List.from(state.widgets));
  }

  void addWidget(WatchfaceWidget widget) {
    final newWidgets = List<WatchfaceWidget>.from(state.widgets)..add(widget);
    _selectedIndex = newWidgets.length - 1;
    state = WatchfaceLayout(bgColor: state.bgColor, widgets: newWidgets);
  }

  void removeSelectedWidget() {
    if (_selectedIndex >= 0 && _selectedIndex < state.widgets.length) {
      final newWidgets = List<WatchfaceWidget>.from(state.widgets)..removeAt(_selectedIndex);
      _selectedIndex = -1;
      state = WatchfaceLayout(bgColor: state.bgColor, widgets: newWidgets);
    }
  }

  void updateWidget(int index, WatchfaceWidget updated) {
    if (index >= 0 && index < state.widgets.length) {
      final newWidgets = List<WatchfaceWidget>.from(state.widgets);
      newWidgets[index] = WatchfaceWidget(
        type: updated.type, x: updated.x, y: updated.y,
        radius: updated.radius, color: updated.color,
        font: updated.font, fontSize: updated.fontSize,
        text: updated.text, dataSrc: updated.dataSrc,
      );
      state = WatchfaceLayout(bgColor: state.bgColor, widgets: newWidgets);
    }
  }

  void setBgColor(String hex) {
    state = WatchfaceLayout(bgColor: hex, widgets: List.from(state.widgets));
  }
}

class WatchfaceDesigner extends ConsumerStatefulWidget {
  final WatchfaceConfig? existingConfig;
  final Future<void> Function(WatchfaceConfig)? onSave;

  const WatchfaceDesigner({super.key, this.existingConfig, this.onSave});

  @override
  ConsumerState<WatchfaceDesigner> createState() => _WatchfaceDesignerState();
}

class _WatchfaceDesignerState extends ConsumerState<WatchfaceDesigner> {
  String _name = '';
  String? _editingId;

  @override
  void initState() {
    super.initState();
    if (widget.existingConfig != null) {
      _name = widget.existingConfig!.name;
      _editingId = widget.existingConfig!.id;
    } else {
      _name = 'My Watchface';
    }
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final notifier = ref.read(watchfaceProvider.notifier);
      if (widget.existingConfig != null) {
        notifier.loadFrom(widget.existingConfig!.layout);
      } else {
        notifier.loadFrom(WatchfaceLayout(bgColor: '#000000', widgets: [
          WatchfaceWidget(type: 'analog_clock', x: 233, y: 233, radius: 180, color: '#7B61FF'),
          WatchfaceWidget(type: 'digital_time', x: 233, y: 140, fontSize: 48, color: '#F2F2F2'),
          WatchfaceWidget(type: 'date', x: 233, y: 330, fontSize: 14, color: '#6E6E6E'),
          WatchfaceWidget(type: 'battery', x: 233, y: 400, fontSize: 12, color: '#39D6FF'),
        ]));
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final layout = ref.watch(watchfaceProvider);
    final notifier = ref.read(watchfaceProvider.notifier);
    final ble = ref.watch(bleProvider);

    return Scaffold(
      backgroundColor: AppColors.bg,
      appBar: AppBar(
        title: Text(_editingId != null ? 'Edit Watchface' : 'New Watchface'),
        actions: [
          IconButton(
            icon: const Icon(Icons.add_circle_outline),
            onPressed: () => _showWidgetPicker(context, ref),
            tooltip: 'Add Widget',
          ),
          IconButton(
            icon: const Icon(Icons.save_outlined),
            onPressed: () => _save(layout),
            tooltip: 'Save',
          ),
          IconButton(
            icon: const Icon(Icons.send),
            onPressed: ble.isConnected
                ? () async {
                    final ok = await ref.read(bleProvider.notifier).sendWatchface(layout);
                    if (mounted) {
                      ScaffoldMessenger.of(context).showSnackBar(
                        SnackBar(
                          content: Text(ok ? 'Watchface sent!' : 'Send failed - check connection'),
                          backgroundColor: ok ? null : AppColors.danger,
                        ),
                      );
                    }
                  }
                : () {
                    ScaffoldMessenger.of(context).showSnackBar(
                      const SnackBar(content: Text('Not connected to watch')),
                    );
                  },
            tooltip: 'Send to Watch',
          ),
        ],
      ),
      body: Column(
        children: [
          // Name input
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
            color: AppColors.surface,
            child: TextField(
              controller: TextEditingController(text: _name)
                ..selection = TextSelection.collapsed(offset: _name.length),
              onChanged: (v) => _name = v,
              style: const TextStyle(color: AppColors.text, fontSize: 16),
              decoration: const InputDecoration(
                hintText: 'Watchface name',
                border: InputBorder.none,
                isDense: true,
                contentPadding: EdgeInsets.symmetric(vertical: 8),
              ),
            ),
          ),
          Expanded(
            child: Center(
              child: SizedBox(
                width: 466,
                height: 466,
                child: Stack(
                  clipBehavior: Clip.none,
                  children: [
                    Container(
                      width: 466,
                      height: 466,
                      decoration: BoxDecoration(
                        color: _hexToColor(layout.bgColor),
                        shape: BoxShape.circle,
                        border: Border.all(color: AppColors.surfaceLight, width: 2),
                      ),
                    ),
                    ClipOval(
                      child: SizedBox(
                        width: 466,
                        height: 466,
                        child: CustomPaint(
                          painter: WatchfacePainter(widgets: layout.widgets),
                        ),
                      ),
                    ),
                    ...layout.widgets.asMap().entries.map((entry) {
                      final i = entry.key;
                      final w = entry.value;
                      final isSelected = i == notifier.selectedIndex;
                      return Positioned(
                        left: w.x - 25,
                        top: w.y - 25,
                        child: GestureDetector(
                          onTap: () => notifier.selectWidget(i),
                          onPanUpdate: (details) {
                            final newX = (w.x + details.delta.dx).clamp(0.0, 466.0);
                            final newY = (w.y + details.delta.dy).clamp(0.0, 466.0);
                            notifier.updateWidget(i, WatchfaceWidget(
                              type: w.type, x: newX, y: newY,
                              radius: w.radius, color: w.color,
                              font: w.font, fontSize: w.fontSize,
                              text: w.text, dataSrc: w.dataSrc,
                            ));
                          },
                          child: Container(
                            width: 50,
                            height: 50,
                            decoration: isSelected
                                ? BoxDecoration(
                                    border: Border.all(color: AppColors.accent, width: 2),
                                    borderRadius: BorderRadius.circular(6),
                                  )
                                : null,
                            child: Center(
                              child: Icon(
                                _widgetIcon(w.type),
                                color: _hexToColor(w.color).withValues(alpha: isSelected ? 1.0 : 0.4),
                                size: 20,
                              ),
                            ),
                          ),
                        ),
                      );
                    }),
                  ],
                ),
              ),
            ),
          ),
          if (notifier.selectedIndex >= 0)
            PropertyPanel(
              widget: layout.widgets[notifier.selectedIndex],
              onUpdate: (updated) => notifier.updateWidget(notifier.selectedIndex, updated),
              onDelete: () => notifier.removeSelectedWidget(),
            )
          else
            _BottomBar(layout: layout),
        ],
      ),
    );
  }

  Future<void> _save(WatchfaceLayout layout) async {
    if (_name.trim().isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Please enter a name')),
      );
      return;
    }

    final config = WatchfaceConfig(
      id: _editingId ?? WatchfaceStorage.generateId(),
      name: _name.trim(),
      layout: layout,
    );

    if (widget.onSave != null) {
      await widget.onSave!(config);
    }

    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Watchface saved!')),
      );
      Navigator.pop(context);
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

  IconData _widgetIcon(String type) {
    switch (type) {
      case 'analog_clock': return Icons.access_time;
      case 'digital_time': return Icons.access_time_filled;
      case 'date': return Icons.calendar_today;
      case 'battery': return Icons.battery_full;
      case 'steps': return Icons.directions_walk;
      case 'weather': return Icons.wb_sunny;
      case 'heart_rate': return Icons.favorite;
      case 'text': return Icons.text_fields;
      case 'seconds_ring': return Icons.watch;
      case 'digital_seconds': return Icons.timer;
      case 'weekday': return Icons.view_week;
      case 'ampm': return Icons.light_mode;
      case 'complication': return Icons.donut_large;
      default: return Icons.widgets;
    }
  }

  void _showWidgetPicker(BuildContext context, WidgetRef ref) {
    final items = [
      _PickerItem('analog_clock', 'Analog Clock', Icons.access_time),
      _PickerItem('digital_time', 'Digital Time', Icons.access_time_filled),
      _PickerItem('date', 'Date', Icons.calendar_today),
      _PickerItem('battery', 'Battery', Icons.battery_full),
      _PickerItem('steps', 'Steps', Icons.directions_walk),
      _PickerItem('weather', 'Weather', Icons.wb_sunny),
      _PickerItem('heart_rate', 'Heart Rate', Icons.favorite),
      _PickerItem('digital_seconds', 'Seconds', Icons.timer),
      _PickerItem('weekday', 'Weekday', Icons.view_week),
      _PickerItem('ampm', 'AM/PM', Icons.light_mode),
      _PickerItem('text', 'Custom Text', Icons.text_fields),
      _PickerItem('seconds_ring', 'Seconds Ring', Icons.watch),
      // Round complications (firmware 3.2+): live data in a ring
      _PickerItem('comp:steps', 'Steps ring', Icons.directions_walk),
      _PickerItem('comp:next_event', 'Next event', Icons.event),
      _PickerItem('comp:weather', 'Weather ring', Icons.cloud),
      _PickerItem('comp:phone_battery', 'Phone battery', Icons.smartphone),
      _PickerItem('comp:notifications', 'Messages', Icons.notifications),
      _PickerItem('comp:todos', 'Memo to-dos', Icons.task_alt),
      _PickerItem('comp:navigation', 'Next turn', Icons.navigation),
    ];

    showModalBottomSheet(
      context: context,
      backgroundColor: AppColors.surface,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
      ),
      builder: (ctx) => Container(
        padding: const EdgeInsets.all(16),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Container(
              width: 40, height: 4,
              decoration: BoxDecoration(
                color: AppColors.textDim,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
            const SizedBox(height: 16),
            const Text('Add Widget',
                style: TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w600)),
            const SizedBox(height: 16),
            SizedBox(
              height: 200,
              child: GridView.builder(
                gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                  crossAxisCount: 3, mainAxisSpacing: 8, crossAxisSpacing: 8,
                ),
                itemCount: items.length,
                itemBuilder: (ctx, index) {
                  final item = items[index];
                  return Material(
                    color: AppColors.surfaceLight,
                    borderRadius: BorderRadius.circular(12),
                    child: InkWell(
                      borderRadius: BorderRadius.circular(12),
                      onTap: () {
                        ref.read(watchfaceProvider.notifier).addWidget(
                          item.type.startsWith('comp:')
                              ? WatchfaceWidget(
                                  type: 'complication', x: 233, y: 233, radius: 42, dataSrc: item.type.substring(5))
                              : WatchfaceWidget(type: item.type, x: 233, y: 233),
                        );
                        Navigator.pop(ctx);
                      },
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Icon(item.icon, color: AppColors.accent, size: 22),
                          const SizedBox(height: 4),
                          Text(item.label,
                              style: const TextStyle(color: AppColors.textDim, fontSize: 10)),
                        ],
                      ),
                    ),
                  );
                },
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _PickerItem {
  final String type;
  final String label;
  final IconData icon;
  _PickerItem(this.type, this.label, this.icon);
}

class WatchfacePainter extends CustomPainter {
  final List<WatchfaceWidget> widgets;
  WatchfacePainter({required this.widgets});

  @override
  void paint(Canvas canvas, Size size) {
    final center = Offset(size.width / 2, size.height / 2);
    final now = DateTime.now();

    for (final w in widgets) {
      final paint = Paint()..color = _hexToColor(w.color);

      switch (w.type) {
        case 'analog_clock':
          _drawAnalogClock(canvas, center, w.radius, paint, now);
          break;
        case 'digital_time':
          final text = '${now.hour.toString().padLeft(2, '0')}:${now.minute.toString().padLeft(2, '0')}';
          _drawText(canvas, Offset(w.x, w.y), text, w.fontSize, paint);
          break;
        case 'date':
          const months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
          _drawText(canvas, Offset(w.x, w.y),
              '${months[now.month - 1]} ${now.day}', w.fontSize, paint);
          break;
        case 'battery':
          _drawText(canvas, Offset(w.x, w.y), '75%', w.fontSize, paint);
          break;
        case 'steps':
          _drawText(canvas, Offset(w.x, w.y), '8,432', w.fontSize, paint);
          break;
        case 'weather':
          _drawText(canvas, Offset(w.x, w.y), '22\u00B0C', w.fontSize, paint);
          break;
        case 'heart_rate':
          _drawText(canvas, Offset(w.x, w.y), '72 bpm', w.fontSize, paint);
          break;
        case 'digital_seconds':
          _drawText(canvas, Offset(w.x, w.y), now.second.toString().padLeft(2, '0'), w.fontSize, paint);
          break;
        case 'weekday':
          const days = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
          _drawText(canvas, Offset(w.x, w.y), days[now.weekday % 7], w.fontSize, paint);
          break;
        case 'ampm':
          _drawText(canvas, Offset(w.x, w.y), now.hour >= 12 ? 'PM' : 'AM', w.fontSize, paint);
          break;
        case 'seconds_ring':
          paint
            ..style = PaintingStyle.stroke
            ..strokeWidth = 4;
          canvas.drawCircle(center, w.radius, paint);
          final secAngle = (now.second / 60) * 2 * pi - pi / 2;
          paint
            ..style = PaintingStyle.fill
            ..strokeWidth = 0;
          final secPos = Offset(
            center.dx + w.radius * cos(secAngle),
            center.dy + w.radius * sin(secAngle),
          );
          canvas.drawCircle(secPos, 6, paint);
          break;
        case 'text':
          _drawText(canvas, Offset(w.x, w.y),
              w.text.isNotEmpty ? w.text : 'Text', w.fontSize, paint);
          break;
        case 'complication':
          final r = w.radius > 10 ? w.radius : 42.0;
          final ring = Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 5
            ..color = paint.color.withValues(alpha: 0.25);
          canvas.drawCircle(Offset(w.x, w.y), r, ring);
          canvas.drawArc(Rect.fromCircle(center: Offset(w.x, w.y), radius: r), -pi / 2, 4.2, false,
              ring..color = paint.color);
          const sample = {
            'steps': '6240', 'next_event': '25m', 'weather': '22C', 'phone_battery': '81%',
            'notifications': '3', 'todos': '4', 'navigation': '300 m', 'battery': '75%', 'seconds': '42',
          };
          _drawText(canvas, Offset(w.x, w.y), sample[w.dataSrc] ?? '--', 16, Paint()..color = Colors.white);
          break;
      }
    }
  }

  void _drawAnalogClock(Canvas canvas, Offset center, double radius, Paint paint, DateTime now) {
    paint
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2;
    canvas.drawCircle(center, radius, paint);

    paint.strokeWidth = 3;
    for (var i = 0; i < 12; i++) {
      final angle = (i * 30 - 90) * pi / 180;
      final outer = Offset(center.dx + radius * 0.9 * cos(angle), center.dy + radius * 0.9 * sin(angle));
      final inner = Offset(center.dx + radius * 0.78 * cos(angle), center.dy + radius * 0.78 * sin(angle));
      canvas.drawLine(inner, outer, paint);
    }

    paint
      ..style = PaintingStyle.fill
      ..strokeWidth = 0;
    canvas.drawCircle(center, 5, paint);

    final hourAngle = ((now.hour % 12) * 30 + now.minute * 0.5 - 90) * pi / 180;
    final minuteAngle = (now.minute * 6 - 90) * pi / 180;

    paint
      ..style = PaintingStyle.stroke
      ..strokeWidth = 3
      ..strokeCap = StrokeCap.round;
    canvas.drawLine(center,
        Offset(center.dx + radius * 0.5 * cos(hourAngle), center.dy + radius * 0.5 * sin(hourAngle)), paint);
    paint.strokeWidth = 2;
    canvas.drawLine(center,
        Offset(center.dx + radius * 0.7 * cos(minuteAngle), center.dy + radius * 0.7 * sin(minuteAngle)), paint);
  }

  void _drawText(Canvas canvas, Offset pos, String text, double fontSize, Paint paint) {
    final tp = TextPainter(
      text: TextSpan(text: text, style: TextStyle(color: paint.color, fontSize: fontSize)),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, Offset(pos.dx - tp.width / 2, pos.dy - tp.height / 2));
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
  bool shouldRepaint(covariant WatchfacePainter old) => true;
}

class _BottomBar extends StatelessWidget {
  final WatchfaceLayout layout;
  const _BottomBar({required this.layout});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: const BoxDecoration(color: AppColors.surface),
      child: Row(
        children: [
          Expanded(
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('${layout.widgets.length} widgets',
                    style: const TextStyle(color: AppColors.text, fontSize: 14)),
                const Text('Tap to select, drag to move',
                    style: TextStyle(color: AppColors.textDim, fontSize: 12)),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
