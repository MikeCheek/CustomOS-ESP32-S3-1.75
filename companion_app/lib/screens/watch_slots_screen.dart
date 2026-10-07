import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import '../providers/settings_provider.dart';
import '../theme/app_theme.dart';
import '../widgets/common.dart';

/// The three round complications on the watch's default and minimal faces
/// (firmware 3.2+). Also settable on the watch: Settings > Watchface slots.
class WatchSlotsScreen extends ConsumerWidget {
  const WatchSlotsScreen({super.key});

  static const options = <String, (String, IconData)>{
    'none': ('Empty', Icons.radio_button_unchecked),
    'steps': ('Steps', Icons.directions_walk_rounded),
    'battery': ('Watch battery', Icons.battery_full_rounded),
    'phone_battery': ('Phone battery', Icons.smartphone_rounded),
    'next_event': ('Next event', Icons.event_rounded),
    'weather': ('Weather', Icons.cloud_rounded),
    'notifications': ('Messages', Icons.notifications_rounded),
    'navigation': ('Next turn', Icons.navigation_rounded),
    'todos': ('Memo to-dos', Icons.task_alt_rounded),
    'seconds': ('Seconds', Icons.timer_rounded),
  };

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final s = ref.watch(settingsProvider);
    final slots = [...s.compSlots];
    while (slots.length < 3) {
      slots.add('none');
    }
    const names = ['Left', 'Middle', 'Right'];
    return Scaffold(
      appBar: AppBar(title: const Text('Watchface slots')),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
        children: [
          // preview
          Center(
            child: Container(
              width: 240,
              height: 240,
              decoration: const BoxDecoration(color: Colors.black, shape: BoxShape.circle),
              child: Stack(alignment: Alignment.center, children: [
                const Positioned(
                  top: 62,
                  child: Text('10:09', style: TextStyle(color: AppColors.text, fontSize: 44, fontWeight: FontWeight.w300)),
                ),
                Positioned(
                  bottom: 46,
                  child: Row(children: [
                    for (var i = 0; i < 3; i++) ...[
                      if (i > 0) const SizedBox(width: 8),
                      Container(
                        width: 48,
                        height: 48,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          border: Border.all(
                              color: (i == 1 ? AppColors.accent : AppColors.accent2)
                                  .withValues(alpha: slots[i] == 'none' ? 0.15 : 0.8),
                              width: 3),
                        ),
                        child: Icon(options[slots[i]]?.$2 ?? Icons.help_outline, size: 20, color: AppColors.text),
                      ),
                    ],
                  ]),
                ),
              ]),
            ),
          ),
          const SizedBox(height: 8),
          for (var i = 0; i < 3; i++) ...[
            SectionLabel(names[i]),
            Panel(
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 4),
              child: DropdownButtonHideUnderline(
                child: DropdownButton<String>(
                  isExpanded: true,
                  value: options.containsKey(slots[i]) ? slots[i] : 'none',
                  dropdownColor: AppColors.surfaceLight,
                  items: [
                    for (final e in options.entries)
                      DropdownMenuItem(
                        value: e.key,
                        child: Row(children: [
                          Icon(e.value.$2, size: 20, color: AppColors.textDim),
                          const SizedBox(width: 12),
                          Text(e.value.$1),
                        ]),
                      ),
                  ],
                  onChanged: (v) {
                    if (v == null) return;
                    final next = [...slots]..[i] = v;
                    ref.read(settingsProvider.notifier).update(s.copyWith(compSlots: next));
                  },
                ),
              ),
            ),
          ],
          const SizedBox(height: 16),
          const Text(
            'Shown on the Default and Minimal watchfaces. Custom watchfaces can add the same rings from the designer. '
            '"Memo to-dos" shows the open to-dos from your voice memos. Needs watch firmware 3.2 or newer.',
            style: TextStyle(color: AppColors.textFaint, fontSize: 12, height: 1.4),
          ),
        ],
      ),
    );
  }
}
