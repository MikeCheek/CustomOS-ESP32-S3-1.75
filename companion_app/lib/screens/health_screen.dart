import 'dart:io';

import 'package:flutter/material.dart';

import '../services/health_sync.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Health Connect (Android) or Apple Health (iOS): the watch's activity
/// written there, so other apps (and the phone's own health app) see it.
class HealthScreen extends StatefulWidget {
  const HealthScreen({super.key});

  @override
  State<HealthScreen> createState() => _HealthScreenState();
}

class _HealthScreenState extends State<HealthScreen> {
  final _sync = HealthSync.instance;
  bool _on = false, _busy = false;
  String? _missing;
  DateTime? _last;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    final on = await _sync.enabled();
    String? missing;
    try {
      missing = await _sync.unavailable();
    } catch (_) {
      missing = Platform.isAndroid ? 'Health Connect isn\'t available' : null;
    }
    final last = await _sync.lastSync();
    if (mounted) {
      setState(() {
        _on = on;
        _missing = missing;
        _last = last;
      });
    }
  }

  Future<void> _toggle(bool v) async {
    setState(() => _busy = true);
    try {
      if (v) {
        final ok = await _sync.enable();
        if (!ok && mounted) {
          showSnack(context, 'Access wasn\'t given - allow it in ${_sync.storeName} to sync');
        }
      } else {
        await _sync.disable();
      }
    } catch (e) {
      if (mounted) showSnack(context, 'Couldn\'t connect to ${_sync.storeName}: $e');
    }
    if (mounted) setState(() => _busy = false);
    await _refresh();
  }

  @override
  Widget build(BuildContext context) {
    final store = _sync.storeName;
    final shared = [
      (Icons.directions_walk_rounded, 'Steps', 'As the day goes, from the watch\'s pedometer'),
      (Icons.local_fire_department_rounded, 'Active energy', 'The Move ring\'s calories'),
      (Icons.directions_run_rounded, 'Workouts', 'Type, time, calories and distance - each one once'),
      if (Platform.isIOS) (Icons.self_improvement_rounded, 'Mindful minutes', 'From Breathe in the Life app'),
      (Icons.monitor_weight_outlined, 'Weight and height', 'When you change them'),
    ];
    return Scaffold(
      appBar: AppBar(title: Text(store)),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          if (_missing != null) ...[
            Panel(
              color: AppColors.warning.withValues(alpha: 0.1),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(_missing!, style: const TextStyle(fontWeight: FontWeight.w600)),
                  const SizedBox(height: 6),
                  const Text(
                    'Health Connect is Google\'s place for health data on Android - built into Android 14 and newer, '
                    'an app from the Play Store before that.',
                    style: TextStyle(color: AppColors.textDim, fontSize: 13),
                  ),
                  const SizedBox(height: 10),
                  FilledButton(
                    onPressed: () async {
                      await _sync.install();
                      await Future.delayed(const Duration(seconds: 1));
                      await _refresh();
                    },
                    child: const Text('Get Health Connect'),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 12),
          ],
          RowGroup(
            children: [
              ToggleRow(
                icon: Icons.favorite_rounded,
                color: AppColors.danger,
                title: 'Write to $store',
                subtitle: _on
                    ? (_last == null
                          ? 'On - the first numbers go with the next report from the watch'
                          : 'Last written ${TimeOfDay.fromDateTime(_last!).format(context)}')
                    : 'The watch\'s activity, alongside your other health data',
                value: _on,
                onChanged: _busy || _missing != null ? null : _toggle,
              ),
            ],
          ),
          const SectionLabel('What\'s written'),
          RowGroup(
            children: [
              for (final (i, t, s) in shared)
                ListTile(
                  leading: Icon(i, color: AppColors.textDim),
                  title: Text(t),
                  subtitle: Text(s),
                ),
            ],
          ),
          const SizedBox(height: 12),
          Text(
            'Write only: the app reads nothing from $store. Totals are added in pieces as the watch reports, so '
            '$store\'s own sums are right and nothing is counted twice. The watch has no heart-rate sensor, so there\'s '
            'no heart rate.${Platform.isAndroid ? ' Health Connect has no mindful-minutes type the app can use yet.' : ''} '
            'Turn individual kinds on or off in $store\'s own settings.',
            style: const TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}
