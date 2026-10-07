import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/ble_provider.dart';
import '../providers/companion_provider.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// Local weather (Open-Meteo, from the phone's location), refreshed every
/// 30 minutes while connected and shown on the watch face's weather widget.
class WeatherScreen extends ConsumerWidget {
  const WeatherScreen({super.key});

  static IconData iconFor(int code) {
    if (code == 0) return Icons.wb_sunny_rounded;
    if (code <= 2) return Icons.wb_cloudy_outlined;
    if (code == 3) return Icons.cloud_rounded;
    if (code == 45 || code == 48) return Icons.foggy;
    if (code >= 71 && code <= 77 || code == 85 || code == 86) return Icons.ac_unit_rounded;
    if (code >= 95) return Icons.thunderstorm_rounded;
    return Icons.water_drop_rounded;
  }

  static IconData iconForText(String c) {
    final l = c.toLowerCase();
    if (l.contains('clear')) return Icons.wb_sunny_rounded;
    if (l.contains('partly')) return Icons.wb_cloudy_outlined;
    if (l.contains('cloud')) return Icons.cloud_rounded;
    if (l.contains('fog')) return Icons.foggy;
    if (l.contains('snow')) return Icons.ac_unit_rounded;
    if (l.contains('storm')) return Icons.thunderstorm_rounded;
    return Icons.water_drop_rounded;
  }

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final link = ref.watch(companionProvider);
    final s = ref.watch(settingsProvider);
    final connected = ref.watch(bleProvider).isConnected;
    final w = link.weather;
    String fmt(double c) => '${(s.fahrenheit ? c * 9 / 5 + 32 : c).round()}°';

    return Scaffold(
      appBar: AppBar(
        title: const Text('Weather'),
        actions: [
          IconButton(
            tooltip: 'Refresh',
            onPressed: link.weatherLoading ? null : () => ref.read(companionProvider.notifier).refreshWeather(interactive: true),
            icon: link.weatherLoading
                ? const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2))
                : const Icon(Icons.refresh_rounded),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          if (w == null)
            Panel(
              child: EmptyState(
                icon: Icons.wb_cloudy_outlined,
                title: link.weatherLoading ? 'Getting the weather…' : 'No weather yet',
                subtitle: link.weatherError ?? 'Uses your phone\'s location and Open-Meteo. Nothing to sign up for.',
                action: link.weatherLoading
                    ? null
                    : FilledButton.icon(
                        onPressed: () => ref.read(companionProvider.notifier).refreshWeather(interactive: true),
                        icon: const Icon(Icons.my_location_rounded, size: 18),
                        label: const Text('Get local weather'),
                      ),
              ),
            )
          else ...[
            Panel(
              padding: const EdgeInsets.all(24),
              gradient: const LinearGradient(
                begin: Alignment.topLeft,
                end: Alignment.bottomRight,
                colors: [Color(0xFF13233A), Color(0xFF0B0B10)],
              ),
              child: Row(
                children: [
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(fmt(w.tempC),
                            style: const TextStyle(color: AppColors.text, fontSize: 64, fontWeight: FontWeight.w300, height: 1)),
                        const SizedBox(height: 8),
                        Text(w.condition, style: const TextStyle(color: AppColors.text, fontSize: 18, fontWeight: FontWeight.w600)),
                        const SizedBox(height: 4),
                        Text('Humidity ${w.humidity}%  ·  ${TimeOfDay.fromDateTime(w.fetchedAt).format(context)}',
                            style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
                      ],
                    ),
                  ),
                  Icon(iconFor(w.code), color: AppColors.warning, size: 72),
                ],
              ),
            ),
            if (link.weatherError != null) ...[
              const SizedBox(height: 8),
              Text(link.weatherError!, style: const TextStyle(color: AppColors.warning, fontSize: 12)),
            ],
            const SectionLabel('Next days'),
            Row(
              children: [
                for (final f in w.forecast) ...[
                  Expanded(
                    child: Panel(
                      padding: const EdgeInsets.symmetric(vertical: 16),
                      child: Column(
                        children: [
                          Text(f.day, style: const TextStyle(color: AppColors.textDim, fontSize: 13, fontWeight: FontWeight.w600)),
                          const SizedBox(height: 10),
                          Icon(iconForText(f.condition), color: AppColors.warning, size: 26),
                          const SizedBox(height: 10),
                          Text(fmt(f.high), style: const TextStyle(color: AppColors.text, fontSize: 16, fontWeight: FontWeight.w700)),
                          Text(fmt(f.low), style: const TextStyle(color: AppColors.textDim, fontSize: 13)),
                        ],
                      ),
                    ),
                  ),
                  if (f != w.forecast.last) const SizedBox(width: 10),
                ],
              ],
            ),
          ],
          const SectionLabel('Settings'),
          RowGroup(children: [
            ToggleRow(
              icon: Icons.autorenew_rounded,
              color: AppColors.accent2,
              title: 'Update automatically',
              subtitle: 'Every 30 minutes while the watch is connected',
              value: s.weatherAuto,
              onChanged: (v) => ref.read(settingsProvider.notifier).update(s.copyWith(weatherAuto: v)),
            ),
            ToggleRow(
              icon: Icons.thermostat_rounded,
              color: AppColors.warning,
              title: 'Fahrenheit',
              subtitle: 'In the app; the watch face widget has its own setting',
              value: s.fahrenheit,
              onChanged: (v) => ref.read(settingsProvider.notifier).update(s.copyWith(fahrenheit: v)),
            ),
            NavRow(
              icon: Icons.watch_outlined,
              title: 'Send to watch now',
              subtitle: connected ? 'Shown by the weather widget on custom faces' : 'Connect the watch first',
              onTap: connected && w != null
                  ? () async {
                      await ref.read(bleServiceProvider).sendWeather(w.toWatch());
                      if (context.mounted) showSnack(context, 'Weather sent');
                    }
                  : null,
            ),
          ]),
        ],
      ),
    );
  }
}
