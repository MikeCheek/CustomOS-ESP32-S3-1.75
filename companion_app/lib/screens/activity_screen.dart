import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/fitness_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// The watch's activity rings (Move, Exercise, Stand), steps, the last week
/// and its workouts.
class ActivityScreen extends ConsumerStatefulWidget {
  const ActivityScreen({super.key});

  @override
  ConsumerState<ActivityScreen> createState() => _ActivityScreenState();
}

class _ActivityScreenState extends ConsumerState<ActivityScreen> {
  int _goal = 8000;

  @override
  void initState() {
    super.initState();
    SharedPreferences.getInstance().then((p) {
      if (mounted) setState(() => _goal = p.getInt('step_goal') ?? 8000);
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (ref.read(bleProvider).isConnected) ref.read(companionProvider.notifier).requestWatchReport();
    });
  }

  Future<void> _editGoal() async {
    final picked = await showModalBottomSheet<int>(
      context: context,
      builder: (ctx) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Padding(
              padding: EdgeInsets.only(bottom: 8),
              child: Text(
                'Daily goal',
                style: TextStyle(color: AppColors.text, fontSize: 18, fontWeight: FontWeight.w600),
              ),
            ),
            for (final g in const [5000, 6000, 8000, 10000, 12000, 15000])
              ListTile(
                title: Text('$g steps'),
                trailing: g == _goal ? const Icon(Icons.check_rounded, color: AppColors.accent) : null,
                onTap: () => Navigator.pop(ctx, g),
              ),
            const SizedBox(height: 8),
          ],
        ),
      ),
    );
    if (picked == null) return;
    final p = await SharedPreferences.getInstance();
    await p.setInt('step_goal', picked);
    ref.read(companionProvider.notifier).setStepGoal(picked);
    if (mounted) setState(() => _goal = picked);
  }

  @override
  Widget build(BuildContext context) {
    final link = ref.watch(companionProvider);
    final connected = ref.watch(bleProvider).isConnected;
    final steps = link.watchSteps;
    final today = DateTime.now();
    final days = List.generate(7, (i) => today.subtract(Duration(days: 6 - i)));
    String key(DateTime d) => '${d.year}-${d.month.toString().padLeft(2, '0')}-${d.day.toString().padLeft(2, '0')}';
    final values = days.map((d) => link.stepsHistory[key(d)] ?? 0).toList();
    if (steps >= 0) values[6] = math.max(values[6], steps);
    final todaySteps = values[6];
    final km = todaySteps * 0.00075;
    final fit = ref.watch(fitnessProvider);

    return Scaffold(
      appBar: AppBar(
        title: const Text('Activity'),
        actions: [
          IconButton(
            tooltip: 'Refresh from watch',
            onPressed: connected ? () => ref.read(companionProvider.notifier).requestWatchReport() : null,
            icon: const Icon(Icons.refresh_rounded),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          if (fit.today != null) ...[
            Panel(
              padding: const EdgeInsets.fromLTRB(18, 22, 18, 18),
              child: Row(
                children: [
                  RingsView(
                    move: fit.today!.moveKcal / fit.moveGoal,
                    exercise: fit.today!.exerciseMin / fit.exerciseGoal,
                    stand: fit.today!.standHours / fit.standGoal,
                    size: 150,
                  ),
                  const SizedBox(width: 22),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        _RingStat('Move', '${fit.today!.moveKcal}/${fit.moveGoal}', 'kcal', ringMove),
                        const SizedBox(height: 12),
                        _RingStat('Exercise', '${fit.today!.exerciseMin}/${fit.exerciseGoal}', 'min', ringExercise),
                        const SizedBox(height: 12),
                        _RingStat('Stand', '${fit.today!.standHours}/${fit.standGoal}', 'hrs', ringStand),
                      ],
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 6),
            Text(
              link.watchReportAt == null
                  ? 'From the watch'
                  : 'Synced ${TimeOfDay.fromDateTime(link.watchReportAt!).format(context)} - goals are set on the watch '
                        '(Settings > Activity)',
              style: const TextStyle(color: AppColors.textFaint, fontSize: 12),
            ),
            const SizedBox(height: 10),
          ] else
            Panel(
              padding: const EdgeInsets.symmetric(vertical: 28),
              child: Column(
                children: [
                  Ring(
                    value: todaySteps / _goal,
                    size: 200,
                    stroke: 16,
                    color: AppColors.success,
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Text(
                          '$todaySteps',
                          style: const TextStyle(
                            color: AppColors.text,
                            fontSize: 40,
                            fontWeight: FontWeight.w700,
                            fontFeatures: [FontFeature.tabularFigures()],
                          ),
                        ),
                        GestureDetector(
                          onTap: _editGoal,
                          child: Text(
                            'of $_goal steps',
                            style: const TextStyle(
                              color: AppColors.textDim,
                              fontSize: 13,
                              decoration: TextDecoration.underline,
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(height: 18),
                  Text(
                    link.watchReportAt == null
                        ? (connected ? 'Waiting for the watch…' : 'Connect the watch to sync steps')
                        : 'Synced ${TimeOfDay.fromDateTime(link.watchReportAt!).format(context)}',
                    style: const TextStyle(color: AppColors.textDim, fontSize: 13),
                  ),
                ],
              ),
            ),
          const SizedBox(height: 10),
          Row(
            children: [
              Expanded(
                child: StatTile(
                  icon: Icons.directions_walk_rounded,
                  color: AppColors.success,
                  value: '$todaySteps',
                  label: 'steps',
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: StatTile(
                  icon: Icons.straighten_rounded,
                  color: AppColors.accent2,
                  value: km.toStringAsFixed(1),
                  label: 'km (est.)',
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: StatTile(
                  icon: link.watchCharging ? Icons.battery_charging_full_rounded : Icons.watch_outlined,
                  color: batteryColor(link.watchBattery),
                  value: link.watchBattery >= 0 ? '${link.watchBattery}%' : '--',
                  label: 'Watch',
                ),
              ),
            ],
          ),
          const SectionLabel('Last 7 days'),
          Panel(
            padding: const EdgeInsets.fromLTRB(14, 18, 14, 12),
            child: fit.today != null
                ? _WeekRings(fit: fit, days: days)
                : SizedBox(
                    height: 160,
                    child: _WeekBars(values: values, days: days, goal: _goal),
                  ),
          ),
          SectionLabel(
            'Workouts',
            trailing: fit.workouts.isEmpty
                ? null
                : Text('${fit.workouts.length}', style: const TextStyle(color: AppColors.textDim)),
          ),
          if (fit.workouts.isEmpty)
            const Panel(
              child: Text(
                'Start one in the watch\'s Workout app - or keep walking briskly or running for 10 minutes and '
                'the watch offers to record it. They show up here.',
                style: TextStyle(color: AppColors.textDim, fontSize: 13, height: 1.4),
              ),
            )
          else
            RowGroup(children: [for (final w in fit.workouts.take(30)) _WorkoutRow(w)]),
          const SectionLabel('Your body'),
          _BodyCard(fit: fit),
          const SizedBox(height: 12),
          const Text(
            'Move counts active calories, Exercise brisk walking, running and workouts, Stand the hours you moved for '
            'at least a minute - all worked out by the watch from its motion sensor (and GPS outdoors on the -G board). '
            'Distance from steps and calories are estimates.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}

const ringMove = Color(0xFFFA114F), ringExercise = Color(0xFF9BE82A), ringStand = Color(0xFF1EEAEF);

/// The three activity rings (fractions of each goal; past 1 a second lap).
class RingsView extends StatelessWidget {
  final double move, exercise, stand, size;
  const RingsView({super.key, required this.move, required this.exercise, required this.stand, this.size = 120});

  @override
  Widget build(BuildContext context) =>
      CustomPaint(size: Size.square(size), painter: _RingsPainter(move, exercise, stand));
}

class _RingsPainter extends CustomPainter {
  final double move, exercise, stand;
  _RingsPainter(this.move, this.exercise, this.stand);

  @override
  void paint(Canvas canvas, Size size) {
    final c = size.center(Offset.zero);
    final stroke = size.width * 0.13;
    final gap = stroke * 0.18;
    void ring(double r, double f, Color col) {
      final bg = Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = stroke
        ..color = col.withValues(alpha: 0.2);
      canvas.drawCircle(c, r, bg);
      if (f <= 0) return;
      final fg = Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = stroke
        ..strokeCap = StrokeCap.round
        ..color = col;
      final rect = Rect.fromCircle(center: c, radius: r);
      if (f >= 1) {
        canvas.drawCircle(
          c,
          r,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = stroke
            ..color = Color.lerp(col, Colors.black, 0.25)!,
        );
        final more = f - f.floorToDouble();
        if (more > 0.005) canvas.drawArc(rect, -math.pi / 2, 2 * math.pi * more, false, fg);
      } else {
        canvas.drawArc(rect, -math.pi / 2, 2 * math.pi * f, false, fg);
      }
    }

    final r0 = size.width / 2 - stroke / 2;
    ring(r0, move, ringMove);
    ring(r0 - stroke - gap, exercise, ringExercise);
    ring(r0 - 2 * (stroke + gap), stand, ringStand);
  }

  @override
  bool shouldRepaint(_RingsPainter o) => o.move != move || o.exercise != exercise || o.stand != stand;
}

class _RingStat extends StatelessWidget {
  final String label, value, unit;
  final Color color;
  const _RingStat(this.label, this.value, this.unit, this.color);

  @override
  Widget build(BuildContext context) => Column(
    crossAxisAlignment: CrossAxisAlignment.start,
    children: [
      Text(
        label,
        style: const TextStyle(color: AppColors.textDim, fontSize: 12, fontWeight: FontWeight.w600),
      ),
      Text.rich(
        TextSpan(
          children: [
            TextSpan(
              text: value,
              style: TextStyle(color: color, fontSize: 22, fontWeight: FontWeight.w700),
            ),
            TextSpan(
              text: ' $unit',
              style: TextStyle(color: color, fontSize: 13),
            ),
          ],
        ),
      ),
    ],
  );
}

class _WeekRings extends StatelessWidget {
  final FitnessState fit;
  final List<DateTime> days;
  const _WeekRings({required this.fit, required this.days});
  static const _names = ['M', 'T', 'W', 'T', 'F', 'S', 'S'];

  @override
  Widget build(BuildContext context) {
    final byDate = {for (final d in fit.days) d.date: d};
    return Row(
      children: [
        for (var i = 0; i < days.length; i++)
          Expanded(
            child: Column(
              children: [
                Builder(
                  builder: (_) {
                    final d = i == days.length - 1
                        ? fit.today
                        : byDate[DateTime(days[i].year, days[i].month, days[i].day)];
                    return RingsView(
                      move: d == null ? 0 : d.moveKcal / fit.moveGoal,
                      exercise: d == null ? 0 : d.exerciseMin / fit.exerciseGoal,
                      stand: d == null ? 0 : d.standHours / fit.standGoal,
                      size: 38,
                    );
                  },
                ),
                const SizedBox(height: 8),
                Text(
                  _names[days[i].weekday - 1],
                  style: TextStyle(
                    color: i == days.length - 1 ? AppColors.text : AppColors.textDim,
                    fontSize: 12,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ],
            ),
          ),
      ],
    );
  }
}

class _WorkoutRow extends ConsumerWidget {
  final Workout w;
  const _WorkoutRow(this.w);

  String _dur(Duration d) => d.inHours > 0
      ? '${d.inHours}:${(d.inMinutes % 60).toString().padLeft(2, '0')}:${(d.inSeconds % 60).toString().padLeft(2, '0')}'
      : '${d.inMinutes}:${(d.inSeconds % 60).toString().padLeft(2, '0')}';

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final p = w.pace;
    final parts = [
      _dur(w.duration),
      if (w.meters > 0) '${(w.meters / 1000).toStringAsFixed(2)} km${w.gps ? ' (GPS)' : ''}',
      '${w.kcal} kcal',
      if (p != null) '${p.inMinutes}\'${(p.inSeconds % 60).toString().padLeft(2, '0')}" /km',
    ];
    final ml = MaterialLocalizations.of(context);
    return ListTile(
      leading: IconChip(
        w.type.contains('Cycl')
            ? Icons.directions_bike_rounded
            : w.type.contains('Run')
            ? Icons.directions_run_rounded
            : w.type.contains('Walk') || w.type.contains('Hik')
            ? Icons.directions_walk_rounded
            : Icons.fitness_center_rounded,
        color: ringExercise,
      ),
      title: Text(w.type),
      subtitle: Text(
        '${ml.formatMediumDate(w.start)}, ${TimeOfDay.fromDateTime(w.start).format(context)}\n${parts.join(' · ')}',
      ),
      isThreeLine: true,
      onLongPress: () async {
        final ok = await showDialog<bool>(
          context: context,
          builder: (ctx) => AlertDialog(
            title: const Text('Delete this workout?'),
            content: Text('${w.type}, ${ml.formatMediumDate(w.start)}'),
            actions: [
              TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
              TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Delete')),
            ],
          ),
        );
        if (ok == true) await ref.read(fitnessProvider.notifier).deleteWorkout(w);
      },
    );
  }
}

class _WeekBars extends StatelessWidget {
  final List<int> values;
  final List<DateTime> days;
  final int goal;
  const _WeekBars({required this.values, required this.days, required this.goal});

  static const _names = ['M', 'T', 'W', 'T', 'F', 'S', 'S'];

  @override
  Widget build(BuildContext context) {
    final maxV = math.max(goal, values.fold<int>(0, math.max)).toDouble();
    return Row(
      crossAxisAlignment: CrossAxisAlignment.end,
      children: [
        for (var i = 0; i < values.length; i++)
          Expanded(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 5),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.end,
                children: [
                  Expanded(
                    child: Align(
                      alignment: Alignment.bottomCenter,
                      child: FractionallySizedBox(
                        heightFactor: maxV <= 0 ? 0.02 : math.max(0.02, values[i] / maxV),
                        child: Container(
                          decoration: BoxDecoration(
                            color: values[i] >= goal
                                ? AppColors.success
                                : (i == values.length - 1 ? AppColors.accent : AppColors.surfaceHigh),
                            borderRadius: BorderRadius.circular(6),
                          ),
                        ),
                      ),
                    ),
                  ),
                  const SizedBox(height: 8),
                  Text(
                    _names[days[i].weekday - 1],
                    style: TextStyle(
                      color: i == values.length - 1 ? AppColors.text : AppColors.textDim,
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ],
              ),
            ),
          ),
      ],
    );
  }
}

/// Weight and height, kept on the watch: calories from the weight, the
/// stride (distance from steps) from the height. Metric or imperial.
class _BodyCard extends ConsumerStatefulWidget {
  final FitnessState fit;
  const _BodyCard({required this.fit});
  @override
  ConsumerState<_BodyCard> createState() => _BodyCardState();
}

class _BodyCardState extends ConsumerState<_BodyCard> {
  bool _imperial = false;

  @override
  void initState() {
    super.initState();
    SharedPreferences.getInstance().then((p) {
      if (mounted) setState(() => _imperial = p.getBool('units_imperial') ?? false);
    });
  }

  String _w(int kg) => _imperial ? '${(kg * 2.20462).round()} lb' : '$kg kg';
  String _h(int cm) {
    if (!_imperial) return '$cm cm';
    final inches = (cm / 2.54).round();
    return '${inches ~/ 12}\' ${inches % 12}"';
  }

  Future<void> _edit() async {
    var kg = widget.fit.weightKg.toDouble(), cm = widget.fit.heightCm.toDouble();
    var imperial = _imperial;
    final ok = await showModalBottomSheet<bool>(
      context: context,
      builder: (ctx) => StatefulBuilder(
        builder: (ctx, set) => SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(20, 16, 20, 12),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    const Expanded(
                      child: Text('Your body', style: TextStyle(fontSize: 17, fontWeight: FontWeight.w600)),
                    ),
                    SegmentedButton<bool>(
                      segments: const [
                        ButtonSegment(value: false, label: Text('kg, cm')),
                        ButtonSegment(value: true, label: Text('lb, ft')),
                      ],
                      selected: {imperial},
                      showSelectedIcon: false,
                      onSelectionChanged: (v) => set(() => imperial = v.first),
                    ),
                  ],
                ),
                const SizedBox(height: 16),
                Text('Weight  ${imperial ? '${(kg * 2.20462).round()} lb' : '${kg.round()} kg'}'),
                Slider(value: kg, min: 30, max: 250, divisions: 220, onChanged: (v) => set(() => kg = v)),
                Text(
                  'Height  ${() {
                    if (!imperial) return '${cm.round()} cm';
                    final i = (cm / 2.54).round();
                    return '${i ~/ 12}\' ${i % 12}"';
                  }()}',
                ),
                Slider(value: cm, min: 100, max: 230, divisions: 130, onChanged: (v) => set(() => cm = v)),
                const SizedBox(height: 4),
                const Text(
                  'Only used on the watch for calories and distance from steps.',
                  style: TextStyle(color: AppColors.textFaint, fontSize: 12),
                ),
                const SizedBox(height: 8),
                Align(
                  alignment: Alignment.centerRight,
                  child: FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('Save')),
                ),
              ],
            ),
          ),
        ),
      ),
    );
    if (ok != true) return;
    final p = await SharedPreferences.getInstance();
    await p.setBool('units_imperial', imperial);
    if (mounted) setState(() => _imperial = imperial);
    await ref.read(fitnessProvider.notifier).setBody(kg.round(), cm.round());
  }

  @override
  Widget build(BuildContext context) {
    final connected = ref.watch(bleProvider).isConnected;
    return RowGroup(
      children: [
        NavRow(
          icon: Icons.monitor_weight_outlined,
          color: AppColors.accent2,
          title: '${_w(widget.fit.weightKg)} · ${_h(widget.fit.heightCm)}',
          subtitle: connected
              ? 'For calories and distance from steps - tap to change'
              : 'Connect the watch to change it',
          onTap: connected ? _edit : null,
        ),
      ],
    );
  }
}
