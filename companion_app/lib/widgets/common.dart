import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../theme/app_theme.dart';

/// Rounded surface used for every grouped block in the app.
class Panel extends StatelessWidget {
  final Widget child;
  final EdgeInsetsGeometry padding;
  final VoidCallback? onTap;
  final Color? color;
  final Gradient? gradient;

  const Panel({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(18),
    this.onTap,
    this.color,
    this.gradient,
  });

  @override
  Widget build(BuildContext context) {
    final radius = BorderRadius.circular(22);
    return Material(
      color: Colors.transparent,
      child: Ink(
        decoration: BoxDecoration(
          color: gradient == null ? (color ?? AppColors.surface) : null,
          gradient: gradient,
          borderRadius: radius,
          border: Border.all(color: AppColors.border),
        ),
        child: InkWell(
          borderRadius: radius,
          onTap: onTap,
          child: Padding(padding: padding, child: child),
        ),
      ),
    );
  }
}

class SectionLabel extends StatelessWidget {
  final String text;
  final Widget? trailing;
  const SectionLabel(this.text, {super.key, this.trailing});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(4, 22, 4, 10),
      child: Row(
        children: [
          Expanded(
            child: Text(
              text.toUpperCase(),
              style: const TextStyle(
                color: AppColors.textDim,
                fontSize: 12,
                fontWeight: FontWeight.w600,
                letterSpacing: 1.1,
              ),
            ),
          ),
          if (trailing != null) trailing!,
        ],
      ),
    );
  }
}

class IconChip extends StatelessWidget {
  final IconData icon;
  final Color color;
  final double size;
  const IconChip(this.icon, {super.key, this.color = AppColors.accent, this.size = 40});

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.16),
        borderRadius: BorderRadius.circular(size * 0.32),
      ),
      child: Icon(icon, color: color, size: size * 0.52),
    );
  }
}

/// Icon + title + subtitle row that navigates somewhere.
class NavRow extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String title;
  final String? subtitle;
  final VoidCallback? onTap;
  final Widget? trailing;

  const NavRow({
    super.key,
    required this.icon,
    required this.title,
    this.subtitle,
    this.onTap,
    this.color = AppColors.accent,
    this.trailing,
  });

  @override
  Widget build(BuildContext context) {
    final enabled = onTap != null;
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(16),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
        child: Opacity(
          opacity: enabled ? 1 : 0.45,
          child: Row(
            children: [
              IconChip(icon, color: color),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(title, style: const TextStyle(color: AppColors.text, fontSize: 15, fontWeight: FontWeight.w600)),
                    if (subtitle != null) ...[
                      const SizedBox(height: 2),
                      Text(subtitle!, style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
                    ],
                  ],
                ),
              ),
              trailing ?? const Icon(Icons.chevron_right_rounded, color: AppColors.textFaint),
            ],
          ),
        ),
      ),
    );
  }
}

/// Icon + title + subtitle row with a switch.
class ToggleRow extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String title;
  final String? subtitle;
  final bool value;
  final ValueChanged<bool>? onChanged;

  const ToggleRow({
    super.key,
    required this.icon,
    required this.title,
    required this.value,
    required this.onChanged,
    this.subtitle,
    this.color = AppColors.accent,
  });

  @override
  Widget build(BuildContext context) {
    return NavRow(
      icon: icon,
      color: color,
      title: title,
      subtitle: subtitle,
      onTap: onChanged == null ? null : () => onChanged!(!value),
      trailing: Switch(value: value, onChanged: onChanged),
    );
  }
}

/// A Panel holding rows separated by thin dividers.
class RowGroup extends StatelessWidget {
  final List<Widget> children;
  const RowGroup({super.key, required this.children});

  @override
  Widget build(BuildContext context) {
    final items = <Widget>[];
    for (var i = 0; i < children.length; i++) {
      if (i > 0) items.add(const Divider(indent: 68, endIndent: 14));
      items.add(children[i]);
    }
    return Panel(padding: const EdgeInsets.symmetric(vertical: 4), child: Column(children: items));
  }
}

/// Small metric tile: icon, big value, label.
class StatTile extends StatelessWidget {
  final IconData icon;
  final Color color;
  final String value;
  final String label;
  final VoidCallback? onTap;

  const StatTile({
    super.key,
    required this.icon,
    required this.value,
    required this.label,
    this.color = AppColors.accent,
    this.onTap,
  });

  @override
  Widget build(BuildContext context) {
    return Panel(
      onTap: onTap,
      padding: const EdgeInsets.all(14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Icon(icon, color: color, size: 22),
          const SizedBox(height: 14),
          FittedBox(
            fit: BoxFit.scaleDown,
            alignment: Alignment.centerLeft,
            child: Text(
              value,
              style: const TextStyle(
                color: AppColors.text,
                fontSize: 22,
                fontWeight: FontWeight.w700,
                fontFeatures: [FontFeature.tabularFigures()],
              ),
            ),
          ),
          const SizedBox(height: 2),
          Text(label, maxLines: 1, overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
        ],
      ),
    );
  }
}

class StatusPill extends StatelessWidget {
  final String text;
  final Color color;
  final bool pulse;
  const StatusPill(this.text, {super.key, this.color = AppColors.success, this.pulse = false});

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.14),
        borderRadius: BorderRadius.circular(20),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(width: 7, height: 7, decoration: BoxDecoration(color: color, shape: BoxShape.circle)),
          const SizedBox(width: 6),
          Text(text, style: TextStyle(color: color, fontSize: 12, fontWeight: FontWeight.w600)),
        ],
      ),
    );
  }
}

class EmptyState extends StatelessWidget {
  final IconData icon;
  final String title;
  final String subtitle;
  final Widget? action;
  const EmptyState({super.key, required this.icon, required this.title, required this.subtitle, this.action});

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Container(
              width: 76,
              height: 76,
              decoration: const BoxDecoration(color: AppColors.surfaceLight, shape: BoxShape.circle),
              child: Icon(icon, color: AppColors.textDim, size: 34),
            ),
            const SizedBox(height: 18),
            Text(title, textAlign: TextAlign.center,
                style: const TextStyle(color: AppColors.text, fontSize: 18, fontWeight: FontWeight.w600)),
            const SizedBox(height: 6),
            Text(subtitle, textAlign: TextAlign.center,
                style: const TextStyle(color: AppColors.textDim, fontSize: 14, height: 1.4)),
            if (action != null) ...[const SizedBox(height: 20), action!],
          ],
        ),
      ),
    );
  }
}

/// A round progress ring (0..1) with content in the middle.
class Ring extends StatelessWidget {
  final double value;
  final double size;
  final double stroke;
  final Color color;
  final Color track;
  final Widget? child;

  const Ring({
    super.key,
    required this.value,
    this.size = 120,
    this.stroke = 10,
    this.color = AppColors.accent,
    this.track = AppColors.surfaceHigh,
    this.child,
  });

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: size,
      height: size,
      child: CustomPaint(
        painter: _RingPainter(value.clamp(0.0, 1.0), stroke, color, track),
        child: Center(child: child),
      ),
    );
  }
}

class _RingPainter extends CustomPainter {
  final double value, stroke;
  final Color color, track;
  _RingPainter(this.value, this.stroke, this.color, this.track);

  @override
  void paint(Canvas canvas, Size size) {
    final c = size.center(Offset.zero);
    final r = (math.min(size.width, size.height) - stroke) / 2;
    final bg = Paint()
      ..color = track
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke;
    canvas.drawCircle(c, r, bg);
    if (value <= 0) return;
    final fg = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round;
    canvas.drawArc(Rect.fromCircle(center: c, radius: r), -math.pi / 2, 2 * math.pi * value, false, fg);
  }

  @override
  bool shouldRepaint(_RingPainter old) =>
      old.value != value || old.color != color || old.stroke != stroke || old.track != track;
}

Color batteryColor(int level) {
  if (level < 0) return AppColors.textDim;
  if (level <= 15) return AppColors.danger;
  if (level <= 35) return AppColors.warning;
  return AppColors.success;
}

void showSnack(BuildContext context, String text) {
  ScaffoldMessenger.of(context)
    ..hideCurrentSnackBar()
    ..showSnackBar(SnackBar(content: Text(text)));
}
