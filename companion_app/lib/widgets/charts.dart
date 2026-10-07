import 'dart:math' as math;
import 'package:flutter/material.dart';
import '../theme/app_theme.dart';

/// Small chart widgets drawn with CustomPaint (no chart package).

const chartPalette = [
  AppColors.accent,
  AppColors.accent2,
  AppColors.accent3,
  AppColors.success,
  AppColors.warning,
  Color(0xFFB18CFF),
  Color(0xFF7FE0C8),
  Color(0xFFFF8F6B),
];

TextPainter _tp(String s, double size, Color c, {FontWeight w = FontWeight.w500}) =>
    TextPainter(text: TextSpan(text: s, style: TextStyle(color: c, fontSize: size, fontWeight: w)),
        textDirection: TextDirection.ltr)
      ..layout();

/// Vertical bars with labels under them; the last bar can be highlighted.
class BarChart extends StatelessWidget {
  final List<num> values;
  final List<String> labels;   // same length; '' to skip a label
  final Color color;
  final bool highlightLast;
  final double height;
  const BarChart({super.key, required this.values, required this.labels, this.color = AppColors.accent,
      this.highlightLast = true, this.height = 140});

  @override
  Widget build(BuildContext context) => SizedBox(
        height: height,
        width: double.infinity,
        child: CustomPaint(painter: _BarPainter(values, labels, color, highlightLast)),
      );
}

class _BarPainter extends CustomPainter {
  final List<num> values;
  final List<String> labels;
  final Color color;
  final bool highlightLast;
  _BarPainter(this.values, this.labels, this.color, this.highlightLast);

  @override
  void paint(Canvas canvas, Size size) {
    if (values.isEmpty) return;
    const labelH = 18.0, topPad = 16.0;
    final maxV = values.fold<num>(0, (a, b) => math.max(a, b)).toDouble();
    final chartH = size.height - labelH - topPad;
    final slot = size.width / values.length;
    final barW = math.min(26.0, slot * 0.62);
    // baseline
    canvas.drawLine(Offset(0, topPad + chartH), Offset(size.width, topPad + chartH),
        Paint()..color = AppColors.border..strokeWidth = 1);
    for (var i = 0; i < values.length; i++) {
      final v = values[i].toDouble();
      final h = maxV > 0 ? chartH * v / maxV : 0.0;
      final x = slot * i + (slot - barW) / 2;
      final last = highlightLast && i == values.length - 1;
      final paint = Paint()..color = (last ? color : color.withValues(alpha: 0.45));
      if (h > 0) {
        canvas.drawRRect(
            RRect.fromRectAndCorners(Rect.fromLTWH(x, topPad + chartH - math.max(h, 2.0), barW, math.max(h, 2.0)),
                topLeft: const Radius.circular(5), topRight: const Radius.circular(5)),
            paint);
      }
      if (v > 0 && (last || v == maxV)) {
        final t = _tp(v % 1 == 0 ? v.toInt().toString() : v.toStringAsFixed(1), 10, AppColors.text, w: FontWeight.w700);
        t.paint(canvas, Offset(x + barW / 2 - t.width / 2, topPad + chartH - h - t.height - 1));
      }
      if (i < labels.length && labels[i].isNotEmpty) {
        final t = _tp(labels[i], 10, AppColors.textDim);
        t.paint(canvas,
            Offset((x + barW / 2 - t.width / 2).clamp(0.0, math.max(0.0, size.width - t.width)), size.height - labelH + 4));
      }
    }
  }

  @override
  bool shouldRepaint(_BarPainter o) => o.values != values || o.labels != labels || o.color != color;
}

/// Ranked rows: label, proportional bar, value. Tappable.
class HBarList extends StatelessWidget {
  final List<(String, num)> items;
  final Color color;
  final String Function(num)? format;
  final void Function(int index)? onTap;
  const HBarList({super.key, required this.items, this.color = AppColors.accent2, this.format, this.onTap});

  @override
  Widget build(BuildContext context) {
    final maxV = items.fold<num>(0, (a, b) => math.max(a, b.$2)).toDouble();
    return Column(
      children: [
        for (var i = 0; i < items.length; i++)
          InkWell(
            onTap: onTap == null ? null : () => onTap!(i),
            borderRadius: BorderRadius.circular(8),
            child: Padding(
              padding: const EdgeInsets.symmetric(vertical: 5),
              child: Row(
                children: [
                  SizedBox(
                    width: 110,
                    child: Text(items[i].$1, maxLines: 1, overflow: TextOverflow.ellipsis,
                        style: const TextStyle(color: AppColors.text, fontSize: 13)),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: LayoutBuilder(
                      builder: (_, c) => Align(
                        alignment: Alignment.centerLeft,
                        child: Container(
                          height: 10,
                          width: maxV > 0 ? math.max(4.0, c.maxWidth * items[i].$2 / maxV) : 4.0,
                          decoration: BoxDecoration(
                            gradient: LinearGradient(colors: [color.withValues(alpha: 0.55), color]),
                            borderRadius: BorderRadius.circular(5),
                          ),
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),
                  SizedBox(
                    width: 34,
                    child: Text(format?.call(items[i].$2) ?? '${items[i].$2}', textAlign: TextAlign.right,
                        style: const TextStyle(color: AppColors.textDim, fontSize: 12, fontWeight: FontWeight.w600)),
                  ),
                ],
              ),
            ),
          ),
      ],
    );
  }
}

/// Several series over the same x axis (e.g. topics per week), with a legend.
class LineChart extends StatelessWidget {
  final Map<String, List<num>> series;
  final List<String> xLabels;
  final double height;
  const LineChart({super.key, required this.series, required this.xLabels, this.height = 150});

  @override
  Widget build(BuildContext context) {
    final names = series.keys.toList();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(height: height, width: double.infinity, child: CustomPaint(painter: _LinePainter(series, xLabels))),
        const SizedBox(height: 10),
        Wrap(
          spacing: 12,
          runSpacing: 6,
          children: [
            for (var i = 0; i < names.length; i++)
              Row(mainAxisSize: MainAxisSize.min, children: [
                Container(width: 10, height: 3, color: chartPalette[i % chartPalette.length]),
                const SizedBox(width: 5),
                Text(names[i], style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
              ]),
          ],
        ),
      ],
    );
  }
}

class _LinePainter extends CustomPainter {
  final Map<String, List<num>> series;
  final List<String> xLabels;
  _LinePainter(this.series, this.xLabels);

  @override
  void paint(Canvas canvas, Size size) {
    const labelH = 16.0, top = 6.0;
    final h = size.height - labelH - top;
    final n = series.values.fold<int>(0, (a, s) => math.max(a, s.length));
    if (n < 2) return;
    final maxV = math.max(1.0, series.values.expand((s) => s).fold<num>(0, (a, b) => math.max(a, b)).toDouble());
    final dx = size.width / (n - 1);
    final grid = Paint()..color = AppColors.border..strokeWidth = 1;
    for (var g = 0; g <= 2; g++) {
      final y = top + h * g / 2;
      canvas.drawLine(Offset(0, y), Offset(size.width, y), grid);
    }
    var si = 0;
    for (final s in series.values) {
      final c = chartPalette[si++ % chartPalette.length];
      if (s.isEmpty) continue;
      final path = Path();
      for (var i = 0; i < s.length; i++) {
        final p = Offset(dx * i, top + h - h * s[i] / maxV);
        if (i == 0) {
          path.moveTo(p.dx, p.dy);
        } else {
          // smooth: horizontal-tangent cubic between points
          final prev = Offset(dx * (i - 1), top + h - h * s[i - 1] / maxV);
          path.cubicTo(prev.dx + dx / 2, prev.dy, p.dx - dx / 2, p.dy, p.dx, p.dy);
        }
      }
      canvas.drawPath(
          path,
          Paint()
            ..color = c
            ..style = PaintingStyle.stroke
            ..strokeWidth = 2.2
            ..strokeCap = StrokeCap.round);
      final lastY = top + h - h * s.last / maxV;
      canvas.drawCircle(Offset(dx * (s.length - 1), lastY), 3, Paint()..color = c);
    }
    for (var i = 0; i < xLabels.length && i < n; i++) {
      if (xLabels[i].isEmpty) continue;
      final t = _tp(xLabels[i], 10, AppColors.textDim);
      t.paint(canvas, Offset((dx * i - t.width / 2).clamp(0.0, math.max(0.0, size.width - t.width)), size.height - labelH + 3));
    }
    final mt = _tp(maxV.round().toString(), 9, AppColors.textFaint);
    mt.paint(canvas, Offset(0, top));
  }

  @override
  bool shouldRepaint(_LinePainter o) => o.series != series;
}

/// Weekday x time-of-day grid: when memos get recorded.
class Heatmap extends StatelessWidget {
  final List<List<int>> grid;     // [7 rows Mon..Sun][columns]
  final List<String> rowLabels;
  final List<String> colLabels;
  final Color color;
  const Heatmap({super.key, required this.grid, required this.rowLabels, required this.colLabels,
      this.color = AppColors.accent});

  @override
  Widget build(BuildContext context) {
    final maxV = grid.expand((r) => r).fold<int>(0, math.max);
    return Column(
      children: [
        for (var r = 0; r < grid.length; r++)
          Padding(
            padding: const EdgeInsets.only(bottom: 4),
            child: Row(
              children: [
                SizedBox(width: 30, child: Text(rowLabels[r], style: const TextStyle(color: AppColors.textDim, fontSize: 11))),
                for (var c = 0; c < grid[r].length; c++)
                  Expanded(
                    child: Tooltip(
                      message: '${grid[r][c]} memo${grid[r][c] == 1 ? '' : 's'}',
                      child: Container(
                        height: 20,
                        margin: const EdgeInsets.symmetric(horizontal: 2),
                        decoration: BoxDecoration(
                          color: grid[r][c] == 0
                              ? AppColors.surfaceLight
                              : color.withValues(alpha: 0.2 + 0.8 * grid[r][c] / math.max(1, maxV)),
                          borderRadius: BorderRadius.circular(5),
                        ),
                      ),
                    ),
                  ),
              ],
            ),
          ),
        Row(
          children: [
            const SizedBox(width: 30),
            for (final l in colLabels)
              Expanded(child: Text(l, textAlign: TextAlign.center, style: const TextStyle(color: AppColors.textDim, fontSize: 10))),
          ],
        ),
      ],
    );
  }
}

/// Ring split into segments with a legend beside it.
class DonutChart extends StatelessWidget {
  final List<(String, num, Color)> segments;
  final String? centerTop, centerBottom;
  final double size;
  const DonutChart({super.key, required this.segments, this.centerTop, this.centerBottom, this.size = 120});

  @override
  Widget build(BuildContext context) {
    final total = segments.fold<num>(0, (a, s) => a + s.$2);
    return Row(
      children: [
        SizedBox(
          width: size,
          height: size,
          child: CustomPaint(
            painter: _DonutPainter(segments),
            child: Center(
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                if (centerTop != null)
                  Text(centerTop!, style: const TextStyle(color: AppColors.text, fontSize: 20, fontWeight: FontWeight.w700)),
                if (centerBottom != null)
                  Text(centerBottom!, style: const TextStyle(color: AppColors.textDim, fontSize: 11)),
              ]),
            ),
          ),
        ),
        const SizedBox(width: 18),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              for (final s in segments)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 3),
                  child: Row(children: [
                    Container(width: 10, height: 10, decoration: BoxDecoration(color: s.$3, shape: BoxShape.circle)),
                    const SizedBox(width: 8),
                    Expanded(child: Text(s.$1, style: const TextStyle(color: AppColors.text, fontSize: 13))),
                    Text(total > 0 ? '${(100 * s.$2 / total).round()}%' : '–',
                        style: const TextStyle(color: AppColors.textDim, fontSize: 12)),
                  ]),
                ),
            ],
          ),
        ),
      ],
    );
  }
}

class _DonutPainter extends CustomPainter {
  final List<(String, num, Color)> segments;
  _DonutPainter(this.segments);

  @override
  void paint(Canvas canvas, Size size) {
    const stroke = 14.0;
    final c = size.center(Offset.zero);
    final r = (math.min(size.width, size.height) - stroke) / 2;
    final rect = Rect.fromCircle(center: c, radius: r);
    final total = segments.fold<num>(0, (a, s) => a + s.$2).toDouble();
    if (total <= 0) {
      canvas.drawCircle(c, r, Paint()..color = AppColors.surfaceHigh..style = PaintingStyle.stroke..strokeWidth = stroke);
      return;
    }
    var start = -math.pi / 2;
    final gap = segments.where((s) => s.$2 > 0).length > 1 ? 0.04 : 0.0;
    for (final s in segments) {
      if (s.$2 <= 0) continue;
      final sweep = 2 * math.pi * s.$2 / total;
      canvas.drawArc(rect, start + gap / 2, math.max(0.001, sweep - gap), false,
          Paint()..color = s.$3..style = PaintingStyle.stroke..strokeWidth = stroke..strokeCap = StrokeCap.butt);
      start += sweep;
    }
  }

  @override
  bool shouldRepaint(_DonutPainter o) => o.segments != segments;
}

/// Thin 0..1 relevance bar with a percentage.
class ScoreBar extends StatelessWidget {
  final double score;
  const ScoreBar(this.score, {super.key});

  Color get _color => score >= 0.7 ? AppColors.success : score >= 0.4 ? AppColors.accent2 : AppColors.warning;

  @override
  Widget build(BuildContext context) => Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          SizedBox(
            width: 54,
            child: ClipRRect(
              borderRadius: BorderRadius.circular(3),
              child: LinearProgressIndicator(
                  value: score, minHeight: 5, color: _color, backgroundColor: AppColors.surfaceHigh),
            ),
          ),
          const SizedBox(width: 6),
          Text('${(score * 100).round()}%',
              style: TextStyle(color: _color, fontSize: 12, fontWeight: FontWeight.w700)),
        ],
      );
}

/// [text] with the character ranges in [hits] highlighted.
class HighlightText extends StatelessWidget {
  final String text;
  final List<(int, int)> hits;
  final int maxLines;
  const HighlightText(this.text, this.hits, {super.key, this.maxLines = 4});

  @override
  Widget build(BuildContext context) {
    const base = TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4);
    final spans = <TextSpan>[];
    var pos = 0;
    final sorted = [...hits]..sort((a, b) => a.$1.compareTo(b.$1));
    for (final h in sorted) {
      if (h.$1 < pos || h.$2 > text.length) continue;
      if (h.$1 > pos) spans.add(TextSpan(text: text.substring(pos, h.$1)));
      spans.add(TextSpan(
          text: text.substring(h.$1, h.$2),
          style: TextStyle(
              color: AppColors.text, fontWeight: FontWeight.w700, backgroundColor: AppColors.accent.withValues(alpha: 0.28))));
      pos = h.$2;
    }
    if (pos < text.length) spans.add(TextSpan(text: text.substring(pos)));
    return Text.rich(TextSpan(style: base, children: spans), maxLines: maxLines, overflow: TextOverflow.ellipsis);
  }
}
