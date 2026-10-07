import 'package:flutter/material.dart';
import 'package:flutter_colorpicker/flutter_colorpicker.dart';
import '../../services/ble_protocol.dart';
import '../../theme/app_theme.dart';

class PropertyPanel extends StatefulWidget {
  final WatchfaceWidget widget;
  final ValueChanged<WatchfaceWidget> onUpdate;
  final VoidCallback onDelete;

  const PropertyPanel({
    super.key,
    required this.widget,
    required this.onUpdate,
    required this.onDelete,
  });

  @override
  State<PropertyPanel> createState() => _PropertyPanelState();
}

class _PropertyPanelState extends State<PropertyPanel> {
  double _x = 0;
  double _y = 0;
  double _radius = 50;
  double _fontSize = 14;
  String _color = '#FFFFFF';
  final TextEditingController _textController = TextEditingController();

  void _syncFromWidget() {
    _x = widget.widget.x;
    _y = widget.widget.y;
    _radius = widget.widget.radius;
    _fontSize = widget.widget.fontSize;
    _color = widget.widget.color;
    if (_textController.text != widget.widget.text) {
      _textController.text = widget.widget.text;
    }
  }

  @override
  void initState() {
    super.initState();
    _syncFromWidget();
  }

  @override
  void didUpdateWidget(covariant PropertyPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    _syncFromWidget();
  }

  void _update() {
    widget.onUpdate(WatchfaceWidget(
      type: widget.widget.type,
      x: _x,
      y: _y,
      radius: _radius,
      fontSize: _fontSize,
      color: _color,
      font: widget.widget.font,
      text: _textController.text,
      dataSrc: widget.widget.dataSrc,
    ));
  }

  @override
  Widget build(BuildContext context) {
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
          const SizedBox(height: 12),
          Row(
            children: [
              Text(
                widget.widget.type.replaceAll('_', ' ').toUpperCase(),
                style: const TextStyle(
                  color: AppColors.text,
                  fontSize: 14,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const Spacer(),
              IconButton(
                icon: const Icon(Icons.delete_outline, color: AppColors.danger, size: 20),
                onPressed: widget.onDelete,
              ),
            ],
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(
                child: _SliderRow(
                  label: 'X',
                  value: _x,
                  min: 0,
                  max: 466,
                  onChanged: (v) => setState(() { _x = v; _update(); }),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: _SliderRow(
                  label: 'Y',
                  value: _y,
                  min: 0,
                  max: 466,
                  onChanged: (v) => setState(() { _y = v; _update(); }),
                ),
              ),
            ],
          ),
          const SizedBox(height: 8),
          if (widget.widget.type == 'analog_clock' || widget.widget.type == 'seconds_ring')
            _SliderRow(
              label: 'Radius',
              value: _radius,
              min: 20,
              max: 230,
              onChanged: (v) => setState(() { _radius = v; _update(); }),
            )
          else
            _SliderRow(
              label: 'Font Size',
              value: _fontSize,
              min: 8,
              max: 72,
              onChanged: (v) => setState(() { _fontSize = v; _update(); }),
            ),
          const SizedBox(height: 8),
          Row(
            children: [
              const Text('Color', style: TextStyle(color: AppColors.textDim, fontSize: 12)),
              const Spacer(),
              GestureDetector(
                onTap: () => _showColorPicker(context),
                child: Container(
                  width: 32,
                  height: 32,
                  decoration: BoxDecoration(
                    color: _hexToColor(_color),
                    shape: BoxShape.circle,
                    border: Border.all(color: AppColors.surfaceLight, width: 2),
                  ),
                ),
              ),
            ],
          ),
          if (widget.widget.type == 'text') ...[
            const SizedBox(height: 8),
            TextField(
              controller: _textController,
              style: const TextStyle(color: AppColors.text),
              decoration: const InputDecoration(
                hintText: 'Custom text',
                isDense: true,
              ),
              onChanged: (_) => _update(),
            ),
          ],
        ],
      ),
    );
  }

  void _showColorPicker(BuildContext context) {
    Color currentColor = _hexToColor(_color);
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: AppColors.surface,
        title: const Text('Pick Color', style: TextStyle(color: AppColors.text)),
        content: SingleChildScrollView(
          child: ColorPicker(
            pickerColor: currentColor,
            onColorChanged: (color) {
              currentColor = color;
            },
            enableAlpha: false,
            pickerAreaHeightPercent: 0.8,
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () {
              setState(() {
                _color = '#${currentColor.value.toRadixString(16).substring(2).toUpperCase()}';
              });
              _update();
              Navigator.pop(ctx);
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
  }

  Color _hexToColor(String hex) {
    hex = hex.replaceAll('#', '');
    if (hex.length == 6) hex = 'FF$hex';
    return Color(int.parse(hex, radix: 16));
  }
}

class _SliderRow extends StatelessWidget {
  final String label;
  final double value;
  final double min;
  final double max;
  final ValueChanged<double> onChanged;

  const _SliderRow({
    required this.label,
    required this.value,
    required this.min,
    required this.max,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        SizedBox(
          width: 30,
          child: Text(label, style: const TextStyle(color: AppColors.textDim, fontSize: 11)),
        ),
        Expanded(
          child: Slider(
            value: value.clamp(min, max),
            min: min,
            max: max,
            onChanged: onChanged,
          ),
        ),
        SizedBox(
          width: 40,
          child: Text(
            value.round().toString(),
            style: const TextStyle(color: AppColors.textDim, fontSize: 11),
            textAlign: TextAlign.right,
          ),
        ),
      ],
    );
  }
}
