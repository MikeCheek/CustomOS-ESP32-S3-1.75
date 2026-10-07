import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Steps counted by the watch (it reports every few minutes while
/// connected), with a daily goal and the last week.
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
              child: Text('Daily goal', style: TextStyle(color: AppColors.text, fontSize: 18, fontWeight: FontWeight.w600)),
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
    String key(DateTime d) =>
        '${d.year}-${d.month.toString().padLeft(2, '0')}-${d.day.toString().padLeft(2, '0')}';
    final values = days.map((d) => link.stepsHistory[key(d)] ?? 0).toList();
    if (steps >= 0) values[6] = math.max(values[6], steps);
    final todaySteps = values[6];
    final km = todaySteps * 0.00075;
    final kcal = (todaySteps * 0.04).round();

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
                      Text('$todaySteps',
                          style: const TextStyle(
                              color: AppColors.text,
                              fontSize: 40,
                              fontWeight: FontWeight.w700,
                              fontFeatures: [FontFeature.tabularFigures()])),
                      GestureDetector(
                        onTap: _editGoal,
                        child: Text('of $_goal steps',
                            style: const TextStyle(color: AppColors.textDim, fontSize: 13, decoration: TextDecoration.underline)),
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
              Expanded(child: StatTile(icon: Icons.straighten_rounded, color: AppColors.accent2, value: km.toStringAsFixed(1), label: 'km (est.)')),
              const SizedBox(width: 10),
              Expanded(child: StatTile(icon: Icons.local_fire_department_outlined, color: AppColors.accent3, value: '$kcal', label: 'kcal (est.)')),
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
            child: SizedBox(
              height: 160,
              child: _WeekBars(values: values, days: days, goal: _goal),
            ),
          ),
          const SizedBox(height: 12),
          const Text(
            'The watch counts steps with its motion sensor and reports them every few minutes while connected. '
            'Distance and calories are rough estimates from the step count.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
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
                  Text(_names[days[i].weekday - 1],
                      style: TextStyle(
                          color: i == values.length - 1 ? AppColors.text : AppColors.textDim,
                          fontSize: 12,
                          fontWeight: FontWeight.w600)),
                ],
              ),
            ),
          ),
      ],
    );
  }
}
