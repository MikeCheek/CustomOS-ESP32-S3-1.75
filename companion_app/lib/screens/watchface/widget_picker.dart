import 'package:flutter/material.dart';
import '../../services/ble_protocol.dart';
import '../../theme/app_theme.dart';

class WidgetPicker extends StatelessWidget {
  final ValueChanged<WatchfaceWidget> onAdd;

  const WidgetPicker({super.key, required this.onAdd});

  @override
  Widget build(BuildContext context) {
    final items = [
      _Item('analog_clock', 'Analog Clock', Icons.access_time, AppColors.accent),
      _Item('digital_time', 'Digital Time', Icons.access_time_filled, AppColors.accent),
      _Item('date', 'Date', Icons.calendar_today, AppColors.accent2),
      _Item('battery', 'Battery', Icons.battery_full, AppColors.accent2),
      _Item('steps', 'Steps', Icons.directions_walk, AppColors.success),
      _Item('weather', 'Weather', Icons.wb_sunny, AppColors.warning),
      _Item('heart_rate', 'Heart Rate', Icons.favorite, AppColors.danger),
      _Item('text', 'Custom Text', Icons.text_fields, AppColors.textDim),
      _Item('seconds_ring', 'Seconds Ring', Icons.watch, AppColors.accent3),
    ];

    return Container(
      padding: const EdgeInsets.all(16),
      decoration: const BoxDecoration(
        color: AppColors.surface,
        borderRadius: BorderRadius.vertical(top: Radius.circular(16)),
      ),
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
            height: 240,
            child: GridView.builder(
              gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 3, mainAxisSpacing: 10, crossAxisSpacing: 10,
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
                      onAdd(WatchfaceWidget(type: item.type, x: 233, y: 233));
                      Navigator.pop(ctx);
                    },
                    child: Column(
                      mainAxisAlignment: MainAxisAlignment.center,
                      children: [
                        Icon(item.icon, color: item.color, size: 24),
                        const SizedBox(height: 6),
                        Text(
                          item.label,
                          style: const TextStyle(color: AppColors.textDim, fontSize: 10),
                          textAlign: TextAlign.center,
                        ),
                      ],
                    ),
                  ),
                );
              },
            ),
          ),
        ],
      ),
    );
  }
}

class _Item {
  final String type;
  final String label;
  final IconData icon;
  final Color color;
  _Item(this.type, this.label, this.icon, this.color);
}
